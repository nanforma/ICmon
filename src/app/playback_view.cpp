#include "playback_view.h"

#include <cmath>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include <glib/gstdio.h>

#include "i18n.h"

namespace icmon {


namespace { constexpr int kDateColumn = 96; }  // width of the Date column of the recordings table

PlaybackView::PlaybackView() {
    // --- calendar and the day's files (placed in the side panel by the window)
    calendar_ = gtk_calendar_new();
    g_signal_connect(calendar_, "day-selected", G_CALLBACK(day_selected), this);
    for (const char* sig : {"next-month", "prev-month", "next-year", "prev-year"})
        g_signal_connect(calendar_, sig, G_CALLBACK(month_changed), this);
    files_list_ = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(files_list_), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(files_list_), FALSE);  // double click / Enter plays
    g_signal_connect(files_list_, "row-activated", G_CALLBACK(file_activated), this);
    dl_btn_ = gtk_button_new_from_icon_name("document-save-symbolic");
    gtk_widget_add_css_class(dl_btn_, "flat");
    gtk_widget_set_valign(dl_btn_, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(dl_btn_, _("Download the selected recording (.mkv); press again to stop"));
    g_signal_connect(dl_btn_, "clicked", G_CALLBACK(download_clicked), this);
    files_scroll_ = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(files_scroll_), files_list_);
    gtk_widget_set_vexpand(files_scroll_, TRUE);
    // A table like the camera search: frame, headings, a separator between the columns.
    files_box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(files_box_, "frame");
    gtk_widget_add_css_class(files_list_, "files-list");
    GtkWidget* head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(head, 8);
    gtk_widget_set_margin_top(head, 4);
    gtk_widget_set_margin_bottom(head, 4);
    GtkWidget* hd = gtk_label_new(_("Date"));
    GtkWidget* ht = gtk_label_new(_("Time"));
    for (GtkWidget* h : {hd, ht}) gtk_label_set_xalign(GTK_LABEL(h), 0);
    gtk_widget_set_size_request(hd, kDateColumn, -1);
    gtk_box_append(GTK_BOX(head), hd);
    gtk_box_append(GTK_BOX(head), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_box_append(GTK_BOX(head), ht);
    gtk_box_append(GTK_BOX(files_box_), head);
    gtk_box_append(GTK_BOX(files_box_), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(files_box_), files_scroll_);
    gtk_widget_set_vexpand(files_box_, TRUE);

    // --- the Playback tab: picture, transport buttons (centred under the picture), timeline
    root_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    video_ = gtk_picture_new();
    gtk_widget_set_vexpand(video_, TRUE);
    gtk_widget_set_hexpand(video_, TRUE);
    gtk_widget_set_size_request(video_, 200, 150);
    gtk_picture_set_can_shrink(GTK_PICTURE(video_), TRUE);
#if GTK_CHECK_VERSION(4, 8, 0)
    gtk_picture_set_content_fit(GTK_PICTURE(video_), GTK_CONTENT_FIT_CONTAIN);
#else
    gtk_picture_set_keep_aspect_ratio(GTK_PICTURE(video_), TRUE);
#endif
    gtk_widget_add_css_class(video_, "video-tile");  // black background
    gtk_widget_set_overflow(video_, GTK_OVERFLOW_HIDDEN);
    gtk_box_append(GTK_BOX(root_), video_);

    GtkWidget* ctl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    // Three equal parts: speed slider at the left, the buttons in the middle, then the time and the sound controls.
    // (a centre box: the buttons stay in the middle, and the row can shrink with the window)
    GtkWidget* row = gtk_center_box_new();
    gtk_widget_set_hexpand(row, TRUE);
    GtkWidget* snd = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    mute_btn_ = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(mute_btn_), "audio-volume-muted-symbolic");
    gtk_widget_set_tooltip_text(mute_btn_, _("Sound on / off (off by default)"));
    gtk_widget_set_valign(mute_btn_, GTK_ALIGN_CENTER);
    vol_ = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 0.05);
    gtk_scale_set_draw_value(GTK_SCALE(vol_), FALSE);
    gtk_range_set_value(GTK_RANGE(vol_), 0.7);
    gtk_widget_set_size_request(vol_, 110, -1);
    gtk_widget_set_valign(vol_, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(vol_, _("Output volume"));
    g_signal_connect(mute_btn_, "toggled", G_CALLBACK(sound_toggled), this);
    g_signal_connect(vol_, "value-changed", G_CALLBACK(volume_changed), this);
    gtk_box_append(GTK_BOX(snd), mute_btn_);
    gtk_box_append(GTK_BOX(snd), vol_);
    info_ = gtk_label_new("");  // playing position (time of day), under the picture at the left
    gtk_widget_set_halign(info_, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(info_), 0);  // the text starts at the left edge of the picture
    gtk_label_set_ellipsize(GTK_LABEL(info_), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(info_, "numeric");
    // speed: -4x, -2x (slow motion: still forward, the time stamps are stretched), 1x, 2x, 4x. Real backward play is NOT implemented.
    speed_ = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 4, 0.01);  // smooth; it clings to the marks (speed_changed)
    gtk_scale_set_draw_value(GTK_SCALE(speed_), FALSE);
    gtk_scale_set_has_origin(GTK_SCALE(speed_), FALSE);
    gtk_range_set_value(GTK_RANGE(speed_), 2);
    gtk_scale_add_mark(GTK_SCALE(speed_), 0, GTK_POS_BOTTOM, "-4×");
    gtk_scale_add_mark(GTK_SCALE(speed_), 1, GTK_POS_BOTTOM, "-2×");
    gtk_scale_add_mark(GTK_SCALE(speed_), 2, GTK_POS_BOTTOM, "1×");
    gtk_scale_add_mark(GTK_SCALE(speed_), 3, GTK_POS_BOTTOM, "2×");
    gtk_scale_add_mark(GTK_SCALE(speed_), 4, GTK_POS_BOTTOM, "4×");
    gtk_widget_set_size_request(speed_, 170, -1);
    gtk_widget_set_valign(speed_, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(speed_, _("Playback speed -4×, -2× (slow motion), 1×, 2×, 4× (sound is off when not 1×)"));
    g_signal_connect(speed_, "value-changed", G_CALLBACK(speed_changed), this);
    GtkGesture* dbl = gtk_gesture_click_new();  // double click on the slider: back to 1x
    g_signal_connect(dbl, "pressed", G_CALLBACK(speed_pressed), this);
    gtk_widget_add_controller(speed_, GTK_EVENT_CONTROLLER(dbl));
    GtkWidget* left = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(left), speed_);
    GtkWidget* right = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_size_request(info_, -1, -1);
    gtk_widget_set_hexpand(info_, FALSE);
    gtk_widget_set_halign(right, GTK_ALIGN_END);
    gtk_widget_set_halign(info_, GTK_ALIGN_END);
    gtk_label_set_xalign(GTK_LABEL(info_), 1);
    gtk_label_set_width_chars(GTK_LABEL(info_), 12);  // wide windows show it whole (28 characters), narrow ones shorten it
    gtk_label_set_max_width_chars(GTK_LABEL(info_), 28);
    gtk_label_set_ellipsize(GTK_LABEL(info_), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(info_, "tnum");  // equal-width digits: the text does not change its width every second
    gtk_box_append(GTK_BOX(right), info_);  // the time is next to the sound button
    gtk_box_append(GTK_BOX(right), snd);
    gtk_widget_set_halign(ctl, GTK_ALIGN_CENTER);
    gtk_center_box_set_start_widget(GTK_CENTER_BOX(row), left);
    gtk_center_box_set_center_widget(GTK_CENTER_BOX(row), ctl);
    gtk_center_box_set_end_widget(GTK_CENTER_BOX(row), right);
    prev_btn_ = gtk_button_new_from_icon_name("media-skip-backward-symbolic");
    play_btn_ = gtk_button_new_from_icon_name("media-playback-start-symbolic");
    stop_btn_ = gtk_button_new_from_icon_name("media-playback-stop-symbolic");
    next_btn_ = gtk_button_new_from_icon_name("media-skip-forward-symbolic");
    gtk_widget_set_tooltip_text(prev_btn_, _("Previous recording"));
    gtk_widget_set_tooltip_text(play_btn_, _("Play / pause"));
    gtk_widget_set_tooltip_text(stop_btn_, _("Stop"));
    gtk_widget_set_tooltip_text(next_btn_, _("Next recording"));
    g_signal_connect(prev_btn_, "clicked", G_CALLBACK(prev_clicked), this);
    g_signal_connect(play_btn_, "clicked", G_CALLBACK(play_clicked), this);
    g_signal_connect(stop_btn_, "clicked", G_CALLBACK(stop_clicked), this);
    g_signal_connect(next_btn_, "clicked", G_CALLBACK(next_clicked), this);
    for (GtkWidget* b : {prev_btn_, play_btn_, stop_btn_, next_btn_}) gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(ctl), prev_btn_);
    gtk_box_append(GTK_BOX(ctl), play_btn_);
    gtk_box_append(GTK_BOX(ctl), stop_btn_);
    gtk_box_append(GTK_BOX(ctl), next_btn_);
    gtk_box_append(GTK_BOX(root_), row);
    show_idle_controls();

    gtk_box_append(GTK_BOX(root_), timeline_.widget());


    player_.on_frame = [this](GdkTexture* t) {
        gtk_picture_set_paintable(GTK_PICTURE(video_), GDK_PAINTABLE(t));
        g_object_unref(t);
    };
    player_.on_message = [this](const std::string& s) { status(s); };
    player_.on_finished = [this]() { playback_ended(); };
    position_timer_ = g_timeout_add(500, position_tick, this);

    timeline_.on_hover = [this](int s) { hover(s); };
    timeline_.on_click = [this](int s) { clicked(s); };
    timeline_.set_day(selected_day_start());
}

PlaybackView::~PlaybackView() {
    if (position_timer_) g_source_remove(position_timer_);
    if (speed_timer_) g_source_remove(speed_timer_);
    alive_->ok = false;
    cancel_->cancel();
    marks_cancel_->cancel();
    std::fill(target_.password.begin(), target_.password.end(), '\0');
}

void PlaybackView::status(const std::string& s) {
    if (on_status) on_status(s);
}

void PlaybackView::set_target(const PlaybackTarget& t) {
    player_.stop();  // a running download has its own connection and goes on
    show_idle_controls();
    cancel_->cancel();  // a listing of the previous camera must not land on this one
    ++gen_;
    marks_cancel_->cancel();
    ++marks_gen_;
    marks_cache_.clear();
    scanning_ = {0, 0};
    gtk_calendar_clear_marks(GTK_CALENDAR(calendar_));
    std::fill(target_.password.begin(), target_.password.end(), '\0');
    target_ = t;
    rejected_ = false;
    need_listing_ = true;
    timeline_.set_day(selected_day_start());
    fill_files();
    cur_sec_ = -1;
    gtk_label_set_text(GTK_LABEL(info_), "");
    gtk_picture_set_paintable(GTK_PICTURE(video_), nullptr);
    try_list();
}

void PlaybackView::set_tab_visible(bool visible) {
    visible_ = visible;
    if (!visible) {
        player_.stop();  // sound must not keep playing from a hidden tab
        show_idle_controls();
        return;
    }
    try_list();
}

void PlaybackView::sound_toggled(GtkToggleButton* b, gpointer self) {
    PlaybackView* w = (PlaybackView*)self;
    bool on = gtk_toggle_button_get_active(b);
    gtk_button_set_icon_name(GTK_BUTTON(b), on ? "audio-volume-high-symbolic" : "audio-volume-muted-symbolic");
    w->player_.set_audio(on, gtk_range_get_value(GTK_RANGE(w->vol_)));
}

void PlaybackView::volume_changed(GtkRange* r, gpointer self) {
    PlaybackView* w = (PlaybackView*)self;
    w->player_.set_audio(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->mute_btn_)), gtk_range_get_value(r));
}

void PlaybackView::day_selected(GtkCalendar*, gpointer self) {
    PlaybackView* w = (PlaybackView*)self;
    w->player_.stop();  // the picture belongs to the previous day
    w->show_idle_controls();
    w->cancel_->cancel();
    ++w->gen_;
    w->timeline_.set_day(w->selected_day_start());
    w->fill_files();
    w->cur_sec_ = -1;
    gtk_label_set_text(GTK_LABEL(w->info_), "");
    w->need_listing_ = true;
    w->try_list();
}

void PlaybackView::month_changed(GtkCalendar*, gpointer self) {
    PlaybackView* w = (PlaybackView*)self;
    gtk_calendar_clear_marks(GTK_CALENDAR(w->calendar_));  // marks are by day number: they belong to the old month
    w->scan_month();
}

void PlaybackView::scan_month() {
    if (!visible_ || !target_.valid || rejected_) return;
    GDateTime* d = gtk_calendar_get_date(GTK_CALENDAR(calendar_));
    int year = g_date_time_get_year(d), month = g_date_time_get_month(d);
    g_date_time_unref(d);
    auto it = marks_cache_.find({year, month});
    if (it != marks_cache_.end()) { apply_marks(year, month, it->second); return; }

    if (scanning_ == std::make_pair(year, month)) return;  // already on its way
    marks_cancel_->cancel();
    marks_cancel_ = std::make_shared<xm::CancelToken>();
    uint64_t gen = ++marks_gen_;
    scanning_ = {year, month};
    auto alive = alive_;
    auto token = marks_cancel_;
    std::string host = target_.host, user = target_.user, pw = target_.password;
    int port = target_.port;
    PlaybackView* self = this;
    int ndays = g_date_get_days_in_month((GDateMonth)month, (GDateYear)year);
    std::thread([=]() mutable {
        auto* r = new MarkResult();
        r->alive = alive; r->w = self; r->gen = gen; r->year = year; r->month = month;
        try {
            xm::Dvrip c;
            c.connect(host, (uint16_t)port, 4000, token.get());
            c.login(user, pw);
            for (int day = 1; day <= ndays; day++) {
                xm::CamTime b = xm::to_cam_time({year, month, day, 0, 0, 0});
                // The camera refuses a query for a day that has not come yet (Ret=119, seen on the real camera for
                // the day after today). A refusal of one day must not end the scan: that day gets no mark.
                try {
                    bool has = xm::has_files(c, b, b + icmon::kDaySeconds - 1, token.get());
                    r->answered++;
                    if (has) r->days.push_back(day);
                } catch (const xm::Error& e) {
                    if (e.code() != xm::Errc::Camera) throw;
                    r->refused++;
                    fprintf(stderr, "ICmon calendar: %04d-%02d-%02d refused: %s\n", year, month, day, e.what());
                }
            }
            r->ok = true;
        } catch (const xm::Error& e) {
            if (e.code() != xm::Errc::Cancelled) r->error = e.what();
            fprintf(stderr, "ICmon calendar: scan of %04d-%02d stopped after %d day(s): %s\n", year, month, r->answered, e.what());
        }
        std::fill(pw.begin(), pw.end(), '\0');
        g_idle_add(deliver_marks, r);
    }).detach();
}

gboolean PlaybackView::deliver_marks(gpointer p) {
    std::unique_ptr<MarkResult> r((MarkResult*)p);
    if (r->alive->ok && r->w->marks_gen_ == r->gen) r->w->scanning_ = {0, 0};
    if (r->alive->ok && r->w->marks_gen_ == r->gen && r->ok) {
        r->w->marks_cache_[{r->year, r->month}] = r->days;
        r->w->apply_marks(r->year, r->month, r->days);
        char b[200];
        snprintf(b, sizeof b, _("Calendar: %zu day(s) with recordings in %04d-%02d (%d day(s) refused by the camera)."), r->days.size(), r->year, r->month, r->refused);
        fprintf(stderr, "ICmon calendar: %04d-%02d: %zu day(s) with recordings\n", r->year, r->month, r->days.size());
        r->w->status(b);
    } else if (r->alive->ok && r->w->marks_gen_ == r->gen && !r->error.empty()) {
        r->w->status(std::string(_("Calendar marks could not be read: ")) + r->error);
    }
    return G_SOURCE_REMOVE;
}

void PlaybackView::apply_marks(int year, int month, const std::vector<int>& days) {
    GDateTime* d = gtk_calendar_get_date(GTK_CALENDAR(calendar_));
    bool same = g_date_time_get_year(d) == year && g_date_time_get_month(d) == month;
    g_date_time_unref(d);
    if (!same) return;  // the user moved on to another month meanwhile
    gtk_calendar_clear_marks(GTK_CALENDAR(calendar_));
    for (int day : days) gtk_calendar_mark_day(GTK_CALENDAR(calendar_), (guint)day);
}

xm::CamTime PlaybackView::selected_day_start() const {
    GDateTime* d = gtk_calendar_get_date(GTK_CALENDAR(calendar_));
    xm::CamTime t = xm::to_cam_time({g_date_time_get_year(d), g_date_time_get_month(d), g_date_time_get_day_of_month(d), 0, 0, 0});
    g_date_time_unref(d);
    return t;
}

void PlaybackView::try_list() {
    if (!visible_ || !target_.valid || !need_listing_ || rejected_) return;
    need_listing_ = false;
    start_listing();
}

void PlaybackView::start_listing() {
    cancel_->cancel();  // supersede a request still in flight
    cancel_ = std::make_shared<xm::CancelToken>();
    uint64_t gen = ++gen_;
    status(_("Connecting and reading the list of recordings…"));

    xm::CamTime day = selected_day_start();
    timeline_.set_day(day);
    auto alive = alive_;
    auto token = cancel_;
    std::string host = target_.host, user = target_.user, pw = target_.password;
    int port = target_.port;
    PlaybackView* self = this;
    std::thread([=]() mutable {
        auto* r = new Result();
        r->alive = alive; r->w = self; r->gen = gen;
        try {
            xm::Dvrip c;
            c.connect(host, (uint16_t)port, 4000, token.get());
            c.login(user, pw);
            r->files = xm::list_files(c, day, day + icmon::kDaySeconds - 1, token.get());
            r->ok = true;
        } catch (const xm::Error& e) {
            // Ret=119: the camera refuses the query for a day without recordings (also days not yet come): an empty day, not a failure.
            if (e.code() == xm::Errc::Camera && (e.ret() == 119 || std::string(e.what()).find("Ret=119") != std::string::npos)) {
                r->ok = true;
                r->refused = true;
            } else {
            r->error = e.what();
            r->auth_failed = (e.code() == xm::Errc::Auth);
            if (e.code() == xm::Errc::Cancelled) r->error.clear();
            }
        }
        std::fill(pw.begin(), pw.end(), '\0');
        g_idle_add(deliver, r);
    }).detach();
}

gboolean PlaybackView::deliver(gpointer p) {
    std::unique_ptr<Result> r((Result*)p);
    if (r->alive->ok) r->w->on_result(*r);  // the view may be gone by now
    return G_SOURCE_REMOVE;
}

void PlaybackView::on_result(Result& r) {
    if (r.gen != gen_) return;  // an older request finished after a newer one started
    if (r.ok) {
        timeline_.set_files(std::move(r.files));
        fill_files();
        char b[160];
        snprintf(b, sizeof b, _("%zu recording(s) on this day."), timeline_.files().size());
        status(r.refused ? std::string(_("No recordings on this day (the camera does not answer for it)."))
               : timeline_.files().empty() ? std::string(_("No recordings on this day.")) : std::string(b));
        scan_month();
        return;
    }
    if (r.error.empty()) return;  // cancelled
    if (r.auth_failed) {          // never retry a rejected login automatically
        rejected_ = true;
        status(std::string(_("The camera rejected the login: ")) + r.error +
               _(" Open the camera settings (gear icon) and check the user name and password.") );
        return;
    }
    status(std::string(_("Error: ")) + r.error);
}

void PlaybackView::hover(int) {
    // The time under the pointer is shown by the timeline's own popup; the line under it keeps the playing position.
}

bool PlaybackView::ready_for_playback() {
    if (!target_.valid || rejected_) { status(_("No camera is ready for playback: check the camera settings.")); return false; }
    return true;
}

// "time now / start-end of the recording it belongs to"
std::string PlaybackView::info_text(int sec) const {
    std::string s = clock_text(sec);
    const xm::RecordingFile* f = file_at(timeline_.files(), timeline_.day_start(), sec);
    if (f) {
        long long day = timeline_.day_start();
        s += " / " + clock_text((int)(f->begin - day)) + "–" + clock_text((int)(f->end - day));
    }
    return s;
}

void PlaybackView::clicked(int sec) {
    const xm::RecordingFile* f = file_at(timeline_.files(), timeline_.day_start(), sec);
    timeline_.set_selected(sec);
    cur_sec_ = sec;
    if (!f) {
        gtk_label_set_text(GTK_LABEL(info_), (clock_text(sec) + "  (" + _("no recording") + ")").c_str());
        return;
    }
    if (ready_for_playback()) start_playback(sec);
}

void PlaybackView::start_playback(int sec) {
    PlayParams p;
    p.host = target_.host;
    p.user = target_.user;
    p.password = target_.password;
    p.port = target_.port;
    p.rate = rate_;
    p.files = timeline_.files();
    p.start = timeline_.day_start() + sec;
    p.end = std::min<xm::CamTime>(p.start + 1800, timeline_.day_start() + kDaySeconds);  // up to 30 minutes ahead
    paused_ = false;
    cur_sec_ = sec;
    timeline_.set_selected(sec);
    show_current_file(sec);
    if (shown_row_ >= 0) scroll_to_row(shown_row_);  // a click on the timeline brings the clip into view
    gtk_widget_set_sensitive(stop_btn_, TRUE);
    status(std::string(_("Playing the camera's archive from ")) + clock_text(sec) + "…");
    player_.start(p);
    update_play_icon();  // after start(): the icon follows player_.active(), which is true only now
}

void PlaybackView::playback_ended() {
    player_.stop();
    show_idle_controls();
    status(_("Playback finished."));
}

void PlaybackView::show_idle_controls() {
    paused_ = false;
    gtk_widget_set_sensitive(stop_btn_, FALSE);
    update_play_icon();
}

void PlaybackView::update_play_icon() {
    bool playing = player_.active() && !paused_;
    gtk_button_set_icon_name(GTK_BUTTON(play_btn_), playing ? "media-playback-pause-symbolic" : "media-playback-start-symbolic");
}

void PlaybackView::speed_pressed(GtkGestureClick*, int n_press, double, double, gpointer self) {
    if (n_press == 2) gtk_range_set_value(GTK_RANGE(((PlaybackView*)self)->speed_), 2);
}

void PlaybackView::speed_changed(GtkRange* r, gpointer self) {
    PlaybackView* w = (PlaybackView*)self;
    double raw = gtk_range_get_value(r);
    int v = (int)(raw + 0.5);
    if (w->speed_snapping_) return;
    // smooth in between, but it clings to a mark when the slider comes near one; the speed is applied when it rests
    if (std::fabs(raw - v) < 0.12 && raw != v) {
        w->speed_snapping_ = true;
        gtk_range_set_value(r, v);
        w->speed_snapping_ = false;
    }
    if (w->speed_timer_) g_source_remove(w->speed_timer_);
    w->speed_timer_ = g_timeout_add(250, +[](gpointer p) -> gboolean {
        PlaybackView* pv = (PlaybackView*)p;
        pv->speed_timer_ = 0;
        int s = (int)(gtk_range_get_value(GTK_RANGE(pv->speed_)) + 0.5);
        pv->speed_snapping_ = true;
        gtk_range_set_value(GTK_RANGE(pv->speed_), s);  // rests on the nearest mark
        pv->speed_snapping_ = false;
        pv->apply_speed(s);
        return G_SOURCE_REMOVE;
    }, w);
}

void PlaybackView::apply_speed(int v) {
    PlaybackView* w = this;
    double rate = v >= 4 ? 4.0 : v == 3 ? 2.0 : v == 2 ? 1.0 : v == 1 ? 0.5 : 0.25;
    if (rate == w->rate_) return;
    w->rate_ = rate;
    // A running playback restarts from where it is at the new speed (one fresh download from the camera).
    if (w->player_.active() && w->cur_sec_ >= 0) w->start_playback(w->cur_sec_);
}

void PlaybackView::play_clicked(GtkButton*, gpointer self) {
    PlaybackView* w = (PlaybackView*)self;
    if (w->player_.active()) {  // pause / resume
        w->paused_ = !w->paused_;
        w->player_.set_paused(w->paused_);
        w->update_play_icon();
        return;
    }
    const auto& files = w->timeline_.files();
    if (files.empty()) { w->status(_("No recordings on this day.")); return; }
    int sec = w->cur_sec_;
    if (sec < 0 || !file_at(files, w->timeline_.day_start(), sec))
        sec = (int)std::max<long long>(files.front().begin - w->timeline_.day_start(), 0);
    if (w->ready_for_playback()) w->start_playback(sec);
}

void PlaybackView::stop_clicked(GtkButton*, gpointer self) {
    PlaybackView* w = (PlaybackView*)self;
    w->player_.stop();
    w->show_idle_controls();
    w->status(_("Stopped."));
}

// Recordings are the camera's 5-minute files, oldest first. "Next" starts the first file that begins after the
// current position; "previous" restarts the current file, or goes to the one before it when less than 3 s in.
void PlaybackView::jump(int direction) {
    const auto& files = timeline_.files();
    if (files.empty()) { status(_("No recordings on this day.")); return; }
    long long day = timeline_.day_start();
    auto begin_sec = [&](const xm::RecordingFile& f) { return std::max<long long>(f.begin - day, 0); };
    long long pos = cur_sec_;
    long long want = -1;
    if (direction > 0) {
        for (const auto& f : files)
            if (begin_sec(f) > pos) { want = begin_sec(f); break; }
    } else {
        for (const auto& f : files)
            if (begin_sec(f) < pos - 3) want = begin_sec(f);
    }
    if (want < 0) { status(direction > 0 ? _("This is the last recording of the day.") : _("This is the first recording of the day.")); return; }
    if (ready_for_playback()) start_playback((int)want);
}

void PlaybackView::prev_clicked(GtkButton*, gpointer self) { ((PlaybackView*)self)->jump(-1); }
void PlaybackView::next_clicked(GtkButton*, gpointer self) { ((PlaybackView*)self)->jump(+1); }

// "DD.MM.YYYY" of a camera time (naive local time).
static std::string date_text(long long cam_time) {
    std::string s = xm::format_cam_time(cam_time);  // "YYYY-MM-DD HH:MM:SS"
    if (s.size() < 10) return s;
    return s.substr(8, 2) + "." + s.substr(5, 2) + "." + s.substr(0, 4);
}

// The side panel's list: one row per recording of the selected day.
void PlaybackView::fill_files() {
    GtkWidget* c;
    while ((c = gtk_widget_get_first_child(files_list_))) gtk_list_box_remove(GTK_LIST_BOX(files_list_), c);
    shown_row_ = -1;
    long long day = timeline_.day_start();
    int i = 0;
    for (const xm::RecordingFile& f : timeline_.files()) {
        std::string t = clock_text((int)std::max<long long>(f.begin - day, 0)) + "  \xE2\x80\x93  " +
                        clock_text((int)std::max<long long>(f.end - day, 0));
        GtkWidget* dl = gtk_label_new(date_text(f.begin).c_str());
        gtk_label_set_xalign(GTK_LABEL(dl), 0);
        gtk_widget_set_size_request(dl, kDateColumn, -1);
        gtk_widget_set_margin_end(dl, 9);  // as wide as the heading's column separator
        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_margin_start(row, 8);
        GtkWidget* l = gtk_label_new(t.c_str());
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_widget_set_hexpand(l, TRUE);
        gtk_widget_set_margin_top(l, 2);
        gtk_widget_set_margin_bottom(l, 2);
        gtk_widget_set_margin_top(dl, 2);
        gtk_widget_set_margin_bottom(dl, 2);
        gtk_box_append(GTK_BOX(row), dl);
        gtk_box_append(GTK_BOX(row), l);
        gtk_list_box_append(GTK_LIST_BOX(files_list_), row);
        i++;
    }
}

void PlaybackView::download_clicked(GtkButton*, gpointer self) {
    PlaybackView* w = (PlaybackView*)self;
    if (w->dl_.active()) { w->start_download(w->downloading_row_); return; }  // second press: stop
    GtkListBoxRow* r = gtk_list_box_get_selected_row(GTK_LIST_BOX(w->files_list_));
    if (!r) { w->status(_("Select a recording in the list first.")); return; }
    w->start_download(gtk_list_box_row_get_index(r));
}

// Saves one whole recording of the camera as <media folder>/video/<camera>-<date>-<time>.mkv. The second press on the
// same row cancels. NOT verified on a real camera: that the file plays, its audio sync, a file's very first frames.
void PlaybackView::start_download(int row) {
    if (dl_.active()) {
        if (row == downloading_row_) { dl_.cancel(); status(_("Cancelling the download…")); }
        else status(_("A download is already running; wait for it or press the button again to stop it."));
        return;
    }
    const auto& files = timeline_.files();
    if (row < 0 || row >= (int)files.size() || !target_.valid || rejected_) { status(_("No camera is ready for downloading.")); return; }
    // The camera may serve only one archive stream at a time: stop the playback before the download starts.
    if (player_.active()) stop_clicked(nullptr, this);
    if (media_dir_.empty()) { status(_("No folder for saved media is set.")); return; }
    std::string dir = media_dir_ + "/video";
    if (g_mkdir_with_parents(dir.c_str(), 0755) != 0) { status(std::string(_("Cannot create the folder ")) + dir); return; }
    std::string cam = target_.name.empty() ? std::string("camera") : target_.name;
    for (char& ch : cam)
        if (ch == '/' || ch == '\\' || ch == ':' || (unsigned char)ch < 0x20) ch = '_';
    std::string stamp = xm::format_cam_time(files[row].begin);  // "YYYY-MM-DD HH:MM:SS"
    // <camera>_DD.MM.YYYY_HH.MM.mkv
    std::string cleaned = stamp.size() >= 16 ? stamp.substr(8, 2) + "." + stamp.substr(5, 2) + "." + stamp.substr(0, 4) + "_" +
                                                   stamp.substr(11, 2) + "." + stamp.substr(14, 2)
                                             : stamp;
    DownloadParams p;
    p.host = target_.host; p.user = target_.user; p.password = target_.password; p.port = target_.port;
    p.file = files[row];
    p.path = dir + "/" + cam + "_" + cleaned + ".mkv";
    dl_.on_progress = [this](long long bytes, int secs) {
        char b[160];
        snprintf(b, sizeof b, _("Downloading… %.1f MB, %d s of video"), bytes / 1048576.0, secs);
        status(b);
    };
    dl_.on_done = [this](bool ok, const std::string& text) {
        gtk_button_set_icon_name(GTK_BUTTON(dl_btn_), "document-save-symbolic");
        downloading_row_ = -1;
        status(ok ? std::string(_("Saved: ")) + text : text);
    };
    if (dl_.start(p)) {
        downloading_row_ = row;
        gtk_button_set_icon_name(GTK_BUTTON(dl_btn_), "process-stop-symbolic");
        status(_("Downloading…"));
    }
    std::fill(p.password.begin(), p.password.end(), '\0');
}

void PlaybackView::file_activated(GtkListBox*, GtkListBoxRow* row, gpointer self) {
    PlaybackView* w = (PlaybackView*)self;
    int i = gtk_list_box_row_get_index(row);
    const auto& files = w->timeline_.files();
    if (i < 0 || i >= (int)files.size()) return;
    if (w->ready_for_playback())
        w->start_playback((int)std::max<long long>(files[i].begin - w->timeline_.day_start(), 0));
}

// Highlights the file that contains second `sec` (does not start anything).
void PlaybackView::show_current_file(int sec) {
    const auto& files = timeline_.files();
    const xm::RecordingFile* f = file_at(files, timeline_.day_start(), sec);
    int row = f ? (int)(f - files.data()) : -1;
    if (row == shown_row_) return;
    shown_row_ = row;
    if (row < 0) { gtk_list_box_unselect_all(GTK_LIST_BOX(files_list_)); return; }
    GtkListBoxRow* r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(files_list_), row);
    if (r) gtk_list_box_select_row(GTK_LIST_BOX(files_list_), r);
    scroll_to_row(row);
}

// Scrolls the side list so that the row is fully visible (the list is in a GtkScrolledWindow).
void PlaybackView::scroll_to_row(int row) {
    GtkListBoxRow* r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(files_list_), row);
    if (!r) return;
    graphene_rect_t b;
    if (!gtk_widget_compute_bounds(GTK_WIDGET(r), files_list_, &b)) return;
    GtkAdjustment* adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(files_scroll_));
    double top = b.origin.y, bottom = b.origin.y + b.size.height;
    double v = gtk_adjustment_get_value(adj), page = gtk_adjustment_get_page_size(adj);
    if (top < v) gtk_adjustment_set_value(adj, top);
    else if (bottom > v + page) gtk_adjustment_set_value(adj, bottom - page);
}

gboolean PlaybackView::position_tick(gpointer self) {
    PlaybackView* w = (PlaybackView*)self;
    xm::CamTime t;
    if (w->player_.active() && w->player_.position(t)) {
        long long sec = t - w->timeline_.day_start();
        if (sec >= 0 && sec <= kDaySeconds) {
            w->cur_sec_ = (int)sec;
            w->timeline_.set_selected((int)sec);
            w->show_current_file((int)sec);
            gtk_label_set_text(GTK_LABEL(w->info_), w->info_text((int)sec).c_str());
        }
    }
    return G_SOURCE_CONTINUE;
}

}  // namespace icmon
