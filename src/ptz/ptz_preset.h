#pragma once
// A saved PTZ position as the camera reports it (ONVIF GetPresets).
#include <string>
#include <vector>

namespace ptz {

struct Preset {
    std::string token;  // the camera's own identifier: the only thing GotoPreset/RemovePreset may be given
    std::string name;
    bool has_position = false;
    double pan = 0, tilt = 0, zoom = 0;
};

struct Profile {
    std::string token, name;
    bool has_ptz = false;  // the profile contains a PTZConfiguration
};

}  // namespace ptz
