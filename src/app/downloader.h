// downloader.h - saves one recording of the camera's SD card to a Matroska file (.mkv) on this computer.
//
// The camera's own stream (HEVC video + G.711 A-law audio in the camera's frame envelope) is fetched with the verified
// archive download (archive.h), split into frames, time-stamped (av_timeline.h) and muxed by GStreamer:
//   appsrc(h265) -> h265parse -> matroskamux <- appsrc(alaw);  matroskamux -> filesink
// The file is written as NAME.part and renamed when complete; a cancelled or failed download leaves no file behind.
// NOT verified on a real camera: that the resulting .mkv plays, that its audio is in sync, and whether a download that
// begins exactly at a file's first byte starts with an I-frame (frames before the first I-frame are dropped).
// Callbacks run on the GTK main thread.
#pragma once
#include <glib.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "archive.h"

namespace icmon {

struct DownloadParams {
    std::string host, user, password;
    int port = 34567;
    xm::RecordingFile file;  // the recording to save, whole
    std::string path;        // final file name (…/x.mkv)
};

class Downloader {
public:
    Downloader();
    ~Downloader();  // cancels and waits
    Downloader(const Downloader&) = delete;
    Downloader& operator=(const Downloader&) = delete;

    // Only one download at a time; returns false (and does nothing) while one is running.
    bool start(const DownloadParams& p);
    void cancel();  // asks the running download to stop; on_done follows with ok=false
    bool active() const;

    std::function<void(long long /*bytes received*/, int /*seconds of video written*/)> on_progress;
    std::function<void(bool ok, const std::string& text)> on_done;  // text: path on success, reason on failure

    struct State;  // internal

private:
    void join();
    std::shared_ptr<State> s_;
    std::thread worker_;
};

}  // namespace icmon
