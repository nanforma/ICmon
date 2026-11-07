// Uses GtkWidget colour lookup that is deprecated since GTK 4.10 but still works; silence the warning here only.
#define GDK_DISABLE_DEPRECATION_WARNINGS
#include "timeline.h"

#include <cairo.h>

#include <algorithm>
#include <cmath>

#include "i18n.h"

namespace icmon {

namespace {
constexpr double kLeft = 0, kRight = 0, kTop = 14, kBottom = 24;  // the bar spans the whole widget width; ruler below it
}

Timeline::Timeline() {
    area_ = gtk_drawing_area_new();
    gtk_widget_set_hexpand(area_, TRUE);
    gtk_widget_set_size_request(area_, 240, 76);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area_), draw_cb, this, nullptr);

    GtkEventController* mc = gtk_event_controller_motion_new();
    g_signal_connect(mc, "motion", G_CALLBACK(motion_cb), this);
    g_signal_connect(mc, "leave", G_CALLBACK(leave_cb), this);
    gtk_widget_add_controller(area_, mc);

    GtkGesture* click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(pressed_cb), this);
    gtk_widget_add_controller(area_, GTK_EVENT_CONTROLLER(click));
    // time popup near the pointer (above the timeline); it never takes the pointer, so hovering stays smooth
    pop_ = gtk_popover_new();
    gtk_widget_set_parent(pop_, area_);
    gtk_popover_set_autohide(GTK_POPOVER(pop_), FALSE);
    gtk_popover_set_has_arrow(GTK_POPOVER(pop_), TRUE);
    gtk_popover_set_position(GTK_POPOVER(pop_), GTK_POS_TOP);
    gtk_widget_set_can_target(pop_, FALSE);
    pop_label_ = gtk_label_new("");
    gtk_popover_set_child(GTK_POPOVER(pop_), pop_label_);
    // When the window is destroyed GTK destroys this widget before our destructor runs; remember that.
    g_signal_connect(area_, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer self) {
        Timeline* t = (Timeline*)self;
        if (t->pop_) { gtk_widget_unparent(t->pop_); t->pop_ = nullptr; }  // a child popover must be removed before its parent
        t->area_ = nullptr;
    }), this);
}

Timeline::~Timeline() {
    // Callbacks hold `this`; stop them from firing. The widget may already be gone (area_ == nullptr).
    on_hover = nullptr;
    on_click = nullptr;
    if (area_) {
        if (pop_) { gtk_widget_unparent(pop_); pop_ = nullptr; }
        g_signal_handlers_disconnect_by_data(area_, this);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area_), nullptr, nullptr, nullptr);
    }
}

void Timeline::set_day(xm::CamTime day_start) {
    day_start_ = day_start;
    files_.clear();
    segments_.clear();
    selected_ = hover_ = -1;
    gtk_widget_queue_draw(area_);
}

void Timeline::set_files(std::vector<xm::RecordingFile> files) {
    files_ = std::move(files);
    segments_ = day_segments(files_, day_start_);
    gtk_widget_queue_draw(area_);
}

void Timeline::set_selected(int sec) {
    selected_ = sec;
    gtk_widget_queue_draw(area_);
}

void Timeline::draw_cb(GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer self) { ((Timeline*)self)->draw(cr, w, h); }

void Timeline::motion_cb(GtkEventControllerMotion*, double x, double, gpointer self) {
    Timeline* t = (Timeline*)self;
    t->hover_ = second_from_x(x, kLeft, t->width_ - kLeft - kRight);
    gtk_widget_queue_draw(t->area_);
    t->show_popup(x, t->hover_);
    if (t->on_hover) t->on_hover(t->hover_);
}

void Timeline::show_popup(double x, int sec) {
    if (!pop_) return;
    if (sec < 0) { gtk_popover_popdown(GTK_POPOVER(pop_)); return; }
    const xm::RecordingFile* f = file_at(files_, day_start_, sec);
    std::string t = clock_text(sec) + (f ? "" : std::string("  (") + _("no recording") + ")");
    gtk_label_set_text(GTK_LABEL(pop_label_), t.c_str());
    GdkRectangle r = {(int)x, (int)kTop, 1, 1};  // the popup points at the top edge of the bar
    gtk_popover_set_pointing_to(GTK_POPOVER(pop_), &r);
    gtk_popover_popup(GTK_POPOVER(pop_));
}

void Timeline::leave_cb(GtkEventControllerMotion*, gpointer self) {
    Timeline* t = (Timeline*)self;
    t->hover_ = -1;
    gtk_widget_queue_draw(t->area_);
    t->show_popup(0, -1);
    if (t->on_hover) t->on_hover(-1);
}

void Timeline::pressed_cb(GtkGestureClick*, int, double x, double, gpointer self) {
    Timeline* t = (Timeline*)self;
    int sec = second_from_x(x, kLeft, t->width_ - kLeft - kRight);
    if (t->on_click) t->on_click(sec);
}

static void rounded_rect(cairo_t* cr, double x, double y, double w, double h, double r) {
    cairo_new_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

void Timeline::draw(cairo_t* cr, int w, int h) {
    width_ = w;
    GdkRGBA fg;
    gtk_widget_get_color(area_, &fg);  // follows the current (light or dark) theme
    const double bw = w - kLeft - kRight, by = kTop, bh = h - kTop - kBottom;
    const double r = 4;

    // the track: darker than the window, rounded (4 px), with the recordings (blue) inside it
    cairo_save(cr);
    rounded_rect(cr, kLeft, by, bw, bh, r);
    cairo_clip(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
    cairo_paint(cr);
    cairo_set_source_rgba(cr, 0.20, 0.55, 0.95, 0.90);  // recordings (regular ones; other tags have not been seen yet)
    for (const Segment& s : segments_) {
        double x0 = x_from_second(s.from, kLeft, bw), x1 = x_from_second(s.to, kLeft, bw);
        cairo_rectangle(cr, x0, by, std::max(x1 - x0, 1.0), bh);
        cairo_fill(cr);
    }
    cairo_restore(cr);
    // border around the whole track, so that a recording that stops before the end of the day is easy to see
    rounded_rect(cr, kLeft + 0.5, by + 0.5, bw - 1, bh - 1, r);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.45);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    // ruler below the bar (no background): ticks and a label every 2 hours
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.6);
    cairo_set_line_width(cr, 1.0);
    cairo_set_font_size(cr, 11);
    for (int hr = 0; hr <= 24; hr++) {
        double x = std::min(x_from_second(hr * 3600, kLeft, bw) + 0.5, (double)w - 0.5);
        cairo_move_to(cr, x, by + bh + 2);
        cairo_line_to(cr, x, by + bh + (hr % 2 == 0 ? 7 : 5));
        cairo_stroke(cr);
        if (hr % 2 == 0) {
            char b[8];
            snprintf(b, sizeof b, "%02d", hr % 24 == 0 && hr > 0 ? 24 : hr);
            cairo_text_extents_t e;
            cairo_text_extents(cr, b, &e);
            double tx = std::max(1.0, std::min(x - e.width / 2, (double)w - 1 - e.width));  // keep the end labels inside
            cairo_move_to(cr, tx, by + bh + 19);
            cairo_show_text(cr, b);
        }
    }

    // hover line (thin)
    if (hover_ >= 0) {
        double x = x_from_second(hover_, kLeft, bw) + 0.5;
        cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.8);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, x, by);
        cairo_line_to(cr, x, by + bh);
        cairo_stroke(cr);
    }
    // playback position: a thin red line and a small handle with rounded top corners
    if (selected_ >= 0) {
        double x = x_from_second(selected_, kLeft, bw) + 0.5;
        cairo_set_source_rgb(cr, 0.90, 0.15, 0.15);
        cairo_set_line_width(cr, 1.5);
        cairo_move_to(cr, x, by);
        cairo_line_to(cr, x, by + bh);
        cairo_stroke(cr);
        // an upside-down drop: a round head and a point at the bar
        const double r = 4.5, top = by - 12, cy = top + r;
        cairo_new_path(cr);
        cairo_arc(cr, x, cy, r, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_new_path(cr);
        cairo_move_to(cr, x - r * 0.87, cy + r * 0.5);
        cairo_line_to(cr, x + r * 0.87, cy + r * 0.5);
        cairo_line_to(cr, x, by);
        cairo_close_path(cr);
        cairo_fill(cr);
    }
}

}  // namespace icmon
