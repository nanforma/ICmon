// live_view.h - the "Live View" tab: one, two or four video tiles with live RTSP pictures.
//
// Layouts: 1 tile; 2 tiles side by side (each tall: suits cameras whose frame holds two stacked pictures);
// 2x2 grid. The window decides which camera goes into which tile and when a stream starts; this class shows the tiles
// and reports clicks. A tile (with its running stream) can be moved to another grid position when the layout changes,
// so that a camera that stays on screen is not reconnected (see arrange()).
// Single click selects a tile, double click asks to open it in Playback.
// A stream that had been playing and then dropped is restarted automatically up to kMaxRetries times in a row (the
// login is known to work); a stream that never played, or whose login was rejected, is NOT retried.
#pragma once
#include <gtk/gtk.h>

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "live_player.h"

namespace icmon {

class LiveView {
public:
    static constexpr int kMaxSlots = 16;  // number of tiles (and the largest layout)
    static constexpr int kMaxRetries = 5;

    LiveView();
    ~LiveView();
    LiveView(const LiveView&) = delete;
    LiveView& operator=(const LiveView&) = delete;

    GtkWidget* widget() const { return root_; }

    // `order[i]` is the tile shown at grid position i; its size (1, 2 or 4) is the layout. Tiles that are not listed are
    // stopped and emptied. Running streams stay running when only their position changes.
    void arrange(const std::vector<int>& order);
    int slots() const { return (int)order_.size(); }

    // Tile content (tile = index 0..kMaxSlots-1). `camera_id` empty = nothing in this tile.
    void assign(int tile, const std::string& camera_id, const std::string& name);
    const std::string& camera_id(int tile) const;
    const std::string& key(int tile) const;  // key of the stream started in this tile; empty when none
    int tile_of(const std::string& camera_id) const;  // tile that holds a started stream of this camera, or -1
    // Starts the stream unless the same `key` is already running or has failed (no retry for a stream that never played).
    void start(int tile, const std::string& key, const std::string& url, int height);
    void stop(int tile, const std::string& message = "");  // stops the stream and shows `message`
    void stop_all();
    void set_message(int tile, const std::string& text);
    void set_selected(int tile);  // -1 = none; the icon bar (save frame, sound, volume) is shown on this tile only

    // Shows the light button's state without reporting it (the window sets it from what the camera answered).
    void set_light(bool on);

    // Base folder of the saved media; frames go to <base>/picture. Set before the first snapshot.
    void set_media_dir(const std::string& base) { media_dir_ = base; }

    std::function<void(int)> on_clicked;    // tile
    // (dragged camera id, camera id of the tile it was dropped on - empty for an empty tile, grid position of that tile)
    std::function<void(const std::string&, const std::string&, int)> on_drop;
    int position_of(int tile) const;  // grid position of a shown tile, -1 when it is not shown
    std::function<void(int)> on_activated;  // tile (double click)
    std::function<void(int, LiveState, const std::string&)> on_state;
    std::function<void(int, bool)> on_light;  // (tile, wanted state) the light button was pressed
    std::function<void(const std::string&)> on_notice;  // short messages (e.g. where a frame was saved)

private:
    struct Tile;
    static void pressed_cb(GtkGestureClick*, int n_press, double, double, gpointer tile);
    static void snapshot_cb(GtkButton*, gpointer tile);
    static void light_cb(GtkToggleButton*, gpointer tile);
    static void mute_cb(GtkToggleButton*, gpointer tile);
    static void volume_cb(GtkRange*, gpointer tile);
    static gboolean retry_cb(gpointer tile);
    void snapshot(int tile);
    void apply_audio();
    void tile_state(Tile& t, LiveState st, const std::string& text);

    GtkWidget *root_ = nullptr, *grid_ = nullptr;
    std::vector<std::unique_ptr<Tile>> tiles_;
    std::vector<int> order_;
    int selected_ = -1;
    bool updating_ = false;
    bool light_on_ = false;
    bool sound_on_ = false;  // the sound button; the sound comes from the selected tile only
    double volume_ = 0.7;
    std::string media_dir_;
};

}  // namespace icmon
