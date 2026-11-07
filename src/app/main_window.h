// main_window.h - the application window: camera list on the left; tabs "Live View" and "Playback" on the right.
//
// Live View shows 1, 2 or 4 cameras over RTSP (live video). Playback shows the recordings stored on the
// camera's SD card (archive over DVRIP). Settings is a placeholder (not implemented yet).
// The camera list is kept in a JSON file; passwords go to the system keyring when this build has libsecret,
// otherwise they are kept in memory until the program closes.
#pragma once
#include <gtk/gtk.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "camera_dialog.h"
#include "camctl.h"
#include "camera_store.h"
#include "ptz_manager.h"
#include "live_view.h"
#include "playback_view.h"

namespace icmon {

class MainWindow {
public:
    explicit MainWindow(GtkApplication* app);
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

private:
    enum class Tab { Live, Playback };
    struct Alive { std::atomic<bool> ok{true}; };
    struct RowCtx { MainWindow* w; std::string id; };
    using PasswordCb = std::function<void(bool ok, const std::string& password, const std::string& problem)>;

    static void window_destroyed(GtkWidget*, gpointer self);
    static void tab_changed(GObject*, GParamSpec*, gpointer self);
    static void row_selected(GtkListBox*, GtkListBoxRow* row, gpointer self);
    static void gear_clicked(GtkButton*, gpointer ctx);
    static void settings_clicked(GtkButton*, gpointer self);
    static void startstop_clicked(GtkButton*, gpointer ctx);
    static void add_clicked(GtkButton*, gpointer self);
    static void remove_clicked(GtkButton*, gpointer self);
    static void reconnect_clicked(GtkButton*, gpointer self);
    static void layout_chosen(GtkButton*, gpointer ctx);
    static void ptz_pressed(GtkGestureClick*, int, double, double, gpointer ctx);
    static void ptz_released(GtkGestureClick*, int, double, double, gpointer ctx);
    static void ptz_cancelled(GtkGesture*, GdkEventSequence*, gpointer ctx);
    static void preset_goto(GtkButton*, gpointer ctx);
    static void preset_delete(GtkGestureClick*, int, double, double, gpointer ctx);
    static void preset_add(GtkButton*, gpointer self);
    static void preset_remove(GtkButton*, gpointer self);
    // The camera is the source of the list: read over ONVIF (src/ptz), see refresh_camera_presets()
    void refresh_camera_presets();
    void move_camera(int delta);
    std::map<std::string, const char*> dot_state_;  // last colour of each camera's status dot
    void trade(int i, int j);
    void drop_camera(const std::string& from, const std::string& to, int pos);
    int prev_layout_ = 0;  // grid before a double click opened one camera alone
    void presets_read(const std::string& camera_id, const ptz::OnvifResult& r);
    void refresh_presets_later(guint ms);
    static void restart_clicked(GtkButton*, gpointer ctx);
    void add_preset_number(bool send_set);
    static gboolean rebuild_presets_idle(gpointer self);
    static gboolean ptz_watchdog_cb(gpointer self);
    static gboolean ptz_stop_cb(gpointer self);
    void ptz_release();
    static void nav_toggled(GtkToggleButton*, gpointer self);

    std::string media_dir() const;  // folder for saved media (configured or default)
    GtkWidget* build_controls();
    void set_controls_enabled(bool on);
    void mark_layout_item();
    void apply_decoration_layout();
    static gboolean key_pressed(GtkEventControllerKey*, guint keyval, guint, GdkModifierType state, gpointer self);
    static void decoration_changed(GObject*, GParamSpec*, gpointer self);
    void update_title();
    void rebuild_presets();
    void queue_rebuild_presets();
    void send_preset(const std::string& cmd, int n, const std::string& done_text);
    // Camera commands (PTZ, presets, light) go through one logged-in connection per camera, in a worker thread.
    void with_ctl(const std::string& camera_id, std::function<void(CamControl&)> f);
    void ptz_move(std::string command, bool start);  // start = false stops the movement in progress
    void say_from_worker(const std::string& text);          // safe from any thread; shown in the status line
    void light_requested(int tile, bool on);
    void load_config();
    void save_config();
    void rebuild_list();
    void set_status(const std::string& s);
    void set_tab(Tab t);
    void set_layout(int n);
    const CameraConfig* find_cam(const std::string& id) const;
    int selected_index() const;
    void apply_selection(const std::string& id);
    void select_row_for(const std::string& id);
    void with_password(const CameraConfig& cam, PasswordCb cb);
    void refresh_live();
    void start_slot(int slot, const CameraConfig& cam);
    void update_playback_target();
    void set_dot(const std::string& camera_id, const char* css_class);
    void update_row_buttons();  // start/stop icon: only on the selected row, only on the Live tab
    void toggle_stream(const std::string& camera_id);
    void edit_camera(const std::string& id);
    void camera_saved(CameraDialogResult r);
    void camera_deleted(const std::string& id);

    GtkWidget *win_ = nullptr, *stack_ = nullptr, *side_stack_ = nullptr, *list_ = nullptr, *status_ = nullptr, *layout_box_ = nullptr;
    GtkWidget* layout_item_[4] = {nullptr, nullptr, nullptr, nullptr};
    ptz::PtzManager ptz_;
    std::map<std::string, int> pending_add_;       // camera id -> position just saved, waiting for the camera to list it
    std::set<std::string> fallback_tried_;         // cameras for which the ONVIF SetPreset fallback was already used
    void save_via_onvif(const std::string& camera_id, int n);
    std::map<std::string, std::map<int, std::string>> preset_names_;  // camera id -> number -> name given by the camera
    GtkWidget *title_name_ = nullptr, *hb_left_ = nullptr, *hb_right_ = nullptr, *grid_popover_ = nullptr, *presets_box_ = nullptr, *positions_box_ = nullptr, *preset_spin_ = nullptr, *preset_pop_ = nullptr;
    int selected_preset_ = 0;
    guint presets_idle_ = 0;
    std::vector<GtkWidget*> ptz_widgets_;  // enabled only on the Live tab with a camera selected
    GtkWidget* nav_btn_[2] = {nullptr, nullptr};
    bool syncing_nav_ = false;
    LiveView live_;
    PlaybackView playback_;

    std::string config_path_;
    StoreData store_;
    Tab tab_ = Tab::Live;
    bool rebuilding_ = false, playback_dirty_ = true;
    std::string playback_cam_id_;
    uint64_t playback_gen_ = 0, slot_gen_[LiveView::kMaxSlots] = {0, 0, 0, 0};
    std::vector<std::unique_ptr<RowCtx>> row_ctx_;  // per camera row; rebuilt with the list
    std::vector<std::unique_ptr<RowCtx>> preset_ctx_;  // saved-position buttons; cleared only after they are removed
    std::vector<std::unique_ptr<RowCtx>> ui_ctx_;   // PTZ / preset / grid buttons: live as long as the window
    std::vector<GtkWidget*> name_stack_, name_plain_, name_long_, name_scroll_;  // the name of each row (plain / scrolling)
    guint marquee_id_ = 0;
    int marquee_ticks_ = 0, marquee_sel_ = -2;
    static gboolean marquee_tick(gpointer p);
    std::vector<GtkWidget*> restart_;  // one per camera, same order as store_.cameras
    std::vector<GtkWidget*> startstop_;  // one per camera, same order as store_.cameras
    std::set<std::string> stopped_;      // cameras whose live stream the user stopped (until started again)
    std::vector<GtkWidget*> dots_;  // one per camera, same order as store_.cameras
    std::map<std::string, std::unique_ptr<CamControl>> ctl_;
    std::string ptz_cmd_, ptz_cam_;  // movement in progress (command empty = none)
    uint64_t ptz_token_ = 0;
    guint ptz_watchdog_ = 0, ptz_stop_timer_ = 0;
    gint64 ptz_press_us_ = 0;
    std::map<std::string, bool> light_state_;                          // what this session did, per camera
    std::map<std::string, std::shared_ptr<std::string>> light_orig_;   // light mode before we changed it, per camera
    std::map<std::string, std::string> pw_cache_;  // camera id -> password, this session only

    std::shared_ptr<Alive> alive_ = std::make_shared<Alive>();
};

}  // namespace icmon
