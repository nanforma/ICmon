#include <malloc.h>
#include "live_player.h"

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <gst/video/video.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

namespace icmon {

struct LivePlayer::Session {
    LivePlayer* owner = nullptr;
    std::atomic<bool> live{true};
    std::atomic<bool> frame_pending{false};
    bool got_first = false;  // main thread only
    GstElement* pipe = nullptr;
    guint bus_watch = 0;
};

LivePlayer::LivePlayer() { static bool once = [] { gst_init(nullptr, nullptr); return true; }(); (void)once; }
LivePlayer::~LivePlayer() { stop(); }

bool LivePlayer::active() const { return s_ && s_->live; }

void LivePlayer::stop() {
    if (!s_) return;
    std::shared_ptr<Session> s = std::move(s_);
    s->live = false;  // late idle handlers do nothing
    if (s->bus_watch) g_source_remove(s->bus_watch);
    gst_element_set_state(s->pipe, GST_STATE_NULL);
    gst_object_unref(s->pipe);
    s->pipe = nullptr;
    malloc_trim(0);  // frames are large: give the freed heap back instead of letting it pile up over restarts
}

namespace {

struct FrameJob {
    std::shared_ptr<LivePlayer::Session> s;
    GBytes* bytes;
    int w, h;
};
struct StateJob {
    std::shared_ptr<LivePlayer::Session> s;
    LiveState st;
    std::string text;
};

}  // namespace

static gboolean deliver_frame(gpointer p) {
    std::unique_ptr<FrameJob> j((FrameJob*)p);
    auto& s = j->s;
    s->frame_pending = false;
    if (s->live && s->owner) {
        if (!s->got_first) {
            s->got_first = true;
            if (s->owner->on_state) s->owner->on_state(LiveState::Playing, "");
        }
        if (s->live && s->owner->on_frame) {  // on_state may have stopped us
            GdkTexture* tex = gdk_memory_texture_new(j->w, j->h, GDK_MEMORY_R8G8B8A8, j->bytes, (gsize)j->w * 4);
            s->owner->on_frame(tex);
        }
    }
    g_bytes_unref(j->bytes);
    return G_SOURCE_REMOVE;
}

static gboolean deliver_state(gpointer p) {
    std::unique_ptr<StateJob> j((StateJob*)p);
    if (j->s->live && j->s->owner && j->s->owner->on_state) j->s->owner->on_state(j->st, j->text);
    return G_SOURCE_REMOVE;
}

// appsink callback; runs on a GStreamer streaming thread.
static GstFlowReturn on_new_sample(GstAppSink* sink, gpointer data) {
    auto* sp = (std::shared_ptr<LivePlayer::Session>*)data;
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;
    LivePlayer::Session& s = **sp;
    if (s.live && !s.frame_pending.exchange(true)) {  // at most one frame in flight; newer ones replace dropped ones
        GstCaps* caps = gst_sample_get_caps(sample);
        GstBuffer* buf = gst_sample_get_buffer(sample);
        int w = 0, h = 0;
        bool sent = false;
        if (caps && buf && gst_structure_get_int(gst_caps_get_structure(caps, 0), "width", &w) &&
            gst_structure_get_int(gst_caps_get_structure(caps, 0), "height", &h) && w > 0 && h > 0) {
            GstMapInfo m;
            if (gst_buffer_map(buf, &m, GST_MAP_READ)) {
                if (m.size == (gsize)w * h * 4) {  // tightly packed RGBA only; anything else is skipped, not guessed
                    g_idle_add(deliver_frame, new FrameJob{*sp, g_bytes_new(m.data, m.size), w, h});
                    sent = true;
                }
                gst_buffer_unmap(buf, &m);
            }
        }
        if (!sent) s.frame_pending = false;
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

// Removes "user:password@" from anything printed: debug texts of rtspsrc can contain the address.
static std::string sanitize(std::string t) {
    for (size_t p = t.find("://"); p != std::string::npos; p = t.find("://", p + 3)) {
        size_t at = t.find('@', p + 3), end = t.find_first_of(" /|", p + 3);
        if (at != std::string::npos && (end == std::string::npos || at < end)) t.replace(p + 3, at - (p + 3), "***");
    }
    return t;
}

static gboolean on_bus(GstBus*, GstMessage* msg, gpointer data) {
    auto* sp = (std::shared_ptr<LivePlayer::Session>*)data;
    LivePlayer::Session& s = **sp;
    if (!s.live) return TRUE;
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            g_idle_add(deliver_state, new StateJob{*sp, s.got_first ? LiveState::Dropped : LiveState::Ended, "The camera closed the stream."});
            break;
        case GST_MESSAGE_ERROR: {
            GError* err = nullptr;
            gchar* dbg = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            // An error inside the optional sound branch must not end the video: log it and carry on.
            gchar* path = msg->src ? gst_object_get_path_string(msg->src) : nullptr;
            bool audio_side = path && (strstr(path, "/adec") || strstr(path, "/asink") || strstr(path, "/vol"));
            g_free(path);
            std::string detail = sanitize(std::string(err ? err->message : "?") + " | " + (dbg ? dbg : ""));
            fprintf(stderr, "ICmon live: %s error: %s\n", audio_side ? "sound" : "stream", detail.c_str());
            if (audio_side) { g_clear_error(&err); g_free(dbg); break; }
            LiveState st = LiveState::Failed;
            std::string t;
            if (err && g_error_matches(err, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_NOT_AUTHORIZED)) {
                st = LiveState::Rejected;
                t = "The camera rejected the user name or password. Check them before trying again: repeated failures can lock the account.";
            } else {
                // A stream that had been playing and then broke is Dropped (the tile may reconnect a few times); one that
                // never played is Failed. The debug text may hold the address: not shown; the failing element's name is safe.
                st = s.got_first ? LiveState::Dropped : LiveState::Failed;
                t = std::string("Stream error: ") + (err ? err->message : "unknown");
                if (msg->src) t += std::string(" [") + GST_OBJECT_NAME(msg->src) + "]";
            }
            g_clear_error(&err);
            g_free(dbg);
            g_idle_add(deliver_state, new StateJob{*sp, st, t});
            break;
        }
        default: break;
    }
    return TRUE;
}

void LivePlayer::set_audio(bool on, double volume) {
    if (!s_ || !s_->pipe) return;
    GstElement* v = gst_bin_get_by_name(GST_BIN(s_->pipe), "vol");
    if (!v) return;  // no audio device
    g_object_set(v, "mute", on ? FALSE : TRUE, "volume", volume < 0 ? 0.0 : (volume > 1 ? 1.0 : volume), NULL);
    gst_object_unref(v);
}

namespace {
struct SnapJob {
    std::shared_ptr<LivePlayer::Session> s;
    std::function<void(bool, const std::string&)> done;
    bool ok;
    std::string text;
};
}  // namespace

static gboolean deliver_snap(gpointer p) {
    std::unique_ptr<SnapJob> j((SnapJob*)p);
    if (j->s->live && j->done) j->done(j->ok, j->text);
    return G_SOURCE_REMOVE;
}

// Takes the newest full-resolution frame, encodes it as PNG in a worker thread and writes it to `path`.
void LivePlayer::snapshot(const std::string& path, std::function<void(bool, const std::string&)> done) {
    if (!s_ || !s_->pipe) { if (done) done(false, "No picture yet."); return; }
    GstElement* sink = gst_bin_get_by_name(GST_BIN(s_->pipe), "ssink");
    GstSample* sample = sink ? gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 300 * GST_MSECOND) : nullptr;
    if (sink) gst_object_unref(sink);
    if (!sample) { if (done) done(false, "No picture yet."); return; }
    std::shared_ptr<Session> s = s_;
    std::thread([s, sample, path, done]() {
        bool ok = false;
        std::string text;
        GstCaps* png = gst_caps_from_string("image/png");
        GError* err = nullptr;
        GstSample* out = gst_video_convert_sample(sample, png, 10 * GST_SECOND, &err);
        gst_caps_unref(png);
        if (!out) {
            text = std::string("Could not encode the frame: ") + (err ? err->message : "?") + " (is gstreamer1.0-plugins-good installed?)";
        } else {
            GstBuffer* buf = gst_sample_get_buffer(out);
            GstMapInfo m;
            if (buf && gst_buffer_map(buf, &m, GST_MAP_READ)) {
                FILE* f = fopen(path.c_str(), "wb");
                if (f) {
                    ok = fwrite(m.data, 1, m.size, f) == m.size;
                    ok = (fclose(f) == 0) && ok;
                }
                if (!ok) text = "Could not write the file.";
                gst_buffer_unmap(buf, &m);
            } else {
                text = "The encoded frame is empty.";
            }
            gst_sample_unref(out);
        }
        g_clear_error(&err);
        gst_sample_unref(sample);
        g_idle_add(deliver_snap, new SnapJob{s, done, ok, text});
    }).detach();
}

void LivePlayer::start(const LiveParams& p) {
    stop();
    auto s = std::make_shared<Session>();
    s->owner = this;
    char desc[1536];
    // The address is set as a property below (not inside this text): it may contain characters that the
    // pipeline syntax would interpret. `application/x-rtp,media=video` picks the video stream; an audio stream,
    // if the camera offers one is linked below. The output height is fixed for the life of the stream (caps of a running
    // pipeline are not changed: that was a suspect for 'Internal data stream error').
    // Video: decoded frames go through a tee. Branch 1 is scaled (lanczos, 2 threads: bilinear showed stair-steps on
    // thin lines) to the size the tile shows and sent to the window. Branch 2 keeps the newest full-resolution frame
    // for "save frame". Sound: always built when an audio device opens, muted until the user switches it on, so
    // switching never restarts the stream. Errors from the sound branch are logged and ignored (see on_bus).
    static const bool audio_ok = [] {
        GstElement* e = gst_element_factory_make("autoaudiosink", nullptr);
        if (!e) return false;
        bool ok = gst_element_set_state(e, GST_STATE_READY) != GST_STATE_CHANGE_FAILURE;
        gst_element_set_state(e, GST_STATE_NULL);
        gst_object_unref(e);
        return ok;
    }();
    const char* audio = audio_ok
        ? " src. ! application/x-rtp,media=audio ! decodebin name=adec ! audioconvert ! audioresample ! "
          "volume name=vol mute=true volume=0.7 ! autoaudiosink name=asink sync=false"
        : "";
    snprintf(desc, sizeof desc,
             "rtspsrc name=src latency=200 protocols=tcp tcp-timeout=8000000 ! application/x-rtp,media=video ! decodebin ! "
             "tee name=t "
             "t. ! queue max-size-buffers=2 leaky=downstream ! videoscale method=lanczos n-threads=2 ! videoconvert ! "
             "capsfilter name=sizer caps=video/x-raw,format=RGBA,height=%d,pixel-aspect-ratio=1/1 ! "
             "appsink name=vsink sync=false max-buffers=1 drop=true "
             "t. ! queue max-size-buffers=1 leaky=downstream ! appsink name=ssink sync=false max-buffers=1 drop=true"
             "%s",
             p.height > 0 ? p.height : 1080, audio);
    GError* err = nullptr;
    s->pipe = gst_parse_launch(desc, &err);
    if (!s->pipe) {
        std::string m = std::string("Cannot build the live pipeline: ") + (err ? err->message : "?") +
                        " (are gstreamer1.0-libav and plugins-good/bad installed?)";
        g_clear_error(&err);
        if (on_state) on_state(LiveState::Failed, m);
        return;
    }
    g_clear_error(&err);
    s_ = s;

    GstElement* src = gst_bin_get_by_name(GST_BIN(s->pipe), "src");
    g_object_set(src, "location", p.url.c_str(), NULL);
    gst_object_unref(src);

    GstElement* vsink = gst_bin_get_by_name(GST_BIN(s->pipe), "vsink");
    auto* sp_sink = new std::shared_ptr<Session>(s);
    GstAppSinkCallbacks cbs{};
    cbs.new_sample = on_new_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(vsink), &cbs, sp_sink, [](gpointer d) { delete (std::shared_ptr<Session>*)d; });
    gst_object_unref(vsink);

    GstBus* bus = gst_element_get_bus(s->pipe);
    auto* sp_bus = new std::shared_ptr<Session>(s);
    s->bus_watch = gst_bus_add_watch_full(bus, G_PRIORITY_DEFAULT, on_bus, sp_bus, [](gpointer d) { delete (std::shared_ptr<Session>*)d; });
    gst_object_unref(bus);

    if (on_state) on_state(LiveState::Connecting, "");
    if (!s_) return;  // the callback stopped us
    gst_element_set_state(s->pipe, GST_STATE_PLAYING);
}

}  // namespace icmon
