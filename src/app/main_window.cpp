#include <cctype>
#include <unistd.h>
#include <thread>
// The CSS provider calls are deprecated since GTK 4.12 but still work; silence the warning for this file only.
#define GDK_DISABLE_DEPRECATION_WARNINGS
#include "main_window.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "camctl.h"

#include "i18n.h"
#include "rtsp_url.h"
#include "secrets.h"
#include "settings_dialog.h"

namespace icmon {

namespace {

// Tab switcher fix. GTK4 themes (Fluent, Mint-Y, ...) are often applied through ~/.config/gtk-4.0/gtk.css, which GTK loads
// at USER priority, above the APPLICATION priority of kCss: rules loaded there cannot override the theme. So these rules
// have their own provider at USER priority (added later = wins a tie) and a specific selector.
const char* const kTabCss =
    // Why a frame flashed on an inactive tab under the pointer: themes draw the tab's hover line as a box-shadow and the
    // resting button has a differently built shadow list (spread, layers), so GTK animates between the two and shows
    // a frame on the way. Giving the resting inactive tab a shadow of the same shape as the hover line (transparent) makes
    // the transition a plain fade. The hover look itself (line or background), the active tab and the speed of the fade
    // stay with the theme. Also: no outline.
    "window stackswitcher.tab-switcher > button.toggle:not(:checked):not(:hover):not(:active) {\n"
    "  box-shadow: inset 0 -2px transparent; border-color: transparent; border-image: none; }\n"
    "window stackswitcher.tab-switcher > button.toggle { outline: none; outline-width: 0; }\n"
    // the buttons on the video are white on the dark bar whatever the theme (a light theme gave them dark grey icons)
    "window .tile-bar button, window .tile-bar button:hover, window .tile-bar button:active, window .tile-bar button image,\n"
    "window .tile-bar button:hover image, window .tile-bar button:disabled image { color: #fff; -gtk-icon-style: symbolic; }\n"
    "window .tile-bar button:hover { background-color: rgba(255,255,255,0.18); }\n"
    "window .tile-bar scale trough { background-color: rgba(255,255,255,0.35); }\n"
    // the selected day of the calendar in the theme's accent colour (light themes drew it in the background colour); in the
    // USER provider so that a theme's own gtk.css cannot win
    "window calendar > grid > label.day-number:selected, window calendar label.day-number:selected:focus,\n"
    "window calendar label.day-number:checked:selected {\n"
    "  background-color: #3584e4; background-color: @theme_selected_bg_color;\n"
    "  color: #fff; color: @theme_selected_fg_color; border: 1px solid #3584e4; border-color: @theme_selected_bg_color;\n"
    "  border-radius: 999px; }\n"
    // the two header bars meet in a straight line: only the outer top corners of the window are rounded
    "window headerbar.hb-left, window.csd headerbar.hb-left { border-top-right-radius: 0; border-bottom-right-radius: 0; }\n"
    "window headerbar.hb-right, window.csd headerbar.hb-right { border-top-left-radius: 0; border-bottom-left-radius: 0; }\n"
    // the text of an inactive tab is dimmer than the active one's
    "window stackswitcher.tab-switcher > button.toggle:not(:checked) > label { opacity: 0.6; transition: opacity 150ms; }\n"
    "window stackswitcher.tab-switcher > button.toggle:not(:checked):hover > label { opacity: 0.85; }\n";

const char* const kCss =
    // a faint 1px border so that the tiles show on a black interface too; an empty tile is a translucent shade of the text
    // colour (visible on dark and on light themes); the selected tile has its blue frame instead of the border
    ".video-tile { background: #000; border-radius: 4px; border: 1px solid alpha(currentColor, 0.16); }\n"
    ".video-tile.empty-tile { background: alpha(currentColor, 0.07); }\n"
    ".video-tile.selected { border-color: transparent; }\n"
    ".video-tile picture { border-radius: 4px; }\n"
    // saved-position squares: themes give text buttons a minimum width of about 64px\n"
    ".sel-name { font-weight: bold; }\n"
    ".tnum { font-feature-settings: \"tnum\"; }\n"
    ".files-list > row { padding: 0; }\n"
    ".preset-btn { padding: 0; }\n"
    ".preset-btn:focus-visible { outline-offset: -3px; }\n"
    ".side-title { font-weight: 700; font-size: 1.05em; }\n"
    ".tile-bar { background: rgba(0,0,0,0.55); color: #fff; border-radius: 4px; margin: 6px; padding: 1px 2px; }\n"
    ".tile-bar button { color: #fff; }\n"
    // Days with recordings: GTK sets the CHECKED state on marked days of a GtkCalendar; they get a slightly lighter background and a 1px border.
    "calendar label.day-number:checked:not(:selected) { background-color: alpha(currentColor, 0.12); border: 1px solid alpha(currentColor, 0.4); border-radius: 6px; }\n"
    "calendar label.day-number:selected, calendar label.day-number:checked:selected { background-color: #3584e4; color: #fff; border: 1px solid #3584e4; border-radius: 6px; }\n"
    ".video-tile.selected { outline: 2px solid #3584e4; outline-offset: -2px; }\n"
    ".tile-name { background: rgba(0,0,0,0.55); color: #fff; padding: 2px 8px; margin: 6px; border-radius: 4px; }\n"
    ".tile-msg { background: rgba(0,0,0,0.65); color: #fff; padding: 8px 12px; border-radius: 6px; }\n"
    ".status-dot { min-width: 10px; min-height: 10px; border-radius: 5px; }\n"
    ".status-dot.dot-ok { background-color: #2ec27e; }\n"
    ".status-dot.dot-bad { background-color: #e01b24; }\n"
    ".status-dot.dot-wait { background-color: #f5c211; }\n"
    ".status-dot.dot-off { background-color: #888888; }\n";

constexpr int kRowHeight = 40;
// nav bar (52) + separator + side panel (300 + 2*10 margins): the left title-bar part is as wide as this, so that
// its own separator lies exactly on the line that ends the side panel
constexpr int kSideColumnWidth = 52 + 1 + 320;  // up to (not including) the separator that ends the side panel

constexpr int kMaxPresets = 16;  // positions per camera

const char* const kDotClasses[] = {"dot-ok", "dot-bad", "dot-wait", "dot-off"};

}  // namespace

// Section title in capitals, bold (CSS class side-title); sections other than the camera list can be folded.
static GtkWidget* section_title(const char* text) {
    gchar* up = g_utf8_strup(text, -1);
    GtkWidget* l = gtk_label_new(up);
    g_free(up);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_widget_add_css_class(l, "side-title");
    return l;
}

// A foldable section: [TITLE ........ action  arrow] and, below, the content in a revealer. When `grow` is set the
// section takes the free height of the panel while it is open. `action` (optional) sits at the right of the title.
static void section_toggle(GtkWidget* box) {
    GtkRevealer* rev = GTK_REVEALER(g_object_get_data(G_OBJECT(box), "rev"));
    GtkWidget* arrow = (GtkWidget*)g_object_get_data(G_OBJECT(box), "arrow");
    bool open = !gtk_revealer_get_reveal_child(rev);
    gtk_revealer_set_reveal_child(rev, open);
    gtk_widget_set_visible(GTK_WIDGET(rev), open);  // a hidden section takes no space: the ones below move up
    gtk_image_set_from_icon_name(GTK_IMAGE(arrow), open ? "pan-down-symbolic" : "pan-end-symbolic");
    if (g_object_get_data(G_OBJECT(box), "grow")) gtk_widget_set_vexpand(box, open);
}
static void section_arrow_clicked(GtkButton*, gpointer box) { section_toggle(GTK_WIDGET(box)); }
static void section_title_clicked(GtkGestureClick*, int, double, double, gpointer box) { section_toggle(GTK_WIDGET(box)); }

static GtkWidget* make_section(const char* title, GtkWidget* child, bool grow, GtkWidget* action = nullptr) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_top(box, 8);  // air above the sub-title
    GtkWidget* head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget* t = section_title(title);
    gtk_widget_set_hexpand(t, TRUE);
    GtkGesture* g = gtk_gesture_click_new();
    g_signal_connect(g, "released", G_CALLBACK(section_title_clicked), box);
    gtk_widget_add_controller(t, GTK_EVENT_CONTROLLER(g));
    gtk_box_append(GTK_BOX(head), t);
    if (action) gtk_box_append(GTK_BOX(head), action);
    GtkWidget* arrow = gtk_image_new_from_icon_name("pan-down-symbolic");
    GtkWidget* ab = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(ab), arrow);
    gtk_widget_add_css_class(ab, "flat");
    gtk_widget_set_valign(ab, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(ab, _("Fold / unfold"));
    g_signal_connect(ab, "clicked", G_CALLBACK(section_arrow_clicked), box);
    gtk_box_append(GTK_BOX(head), ab);
    gtk_box_append(GTK_BOX(box), head);

    GtkWidget* rev = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(rev), GTK_REVEALER_TRANSITION_TYPE_NONE);
    gtk_revealer_set_reveal_child(GTK_REVEALER(rev), TRUE);
    gtk_widget_set_margin_top(child, 8);  // air between the sub-title and its content
    gtk_revealer_set_child(GTK_REVEALER(rev), child);
    gtk_widget_set_vexpand(rev, grow);
    gtk_box_append(GTK_BOX(box), rev);
    g_object_set_data(G_OBJECT(box), "rev", rev);
    g_object_set_data(G_OBJECT(box), "arrow", arrow);
    if (grow) g_object_set_data(G_OBJECT(box), "grow", GINT_TO_POINTER(1));
    gtk_widget_set_vexpand(box, grow);
    return box;
}

// PTZ pad and saved positions. The commands follow the open-source DVRIP clients and have NOT been verified on the
// real camera.
GtkWidget* MainWindow::build_controls() {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget* ptz = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(ptz), 6);
    gtk_widget_set_margin_bottom(ptz, 4);  // room for the focus frame of the lower buttons
    gtk_grid_set_column_spacing(GTK_GRID(ptz), 4);
    gtk_widget_set_halign(ptz, GTK_ALIGN_CENTER);
    {  // an empty column keeps the zoom buttons well apart from the position pad
        GtkWidget* gap = gtk_label_new("");
        gtk_widget_set_size_request(gap, 30, -1);
        gtk_grid_attach(GTK_GRID(ptz), gap, 3, 0, 1, 1);
    }
    struct B { const char* icon; const char* tip; const char* cmd; int col, row; };
    const B pad[] = {{"go-up-symbolic", _("Up"), "DirectionUp", 1, 0},        {"go-previous-symbolic", _("Left"), "DirectionLeft", 0, 1},
                     {"go-next-symbolic", _("Right"), "DirectionRight", 2, 1}, {"go-down-symbolic", _("Down"), "DirectionDown", 1, 2},
                     {"zoom-in-symbolic", _("Zoom in"), "ZoomTile", 4, 0},     {"zoom-out-symbolic", _("Zoom out"), "ZoomWide", 4, 2}};
    for (const B& b : pad) {
        GtkWidget* w = gtk_button_new_from_icon_name(b.icon);
        gtk_widget_set_tooltip_text(w, b.tip);
        gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
        // Hold to move, release to stop. The gesture runs in the capture phase so that it sees the press before the button.
        ui_ctx_.push_back(std::make_unique<RowCtx>(RowCtx{this, b.cmd}));
        GtkGesture* g = gtk_gesture_click_new();
        gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(g), GTK_PHASE_CAPTURE);
        g_signal_connect(g, "pressed", G_CALLBACK(ptz_pressed), ui_ctx_.back().get());
        g_signal_connect(g, "released", G_CALLBACK(ptz_released), ui_ctx_.back().get());
        g_signal_connect(g, "cancel", G_CALLBACK(ptz_cancelled), ui_ctx_.back().get());
        gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(g));
        gtk_grid_attach(GTK_GRID(ptz), w, b.col, b.row, 1, 1);
        ptz_widgets_.push_back(w);
    }
    gtk_box_append(GTK_BOX(box), ptz);

    // saved positions: own heading with "+" (add) and "-" (forget the selected one), then one fixed-size square per position
    GtkWidget* phead = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget* ptitle = section_title(_("Saved positions"));
    gtk_widget_set_hexpand(ptitle, TRUE);
    gtk_box_append(GTK_BOX(phead), ptitle);
    GtkWidget* padd = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(padd), "list-add-symbolic");
    gtk_widget_add_css_class(padd, "flat");
    gtk_widget_set_valign(padd, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(padd, _("Add a saved position"));
    GtkWidget* pop = gtk_popover_new();
    GtkWidget* pb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget* pl = gtk_label_new(_("Position number (1-16)"));
    gtk_label_set_xalign(GTK_LABEL(pl), 0);
    preset_spin_ = gtk_spin_button_new_with_range(1, kMaxPresets, 1);
    gtk_widget_set_halign(preset_spin_, GTK_ALIGN_START);
    gtk_editable_set_width_chars(GTK_EDITABLE(preset_spin_), 3);
    GtkWidget* b_save = gtk_button_new_with_label(_("Save position"));
    GtkWidget* warn = gtk_label_new(_("Position in the camera will be overwritten"));
    gtk_label_set_xalign(GTK_LABEL(warn), 0);
    gtk_widget_add_css_class(warn, "dim-label");
    g_signal_connect(b_save, "clicked", G_CALLBACK(preset_add), this);
    for (GtkWidget* w : {pl, preset_spin_, b_save, warn}) gtk_box_append(GTK_BOX(pb), w);
    gtk_popover_set_child(GTK_POPOVER(pop), pb);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(padd), pop);
    preset_pop_ = pop;
    GtkWidget* prem = gtk_button_new_from_icon_name("list-remove-symbolic");
    gtk_widget_add_css_class(prem, "flat");
    gtk_widget_set_valign(prem, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(prem, _("Remove the selected saved position from the list"));
    g_signal_connect(prem, "clicked", G_CALLBACK(preset_remove), this);
    gtk_box_append(GTK_BOX(phead), padd);
    gtk_box_append(GTK_BOX(phead), prem);
    GtkWidget* pos = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);  // own block under the controls, not foldable
    gtk_widget_set_margin_top(pos, 14);
    positions_box_ = pos;
    gtk_box_append(GTK_BOX(pos), phead);
    ptz_widgets_.push_back(phead);

    presets_box_ = gtk_grid_new();  // 6 squares per row, filled by rebuild_presets()
    gtk_grid_set_row_spacing(GTK_GRID(presets_box_), 6);
    gtk_grid_set_column_spacing(GTK_GRID(presets_box_), 6);
    gtk_grid_set_column_homogeneous(GTK_GRID(presets_box_), TRUE);
    gtk_widget_set_halign(presets_box_, GTK_ALIGN_FILL);
    gtk_widget_set_valign(presets_box_, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(pos), presets_box_);
    ptz_widgets_.push_back(presets_box_);
    set_controls_enabled(false);
    return box;
}

void MainWindow::set_controls_enabled(bool on) {
    for (GtkWidget* w : ptz_widgets_) gtk_widget_set_sensitive(w, on);
}

// The window buttons go where the system setting puts them: the part of "left:right" before the colon on the left title
// part (above the side panel), the part after it on the right end of the right part.
void MainWindow::apply_decoration_layout() {
    gchar* layout = nullptr;
    g_object_get(gtk_settings_get_default(), "gtk-decoration-layout", &layout, NULL);
    std::string l = layout ? layout : "";
    g_free(layout);
    size_t colon = l.find(':');
    std::string left = colon == std::string::npos ? "" : l.substr(0, colon);
    std::string right = colon == std::string::npos ? l : l.substr(colon + 1);
    gtk_header_bar_set_decoration_layout(GTK_HEADER_BAR(hb_left_), (left + ":").c_str());
    gtk_header_bar_set_decoration_layout(GTK_HEADER_BAR(hb_right_), (":" + right).c_str());
}

gboolean MainWindow::key_pressed(GtkEventControllerKey*, guint keyval, guint, GdkModifierType state, gpointer self) {
    MainWindow* w = (MainWindow*)self;
    // Keyboard: Ctrl+1 / Ctrl+2 tabs, Ctrl+Up / Ctrl+Down move the selected camera in the list, F5 reconnects the streams;
    // Tab / Shift+Tab walk through the controls, the arrows move in the camera list.
    if ((state & GDK_CONTROL_MASK) && (keyval == GDK_KEY_q || keyval == GDK_KEY_Q)) { gtk_window_close(GTK_WINDOW(w->win_)); return TRUE; }
    if (keyval == GDK_KEY_F5) { reconnect_clicked(nullptr, w); return TRUE; }
    if (state & GDK_CONTROL_MASK) {
        if (keyval == GDK_KEY_1) { gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->nav_btn_[0]), TRUE); return TRUE; }
        if (keyval == GDK_KEY_2) { gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->nav_btn_[1]), TRUE); return TRUE; }
        if (keyval == GDK_KEY_Up) { w->move_camera(-1); return TRUE; }
        if (keyval == GDK_KEY_Down) { w->move_camera(1); return TRUE; }
    }
    if (!(state & GDK_CONTROL_MASK) || (keyval != GDK_KEY_r && keyval != GDK_KEY_R)) return FALSE;
    if (w->selected_preset_ <= 0) { w->set_status(_("Select a saved position first.")); return TRUE; }
    int n = w->selected_preset_;
    // nothing is sent: the camera is not touched until its preset commands are known
    w->send_preset("SetPreset", n, std::string(_("Position ")) + std::to_string(n) + _(" stored again at the current camera position.")); (void)n;
    return TRUE;
}

void MainWindow::decoration_changed(GObject*, GParamSpec*, gpointer self) { ((MainWindow*)self)->apply_decoration_layout(); }

MainWindow::MainWindow(GtkApplication* app) {
    marquee_id_ = g_timeout_add(50, marquee_tick, this);
    GtkCssProvider* css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, kCss, -1);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
    GtkCssProvider* tabcss = gtk_css_provider_new();
    gtk_css_provider_load_from_data(tabcss, kTabCss, -1);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(tabcss),
                                               GTK_STYLE_PROVIDER_PRIORITY_USER);
    g_object_unref(tabcss);

    win_ = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win_), APP_NAME);
    gtk_window_set_default_size(GTK_WINDOW(win_), 1280, 820);
    g_signal_connect(win_, "destroy", G_CALLBACK(window_destroyed), this);

    // --- title bar in two parts, as in GNOME Disks: program name above the side panel; on the right the name of the
    // selected camera (with its address below) and the buttons of the live view.
    GtkWidget* tb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget* hb_left = gtk_header_bar_new();
    GtkWidget* app_title = gtk_label_new(APP_NAME);
    gtk_widget_add_css_class(app_title, "title");  // the header bar's own title style: system size
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(hb_left), app_title);
    gtk_widget_set_hexpand(hb_left, FALSE);
    gtk_box_append(GTK_BOX(tb), hb_left);
    gtk_box_append(GTK_BOX(tb), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    GtkWidget* hb_right = gtk_header_bar_new();
    gtk_widget_set_hexpand(hb_right, TRUE);
    title_name_ = gtk_label_new("");  // name of the selected camera
    gtk_widget_add_css_class(title_name_, "title");
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(hb_right), title_name_);

    layout_box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);  // live-view buttons: the grid, restart
    gtk_widget_set_margin_start(layout_box_, 4);  // header bar adds 6: the buttons start at the left edge of the video (10)
    GtkWidget* gm = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(gm), "view-grid-symbolic");
    gtk_widget_set_tooltip_text(gm, _("Number of cameras on the screen"));
    gtk_widget_set_valign(gm, GTK_ALIGN_CENTER);
    GtkWidget* gpop = gtk_popover_new();
    GtkWidget* gbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    const char* glabels[4] = {"1 × 1", "1 × 2", "2 × 2", "4 × 4"};
    const int gvals[4] = {1, 2, 4, 16};
    for (int i = 0; i < 4; i++) {
        layout_item_[i] = gtk_button_new_with_label(glabels[i]);
        gtk_widget_add_css_class(layout_item_[i], "flat");
        ui_ctx_.push_back(std::make_unique<RowCtx>(RowCtx{this, std::to_string(gvals[i])}));
        g_signal_connect(layout_item_[i], "clicked", G_CALLBACK(layout_chosen), ui_ctx_.back().get());
        gtk_box_append(GTK_BOX(gbox), layout_item_[i]);
    }
    gtk_popover_set_child(GTK_POPOVER(gpop), gbox);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(gm), gpop);
    grid_popover_ = gpop;
    gtk_box_append(GTK_BOX(layout_box_), gm);  // grid first, then the restart button, both at the left of the right part
    GtkWidget* reconnect = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(reconnect, _("Restart the streams of the cameras shown (use after a failure)"));
    gtk_widget_set_valign(reconnect, GTK_ALIGN_CENTER);
    g_signal_connect(reconnect, "clicked", G_CALLBACK(reconnect_clicked), this);
    gtk_box_append(GTK_BOX(layout_box_), reconnect);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(hb_right), layout_box_);
    gtk_box_append(GTK_BOX(tb), hb_right);
    gtk_widget_add_css_class(hb_left, "hb-left");
    gtk_widget_add_css_class(hb_right, "hb-right");
    hb_left_ = hb_left;
    hb_right_ = hb_right;
    apply_decoration_layout();  // window buttons where the system setting puts them; follows later changes
    g_signal_connect(gtk_settings_get_default(), "notify::gtk-decoration-layout", G_CALLBACK(decoration_changed), this);
    gtk_window_set_titlebar(GTK_WINDOW(win_), tb);

    GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_window_set_child(GTK_WINDOW(win_), outer);

    // --- far left: narrow bar with the tab buttons and Settings (icons only)
    GtkWidget* nav = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(nav, 10);
    gtk_widget_set_margin_bottom(nav, 10);
    gtk_widget_set_margin_start(nav, 6);
    gtk_widget_set_margin_end(nav, 6);
    nav_btn_[0] = gtk_toggle_button_new();
    nav_btn_[1] = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(nav_btn_[0]), "camera-video-symbolic");
    gtk_button_set_icon_name(GTK_BUTTON(nav_btn_[1]), "video-x-generic-symbolic");
    gtk_widget_set_tooltip_text(nav_btn_[0], _("Live View"));
    gtk_widget_set_tooltip_text(nav_btn_[1], _("Playback"));
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(nav_btn_[1]), GTK_TOGGLE_BUTTON(nav_btn_[0]));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(nav_btn_[0]), TRUE);
    GtkWidget* nav_space = gtk_label_new("");
    gtk_widget_set_vexpand(nav_space, TRUE);
    GtkWidget* settings = gtk_button_new_from_icon_name("emblem-system-symbolic");
    gtk_widget_set_tooltip_text(settings, _("Settings"));
    g_signal_connect(settings, "clicked", G_CALLBACK(settings_clicked), this);
    for (GtkWidget* w : {nav_btn_[0], nav_btn_[1], settings}) {
        gtk_widget_set_size_request(w, 40, 40);
        gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(w, "flat");
    }
    gtk_box_append(GTK_BOX(nav), nav_btn_[0]);
    gtk_box_append(GTK_BOX(nav), nav_btn_[1]);
    gtk_box_append(GTK_BOX(nav), nav_space);
    gtk_box_append(GTK_BOX(nav), settings);
    // nav, separator and side panel live in one box; the left title-bar part shares its width through a size group, so
    // the title separator lies exactly on the line that ends the side panel, whatever the sizes of the contents are
    GtkWidget* leftcol = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(leftcol, FALSE);
    gtk_widget_set_size_request(leftcol, kSideColumnWidth, -1);  // fixed: neither grows nor shrinks relative to the title part
    gtk_widget_set_size_request(hb_left, kSideColumnWidth, -1);
    GtkSizeGroup* sg = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
    gtk_size_group_add_widget(sg, hb_left);
    gtk_size_group_add_widget(sg, leftcol);
    g_object_unref(sg);
    gtk_box_append(GTK_BOX(outer), leftcol);
    gtk_box_append(GTK_BOX(leftcol), nav);
    gtk_box_append(GTK_BOX(leftcol), gtk_separator_new(GTK_ORIENTATION_VERTICAL));

    // --- left: camera list with add / remove, then (per tab) the control module or calendar + files
    GtkWidget* side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_size_request(side, 300, -1);
    gtk_widget_set_hexpand(side, FALSE);  // fixed width on both tabs: what is inside must not make it grow
    gtk_widget_set_margin_top(side, 10);
    gtk_widget_set_margin_bottom(side, 10);
    gtk_widget_set_margin_start(side, 10);
    gtk_widget_set_margin_end(side, 10);
    GtkWidget* head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget* title = section_title(_("Cameras"));  // the camera list cannot be folded
    gtk_widget_set_hexpand(title, TRUE);
    GtkWidget* add = gtk_button_new_from_icon_name("list-add-symbolic");
    GtkWidget* rem = gtk_button_new_from_icon_name("list-remove-symbolic");
    gtk_widget_set_tooltip_text(add, _("Add a camera"));
    gtk_widget_set_tooltip_text(rem, _("Remove the selected camera"));
    gtk_widget_add_css_class(add, "flat");
    gtk_widget_add_css_class(rem, "flat");
    gtk_widget_set_valign(add, GTK_ALIGN_CENTER);  // an explicit alignment avoids GtkImage baseline warnings
    gtk_widget_set_valign(rem, GTK_ALIGN_CENTER);
    g_signal_connect(add, "clicked", G_CALLBACK(add_clicked), this);
    g_signal_connect(rem, "clicked", G_CALLBACK(remove_clicked), this);
    gtk_box_append(GTK_BOX(head), title);
    gtk_box_append(GTK_BOX(head), add);
    gtk_box_append(GTK_BOX(head), rem);
    gtk_box_append(GTK_BOX(side), head);

    list_ = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list_), GTK_SELECTION_SINGLE);
    GtkWidget* sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), list_);
    // exactly six camera rows are visible; more cameras scroll
    gtk_widget_set_size_request(sw, -1, kRowHeight * 6 + 2);
    gtk_widget_set_vexpand(sw, FALSE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(sw), FALSE);
    gtk_box_append(GTK_BOX(side), sw);
    {  // order of the cameras: move the selected one up / down (the live view follows the list order)
        GtkWidget* mv = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_widget_set_halign(mv, GTK_ALIGN_END);
        GtkWidget* up = gtk_button_new_from_icon_name("go-up-symbolic");
        GtkWidget* dn = gtk_button_new_from_icon_name("go-down-symbolic");
        for (GtkWidget* b : {up, dn}) { gtk_widget_add_css_class(b, "flat"); gtk_widget_set_valign(b, GTK_ALIGN_CENTER); gtk_box_append(GTK_BOX(mv), b); }
        gtk_widget_set_tooltip_text(up, _("Move the camera up (Ctrl+Up)"));
        gtk_widget_set_tooltip_text(dn, _("Move the camera down (Ctrl+Down)"));
        g_signal_connect(up, "clicked", G_CALLBACK(+[](GtkButton*, gpointer w) { ((MainWindow*)w)->move_camera(-1); }), this);
        g_signal_connect(dn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer w) { ((MainWindow*)w)->move_camera(1); }), this);
        gtk_box_append(GTK_BOX(side), mv);
    }

    side_stack_ = gtk_stack_new();
    gtk_widget_set_vexpand(side_stack_, TRUE);
    GtkWidget* live_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(live_page), make_section(_("Controls"), build_controls(), false));
    gtk_box_append(GTK_BOX(live_page), positions_box_);
    gtk_stack_add_named(GTK_STACK(side_stack_), live_page, "live");
    GtkWidget* arch = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_append(GTK_BOX(arch), make_section(_("Calendar"), playback_.calendar_widget(), false));
    gtk_box_append(GTK_BOX(arch), make_section(_("Recordings of the day"), playback_.files_widget(), true, playback_.download_button()));
    gtk_stack_add_named(GTK_STACK(side_stack_), arch, "playback");
    gtk_box_append(GTK_BOX(side), side_stack_);
    gtk_box_append(GTK_BOX(leftcol), side);
    gtk_box_append(GTK_BOX(outer), gtk_separator_new(GTK_ORIENTATION_VERTICAL));

    // --- right: tabs, layout buttons, content, status line
    GtkWidget* main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_hexpand(main, TRUE);
    gtk_widget_set_margin_top(main, 10);
    gtk_widget_set_margin_bottom(main, 10);
    gtk_widget_set_margin_start(main, 10);
    gtk_widget_set_margin_end(main, 10);

    stack_ = gtk_stack_new();
    gtk_widget_set_vexpand(stack_, TRUE);
    gtk_stack_add_titled(GTK_STACK(stack_), live_.widget(), "live", _("Live View"));
    gtk_stack_add_titled(GTK_STACK(stack_), playback_.widget(), "playback", _("Playback"));

    gtk_box_append(GTK_BOX(main), stack_);

    status_ = gtk_label_new("");
    gtk_widget_set_halign(status_, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(status_), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(main), status_);
    gtk_box_append(GTK_BOX(outer), main);

    // --- callbacks
    live_.on_clicked = [this](int slot) {
        std::string id = live_.camera_id(slot);  // a copy: selecting may reassign the slots
        if (!id.empty()) select_row_for(id);
    };
    live_.on_activated = [this](int slot) {  // double click: this camera alone; a second double click goes back to the grid
        std::string id = live_.camera_id(slot);
        if (id.empty()) return;
        select_row_for(id);
        if (store_.layout != 1) { prev_layout_ = store_.layout; set_layout(1); }
        else if (prev_layout_ > 1) { set_layout(prev_layout_); }
    };
    // double click on a camera in the list: select it and show it alone (the grid before is remembered once)
    GtkGesture* list_dbl = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(list_dbl), GDK_BUTTON_PRIMARY);
    g_signal_connect(list_dbl, "pressed", G_CALLBACK(+[](GtkGestureClick* g, int n_press, double, double y, gpointer self) {
        if (n_press != 2) return;
        MainWindow* w = (MainWindow*)self;
        GtkListBoxRow* row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(w->list_), (int)y);
        if (!row) return;
        int i = gtk_list_box_row_get_index(row);
        if (i < 0 || i >= (int)w->store_.cameras.size()) return;
        std::string id = w->store_.cameras[i].id;
        w->select_row_for(id);
        if (w->tab_ == Tab::Live && w->store_.layout != 1) { w->prev_layout_ = w->store_.layout; w->set_layout(1); }
        (void)g;
    }), this);
    gtk_widget_add_controller(list_, GTK_EVENT_CONTROLLER(list_dbl));
    live_.on_state = [this](int slot, LiveState st, const std::string&) {
        const std::string& id = live_.camera_id(slot);
        if (id.empty()) return;
        switch (st) {
            case LiveState::Connecting: set_dot(id, "dot-wait"); break;
            case LiveState::Playing: set_dot(id, "dot-ok"); break;
            case LiveState::Failed:
            case LiveState::Rejected:
            case LiveState::Ended:
            case LiveState::Dropped: set_dot(id, "dot-bad"); break;
        }
    };
    playback_.on_status = [this](const std::string& s) { set_status(s); };
    live_.on_notice = [this](const std::string& s) { set_status(s); };
    live_.on_light = [this](int tile, bool on) { light_requested(tile, on); };
    live_.on_drop = [this](const std::string& a, const std::string& b, int pos) { drop_camera(a, b, pos); };

    load_config();
    live_.set_media_dir(media_dir());
    playback_.set_media_dir(media_dir());
    mark_layout_item();
    rebuild_list();  // also sets the title
    for (int i = 0; i < 2; i++) g_signal_connect(nav_btn_[i], "toggled", G_CALLBACK(nav_toggled), this);
    g_signal_connect(list_, "row-selected", G_CALLBACK(row_selected), this);
    g_signal_connect(stack_, "notify::visible-child-name", G_CALLBACK(tab_changed), this);

    // Ctrl+R: store the camera's current position again under the selected saved position (after moving it)
    GtkEventController* keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), this);
    gtk_widget_add_controller(win_, keys);
    if (store_.start_maximized) gtk_window_maximize(GTK_WINDOW(win_));
    gtk_window_present(GTK_WINDOW(win_));
    refresh_live();
    refresh_presets_later(600);
}

MainWindow::~MainWindow() {
    g_signal_handlers_disconnect_by_data(gtk_settings_get_default(), this);
    if (presets_idle_) g_source_remove(presets_idle_);
    if (marquee_id_) g_source_remove(marquee_id_);
    if (ptz_stop_timer_) g_source_remove(ptz_stop_timer_);
    if (ptz_watchdog_) g_source_remove(ptz_watchdog_);
    if (!ptz_cmd_.empty()) {  // never leave a motor running
        auto it = ctl_.find(ptz_cam_);
        if (it != ctl_.end()) it->second->post([](xm::Dvrip& d) { d.request(kMsgPtz, make_ptz_move("DirectionUp", 0, 5, false)); }, nullptr);
    }
    alive_->ok = false;
    for (auto& kv : pw_cache_) std::fill(kv.second.begin(), kv.second.end(), '\0');
}

void MainWindow::window_destroyed(GtkWidget*, gpointer self) {
    // Safety net: stopping a stream or a sound device can block (a stuck network or sound server). The settings are
    // already saved, so the process is ended after a few seconds instead of staying in the process list.
    std::thread([] { sleep(4); _exit(0); }).detach();
    delete (MainWindow*)self;
}

void MainWindow::set_status(const std::string& s) { gtk_label_set_text(GTK_LABEL(status_), s.c_str()); }

// ---------------------------------------------------------------- configuration

void MainWindow::load_config() {
    config_path_ = default_config_dir() + "/cameras.json";
    LoadResult r = load_store(config_path_);
    store_ = std::move(r.data);
    std::string msg = r.error;
    for (const std::string& n : r.notes) msg += (msg.empty() ? "" : " ") + n;
    if (!msg.empty()) set_status(msg);
    if (!find_cam(store_.selected_id)) store_.selected_id = store_.cameras.empty() ? "" : store_.cameras[0].id;
}

void MainWindow::save_config() {
    std::string e = save_store(config_path_, store_);
    if (!e.empty()) set_status(e);
}

const CameraConfig* MainWindow::find_cam(const std::string& id) const {
    for (const CameraConfig& c : store_.cameras)
        if (c.id == id) return &c;
    return nullptr;
}

int MainWindow::selected_index() const {
    for (size_t i = 0; i < store_.cameras.size(); i++)
        if (store_.cameras[i].id == store_.selected_id) return (int)i;
    return -1;
}

// ---------------------------------------------------------------- camera list

void MainWindow::rebuild_list() {
    rebuilding_ = true;
    GtkWidget* c;
    while ((c = gtk_widget_get_first_child(list_))) gtk_list_box_remove(GTK_LIST_BOX(list_), c);
    row_ctx_.clear();
    dots_.clear();
    startstop_.clear();
    restart_.clear();
    name_stack_.clear();
    name_plain_.clear();
    name_long_.clear();
    name_scroll_.clear();
    for (const CameraConfig& cam : store_.cameras) {
        GtkWidget* b = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_top(b, 4);
        gtk_widget_set_margin_bottom(b, 4);
        gtk_widget_set_margin_start(b, 6);
        GtkWidget* dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_valign(dot, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(dot, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(dot, "status-dot");
        {
            auto st = dot_state_.find(cam.id);
            gtk_widget_add_css_class(dot, st != dot_state_.end() ? st->second : "dot-off");
        }
        GtkWidget* name = gtk_label_new(cam.name.c_str());
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(name, TRUE);
        // the selected row shows its name in a scroller, so a long name runs through (marquee_tick); others are ellipsized
        GtkWidget* long_name = gtk_label_new(cam.name.c_str());
        gtk_label_set_xalign(GTK_LABEL(long_name), 0);
        GtkWidget* scroller = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
        gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroller), FALSE);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), long_name);
        gtk_widget_set_can_target(scroller, FALSE);  // the wheel and clicks go to the list
        GtkWidget* stack = gtk_stack_new();
        gtk_widget_set_hexpand(stack, TRUE);
        gtk_stack_add_named(GTK_STACK(stack), name, "plain");
        gtk_stack_add_named(GTK_STACK(stack), scroller, "scroll");
        GtkWidget* gear = gtk_button_new_from_icon_name("emblem-system-symbolic");
        gtk_widget_add_css_class(gear, "flat");
        gtk_widget_set_valign(gear, GTK_ALIGN_CENTER);
        GtkWidget* rs = gtk_button_new_from_icon_name("view-refresh-symbolic");
        gtk_widget_add_css_class(rs, "flat");
        gtk_widget_set_valign(rs, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(rs, _("Restart the stream of this camera"));
        GtkWidget* ss = gtk_button_new_from_icon_name("media-playback-stop-symbolic");
        gtk_widget_add_css_class(ss, "flat");
        gtk_widget_set_valign(ss, GTK_ALIGN_CENTER);
        gtk_widget_set_visible(ss, FALSE);  // shown on the selected row only (update_row_buttons)
        gtk_widget_set_tooltip_text(gear, _("Camera settings"));
        row_ctx_.push_back(std::make_unique<RowCtx>(RowCtx{this, cam.id}));
        g_signal_connect(gear, "clicked", G_CALLBACK(gear_clicked), row_ctx_.back().get());
        g_signal_connect(ss, "clicked", G_CALLBACK(startstop_clicked), row_ctx_.back().get());
        g_signal_connect(rs, "clicked", G_CALLBACK(restart_clicked), row_ctx_.back().get());
        gtk_widget_set_size_request(b, -1, kRowHeight - 2);
        gtk_box_append(GTK_BOX(b), dot);
        gtk_box_append(GTK_BOX(b), stack);
        name_stack_.push_back(stack);
        name_plain_.push_back(name);
        name_long_.push_back(long_name);
        name_scroll_.push_back(scroller);
        gtk_widget_set_margin_end(b, 10);  // keeps the icons clear of the list's scrollbar
        gtk_box_append(GTK_BOX(b), rs);
        gtk_box_append(GTK_BOX(b), ss);
        gtk_box_append(GTK_BOX(b), gear);
        gtk_list_box_append(GTK_LIST_BOX(list_), b);
        dots_.push_back(dot);
        startstop_.push_back(ss);
        restart_.push_back(rs);
    }
    int sel = selected_index();
    if (sel >= 0) gtk_list_box_select_row(GTK_LIST_BOX(list_), gtk_list_box_get_row_at_index(GTK_LIST_BOX(list_), sel));
    rebuilding_ = false;
    update_row_buttons();
    update_title();
}

// Runs the long name of the selected camera through its row: a pause, a slow run to the end, a pause, back to the start.
gboolean MainWindow::marquee_tick(gpointer p) {
    MainWindow* w = (MainWindow*)p;
    int idx = w->selected_index();
    if (w->marquee_sel_ != idx) { w->marquee_sel_ = idx; w->marquee_ticks_ = 0; }
    w->marquee_ticks_++;
    if (idx < 0 || idx >= (int)w->name_scroll_.size()) return G_SOURCE_CONTINUE;
    GtkAdjustment* adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(w->name_scroll_[idx]));
    double range = gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj);
    if (range <= 1) { gtk_adjustment_set_value(adj, 0); return G_SOURCE_CONTINUE; }
    const int pause = 40, step_ms = 50;  // ticks of 50 ms
    int run = (int)(range / 1.5) + 1;     // 1.5 px per tick = 30 px/s
    int cycle = pause + run + pause;
    int t = w->marquee_ticks_ % cycle;
    double v = t < pause ? 0 : (t < pause + run ? (t - pause) * 1.5 : range);
    gtk_adjustment_set_value(adj, std::min(v, range));
    (void)step_ms;
    return G_SOURCE_CONTINUE;
}

void MainWindow::update_row_buttons() {
    for (size_t i = 0; i < store_.cameras.size() && i < startstop_.size(); i++) {
        const std::string& id = store_.cameras[i].id;
        bool show = tab_ == Tab::Live && id == store_.selected_id;
        gtk_widget_set_visible(startstop_[i], show);
        gtk_widget_set_visible(restart_[i], show && stopped_.count(id) == 0);
        bool stopped = stopped_.count(id) > 0;
        if (i < name_stack_.size()) {
            bool sel = id == store_.selected_id;
            gtk_stack_set_visible_child_name(GTK_STACK(name_stack_[i]), sel ? "scroll" : "plain");
            if (sel) { gtk_widget_add_css_class(name_plain_[i], "sel-name"); gtk_widget_add_css_class(name_long_[i], "sel-name"); }
            else { gtk_widget_remove_css_class(name_plain_[i], "sel-name"); gtk_widget_remove_css_class(name_long_[i], "sel-name"); }
        }
        gtk_button_set_icon_name(GTK_BUTTON(startstop_[i]), stopped ? "media-playback-start-symbolic" : "media-playback-stop-symbolic");
        gtk_widget_set_tooltip_text(startstop_[i], stopped ? _("Start the live stream") : _("Stop the live stream"));
    }
    set_controls_enabled(tab_ == Tab::Live && find_cam(store_.selected_id) != nullptr);
}

void MainWindow::update_title() {
    const CameraConfig* c = find_cam(store_.selected_id);
    gtk_label_set_text(GTK_LABEL(title_name_), c ? c->name.c_str() : "");
    rebuild_presets();
}

void MainWindow::startstop_clicked(GtkButton*, gpointer ctx) {
    RowCtx* c = (RowCtx*)ctx;
    c->w->toggle_stream(c->id);
}

void MainWindow::restart_clicked(GtkButton*, gpointer ctx) {
    RowCtx* r = (RowCtx*)ctx;
    int t = r->w->live_.tile_of(r->id);
    if (t >= 0) r->w->live_.stop(t);  // forgets the stream; refresh_live starts it again
    r->w->refresh_live();
}

void MainWindow::toggle_stream(const std::string& id) {
    if (stopped_.count(id)) stopped_.erase(id);
    else stopped_.insert(id);
    update_row_buttons();
    refresh_live();
}

void MainWindow::set_dot(const std::string& camera_id, const char* css_class) {
    dot_state_[camera_id] = css_class;  // remembered: the list is rebuilt when the order changes
    for (size_t i = 0; i < store_.cameras.size() && i < dots_.size(); i++) {
        if (store_.cameras[i].id != camera_id) continue;
        for (const char* k : kDotClasses) gtk_widget_remove_css_class(dots_[i], k);
        gtk_widget_add_css_class(dots_[i], css_class);
    }
}

void MainWindow::move_camera(int delta) {
    int i = selected_index();
    int j = i + delta;
    if (i < 0 || j < 0 || j >= (int)store_.cameras.size()) return;
    trade(i, j);
    save_config();
    rebuild_list();
    refresh_live();
}

// A tile was dragged onto another one. In the 4 x 4 grid the camera takes the tile it was dropped on (an empty tile too;
// the camera that was there takes the old tile); in the smaller grids the two cameras trade places.
void MainWindow::drop_camera(const std::string& from, const std::string& to, int pos) {
    int ia = -1, ib = -1;
    for (size_t k = 0; k < store_.cameras.size(); k++) {
        if (store_.cameras[k].id == from) ia = (int)k;
        if (store_.cameras[k].id == to) ib = (int)k;
    }
    if (ia < 0) return;
    if (ib < 0 || ib == ia) return;  // every grid: only between two active tiles (neighbours in the list); an empty tile does nothing
    trade(ia, ib);
    save_config();
    rebuild_list();
    select_row_for(from);  // the moved camera stays the selected one, its tile carries the frame
    refresh_live();
}

// Two active cameras trade their places in the list and their tiles.
void MainWindow::trade(int i, int j) {
    std::swap(store_.cameras[i], store_.cameras[j]);
    std::swap(store_.cameras[i].slot, store_.cameras[j].slot);
}

void MainWindow::row_selected(GtkListBox*, GtkListBoxRow* row, gpointer self) {
    MainWindow* w = (MainWindow*)self;
    if (w->rebuilding_ || !row) return;
    int i = gtk_list_box_row_get_index(row);
    if (i < 0 || i >= (int)w->store_.cameras.size()) return;
    w->apply_selection(w->store_.cameras[i].id);
}

void MainWindow::select_row_for(const std::string& id) {
    for (size_t i = 0; i < store_.cameras.size(); i++)
        if (store_.cameras[i].id == id) {
            GtkListBoxRow* row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list_), (int)i);
            if (row) gtk_list_box_select_row(GTK_LIST_BOX(list_), row);  // emits row-selected when it changes
            return;
        }
}

void MainWindow::apply_selection(const std::string& id) {
    if (id == store_.selected_id) return;
    store_.selected_id = id;
    update_row_buttons();
    update_title();
    live_.set_light(light_state_.count(id) ? light_state_[id] : false);
    ptz_move("", false);
    save_config();
    refresh_presets_later(300);  // the list is the camera's own
    if (tab_ == Tab::Live) refresh_live();
    else update_playback_target();
}

std::string MainWindow::media_dir() const { return store_.media_dir.empty() ? default_media_dir() : store_.media_dir; }

void MainWindow::settings_clicked(GtkButton*, gpointer self) {
    MainWindow* w = (MainWindow*)self;
    show_settings_dialog(GTK_WINDOW(w->win_), w->media_dir(), default_media_dir(), w->store_.language, w->store_.start_maximized, w->store_.hw_decoding,
                         [w](const std::string& dir, const std::string& lang, bool maximized, bool hwdec) {
                             w->store_.media_dir = dir == default_media_dir() ? "" : dir;  // empty = follow the default
                             w->store_.language = lang;
                             w->store_.start_maximized = maximized;
                             w->store_.hw_decoding = hwdec;
                             w->save_config();
                             w->live_.set_media_dir(w->media_dir());
                             w->playback_.set_media_dir(w->media_dir());
                             w->set_status(std::string(_("Saved media go to ")) + w->media_dir());
                         });
}

void MainWindow::gear_clicked(GtkButton*, gpointer ctx) {
    RowCtx* c = (RowCtx*)ctx;
    c->w->edit_camera(c->id);
}

void MainWindow::add_clicked(GtkButton*, gpointer self) {
    MainWindow* w = (MainWindow*)self;
    show_camera_dialog(
        GTK_WINDOW(w->win_), nullptr, keyring_compiled_in(), [w](CameraDialogResult r) { w->camera_saved(std::move(r)); },
        [w](std::string id) { w->camera_deleted(id); });
}

void MainWindow::remove_clicked(GtkButton*, gpointer self) {
    MainWindow* w = (MainWindow*)self;
    const CameraConfig* c = w->find_cam(w->store_.selected_id);
    if (!c) { w->set_status(_("Select a camera to remove.")); return; }
    std::string id = c->id;
    show_confirm(GTK_WINDOW(w->win_), _("Remove camera"),
                 std::string(_("Remove the camera \"")) + c->name + _("\" from the list? Its saved password is deleted too. Recordings on the camera are not touched."),
                 _("Remove"), [w, id]() { w->camera_deleted(id); });
}

void MainWindow::edit_camera(const std::string& id) {
    const CameraConfig* c = find_cam(id);
    if (!c) return;
    CameraConfig copy = *c;  // the dialog edits a copy
    show_camera_dialog(
        GTK_WINDOW(win_), &copy, keyring_compiled_in(), [this](CameraDialogResult r) { camera_saved(std::move(r)); },
        [this](std::string cid) { camera_deleted(cid); });
}

void MainWindow::camera_saved(CameraDialogResult r) {
    CameraConfig c = r.cam;
    bool is_new = c.id.empty();
    if (is_new) c.id = new_camera_id();
    if (is_new) {
        c.slot = first_free_slot(store_);  // the first empty tile; the list order follows the slots
        insert_by_slot(store_, c);
    } else {
        for (CameraConfig& e : store_.cameras)
            if (e.id == c.id) { std::vector<int> keep = e.presets; int keep_slot = e.slot; e = c; e.slot = keep_slot; if (c.presets.empty()) e.presets = keep; }
    }
    if (c.no_password) {
        pw_cache_.erase(c.id);
    } else if (r.password_given) {
        pw_cache_[c.id] = r.password;
        if (r.save_to_keyring && keyring_compiled_in()) {
            auto alive = alive_;
            MainWindow* self = this;
            secret_store(c.id, "ICmon: " + c.name, r.password, [alive, self](SecretResult s) {
                if (!alive->ok || s.ok) return;
                self->set_status(std::string(_("The password could not be saved in the keyring (")) + s.error +
                                 _("). It is kept until the program closes."));
            });
        } else {
            set_status(_("The password is kept until the program closes."));
        }
    }
    std::fill(r.password.begin(), r.password.end(), '\0');
    ctl_.erase(c.id);  // connection settings may have changed; the next command logs in again
    light_state_.erase(c.id);
    store_.selected_id = c.id;
    if (is_new) {  // the smallest grid that holds all cameras; a grid that is already big enough is kept
        int count = (int)store_.cameras.size();
        int need = count <= 1 ? 1 : count == 2 ? 2 : count <= 4 ? 4 : 16;
        if (store_.layout < need) { store_.layout = need; mark_layout_item(); }
    }
    save_config();
    rebuild_list();
    {  // connection settings of this camera may have changed: only its own stream is restarted
        int t = live_.tile_of(c.id);
        if (t >= 0) live_.stop(t, "");
    }
    playback_dirty_ = true;
    if (tab_ == Tab::Live) refresh_live();
    else update_playback_target();
}

void MainWindow::camera_deleted(const std::string& id) {
    {  // release the stream of the removed camera; the others keep running
        int t = live_.tile_of(id);
        if (t >= 0) { ++slot_gen_[t]; live_.assign(t, "", ""); live_.stop(t, ""); }
    }
    store_.cameras.erase(std::remove_if(store_.cameras.begin(), store_.cameras.end(),
                                        [&](const CameraConfig& c) { return c.id == id; }),
                         store_.cameras.end());
    stopped_.erase(id);
    ctl_.erase(id);
    light_state_.erase(id);
    light_orig_.erase(id);
    auto it = pw_cache_.find(id);
    if (it != pw_cache_.end()) {
        std::fill(it->second.begin(), it->second.end(), '\0');
        pw_cache_.erase(it);
    }
    auto alive = alive_;
    MainWindow* self = this;
    secret_clear(id, [alive, self](SecretResult s) {
        if (alive->ok && !s.ok && s.available) self->set_status(_("The saved password could not be removed from the keyring."));
    });
    if (store_.selected_id == id) store_.selected_id = store_.cameras.empty() ? "" : store_.cameras[0].id;
    for (size_t k = 0; k < store_.cameras.size(); k++) store_.cameras[k].slot = (int)k;  // close the gap: the others move up
    save_config();
    rebuild_list();
    playback_dirty_ = true;
    if (tab_ == Tab::Live) refresh_live();
    else update_playback_target();
}

// ---------------------------------------------------------------- tabs and layout

void MainWindow::tab_changed(GObject*, GParamSpec*, gpointer self) {
    MainWindow* w = (MainWindow*)self;
    const char* n = gtk_stack_get_visible_child_name(GTK_STACK(w->stack_));
    Tab t = n && std::strcmp(n, "playback") == 0 ? Tab::Playback : Tab::Live;
    w->syncing_nav_ = true;  // the stack was switched from code (double click on a tile): follow with the buttons
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->nav_btn_[t == Tab::Playback ? 1 : 0]), TRUE);
    w->syncing_nav_ = false;
    w->set_tab(t);
}

void MainWindow::nav_toggled(GtkToggleButton* b, gpointer self) {
    MainWindow* w = (MainWindow*)self;
    if (w->syncing_nav_ || !gtk_toggle_button_get_active(b)) return;
    gtk_stack_set_visible_child_name(GTK_STACK(w->stack_), b == GTK_TOGGLE_BUTTON(w->nav_btn_[1]) ? "playback" : "live");
}

void MainWindow::set_tab(Tab t) {
    if (t == tab_) return;
    tab_ = t;
    ptz_move("", false);
    update_row_buttons();
    gtk_stack_set_visible_child_name(GTK_STACK(side_stack_), t == Tab::Live ? "live" : "playback");
    gtk_widget_set_visible(layout_box_, t == Tab::Live);
    if (t == Tab::Live) {
        playback_.set_tab_visible(false);
        refresh_live();
    } else {
        live_.stop_all();  // one connection at a time: cameras limit their simultaneous sessions
        for (const CameraConfig& c : store_.cameras) set_dot(c.id, "dot-off");
        playback_.set_tab_visible(true);
        if (playback_dirty_ || playback_cam_id_ != store_.selected_id) update_playback_target();
    }
}

void MainWindow::layout_chosen(GtkButton*, gpointer ctx) {
    RowCtx* r = (RowCtx*)ctx;
    gtk_popover_popdown(GTK_POPOVER(r->w->grid_popover_));
    r->w->set_layout(std::atoi(r->id.c_str()));
}

// The chosen grid is shown with a check mark in front of its entry.
void MainWindow::mark_layout_item() {
    const char* base[4] = {"1 × 1", "1 × 2", "2 × 2", "4 × 4"};
    const int vals[4] = {1, 2, 4, 16};
    for (int i = 0; i < 4; i++) {
        std::string t = (store_.layout == vals[i] ? "✓  " : "     ") + std::string(base[i]);
        gtk_button_set_label(GTK_BUTTON(layout_item_[i]), t.c_str());
    }
}

void MainWindow::set_layout(int n) {
    if (n == store_.layout) return;
    store_.layout = n;
    mark_layout_item();
    save_config();
    refresh_live();
}

void MainWindow::reconnect_clicked(GtkButton*, gpointer self) {
    MainWindow* w = (MainWindow*)self;
    w->live_.stop_all();
    w->refresh_live();
    w->refresh_camera_presets();
}

// ---------------------------------------------------------------- passwords

// Calls `cb` either at once (cached / no password) or later, on the main thread, after a keyring lookup.
void MainWindow::with_password(const CameraConfig& cam, PasswordCb cb) {
    if (cam.no_password) { cb(true, "", ""); return; }
    auto it = pw_cache_.find(cam.id);
    if (it != pw_cache_.end()) { cb(true, it->second, ""); return; }
    auto alive = alive_;
    MainWindow* self = this;
    std::string id = cam.id;
    secret_lookup(id, [alive, self, id, cb](SecretResult r) {
        if (!alive->ok) return;
        if (r.ok) {
            self->pw_cache_[id] = r.password;
            cb(true, r.password, "");
            return;
        }
        std::string why = r.available
            ? std::string(_("The password of this camera is not saved. Open its settings (gear icon) and enter it."))
            : std::string(_("The password is not saved and the keyring is not available (")) + r.error +
                  _("). Open the camera settings (gear icon) and enter the password.");
        cb(false, "", why);
    });
}

// ---------------------------------------------------------------- live view

// The cameras shown are a "page" of the list: with n tiles, the page is the group of n cameras (in list order)
// that contains the selected camera. One tile shows the selected camera itself.
void MainWindow::refresh_live() {
    if (tab_ != Tab::Live) return;
    int n = store_.layout;
    int sel = selected_index();
    int count = (int)store_.cameras.size();
    // 1 x 1: the selected camera. 1 x 2 / 2 x 2: the page of the list that holds the selected camera (the cameras in list
    // order). 4 x 4: every camera sits on the tile of its slot. A lone camera is shown large and centred.
    int first = sel < 0 ? 0 : (n == 1 ? sel : (n == 16 ? 0 : (sel / n) * n));
    int used = n == 1 ? std::max(0, std::min(1, count - first)) : (n == 16 ? count : std::max(0, std::min(n, count - first)));
    int layout = used <= 1 ? 1 : n;

    // Which tile shows which camera. A camera that already has a running stream keeps its tile, wherever it sits in
    // the new grid, so that changing the layout does not reconnect the cameras that stay on screen.
    std::vector<int> want_cam;  // camera index per grid position (-1 = empty position)
    for (int p = 0; p < layout; p++) want_cam.push_back(first + p < count && p < std::max(used, 1) ? first + p : -1);
    if (layout == 16) {
        for (int p = 0; p < layout; p++) {
            want_cam[p] = -1;
            for (int k = 0; k < count; k++) if (store_.cameras[k].slot == p) want_cam[p] = k;
        }
    }
    std::vector<int> tile_for(layout, -1);
    bool taken[LiveView::kMaxSlots] = {};
    for (int p = 0; p < layout; p++) {
        if (want_cam[p] < 0) continue;
        int t = live_.tile_of(store_.cameras[want_cam[p]].id);
        if (t >= 0 && !taken[t]) { tile_for[p] = t; taken[t] = true; }
    }
    for (int p = 0; p < layout; p++) {
        if (tile_for[p] >= 0) continue;
        for (int t = 0; t < LiveView::kMaxSlots; t++)
            if (!taken[t]) { tile_for[p] = t; taken[t] = true; break; }
    }
    live_.arrange(tile_for);  // stops the tiles that are no longer shown
    for (int p = 0; p < layout; p++) {
        int tile = tile_for[p];
        if (want_cam[p] >= 0) {
            const CameraConfig& c = store_.cameras[want_cam[p]];
            live_.assign(tile, c.id, c.name);
            start_slot(tile, c);
        } else {
            ++slot_gen_[tile];
            live_.assign(tile, "", "");
            live_.stop(tile, count == 0 && p == 0 ? _("No cameras yet. Press the + button above the list to add a camera.") : "");
        }
    }
    int sel_tile = -1;
    for (int p = 0; p < layout; p++) if (sel >= 0 && want_cam[p] == sel) sel_tile = tile_for[p];
    live_.set_selected(sel_tile);
    if (count == 0) set_status(_("No cameras yet. Press the + button above the list to add a camera."));
}

void MainWindow::start_slot(int slot, const CameraConfig& cam) {
    if (stopped_.count(cam.id)) {  // stopped by the user with the icon in the list: stays stopped until started
        ++slot_gen_[slot];
        live_.stop(slot, _("Stopped. Press the start icon in the camera list."));
        set_dot(cam.id, "dot-off");
        return;
    }
    // The stream is the camera's main stream, or its sub stream when the user chose that for several cameras. The key
    // does not depend on the layout otherwise, so a layout change does not reconnect a camera.
    int stream = (cam.use_sub_in_grid && store_.layout > 1) ? cam.sub_stream : cam.main_stream;
    // Display height of the scaled picture, fixed for the life of the stream (the full-resolution frame is kept for
    // "save frame"). 1080 for 1 or 2 cameras; 720 when the 4-camera layout was chosen to keep the load down.
    int height = store_.layout == 4 ? 720 : 1080;
    std::string key = cam.id + ":" + std::to_string(stream);
    if (live_.key(slot) == key) return;  // already running (or failed: the user presses Reconnect)
    uint64_t gen = ++slot_gen_[slot];
    CameraConfig c = cam;
    with_password(c, [this, slot, gen, c, stream, height, key](bool ok, const std::string& pw, const std::string& problem) {
        if (gen != slot_gen_[slot] || live_.camera_id(slot) != c.id) return;  // superseded meanwhile
        if (!ok) { live_.stop(slot, problem); set_dot(c.id, "dot-bad"); return; }
        UrlResult u = build_rtsp_url(c.rtsp_template, c, pw, stream);
        if (!u.error.empty()) { live_.stop(slot, u.error); set_dot(c.id, "dot-bad"); return; }
        live_.start(slot, key, u.url, height);
    });
}

// ---------------------------------------------------------------- camera commands


void MainWindow::say_from_worker(const std::string& text) {
    if (text.empty()) return;
    struct Note { std::shared_ptr<Alive> alive; MainWindow* w; std::string text; };
    g_idle_add(+[](gpointer p) -> gboolean {
        std::unique_ptr<Note> n((Note*)p);
        if (n->alive->ok) n->w->set_status(n->text);
        return G_SOURCE_REMOVE;
    }, new Note{alive_, this, text});
}

// Runs `f` with the camera's command connection, creating it (one login) when needed.
void MainWindow::with_ctl(const std::string& id, std::function<void(CamControl&)> f) {
    auto it = ctl_.find(id);
    if (it != ctl_.end()) { f(*it->second); return; }
    const CameraConfig* cam = find_cam(id);
    if (!cam) return;
    CameraConfig c = *cam;
    with_password(c, [this, c, f](bool ok, const std::string& pw, const std::string& problem) {
        if (!ok) { set_status(problem); return; }
        if (!find_cam(c.id)) return;
        auto& slot = ctl_[c.id];
        if (!slot) slot = std::make_unique<CamControl>(c.host, c.dvrip_port, c.user, pw);
        f(*slot);
    });
}

void MainWindow::ptz_pressed(GtkGestureClick*, int, double, double, gpointer ctx) {
    RowCtx* r = (RowCtx*)ctx;
    r->w->ptz_move(r->id, true);
}
void MainWindow::ptz_released(GtkGestureClick*, int, double, double, gpointer ctx) { ((RowCtx*)ctx)->w->ptz_release(); }
void MainWindow::ptz_cancelled(GtkGesture*, GdkEventSequence*, gpointer ctx) { ((RowCtx*)ctx)->w->ptz_release(); }

// Other open-source clients send the stop about 250 ms after the start (a short tap is a short pulse); an earlier stop
// may be ignored by the camera. So a release sooner than that waits for the rest of the 250 ms.
void MainWindow::ptz_release() {
    gint64 ms = (g_get_monotonic_time() - ptz_press_us_) / 1000;
    if (ptz_cmd_.empty() || ms >= 250) { ptz_move("", false); return; }
    if (ptz_stop_timer_) g_source_remove(ptz_stop_timer_);
    ptz_stop_timer_ = g_timeout_add((guint)(250 - ms), ptz_stop_cb, this);
}

gboolean MainWindow::ptz_stop_cb(gpointer self) {
    MainWindow* w = (MainWindow*)self;
    w->ptz_stop_timer_ = 0;
    w->ptz_move("", false);
    return G_SOURCE_REMOVE;
}

gboolean MainWindow::ptz_watchdog_cb(gpointer self) {
    MainWindow* w = (MainWindow*)self;
    w->ptz_watchdog_ = 0;
    w->ptz_move("", false);  // a lost button release must not leave the camera turning
    return G_SOURCE_REMOVE;
}

// Hold-to-move: the start message when the button goes down, the stop message when it is released (or after 6 s).
void MainWindow::ptz_move(std::string command, bool start) {
    if (ptz_stop_timer_) { g_source_remove(ptz_stop_timer_); ptz_stop_timer_ = 0; }
    if (!start) {
        if (ptz_watchdog_) { g_source_remove(ptz_watchdog_); ptz_watchdog_ = 0; }
        if (ptz_cmd_.empty()) return;
        std::string cmd = ptz_cmd_, id = ptz_cam_;
        ptz_cmd_.clear();
        ++ptz_token_;
        with_ctl(id, [this, cmd](CamControl& ctl) {
            ctl.post([cmd](xm::Dvrip& d) { d.request(kMsgPtz, make_ptz_move(cmd, 0, 5, false)); },
                     [this](const std::string& err) { say_from_worker(err.empty() ? "" : std::string(_("PTZ stop: ")) + err); });
        });
        return;
    }
    if (!ptz_cmd_.empty()) ptz_move("", false);
    const CameraConfig* cam = find_cam(store_.selected_id);
    if (!cam) return;
    if (cam->ptz_flip_v && command == "DirectionUp") command = "DirectionDown";
    else if (cam->ptz_flip_v && command == "DirectionDown") command = "DirectionUp";
    if (cam->ptz_flip_h && command == "DirectionLeft") command = "DirectionRight";
    else if (cam->ptz_flip_h && command == "DirectionRight") command = "DirectionLeft";
    ptz_cmd_ = command;
    ptz_cam_ = cam->id;
    ptz_press_us_ = g_get_monotonic_time();
    uint64_t token = ++ptz_token_;
    ptz_watchdog_ = g_timeout_add(6000, ptz_watchdog_cb, this);
    with_ctl(cam->id, [this, command, token](CamControl& ctl) {
        if (token != ptz_token_) return;  // released before the connection was ready
        ctl.post([command](xm::Dvrip& d) { d.request(kMsgPtz, make_ptz_move(command, 0, 5, true)); },
                 [this](const std::string& err) {
                     say_from_worker(err.empty() ? "" : std::string(_("PTZ: ")) + err +
                                                            _(" (the PTZ command format is taken from other programs, not verified on this camera)"));
                 });
    });
}

// Saved positions of the selected camera as buttons. Left click = go there; right click = forget it (ClearPreset);
// "+" saves the current position under the next free number (SetPreset). Rebuilt from an idle callback, never from
// inside one of its own buttons' handlers.
void MainWindow::rebuild_presets() {
    GtkWidget* ch;
    while ((ch = gtk_widget_get_first_child(presets_box_))) gtk_grid_remove(GTK_GRID(presets_box_), ch);
    preset_ctx_.clear();  // after the widgets are gone
    const CameraConfig* cam = find_cam(store_.selected_id);
    if (cam) {
        if (std::find(cam->presets.begin(), cam->presets.end(), selected_preset_) == cam->presets.end()) selected_preset_ = 0;
        int idx = 0;
        for (int n : cam->presets) {
            // The number is a child label, not the button's own text: themes give "text buttons" a minimum width
            // of about 64 px, which widened the side panel. The buttons fill their cell, height as in the theme.
            GtkWidget* b = gtk_toggle_button_new();
            gtk_button_set_child(GTK_BUTTON(b), gtk_label_new(std::to_string(n).c_str()));
            gtk_widget_add_css_class(b, "preset-btn");
            gtk_widget_set_hexpand(b, TRUE);
            gtk_widget_set_halign(b, GTK_ALIGN_FILL);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), n == selected_preset_);
            {
                auto nm = preset_names_.find(cam->id);
                std::string tip = _("Go to this saved position");
                if (nm != preset_names_.end() && nm->second.count(n) && !nm->second[n].empty()) tip = nm->second[n] + " - " + tip;
                gtk_widget_set_tooltip_text(b, tip.c_str());
            }
            preset_ctx_.push_back(std::make_unique<RowCtx>(RowCtx{this, std::to_string(n)}));
            g_signal_connect(b, "clicked", G_CALLBACK(preset_goto), preset_ctx_.back().get());
            gtk_grid_attach(GTK_GRID(presets_box_), b, idx % 4, idx / 4, 1, 1);
            idx++;
        }
        // empty cells keep the first row four columns wide: with fewer buttons they would else spread over the whole row
        for (int k = idx; k < 4; k++) {
            GtkWidget* gap = gtk_label_new("");
            gtk_widget_set_hexpand(gap, TRUE);
            gtk_grid_attach(GTK_GRID(presets_box_), gap, k, 0, 1, 1);
        }
        // the number offered in the "+" popover: the next one after the last, else the first free one
        int sug = cam->presets.empty() ? 1 : cam->presets.back() + 1;
        if (sug > kMaxPresets) {
            sug = 1;
            while (sug <= kMaxPresets && std::find(cam->presets.begin(), cam->presets.end(), sug) != cam->presets.end()) sug++;
        }
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(preset_spin_), std::min(sug, kMaxPresets));
    }
}

gboolean MainWindow::rebuild_presets_idle(gpointer self) {
    MainWindow* w = (MainWindow*)self;
    w->presets_idle_ = 0;
    w->rebuild_presets();
    return G_SOURCE_REMOVE;
}

void MainWindow::queue_rebuild_presets() {
    if (!presets_idle_) presets_idle_ = g_idle_add(rebuild_presets_idle, this);
}

void MainWindow::send_preset(const std::string& cmd, int n, const std::string& done_text) {
    const CameraConfig* cam = find_cam(store_.selected_id);
    if (!cam) return;
    with_ctl(cam->id, [this, cmd, n, done_text](CamControl& ctl) {
        ctl.post([cmd, n](xm::Dvrip& d) { d.request(kMsgPtz, make_ptz_preset(cmd, 0, n)); },
                 [this, done_text, cmd, n](const std::string& err) {
                     fprintf(stderr, "ICmon DVRIP %s %d: %s\n", cmd.c_str(), n, err.empty() ? "camera answered Ret=100" : err.c_str());
                     say_from_worker(err.empty() ? done_text : std::string(_("Saved positions: ")) + err);
                 });
    });
}

void MainWindow::preset_goto(GtkButton*, gpointer ctx) {
    RowCtx* r = (RowCtx*)ctx;
    int n = atoi(r->id.c_str());
    r->w->selected_preset_ = n;
    r->w->send_preset("GotoPreset", n, std::string(_("Moving to position ")) + std::to_string(n));
    r->w->queue_rebuild_presets();  // shows which one is selected
}

// "-": deletes the selected position in the camera (ONVIF RemovePreset with the token the camera reported), after a
// confirmation. A position the camera did not report is cleared with the DVRIP ClearPreset used before.
void MainWindow::preset_remove(GtkButton*, gpointer self) {
    MainWindow* w = (MainWindow*)self;
    int n = w->selected_preset_;
    if (n <= 0) { w->set_status(_("Select a saved position first.")); return; }
    show_confirm(GTK_WINDOW(w->win_), _("Delete saved position"),
                 std::string(_("Delete position ")) + std::to_string(n) + _(" in the camera? It disappears in the phone app too."),
                 _("Delete"), [w, n]() {
                     const CameraConfig* cam = w->find_cam(w->store_.selected_id);
                     if (!cam) return;
                     std::string id = cam->id, host = cam->host, user = cam->user;
                     w->selected_preset_ = 0;
                     if (!w->ptz_.token_for(id, std::to_string(n))) {  // not in the camera's own list: the old way
                         w->send_preset("ClearPreset", n, std::string(_("Position ")) + std::to_string(n) + _(" cleared in the camera."));
                         for (CameraConfig& c : w->store_.cameras)
                             if (c.id == id) c.presets.erase(std::remove(c.presets.begin(), c.presets.end(), n), c.presets.end());
                         w->save_config();
                         w->rebuild_presets();
                         w->refresh_presets_later(1500);
                         return;
                     }
                     w->with_password(*cam, [w, id, host, user, n](bool ok, const std::string& pw, const std::string& why) {
                         if (!ok) { w->set_status(why); return; }
                         ptz::OnvifTarget t;
                         t.host = host; t.user = user; t.password = pw; t.ports = ptz::default_onvif_ports();
                         auto alive = w->alive_;
                         struct Job { std::shared_ptr<Alive> alive; MainWindow* w; int n; std::string err, log; };
                         w->ptz_.delete_preset(id, t, std::to_string(n), [alive, w, n](const std::string& err, const std::string& log) {
                             g_idle_add(+[](gpointer p) -> gboolean {
                                 std::unique_ptr<Job> j((Job*)p);
                                 if (!j->alive->ok) return G_SOURCE_REMOVE;
                                 if (!j->log.empty()) fprintf(stderr, "ICmon ONVIF RemovePreset:\n%s", j->log.c_str());
                                 if (j->err.empty()) {
                                     j->w->set_status(std::string(_("Position ")) + std::to_string(j->n) + _(" deleted in the camera."));
                                     j->w->refresh_camera_presets();
                                 } else {
                                     j->w->set_status(std::string(_("Cannot delete the position: ")) + j->err);
                                 }
                                 return G_SOURCE_REMOVE;
                             }, new Job{alive, w, n, err, log});
                         });
                     });
                 });
}

// ---- reading the list from the camera

void MainWindow::refresh_presets_later(guint ms) {
    auto alive = alive_;
    struct T { std::shared_ptr<Alive> alive; MainWindow* w; };
    g_timeout_add(ms, +[](gpointer p) -> gboolean {
        std::unique_ptr<T> t((T*)p);
        if (t->alive->ok) t->w->refresh_camera_presets();
        return G_SOURCE_REMOVE;
    }, new T{alive, this});
}

// Reads the positions the camera itself holds (all media profiles are inspected, see src/ptz/ptz_onvif.cpp). The answer
// is logged to stderr in full: names, tokens, PTZ positions, and which profile was used.
void MainWindow::refresh_camera_presets() {
    const CameraConfig* cam = find_cam(store_.selected_id);
    if (!cam) return;
    std::string id = cam->id, host = cam->host, user = cam->user;
    with_password(*cam, [this, id, host, user](bool ok, const std::string& pw, const std::string& why) {
        if (!ok) { set_status(why); return; }
        ptz::OnvifTarget t;
        t.host = host; t.user = user; t.password = pw; t.ports = ptz::default_onvif_ports();
        t.timeout_ms = 3000;
        auto alive = alive_;
        struct Job { std::shared_ptr<Alive> alive; MainWindow* w; std::string id; ptz::OnvifResult r; };
        ptz_.refresh_presets(id, t, [alive, this, id](const ptz::OnvifResult& r) {
            g_idle_add(+[](gpointer p) -> gboolean {
                std::unique_ptr<Job> j((Job*)p);
                if (j->alive->ok) j->w->presets_read(j->id, j->r);
                return G_SOURCE_REMOVE;
            }, new Job{alive, this, id, r});
        });
    });
}

void MainWindow::save_via_onvif(const std::string& id, int n) {
    const CameraConfig* cam = find_cam(id);
    if (!cam) return;
    std::string host = cam->host, user = cam->user;
    with_password(*cam, [this, id, host, user, n](bool ok, const std::string& pw, const std::string& why) {
        if (!ok) { set_status(why); return; }
        ptz::OnvifTarget t;
        t.host = host; t.user = user; t.password = pw; t.ports = ptz::default_onvif_ports();
        auto alive = alive_;
        struct Job { std::shared_ptr<Alive> alive; MainWindow* w; std::string id, err, log; };
        ptz_.save_preset(id, t, std::to_string(n), [alive, this, id](const std::string& err, const std::string& log) {
            g_idle_add(+[](gpointer p) -> gboolean {
                std::unique_ptr<Job> j((Job*)p);
                if (!j->alive->ok) return G_SOURCE_REMOVE;
                fprintf(stderr, "ICmon ONVIF SetPreset:\n%s%s%s\n", j->log.c_str(), j->err.empty() ? "" : "error: ", j->err.c_str());
                j->w->refresh_presets_later(1000);
                return G_SOURCE_REMOVE;
            }, new Job{alive, this, id, err, log});
        });
    });
}

void MainWindow::presets_read(const std::string& id, const ptz::OnvifResult& r) {
    fprintf(stderr, "ICmon ONVIF (%s):\n%s", id.c_str(), r.log.c_str());
    if (!r.ok) {
        if (id == store_.selected_id) set_status(std::string(_("Cannot read the saved positions from the camera: ")) + r.error);
        fprintf(stderr, "ICmon ONVIF: %s\n", r.error.c_str());
        return;
    }
    std::vector<int> nums;
    std::map<int, std::string> names;
    int skipped = 0;
    for (const ptz::Preset& p : r.presets) {
        bool digits = !p.token.empty() && p.token.find_first_not_of("0123456789") == std::string::npos;
        if (!digits) { skipped++; continue; }
        int n = atoi(p.token.c_str());
        if (n < 1 || std::find(nums.begin(), nums.end(), n) != nums.end()) { skipped++; continue; }
        nums.push_back(n);
        names[n] = p.name;
    }
    if (skipped) fprintf(stderr, "ICmon ONVIF: %d preset token(s) are not plain position numbers and are not shown\n", skipped);
    std::sort(nums.begin(), nums.end());
    CameraConfig* cam = nullptr;
    for (CameraConfig& c : store_.cameras) if (c.id == id) cam = &c;
    if (!cam) return;
    // A position just saved here is only "saved" once the camera lists it.
    std::string pend_msg;
    auto pend = pending_add_.find(id);
    if (pend != pending_add_.end()) {
        int pn = pend->second;
        if (std::find(nums.begin(), nums.end(), pn) != nums.end()) {
            pend_msg = std::string(_("Position ")) + std::to_string(pn) + _(" is stored in the camera.");
            pending_add_.erase(pend);
        } else if (!fallback_tried_.count(id)) {
            fallback_tried_.insert(id);
            fprintf(stderr, "ICmon: the camera did not list position %d after the DVRIP SetPreset; trying ONVIF SetPreset\n", pn);
            set_status(std::string(_("The camera does not list position ")) + std::to_string(pn) + _(" yet; trying the other save command."));
            save_via_onvif(id, pn);
            return;  // the list is read again after that
        } else {
            pend_msg = std::string(_("The camera did not confirm position ")) + std::to_string(pn) + _(" (see the terminal log).");
            pending_add_.erase(pend);
        }
    }
    if (nums.empty() && !cam->presets.empty()) {
        // The camera answered but lists nothing, while positions were made here: do not wipe them (see the log).
        if (id == store_.selected_id) set_status(_("The camera reports no saved positions over ONVIF; the list is kept."));
        return;
    }
    preset_names_[id] = names;
    if (cam->presets != nums) {
        cam->presets = nums;
        save_config();
    }
    if (id == store_.selected_id) {
        queue_rebuild_presets();
        set_status(!pend_msg.empty() ? pend_msg : std::string(_("Saved positions read from the camera: ")) + std::to_string(nums.size()));
    }
}

// Puts the number from the popover into the selected camera's list; `send_set` also stores the camera's current position
// under that number (SetPreset). Without it nothing is sent: the number is one the camera already has (made in a phone app).
void MainWindow::add_preset_number(bool send_set) {
    gtk_spin_button_update(GTK_SPIN_BUTTON(preset_spin_));  // a typed number counts without pressing Enter
    int n = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(preset_spin_));
    for (CameraConfig& c : store_.cameras) {
        if (c.id != store_.selected_id) continue;
        auto taken = [&](int k) { return std::find(c.presets.begin(), c.presets.end(), k) != c.presets.end(); };
        if (taken(n)) {  // the camera adds, it does not overwrite: offer the next free number, else the previous free one
            int sug = 0;
            for (int k = n + 1; k <= kMaxPresets && !sug; k++) if (!taken(k)) sug = k;
            for (int k = n - 1; k >= 1 && !sug; k--) if (!taken(k)) sug = k;
            if (!sug) { set_status(_("All 16 positions are used.")); return; }
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(preset_spin_), sug);
            set_status(std::string(_("Position ")) + std::to_string(n) + _(" exists; ") + std::to_string(sug) +
                       _(" is free: press Save position to use it."));
            return;
        }
        c.presets.push_back(n);
        std::sort(c.presets.begin(), c.presets.end());
        selected_preset_ = n;
        save_config();

        if (send_set) { pending_add_[c.id] = n; fallback_tried_.erase(c.id); }
        if (send_set) refresh_presets_later(1500);  // the camera's own list shows what it really stored
        else set_status(std::string(_("Position ")) + std::to_string(n) + _(" added to the list."));
        gtk_popover_popdown(GTK_POPOVER(preset_pop_));
        queue_rebuild_presets();
        return;
    }
}
void MainWindow::preset_add(GtkButton*, gpointer self) { ((MainWindow*)self)->add_preset_number(true); }

// Light button. The camera's own lighting mode (set in the camera's settings) is never overwritten for good: turning the
// light on remembers the current WorkMode, turning it off writes that mode back. The value "KeepOpen" is NOT verified on
// this camera; the camera's answer is shown as it is. The state of the button is what this session did.
void MainWindow::light_requested(int tile, bool on) {
    std::string id = live_.camera_id(tile);
    if (id.empty()) return;
    auto& orig = light_orig_[id];
    if (!orig) orig = std::make_shared<std::string>();
    std::shared_ptr<std::string> o = orig;
    with_ctl(id, [this, id, on, o](CamControl& ctl) {
        ctl.post(
            [on, o](xm::Dvrip& d) {
                const std::string name = "Camera.WhiteLight";
                xm::Json reply = config_get(d, name);
                xm::Json obj = config_object(reply, name);
                if (!obj.is_object() || !obj.contains("WorkMode"))
                    throw xm::Error(xm::Errc::Protocol, "unexpected answer to the light settings request");
                std::string cur = obj["WorkMode"].str();
                if (on) {
                    if (cur != "KeepOpen") *o = cur;
                    config_set(d, name, config_with(reply, name, "WorkMode", "KeepOpen"));
                } else {
                    if (o->empty()) throw xm::Error(xm::Errc::Protocol, "the earlier light mode is not known in this session; set it in the camera settings");
                    config_set(d, name, config_with(reply, name, "WorkMode", *o));
                }
            },
            [this, id, on](const std::string& err) {
                struct R { std::shared_ptr<Alive> alive; MainWindow* w; std::string id, err; bool on; };
                g_idle_add(+[](gpointer p) -> gboolean {
                    std::unique_ptr<R> r((R*)p);
                    if (!r->alive->ok) return G_SOURCE_REMOVE;
                    MainWindow* w = r->w;
                    bool selected = w->store_.selected_id == r->id;
                    if (r->err.empty()) {
                        w->light_state_[r->id] = r->on;
                        if (selected) w->live_.set_light(r->on);
                        w->set_status(r->on ? _("Light: the camera accepted the command (on).") : _("Light: the camera accepted the command (off)."));
                    } else {
                        if (selected) w->live_.set_light(!r->on);  // the camera did not do it: show the old state
                        w->set_status(std::string(_("Light: ")) + r->err + _(" (command format not verified on this camera)"));
                    }
                    return G_SOURCE_REMOVE;
                }, new R{alive_, this, id, err, on});
            });
    });
}

// ---------------------------------------------------------------- playback

void MainWindow::update_playback_target() {
    playback_dirty_ = false;
    const CameraConfig* c = find_cam(store_.selected_id);
    playback_cam_id_ = c ? c->id : "";
    PlaybackTarget none;
    playback_.set_target(none);  // stops and clears right now
    uint64_t gen = ++playback_gen_;
    if (!c) { set_status(_("No camera selected.")); return; }
    CameraConfig cam = *c;
    with_password(cam, [this, gen, cam](bool ok, const std::string& pw, const std::string& problem) {
        if (gen != playback_gen_) return;
        if (!ok) { set_status(problem); return; }
        PlaybackTarget t;
        t.valid = true;
        t.name = cam.name;
        t.host = cam.host;
        t.user = cam.user;
        t.password = pw;
        t.port = cam.dvrip_port;
        playback_.set_target(t);
    });
}

}  // namespace icmon
