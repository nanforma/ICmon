#include <malloc.h>
#include "player.h"

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

#include "av_timeline.h"

namespace icmon {

// One playback. Everything the callbacks touch lives here and is kept alive by shared_ptr; `live` is cleared
// by stop() so late callbacks (idle handlers already queued) do nothing.
struct Player::Session {
    Player* owner = nullptr;
    std::atomic<bool> live{true};
    std::atomic<bool> frame_pending{false};
    std::atomic<long long> first_i_time{-1};
    double rate = 1.0;
    xm::CancelToken cancel;
    GstElement* pipe = nullptr;
    GstAppSrc *vsrc = nullptr, *asrc = nullptr;
    guint bus_watch = 0;
    std::thread worker;
    bool paused = false;
};

Player::Player() { static bool once = [] { gst_init(nullptr, nullptr); return true; }(); (void)once; }
Player::~Player() { stop(); }

bool Player::active() const { return s_ && s_->live; }

bool Player::position(xm::CamTime& t) const {
    if (!s_ || !s_->live || !s_->pipe) return false;
    long long first = s_->first_i_time.load();
    gint64 pos = 0;
    if (first < 0 || !gst_element_query_position(s_->pipe, GST_FORMAT_TIME, &pos)) return false;
    t = first + (long long)(pos * s_->rate / GST_SECOND);
    return true;
}

void Player::set_paused(bool paused) {
    if (!s_ || !s_->live) return;
    s_->paused = paused;
    gst_element_set_state(s_->pipe, paused ? GST_STATE_PAUSED : GST_STATE_PLAYING);
}

void Player::stop() {
    if (!s_) return;
    std::shared_ptr<Session> s = std::move(s_);
    s->live = false;
    s->cancel.cancel();                       // ends a blocking download within ~200 ms
    if (s->bus_watch) g_source_remove(s->bus_watch);
    gst_element_set_state(s->pipe, GST_STATE_NULL);  // unblocks a worker waiting in appsrc push
    if (s->worker.joinable()) s->worker.join();
    gst_object_unref(s->vsrc);
    gst_object_unref(s->asrc);
    gst_object_unref(s->pipe);
    malloc_trim(0);
    s->pipe = nullptr;
}

namespace {

struct FrameJob {
    std::shared_ptr<Player::Session> s;
    GBytes* bytes;
    int w, h;
};
struct TextJob {
    std::shared_ptr<Player::Session> s;
    std::string text;
    bool finished;
};

}  // namespace

// Called on the main thread through g_idle_add.
static gboolean deliver_frame(gpointer p) {
    std::unique_ptr<FrameJob> j((FrameJob*)p);
    j->s->frame_pending = false;
    if (j->s->live && j->s->owner && j->s->owner->on_frame) {
        GdkTexture* tex = gdk_memory_texture_new(j->w, j->h, GDK_MEMORY_R8G8B8A8, j->bytes, (gsize)j->w * 4);
        j->s->owner->on_frame(tex);
    }
    g_bytes_unref(j->bytes);
    return G_SOURCE_REMOVE;
}

static gboolean deliver_text(gpointer p) {
    std::unique_ptr<TextJob> j((TextJob*)p);
    Player* o = j->s->owner;
    if (j->s->live && o) {
        if (!j->text.empty() && o->on_message) o->on_message(j->text);
        if (j->finished && o->on_finished) o->on_finished();
    }
    return G_SOURCE_REMOVE;
}

static void post_text(const std::shared_ptr<Player::Session>& s, const std::string& t, bool finished) {
    g_idle_add(deliver_text, new TextJob{s, t, finished});
}

// appsink callback; runs on a GStreamer streaming thread, at presentation time (sync=true).
static GstFlowReturn on_new_sample(GstAppSink* sink, gpointer data) {
    auto* sp = (std::shared_ptr<Player::Session>*)data;
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;
    Player::Session& s = **sp;
    if (s.live && !s.frame_pending.exchange(true)) {  // keep at most one frame in flight; drop the rest
        GstCaps* caps = gst_sample_get_caps(sample);
        GstBuffer* buf = gst_sample_get_buffer(sample);
        int w = 0, h = 0;
        if (caps && buf && gst_structure_get_int(gst_caps_get_structure(caps, 0), "width", &w) &&
            gst_structure_get_int(gst_caps_get_structure(caps, 0), "height", &h) && w > 0 && h > 0) {
            GstMapInfo m;
            if (gst_buffer_map(buf, &m, GST_MAP_READ)) {
                if (m.size == (gsize)w * h * 4) {  // tightly packed RGBA; anything else is skipped, not guessed
                    g_idle_add(deliver_frame, new FrameJob{*sp, g_bytes_new(m.data, m.size), w, h});
                } else {
                    s.frame_pending = false;
                }
                gst_buffer_unmap(buf, &m);
            } else {
                s.frame_pending = false;
            }
        } else {
            s.frame_pending = false;
        }
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

static gboolean on_bus(GstBus*, GstMessage* msg, gpointer data) {
    auto* sp = (std::shared_ptr<Player::Session>*)data;
    Player::Session& s = **sp;
    if (!s.live) return TRUE;
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            post_text(*sp, "", true);
            break;
        case GST_MESSAGE_ERROR: {
            GError* err = nullptr;
            gchar* dbg = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            std::string t = std::string("Playback error: ") + (err ? err->message : "unknown");
            g_clear_error(&err);
            g_free(dbg);
            s.cancel.cancel();
            post_text(*sp, t, true);
            break;
        }
        default: break;
    }
    return TRUE;
}

void Player::apply_audio() {
    if (!s_ || !s_->pipe) return;
    GstElement* v = gst_bin_get_by_name(GST_BIN(s_->pipe), "vol");
    if (!v) return;
    g_object_set(v, "mute", (audio_on_ && rate_ == 1.0) ? FALSE : TRUE, "volume", volume_, NULL);
    gst_object_unref(v);
}

void Player::set_audio(bool on, double volume) {
    audio_on_ = on;
    volume_ = volume < 0 ? 0 : (volume > 1 ? 1 : volume);
    apply_audio();
}

void Player::start(const PlayParams& p) {
    stop();
    auto s = std::make_shared<Session>();
    s->owner = this;
    s->rate = (p.rate == 0.25 || p.rate == 0.5 || p.rate == 2.0 || p.rate == 4.0) ? p.rate : 1.0;
    rate_ = s->rate;
    GError* err = nullptr;
    s->pipe = gst_parse_launch(
        "appsrc name=vsrc is-live=false format=time block=true max-bytes=8000000 "
        // Pictures taller than 1296 px are scaled down to it: full 2304x2592 RGBA frames (24 MB each) made the window stutter.
        "caps=video/x-h265,stream-format=byte-stream,alignment=au ! h265parse ! decodebin ! videoconvert ! "
        "videoscale ! video/x-raw,format=RGBA,height=[1,1296],pixel-aspect-ratio=1/1 ! appsink name=vsink sync=true max-buffers=2 drop=true "
        "appsrc name=asrc is-live=false format=time block=true max-bytes=1000000 "
        "caps=audio/x-alaw,rate=8000,channels=1 ! alawdec ! audioconvert ! audioresample ! volume name=vol mute=true ! autoaudiosink name=asink",
        &err);
    if (!s->pipe) {
        std::string m = std::string("Cannot build the playback pipeline: ") + (err ? err->message : "?") +
                        " (are gstreamer1.0-libav and plugins-good/bad installed?)";
        g_clear_error(&err);
        s_ = s;  // so that stop()/post work uniformly
        s->vsrc = s->asrc = nullptr;
        if (on_message) on_message(m);
        s_.reset();
        if (on_finished) on_finished();
        return;
    }
    g_clear_error(&err);
    s_ = s;
    apply_audio();
    s->vsrc = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(s->pipe), "vsrc"));
    s->asrc = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(s->pipe), "asrc"));
    GstElement* vsink = gst_bin_get_by_name(GST_BIN(s->pipe), "vsink");
    // The callbacks get a heap-allocated shared_ptr copy that lives as long as the pipeline (freed on destroy notify).
    auto* sp_sink = new std::shared_ptr<Session>(s);
    GstAppSinkCallbacks cbs{};
    cbs.new_sample = on_new_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(vsink), &cbs, sp_sink, [](gpointer d) { delete (std::shared_ptr<Session>*)d; });
    gst_object_unref(vsink);

    // A small buffer in the sound device: a change of volume is heard after the buffer, not after a second.
    if (GstElement* asink = gst_bin_get_by_name(GST_BIN(s->pipe), "asink")) {
        g_signal_connect(asink, "child-added", G_CALLBACK(+[](GstChildProxy*, GObject* child, const gchar*, gpointer) {
            GObjectClass* k = G_OBJECT_GET_CLASS(child);
            if (g_object_class_find_property(k, "buffer-time")) g_object_set(child, "buffer-time", (gint64)80000, NULL);
            if (g_object_class_find_property(k, "latency-time")) g_object_set(child, "latency-time", (gint64)20000, NULL);
        }), nullptr);
        gst_object_unref(asink);
    }
    GstBus* bus = gst_element_get_bus(s->pipe);
    auto* sp_bus = new std::shared_ptr<Session>(s);
    s->bus_watch = gst_bus_add_watch_full(bus, G_PRIORITY_DEFAULT, on_bus, sp_bus,
                                          [](gpointer d) { delete (std::shared_ptr<Session>*)d; });
    gst_object_unref(bus);

    PlayParams params = p;
    Session* raw = s.get();
    s->worker = std::thread([s, params, raw]() mutable {
        xm::AvTimeline tl;
        xm::Connector connector = [&]() {
            auto d = std::make_unique<xm::Dvrip>();
            d->connect(params.host, (uint16_t)params.port, 4000, &raw->cancel);
            d->login(params.user, params.password);
            return d;
        };
        std::string note;
        try {
            auto res = xm::fetch_range(connector, params.files, params.start, params.end, params.lead_secs,
                                       [&](const xm::Frame& f, size_t) {
                                           xm::Stamped st;
                                           if (!tl.stamp(f, st)) return;
                                           if (st.video && f.kind == xm::Kind::IFrame) {
                                               long long none = -1;
                                               raw->first_i_time.compare_exchange_strong(none, xm::packed_to_cam_time(f.time_raw));
                                           }
                                           GstAppSrc* src = st.video ? raw->vsrc : raw->asrc;
                                           GstBuffer* b = gst_buffer_new_allocate(nullptr, f.payload_len, nullptr);
                                           gst_buffer_fill(b, 0, f.payload, f.payload_len);
                                           // faster playback = the same data on a shorter time line
                                           GST_BUFFER_PTS(b) = (guint64)(st.pts_ns / raw->rate);
                                           GST_BUFFER_DTS(b) = (guint64)(st.pts_ns / raw->rate);
                                           GST_BUFFER_DURATION(b) = (guint64)(st.dur_ns / raw->rate);
                                           if (gst_app_src_push_buffer(src, b) != GST_FLOW_OK) raw->cancel.cancel();
                                       });
            for (const auto& n : res.notes) note += (note.empty() ? "" : "; ") + n;
            if (res.resyncs) note += (note.empty() ? "" : "; ") + std::string("stream irregularities: ") + std::to_string(res.resyncs);
        } catch (const xm::Error& e) {
            if (e.code() != xm::Errc::Cancelled) note = std::string("Download error: ") + e.what();
        }
        std::fill(params.password.begin(), params.password.end(), '\0');
        if (!note.empty()) post_text(s, note, false);
        gst_app_src_end_of_stream(raw->vsrc);  // the bus reports EOS when everything has been played
        gst_app_src_end_of_stream(raw->asrc);
    });

    gst_element_set_state(s->pipe, GST_STATE_PLAYING);
}

}  // namespace icmon
