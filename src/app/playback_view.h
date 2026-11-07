// playback_view.h - the camera's SD-card archive: calendar and the day's file list (shown in the window's side
// panel), and the "Playback" tab with the picture, transport buttons and the 24-hour timeline.
//
// Scope: browse which recordings exist on the camera for a day, and play the camera's ARCHIVE from a clicked
// moment or file (up to 30 minutes ahead; play/pause, stop, previous/next recording; no seeking by dragging,
// no speed control yet).
// Network work runs in worker threads; results come back to the GTK main thread via g_idle_add.
// It never retries a rejected login by itself.
#pragma once
#include <gtk/gtk.h>

#include <atomic>
#include <functional>
#include <map>
#include <utility>
#include <memory>
#include <string>
#include <vector>

#include "archive.h"
#include "downloader.h"
#include "player.h"
#include "timeline.h"

namespace icmon {

struct PlaybackTarget {
    bool valid = false;  // false: no camera, or its password is not known yet
    std::string name, host, user, password;
    int port = 34567;
};

class PlaybackView {
public:
    PlaybackView();
    ~PlaybackView();
    PlaybackView(const PlaybackView&) = delete;
    PlaybackView& operator=(const PlaybackView&) = delete;

    GtkWidget* widget() const { return root_; }            // picture, buttons, timeline (the Playback tab)
    GtkWidget* calendar_widget() const { return calendar_; }
    GtkWidget* files_widget() const { return files_box_; }  // list of the recordings of the selected day
    GtkWidget* download_button() const { return dl_btn_; }     // saves the selected recording; the window puts it in a title

    // The camera changed (or its password became known). Stops playback and clears the timeline.
    void set_target(const PlaybackTarget& t);
    // The tab was shown or hidden. Hiding stops playback; showing reads the day's list if needed.
    void set_tab_visible(bool visible);

    // Base folder for saved media; downloaded recordings go to <base>/video.
    void set_media_dir(const std::string& base) { media_dir_ = base; }
    // Cancels a running download (e.g. before the window closes).
    void cancel_download() { dl_.cancel(); }

    std::function<void(const std::string&)> on_status;

private:
    struct Alive { std::atomic<bool> ok{true}; };
    struct Result {
        std::shared_ptr<Alive> alive;
        PlaybackView* w = nullptr;
        uint64_t gen = 0;
        bool ok = false, auth_failed = false, refused = false;
        std::string error;
        std::vector<xm::RecordingFile> files;
    };

    struct MarkResult {
        std::shared_ptr<Alive> alive;
        PlaybackView* w = nullptr;
        uint64_t gen = 0;
        int year = 0, month = 0;
        std::vector<int> days;  // days of the month that have at least one recording
        bool ok = false;
        std::string error;      // why the scan failed (shown in the status line, never swallowed)
        int refused = 0;        // days the camera refused to answer (e.g. days in the future)
        int answered = 0;       // days the camera answered for, to tell "no recordings" from "no answers"
    };

    static void download_clicked(GtkButton*, gpointer self);
    void start_download(int row);
    void scroll_to_row(int row);
    std::string info_text(int sec) const;
    static void speed_pressed(GtkGestureClick*, int n_press, double, double, gpointer self);
    static void speed_changed(GtkRange*, gpointer self);
    static void sound_toggled(GtkToggleButton*, gpointer self);
    static void volume_changed(GtkRange*, gpointer self);
    static void day_selected(GtkCalendar*, gpointer self);
    static void month_changed(GtkCalendar*, gpointer self);
    static gboolean deliver_marks(gpointer result);
    static gboolean deliver(gpointer result);
    static void play_clicked(GtkButton*, gpointer self);
    static void stop_clicked(GtkButton*, gpointer self);
    static void prev_clicked(GtkButton*, gpointer self);
    static void next_clicked(GtkButton*, gpointer self);
    static void file_activated(GtkListBox*, GtkListBoxRow* row, gpointer self);
    static gboolean position_tick(gpointer self);

    xm::CamTime selected_day_start() const;
    void try_list();
    void scan_month();                         // yellow dots: which days of the shown month have recordings
    void apply_marks(int year, int month, const std::vector<int>& days);
    void start_listing();
    void on_result(Result& r);
    void status(const std::string& s);
    void hover(int sec);
    void clicked(int sec);
    void start_playback(int sec);
    void playback_ended();
    void show_idle_controls();
    void update_play_icon();
    void fill_files();
    void show_current_file(int sec);
    bool ready_for_playback();
    void jump(int direction);  // +1 next recording, -1 previous

    GtkWidget *root_ = nullptr, *calendar_ = nullptr, *info_ = nullptr, *files_list_ = nullptr, *files_scroll_ = nullptr, *files_box_ = nullptr;
    GtkWidget *video_ = nullptr, *play_btn_ = nullptr, *stop_btn_ = nullptr, *prev_btn_ = nullptr, *next_btn_ = nullptr;
    GtkWidget *mute_btn_ = nullptr, *vol_ = nullptr, *dl_btn_ = nullptr, *speed_ = nullptr;
    double rate_ = 1.0;   // playback speed
    guint speed_timer_ = 0;
    bool speed_snapping_ = false;
    void apply_speed(int mark);
    int cur_sec_ = -1;     // second of the day shown/selected, -1 = none
    int shown_row_ = -1;   // row of the file list that is highlighted
    Timeline timeline_;
    Player player_;
    guint position_timer_ = 0;
    bool paused_ = false;
    Downloader dl_;
    std::string media_dir_;
    int downloading_row_ = -1;

    PlaybackTarget target_;
    bool visible_ = false, need_listing_ = false, rejected_ = false;

    std::shared_ptr<Alive> alive_ = std::make_shared<Alive>();
    std::shared_ptr<xm::CancelToken> cancel_ = std::make_shared<xm::CancelToken>();
    uint64_t gen_ = 0;

    // Calendar marks. One connection, one OPFileQuery per day of the month, in a worker thread; results are cached
    // per month until the camera changes. Started only after the day's listing worked (so the login is known good).
    std::shared_ptr<xm::CancelToken> marks_cancel_ = std::make_shared<xm::CancelToken>();
    uint64_t marks_gen_ = 0;
    std::pair<int, int> scanning_{0, 0};  // month being scanned, {0,0} = none
    std::map<std::pair<int, int>, std::vector<int>> marks_cache_;
};

}  // namespace icmon
