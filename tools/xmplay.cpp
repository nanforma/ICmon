// xmplay - plays a time window from the camera's SD card straight from the network (no temp file).
//
//   xmplay --host IP --user U --start "YYYY-MM-DD HH:MM:SS" --secs N [--lead S] [--port N]
//
// Pipeline (GStreamer): appsrc(HEVC byte-stream) -> h265parse -> decodebin -> videoconvert -> autovideosink
//                       appsrc(G.711 A-law 8 kHz) -> alawdec -> audioconvert -> audioresample -> autoaudiosink
// A worker thread downloads and demuxes; frames are pushed to the two appsrc elements. appsrc blocks when its
// queue is full, which also throttles the download. Ctrl-C stops playback and closes the connection.
//
// This is a test tool for the decode/sync path, not the final player. Seeking, pause and the GUI are not here.
// SECURITY: the DVRIP login is not encrypted (see src/xm/dvrip.h).
#include <glib-unix.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <atomic>
#include <cstdio>
#include <iostream>
#include <memory>
#include <thread>

#include "archive.h"
#include "av_timeline.h"
#include "cli_common.h"

using namespace cli;

namespace {

struct Ctx {
    GMainLoop* loop = nullptr;
    GstElement* pipe = nullptr;
    GstAppSrc *vsrc = nullptr, *asrc = nullptr;
    std::atomic<bool> finished{false};
};

gboolean on_bus(GstBus*, GstMessage* msg, gpointer data) {
    Ctx* c = (Ctx*)data;
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            std::cerr << "end of stream\n";
            g_main_loop_quit(c->loop);
            break;
        case GST_MESSAGE_ERROR: {
            GError* err = nullptr;
            gchar* dbg = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            std::cerr << "GStreamer error: " << (err ? err->message : "?") << "\n";
            if (dbg) std::cerr << "  debug: " << dbg << "\n";
            g_clear_error(&err);
            g_free(dbg);
            g_cancel.cancel();
            g_main_loop_quit(c->loop);
            break;
        }
        case GST_MESSAGE_WARNING: {
            GError* err = nullptr;
            gchar* dbg = nullptr;
            gst_message_parse_warning(msg, &err, &dbg);
            std::cerr << "GStreamer warning: " << (err ? err->message : "?") << "\n";
            g_clear_error(&err);
            g_free(dbg);
            break;
        }
        default: break;
    }
    return TRUE;
}

gboolean on_sigint_glib(gpointer data) {
    Ctx* c = (Ctx*)data;
    std::cerr << "\ncancelled\n";
    g_cancel.cancel();
    g_main_loop_quit(c->loop);
    return G_SOURCE_REMOVE;
}

// Returns false when the pipeline no longer accepts data (flushing / error): the caller cancels the download.
bool push(GstAppSrc* src, const uint8_t* d, size_t n, const xm::Stamped& s) {
    GstBuffer* b = gst_buffer_new_allocate(nullptr, n, nullptr);
    gst_buffer_fill(b, 0, d, n);
    GST_BUFFER_PTS(b) = s.pts_ns;
    GST_BUFFER_DTS(b) = s.pts_ns;
    GST_BUFFER_DURATION(b) = s.dur_ns;
    return gst_app_src_push_buffer(src, b) == GST_FLOW_OK;  // takes ownership of b
}

void usage() {
    std::cerr << "usage: xmplay --host IP --user U --start \"YYYY-MM-DD HH:MM:SS\" --secs N [--lead S] [--port N]\n"
                 "password: env CAM_PASS or hidden prompt. Times are the camera's clock.\n";
}

}  // namespace

int main(int argc, char** argv) {
    Args a;
    if (!parse_args(argc, argv, a, false) || a.get("host").empty() || a.get("start").empty() || a.get("secs").empty()) {
        usage();
        return 2;
    }
    gst_init(&argc, &argv);
    int secs = atoi(a.get("secs").c_str());
    int lead = atoi(a.get("lead", "6").c_str());
    if (secs <= 0 || secs > 3600 || lead < 0 || lead > 60) { std::cerr << "--secs must be 1..3600 and --lead 0..60\n"; return 2; }

    std::vector<xm::RecordingFile> files;
    xm::CamTime start = 0, end = 0;
    std::string pw;
    try {
        start = xm::parse_cam_time(a.get("start"));
        end = start + secs;
        pw = get_password(a.get("user", "admin"));
        xm::Dvrip c;
        connect_and_login(c, a, pw);
        files = xm::list_files(c, start - 3600, end + 3600, &g_cancel);
    } catch (const xm::Error& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    Ctx ctx;
    GError* err = nullptr;
    ctx.pipe = gst_parse_launch(
        "appsrc name=vsrc is-live=false format=time block=true max-bytes=8000000 "
        "caps=video/x-h265,stream-format=byte-stream,alignment=au ! h265parse ! decodebin ! videoconvert ! autovideosink "
        "appsrc name=asrc is-live=false format=time block=true max-bytes=1000000 "
        "caps=audio/x-alaw,rate=8000,channels=1 ! alawdec ! audioconvert ! audioresample ! autoaudiosink",
        &err);
    if (!ctx.pipe) {
        std::cerr << "cannot build the pipeline: " << (err ? err->message : "?") << "\n(are gstreamer1.0-libav and plugins-good/bad installed?)\n";
        g_clear_error(&err);
        return 1;
    }
    g_clear_error(&err);
    ctx.vsrc = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(ctx.pipe), "vsrc"));
    ctx.asrc = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(ctx.pipe), "asrc"));
    ctx.loop = g_main_loop_new(nullptr, FALSE);
    GstBus* bus = gst_element_get_bus(ctx.pipe);
    gst_bus_add_watch(bus, on_bus, &ctx);
    g_unix_signal_add(SIGINT, on_sigint_glib, &ctx);

    xm::Connector connector = [&]() {
        auto d = std::make_unique<xm::Dvrip>();
        connect_and_login(*d, a, pw);
        return d;
    };

    std::thread worker([&] {
        xm::AvTimeline tl;
        size_t vframes = 0, aframes = 0;
        try {
            auto res = xm::fetch_range(connector, files, start, end, lead, [&](const xm::Frame& f, size_t) {
                xm::Stamped s;
                if (!tl.stamp(f, s)) return;
                bool ok = s.video ? push(ctx.vsrc, f.payload, f.payload_len, s) : push(ctx.asrc, f.payload, f.payload_len, s);
                (s.video ? vframes : aframes)++;
                if (!ok) g_cancel.cancel();  // pipeline refused data: stop downloading
            });
            for (const auto& n : res.notes) std::cerr << "NOTE: " << n << "\n";
            if (res.resyncs) std::cerr << "WARNING: " << res.resyncs << " resync(s), " << res.skipped_bytes << " byte(s) skipped\n";
        } catch (const xm::Error& e) {
            if (e.code() != xm::Errc::Cancelled) std::cerr << "download error: " << e.what() << "\n";
        }
        std::cerr << "pushed " << vframes << " video and " << aframes << " audio frames\n";
        gst_app_src_end_of_stream(ctx.vsrc);
        gst_app_src_end_of_stream(ctx.asrc);
        ctx.finished = true;
    });

    gst_element_set_state(ctx.pipe, GST_STATE_PLAYING);
    g_main_loop_run(ctx.loop);

    // Shutdown: stop the pipeline first (unblocks a worker waiting in push), then wait for the worker.
    g_cancel.cancel();
    gst_element_set_state(ctx.pipe, GST_STATE_NULL);
    worker.join();
    gst_object_unref(bus);
    gst_object_unref(ctx.vsrc);
    gst_object_unref(ctx.asrc);
    gst_object_unref(ctx.pipe);
    g_main_loop_unref(ctx.loop);
    return 0;
}
