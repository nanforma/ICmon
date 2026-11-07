// timeline.h - GTK4 widget: 24-hour bar showing which parts of the day have recordings on the camera's SD card.
// Drawn with Cairo in a GtkDrawingArea. Shows ARCHIVE data only (not live view, not local PC recordings).
#pragma once
#include <gtk/gtk.h>

#include <functional>
#include <vector>

#include "archive.h"
#include "timeline_geometry.h"

namespace icmon {

class Timeline {
public:
    Timeline();
    ~Timeline();
    Timeline(const Timeline&) = delete;
    Timeline& operator=(const Timeline&) = delete;

    GtkWidget* widget() const { return area_; }

    void set_day(xm::CamTime day_start);                          // clears recordings and the selection
    void set_files(std::vector<xm::RecordingFile> files);         // recordings of that day
    void set_selected(int second_of_day);                         // -1 = none

    std::function<void(int)> on_hover;  // second of the day under the pointer; -1 when the pointer leaves
    std::function<void(int)> on_click;  // second of the day that was clicked

    const std::vector<xm::RecordingFile>& files() const { return files_; }
    xm::CamTime day_start() const { return day_start_; }

private:
    static void draw_cb(GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer self);
    static void motion_cb(GtkEventControllerMotion*, double x, double y, gpointer self);
    static void leave_cb(GtkEventControllerMotion*, gpointer self);
    static void pressed_cb(GtkGestureClick*, int n_press, double x, double y, gpointer self);
    void draw(cairo_t* cr, int w, int h);

    GtkWidget* area_ = nullptr;
    GtkWidget* pop_ = nullptr;        // time popup above the pointer
    GtkWidget* pop_label_ = nullptr;
    void show_popup(double x, int sec);
    xm::CamTime day_start_ = 0;
    std::vector<xm::RecordingFile> files_;
    std::vector<Segment> segments_;
    int selected_ = -1, hover_ = -1;
    int width_ = 0;
};

}  // namespace icmon
