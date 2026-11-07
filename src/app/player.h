// player.h - plays a time window of the camera's SD-card archive inside the GTK main window.
//
// Download + demux run in a worker thread (xm::fetch_range); frames are pushed into a GStreamer pipeline
// (HEVC video, G.711 A-law audio). Video leaves the pipeline through an appsink as RGBA frames scaled to a
// modest height and is shown by the window in a GtkPicture. No extra GStreamer GTK plugin is needed.
//
// All callbacks are called on the GTK main thread. This plays ARCHIVE data from the camera, not live video.
// Pause stops pulling data from the camera (the download is throttled by the pipeline); how long the
// camera keeps such an idle connection open has not been measured.
#pragma once
#include <gdk/gdk.h>
#include <glib.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "archive.h"

namespace icmon {

struct PlayParams {
    std::string host, user, password;
    int port = 34567;
    std::vector<xm::RecordingFile> files;  // the day's listing
    xm::CamTime start = 0, end = 0;        // camera clock
    int lead_secs = 6;
    double rate = 1.0;                     // 1, 2 or 4: forward speed (video time stamps are divided by it; sound is muted above 1)
};

class Player {
public:
    Player();
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    void start(const PlayParams& p);  // stops a running playback first
    void stop();                      // returns after the worker thread has ended
    void set_paused(bool paused);
    bool active() const;
    // Sound: muted by default; applies to the running and to later playbacks. volume 0..1.
    void set_audio(bool on, double volume);
    // Camera time of the picture being played (needs a first I-frame to have arrived).
    bool position(xm::CamTime& t) const;

    std::function<void(GdkTexture* /*owned by the callee*/)> on_frame;
    std::function<void(const std::string&)> on_message;  // errors and notes
    std::function<void()> on_finished;                   // end of data or error; playback is over

    struct Session;  // internal; public only so the file-local callbacks in player.cpp can name it

private:
    std::shared_ptr<Session> s_;
    bool audio_on_ = false;
    double volume_ = 0.7;
    double rate_ = 1.0;
    void apply_audio();
};

}  // namespace icmon
