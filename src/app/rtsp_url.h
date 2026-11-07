// rtsp_url.h - builds the RTSP address of a camera from a user-editable template.
//
// The path part of an RTSP address differs between camera firmwares. The default below is the form the user's
// own working mpv script uses for the target camera; it has no stream number, so a sub stream cannot be chosen
// with it. If another form is needed, the user edits the address in the camera settings.
//
// Placeholders: {host} {port} {user} {password} {channel} {stream}. Anything else in braces is an error.
// user and password are percent-encoded. The result may contain the password: never log or display it.
#pragma once
#include <string>

#include "camera_store.h"

namespace icmon {

extern const char* const kDefaultRtspTemplate;

struct UrlResult {
    std::string url, error;  // error is empty on success
};

// `stream` selects the stream index put into {stream} (the caller picks main or sub stream).
UrlResult build_rtsp_url(const std::string& tmpl, const CameraConfig& cam, const std::string& password, int stream);

// Percent-encodes everything except A-Z a-z 0-9 - . _ ~
std::string percent_encode(const std::string& s);

}  // namespace icmon
