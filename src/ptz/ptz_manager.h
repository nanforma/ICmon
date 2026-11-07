#pragma once
// What the user interface talks to: the camera's presets (read over ONVIF), asynchronously, with a small cache of the
// last answer per camera. No GTK here. Callbacks run on a worker thread: the caller moves them to its own thread.
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "ptz_onvif.h"

namespace ptz {

class PtzManager {
public:
    using ReadDone = std::function<void(const OnvifResult&)>;
    using RemoveDone = std::function<void(const std::string& error, const std::string& log)>;

    // refresh_presets(): GetPresets on every profile that has a PTZConfiguration (see ptz_onvif.h).
    void refresh_presets(const std::string& camera_id, OnvifTarget target, ReadDone done);
    // delete_preset(): RemovePreset for a token the camera itself reported earlier (refuses an unknown token).
    void delete_preset(const std::string& camera_id, OnvifTarget target, const std::string& token, RemoveDone done);

    // save_preset(): ONVIF SetPreset under `token` (needs one successful refresh first: port and profile come from it).
    void save_preset(const std::string& camera_id, OnvifTarget target, const std::string& token, RemoveDone done);

    // The last successful answer for the camera (empty before the first one).
    std::vector<Preset> get_presets(const std::string& camera_id) const;
    bool token_for(const std::string& camera_id, const std::string& token, Preset* out = nullptr) const;

private:
    struct Entry {
        int port = 0;
        std::string profile_token;
        std::vector<Preset> presets;
    };
    mutable std::mutex m_;
    std::map<std::string, Entry> cache_;
};

// Ports probed for the ONVIF device service; Xiongmai cameras usually answer on 8899, some on 80.
std::vector<int> default_onvif_ports();

}  // namespace ptz
