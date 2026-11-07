#include "live_view.h"

#include <glib/gstdio.h>

#include <algorithm>

#include "i18n.h"

namespace icmon {

namespace {
constexpr gint64 kStableMicros = 30 * G_USEC_PER_SEC;  // a stream that played this long resets the retry counter
constexpr guint kRetryDelayMs = 2500;
}  // namespace

struct LiveView::Tile {
    LiveView* owner = nullptr;
    int index = 0;
    GtkWidget *frame = nullptr, *picture = nullptr, *name = nullptr, *msg = nullptr;
    GtkWidget *bar = nullptr, *light_btn = nullptr, *snap_btn = nullptr, *mute_btn = nullptr, *vol = nullptr;
    bool attached = false;
    std::string camera_id, key;
    LiveParams params;  // kept for automatic restarts after a drop (holds the address: cleared on stop)
    int retries = 0;
    gint64 playing_since = 0;
    guint retry_timer = 0;
    LivePlayer player;
};

LiveView::LiveView() {
    root_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    grid_ = gtk_grid_new();
    gtk_grid_set_row_homogeneous(GTK_GRID(grid_), TRUE);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid_), TRUE);
    gtk_grid_set_row_spacing(GTK_GRID(grid_), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid_), 8);
    gtk_widget_set_hexpand(grid_, TRUE);
    gtk_widget_set_vexpand(grid_, TRUE);
    gtk_box_append(GTK_BOX(root_), grid_);

    for (int i = 0; i < kMaxSlots; i++) {
        auto t = std::make_unique<Tile>();
        t->owner = this;
        t->index = i;
        t->frame = gtk_overlay_new();
        // The tile is owned by this class, not by the grid: gtk_grid_remove() would otherwise destroy it when the
        // layout changes, and the next use would touch a dead widget.
        g_object_ref_sink(t->frame);
        gtk_widget_add_css_class(t->frame, "video-tile");
        gtk_widget_add_css_class(t->frame, "empty-tile");
        gtk_widget_set_overflow(t->frame, GTK_OVERFLOW_HIDDEN);  // rounded corners clip the picture (no CSS "overflow" in GTK 4.6)
        gtk_widget_set_hexpand(t->frame, TRUE);
        gtk_widget_set_vexpand(t->frame, TRUE);
        gtk_widget_set_size_request(t->frame, 160, 120);

        t->picture = gtk_picture_new();
        gtk_picture_set_can_shrink(GTK_PICTURE(t->picture), TRUE);
#if GTK_CHECK_VERSION(4, 8, 0)
        gtk_picture_set_content_fit(GTK_PICTURE(t->picture), GTK_CONTENT_FIT_CONTAIN);
#else
        gtk_picture_set_keep_aspect_ratio(GTK_PICTURE(t->picture), TRUE);
#endif
        gtk_overlay_set_child(GTK_OVERLAY(t->frame), t->picture);

        t->name = gtk_label_new("");
        gtk_widget_add_css_class(t->name, "tile-name");
        gtk_widget_set_halign(t->name, GTK_ALIGN_START);
        gtk_widget_set_valign(t->name, GTK_ALIGN_START);
        gtk_overlay_add_overlay(GTK_OVERLAY(t->frame), t->name);

        t->msg = gtk_label_new("");
        gtk_widget_add_css_class(t->msg, "tile-msg");
        gtk_widget_set_halign(t->msg, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(t->msg, GTK_ALIGN_CENTER);
        gtk_label_set_wrap(GTK_LABEL(t->msg), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(t->msg), 40);
        gtk_label_set_justify(GTK_LABEL(t->msg), GTK_JUSTIFY_CENTER);
        gtk_widget_set_visible(t->msg, FALSE);
        gtk_overlay_add_overlay(GTK_OVERLAY(t->frame), t->msg);

        // icon bar of the selected tile: save frame, sound on/off (muted by default), volume (always visible)
        t->bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        gtk_widget_add_css_class(t->bar, "tile-bar");
        gtk_widget_set_halign(t->bar, GTK_ALIGN_END);
        gtk_widget_set_valign(t->bar, GTK_ALIGN_START);
        t->light_btn = gtk_toggle_button_new();
        gtk_button_set_icon_name(GTK_BUTTON(t->light_btn), "weather-clear-night-symbolic");
        gtk_widget_set_tooltip_text(t->light_btn, _("Camera light on / off (shows what was done in this session)"));
        t->snap_btn = gtk_button_new_from_icon_name("camera-photo-symbolic");
        gtk_widget_set_tooltip_text(t->snap_btn, _("Save the current frame (PNG, full camera resolution)"));
        t->mute_btn = gtk_toggle_button_new();
        gtk_button_set_icon_name(GTK_BUTTON(t->mute_btn), "audio-volume-muted-symbolic");
        gtk_widget_set_tooltip_text(t->mute_btn, _("Sound on / off (off by default)"));
        t->vol = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 0.05);
        gtk_scale_set_draw_value(GTK_SCALE(t->vol), FALSE);
        gtk_range_set_value(GTK_RANGE(t->vol), volume_);
        gtk_widget_set_size_request(t->vol, 90, -1);
        gtk_widget_set_tooltip_text(t->vol, _("Output volume"));
        for (GtkWidget* b : {t->light_btn, t->snap_btn, t->mute_btn, t->vol}) {
            gtk_widget_set_valign(b, GTK_ALIGN_CENTER);  // explicit alignment avoids GtkImage baseline warnings
            gtk_box_append(GTK_BOX(t->bar), b);
        }
        gtk_widget_add_css_class(t->light_btn, "flat");
        gtk_widget_add_css_class(t->snap_btn, "flat");
        g_signal_connect(t->light_btn, "toggled", G_CALLBACK(light_cb), t.get());
        gtk_widget_add_css_class(t->mute_btn, "flat");
        g_signal_connect(t->snap_btn, "clicked", G_CALLBACK(snapshot_cb), t.get());
        g_signal_connect(t->mute_btn, "toggled", G_CALLBACK(mute_cb), t.get());
        g_signal_connect(t->vol, "value-changed", G_CALLBACK(volume_cb), t.get());
        gtk_widget_set_visible(t->bar, FALSE);
        gtk_overlay_add_overlay(GTK_OVERLAY(t->frame), t->bar);

        GtkGesture* click = gtk_gesture_click_new();
        g_signal_connect(click, "pressed", G_CALLBACK(pressed_cb), t.get());
        gtk_widget_add_controller(t->frame, GTK_EVENT_CONTROLLER(click));

        // Manual order: drag a window onto another one to swap the two cameras (the list order changes with it).
        {
            GtkDragSource* src = gtk_drag_source_new();
            gtk_drag_source_set_actions(src, GDK_ACTION_MOVE);
            g_signal_connect(src, "prepare", G_CALLBACK(+[](GtkDragSource*, double, double, gpointer p) -> GdkContentProvider* {
                Tile* tl = (Tile*)p;
                if (tl->camera_id.empty()) return nullptr;
                return gdk_content_provider_new_typed(G_TYPE_STRING, tl->camera_id.c_str());
            }), t.get());
            // no text under the pointer while dragging: an empty 1x1 picture is the drag icon
            g_signal_connect(src, "drag-begin", G_CALLBACK(+[](GtkDragSource* s, GdkDrag*, gpointer) {
                GdkPaintable* none = gdk_paintable_new_empty(1, 1);
                gtk_drag_source_set_icon(s, none, 0, 0);
                g_object_unref(none);
            }), nullptr);
            gtk_widget_add_controller(t->frame, GTK_EVENT_CONTROLLER(src));
            GtkDropTarget* dst = gtk_drop_target_new(G_TYPE_STRING, GDK_ACTION_MOVE);
            g_signal_connect(dst, "drop", G_CALLBACK(+[](GtkDropTarget*, const GValue* v, double, double, gpointer p) -> gboolean {
                Tile* tl = (Tile*)p;
                if (!G_VALUE_HOLDS_STRING(v) || !tl->owner->on_drop) return FALSE;
                std::string from = g_value_get_string(v);
                int pos = tl->owner->position_of(tl->index);
                if (from == tl->camera_id || pos < 0) return FALSE;
                tl->owner->on_drop(from, tl->camera_id, pos);
                return TRUE;
            }), t.get());
            gtk_widget_add_controller(t->frame, GTK_EVENT_CONTROLLER(dst));
        }

        Tile* raw = t.get();
        t->player.on_frame = [raw](GdkTexture* tex) {
            gtk_picture_set_paintable(GTK_PICTURE(raw->picture), GDK_PAINTABLE(tex));
            g_object_unref(tex);
        };
        t->player.on_state = [raw](LiveState st, const std::string& text) { raw->owner->tile_state(*raw, st, text); };
        tiles_.push_back(std::move(t));
    }
    arrange({0});
}

LiveView::~LiveView() {
    for (auto& t : tiles_) {
        if (t->retry_timer) g_source_remove(t->retry_timer);
        t->player.stop();
        g_object_unref(t->frame);  // our own reference (see the constructor)
    }
}

// State changes of one tile's stream. A drop after the stream had played is retried a few times (the login worked);
// everything else is reported and left to the user.
void LiveView::tile_state(Tile& t, LiveState st, const std::string& text) {
    int i = t.index;
    switch (st) {
        case LiveState::Connecting:
            set_message(i, _("Connecting…"));
            break;
        case LiveState::Playing:
            t.playing_since = g_get_monotonic_time();
            set_message(i, "");
            break;
        case LiveState::Dropped: {
            if (t.playing_since && g_get_monotonic_time() - t.playing_since > kStableMicros) t.retries = 0;
            t.playing_since = 0;
            if (t.retries < kMaxRetries && !t.params.url.empty()) {
                t.retries++;
                std::string m = std::string(_("Connection lost, reconnecting (")) + std::to_string(t.retries) + "/" +
                                std::to_string(kMaxRetries) + ")…";
                set_message(i, m);
                if (on_state) on_state(i, LiveState::Connecting, m);
                if (!t.retry_timer) t.retry_timer = g_timeout_add(kRetryDelayMs, retry_cb, &t);
                return;
            }
            std::string m = text + " " + _("(automatic reconnection gave up; press the restart button)");
            set_message(i, m);
            if (on_state) on_state(i, LiveState::Failed, m);
            return;
        }
        case LiveState::Failed:
        case LiveState::Rejected:
        case LiveState::Ended:
            set_message(i, text);
            break;
    }
    if (on_state) on_state(i, st, text);
}

gboolean LiveView::retry_cb(gpointer p) {
    Tile* t = (Tile*)p;
    t->retry_timer = 0;
    if (!t->params.url.empty()) t->player.start(t->params);  // reports Connecting through on_state
    return G_SOURCE_REMOVE;
}

void LiveView::pressed_cb(GtkGestureClick* g, int n_press, double, double, gpointer p) {
    Tile* t = (Tile*)p;
    LiveView* v = t->owner;
    if (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g)) == 3) return;  // right click: nothing
    if (n_press == 1 && v->on_clicked) v->on_clicked(t->index);
    else if (n_press == 2 && v->on_activated) v->on_activated(t->index);
}

void LiveView::arrange(const std::vector<int>& want) {
    std::vector<int> order;
    for (int t : want)
        if (t >= 0 && t < kMaxSlots && std::find(order.begin(), order.end(), t) == order.end()) order.push_back(t);
    size_t n = order.size() >= 16 ? 16 : order.size() >= 4 ? 4 : order.size() >= 2 ? 2 : 1;
    if (order.empty()) order = {0};
    order.resize(n);
    // tiles not shown must not keep streaming
    for (int i = 0; i < kMaxSlots; i++)
        if (std::find(order.begin(), order.end(), i) == order.end()) { stop(i); assign(i, "", ""); }
    if (order == order_) return;
    order_ = order;
    for (auto& t : tiles_)
        if (t->attached) { gtk_grid_remove(GTK_GRID(grid_), t->frame); t->attached = false; }
    int slots = (int)order_.size();
    for (int i = 0; i < slots; i++) {
        int cols = slots == 16 ? 4 : slots >= 2 ? 2 : 1;
        int col = i % cols, row = i / cols;
        gtk_grid_attach(GTK_GRID(grid_), tiles_[order_[i]]->frame, col, row, 1, 1);
        tiles_[order_[i]]->attached = true;
    }
}

int LiveView::position_of(int tile) const {
    for (size_t i = 0; i < order_.size(); i++)
        if (order_[i] == tile) return (int)i;
    return -1;
}

void LiveView::assign(int tile, const std::string& camera_id, const std::string& name) {
    if (tile < 0 || tile >= kMaxSlots) return;
    Tile& t = *tiles_[tile];
    t.camera_id = camera_id;
    if (camera_id.empty()) gtk_widget_add_css_class(t.frame, "empty-tile");
    else gtk_widget_remove_css_class(t.frame, "empty-tile");
    gtk_label_set_text(GTK_LABEL(t.name), name.c_str());
    gtk_widget_set_visible(t.name, !name.empty());
}

const std::string& LiveView::camera_id(int tile) const {
    static const std::string none;
    return (tile < 0 || tile >= kMaxSlots) ? none : tiles_[tile]->camera_id;
}

const std::string& LiveView::key(int tile) const {
    static const std::string none;
    return (tile < 0 || tile >= kMaxSlots) ? none : tiles_[tile]->key;
}

int LiveView::tile_of(const std::string& id) const {
    for (int i = 0; i < kMaxSlots; i++)
        if (tiles_[i]->camera_id == id && !tiles_[i]->key.empty()) return i;
    return -1;
}

void LiveView::start(int tile, const std::string& key, const std::string& url, int height) {
    if (tile < 0 || tile >= kMaxSlots) return;
    Tile& t = *tiles_[tile];
    if (t.key == key) return;  // already running with these settings, or it failed: the user decides when to retry
    if (t.retry_timer) { g_source_remove(t.retry_timer); t.retry_timer = 0; }
    t.key = key;
    t.retries = 0;
    t.playing_since = 0;
    t.params = LiveParams{};
    t.params.url = url;
    t.params.height = height;
    t.player.start(t.params);
    apply_audio();
}

void LiveView::stop(int tile, const std::string& message) {
    if (tile < 0 || tile >= kMaxSlots) return;
    Tile& t = *tiles_[tile];
    if (t.retry_timer) { g_source_remove(t.retry_timer); t.retry_timer = 0; }
    t.player.stop();
    t.key.clear();
    std::fill(t.params.url.begin(), t.params.url.end(), '\0');  // the address holds the password
    t.params = LiveParams{};
    gtk_picture_set_paintable(GTK_PICTURE(t.picture), nullptr);
    set_message(tile, message);
}

void LiveView::stop_all() {
    for (int i = 0; i < kMaxSlots; i++) stop(i);
}

void LiveView::set_message(int tile, const std::string& text) {
    if (tile < 0 || tile >= kMaxSlots) return;
    Tile& t = *tiles_[tile];
    gtk_label_set_text(GTK_LABEL(t.msg), text.c_str());
    gtk_widget_set_visible(t.msg, !text.empty());
}

void LiveView::set_selected(int tile) {
    selected_ = tile;
    for (int i = 0; i < kMaxSlots; i++) {
        if (i == tile && slots() > 1) gtk_widget_add_css_class(tiles_[i]->frame, "selected");
        else gtk_widget_remove_css_class(tiles_[i]->frame, "selected");
        gtk_widget_set_visible(tiles_[i]->bar, i == tile && !tiles_[i]->camera_id.empty());
    }
    apply_audio();
}

// Sound comes from the selected tile only, and only while the sound button is on. Nothing is restarted.
void LiveView::apply_audio() {
    for (int i = 0; i < kMaxSlots; i++) tiles_[i]->player.set_audio(sound_on_ && i == selected_, volume_);
}

static void light_icon(GtkWidget* b, bool on);

void LiveView::light_cb(GtkToggleButton* b, gpointer p) {
    Tile* t = (Tile*)p;
    LiveView* v = t->owner;
    if (v->updating_) return;
    light_icon(GTK_WIDGET(b), gtk_toggle_button_get_active(b));
    if (v->on_light) v->on_light(t->index, gtk_toggle_button_get_active(b));
}

// The icon shows the state: a sun while the light is on, a moon while it is off.
static void light_icon(GtkWidget* b, bool on) {
    gtk_button_set_icon_name(GTK_BUTTON(b), on ? "display-brightness-symbolic" : "weather-clear-night-symbolic");
}

void LiveView::set_light(bool on) {
    light_on_ = on;
    updating_ = true;
    for (auto& t : tiles_) { gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(t->light_btn), on); light_icon(t->light_btn, on); }
    updating_ = false;
}

void LiveView::mute_cb(GtkToggleButton* b, gpointer p) {
    Tile* t = (Tile*)p;
    LiveView* v = t->owner;
    if (v->updating_) return;
    v->sound_on_ = gtk_toggle_button_get_active(b);
    v->updating_ = true;
    for (auto& o : v->tiles_) {  // every tile's button shows the same state
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(o->mute_btn), v->sound_on_);
        gtk_button_set_icon_name(GTK_BUTTON(o->mute_btn), v->sound_on_ ? "audio-volume-high-symbolic" : "audio-volume-muted-symbolic");
    }
    v->updating_ = false;
    v->apply_audio();
}

void LiveView::volume_cb(GtkRange* r, gpointer p) {
    Tile* t = (Tile*)p;
    LiveView* v = t->owner;
    if (v->updating_) return;
    v->volume_ = gtk_range_get_value(r);
    v->updating_ = true;
    for (auto& o : v->tiles_) gtk_range_set_value(GTK_RANGE(o->vol), v->volume_);
    v->updating_ = false;
    v->apply_audio();
}

void LiveView::snapshot_cb(GtkButton*, gpointer p) {
    Tile* t = (Tile*)p;
    t->owner->snapshot(t->index);
}

// Saves the newest full-resolution frame of the tile's camera as PNG under <media folder>/picture.
void LiveView::snapshot(int tile) {
    Tile& t = *tiles_[tile];
    if (media_dir_.empty()) { if (on_notice) on_notice(_("No folder for saved media is set.")); return; }
    std::string dir = media_dir_ + "/picture";
    if (g_mkdir_with_parents(dir.c_str(), 0755) != 0) {
        if (on_notice) on_notice(std::string(_("Cannot create the folder ")) + dir);
        return;
    }
    std::string cam = gtk_label_get_text(GTK_LABEL(t.name));
    for (char& ch : cam)
        if (ch == '/' || ch == '\\' || ch == ':' || (unsigned char)ch < 0x20) ch = '_';
    if (cam.empty()) cam = "camera";
    GDateTime* now = g_date_time_new_now_local();
    gchar* stamp = g_date_time_format(now, "%d.%m.%Y_%H.%M.%S");
    g_date_time_unref(now);
    std::string path = dir + "/" + cam + "_" + stamp + ".png";
    g_free(stamp);
    t.player.snapshot(path, [this, path](bool ok, const std::string& text) {
        if (on_notice) on_notice(ok ? std::string(_("Frame saved: ")) + path : std::string(_("Could not save the frame: ")) + text);
    });
}

}  // namespace icmon
