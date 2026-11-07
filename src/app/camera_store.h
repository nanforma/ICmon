// camera_store.h - the list of cameras the user has added, kept in a JSON file in the user's config directory.
//
// The file never contains passwords (they go to the system keyring, see secrets.h, or stay in memory for the
// session). Only what the user typed in the "add camera" window is stored: nothing here is discovered or guessed.
#pragma once
#include <string>
#include <vector>

namespace icmon {

struct CameraConfig {
    std::string id;    // random, stable; the key under which the password is kept in the keyring
    std::string name;  // shown in the list and on the video tile
    std::string host;
    std::string user = "admin";
    int dvrip_port = 34567;  // archive, listing (DVRIP)
    int rtsp_port = 554;     // live view
    int channel = 1, main_stream = 0, sub_stream = 1;  // values put into the RTSP template
    bool use_sub_in_grid = false;  // use the sub stream when several cameras are shown at once
    bool no_password = false;      // the camera account has an empty password
    std::string rtsp_template;     // see rtsp_url.h
    int slot = -1;                 // position in the 4x4 grid (0 = top left); the list order follows the slots
    bool ptz_flip_v = false;       // swap up and down of the PTZ pad
    bool ptz_flip_h = false;       // swap left and right of the PTZ pad (this camera moves the other way round)
    std::vector<int> presets;      // numbers (1..255) of the saved PTZ positions made with this program; the camera
                                   // cannot list its own, so only these are shown as buttons
};

struct StoreData {
    std::vector<CameraConfig> cameras;
    int layout = 1;           // cameras shown at once in live view: 1, 2, 4 or 16 (4 x 4)
    std::string selected_id;  // last selected camera
    bool start_maximized = false;  // open the window maximized
    std::string language;     // "" = system, else "en", "uk" or "ru"
    std::string media_dir;    // base folder for saved frames ("pictures") and video ("video"); empty = default_media_dir()
};

constexpr int kGridSlots = 16;  // tiles of the largest grid

// Gives every camera a unique slot (cameras without one, or with a taken one, get the first free slot). The order of
// the list is kept: it only changes between active cameras (buttons, swapping two tiles), never through empty tiles.
void normalize_slots(StoreData& d);
int first_free_slot(const StoreData& d);
// Adds a camera to the list between the cameras on the tiles before and after its own tile.
void insert_by_slot(StoreData& d, const CameraConfig& c);

// Empty string when the camera is acceptable, otherwise a short English reason (the GUI shows it).
std::string validate_camera(const CameraConfig& c);

std::string new_camera_id();      // 16 random hex digits
std::string default_media_dir();  // <Downloads>/icmon_media (XDG download dir, else $HOME/Downloads)
std::string default_config_dir(); // $XDG_CONFIG_HOME/icmon, else $HOME/.config/icmon

struct LoadResult {
    StoreData data;
    std::string error;               // fatal: the file could not be read as a camera list (moved aside as .bad)
    std::vector<std::string> notes;  // entries that were skipped, and why
};

// A missing file is not an error: it yields an empty list.
LoadResult load_store(const std::string& path);

// Only the "language" entry of the file ("" when missing or unreadable); no side effects, used before the GUI starts.
std::string read_language(const std::string& path);

// Writes through a temporary file and a rename. Returns an error text, or "" on success.
std::string save_store(const std::string& path, const StoreData& d);

}  // namespace icmon
