#pragma once
// ONVIF client for the PTZ presets of a camera: SOAP over plain HTTP, WS-Security UsernameToken, no dependencies.
// Nothing here knows about GTK. Every step is written to `log` (the application prints it to stderr), so that a
// camera that answers differently can be diagnosed from the program's own output.
#include <string>
#include <vector>

#include "ptz_preset.h"

namespace ptz {

struct OnvifResult {
    bool ok = false;
    std::string error;  // short English reason when !ok
    std::string log;    // every request and answer in short form
    int port = 0;       // the port where the ONVIF device service answered
    std::string ptz_url, media_url;  // paths (as the camera reported them)
    std::vector<Profile> profiles;   // ALL media profiles
    std::string profile_token;       // the profile whose presets are returned
    std::vector<Preset> presets;
};

struct OnvifTarget {
    std::string host, user, password;
    std::vector<int> ports;  // ports to probe for the device service; a port that answered before goes first
    int timeout_ms = 4000;
};

// Finds the device service, lists all media profiles, asks every profile that has a PTZConfiguration for its presets
// and returns the first non-empty list (else the first PTZ profile's empty list).
OnvifResult onvif_read_presets(const OnvifTarget& t);

// RemovePreset on a known port / profile / preset token. Empty string = the camera accepted it, else the reason.
std::string onvif_remove_preset(const OnvifTarget& t, int port, const std::string& profile_token, const std::string& preset_token,
                                std::string* log = nullptr);

// SetPreset: stores the camera's CURRENT position under the given token (ONVIF way; the DVRIP command is the first choice).
std::string onvif_set_preset(const OnvifTarget& t, int port, const std::string& profile_token, const std::string& preset_token,
                             const std::string& name, std::string* log = nullptr);

// Pieces exposed for the tests.
std::string sha1_raw(const std::string& data);
std::string base64_encode(const std::string& data);

}  // namespace ptz
