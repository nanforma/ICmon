// live_player.h - live view of one camera over RTSP through GStreamer.
//
// rtspsrc -> decode -> RGBA frames in an appsink -> GdkTexture (shown by a GtkPicture in the window).
// This is LIVE video from the camera; the archive on its SD card is played by Player (player.h).
// This class never reconnects by itself (a camera locks the account after repeated failed logins); a failure is
// reported. Sound is part of the stream, muted until set_audio(); untested with real sound. All callbacks run on the GTK main thread.
#pragma once
#include <gdk/gdk.h>
#include <glib.h>

#include <functional>
#include <memory>
#include <string>

namespace icmon {

enum class LiveState { Connecting, Playing, Failed, Rejected /* login refused: never retried */, Ended,
                       Dropped /* had been playing, then broke (not a login failure) */ };

struct LiveParams {
    std::string url;  // may contain the password: never log it
    int height = 1080; // frames are scaled to this height for display (fixed for the life of the stream), aspect kept
};

class LivePlayer {
public:
    LivePlayer();
    ~LivePlayer();
    LivePlayer(const LivePlayer&) = delete;
    LivePlayer& operator=(const LivePlayer&) = delete;

    void start(const LiveParams& p);  // stops a running stream first
    void stop();
    bool active() const;
    void set_audio(bool on, double volume);  // 0..1; the sound branch always starts muted; no effect without an audio device
    // Saves the newest full-resolution frame (as decoded, e.g. 2304x2592) as PNG. `done` runs on the main thread.
    void snapshot(const std::string& path, std::function<void(bool ok, const std::string& text)> done);

    std::function<void(GdkTexture* /*owned by the callee*/)> on_frame;
    std::function<void(LiveState, const std::string&)> on_state;  // text: reason for Failed/Ended

    struct Session;  // internal; public so the file-local callbacks in live_player.cpp can name it

private:
    std::shared_ptr<Session> s_;
};

}  // namespace icmon
