#include "downloader.h"

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <glib/gstdio.h>

#include <cstdio>

#include "av_timeline.h"

namespace icmon {

struct Downloader::State {
    Downloader* owner = nullptr;
    std::atomic<bool> live{true};     // false once the owner no longer wants callbacks
    std::atomic<bool> running{true};
    xm::CancelToken cancel;
    std::atomic<GstElement*> pipe{nullptr};
};

namespace {
struct Job {
    std::shared_ptr<Downloader::State> s;
    bool done = false, ok = false;
    long long bytes = 0;
    int secs = 0;
    std::string text;
};
}  // namespace

static gboolean deliver(gpointer p) {
    std::unique_ptr<Job> j((Job*)p);
    Downloader* d = j->s->owner;
    if (!j->s->live || !d) return G_SOURCE_REMOVE;
    if (j->done) {
        j->s->running = false;
        if (d->on_done) d->on_done(j->ok, j->text);
    } else if (d->on_progress) {
        d->on_progress(j->bytes, j->secs);
    }
    return G_SOURCE_REMOVE;
}

Downloader::Downloader() { static bool once = [] { gst_init(nullptr, nullptr); return true; }(); (void)once; }

Downloader::~Downloader() {
    if (s_) { s_->live = false; s_->cancel.cancel(); }
    cancel();
    join();
}

bool Downloader::active() const { return s_ && s_->running; }

void Downloader::join() {
    if (worker_.joinable()) worker_.join();
}

void Downloader::cancel() {
    if (!s_ || !s_->running) return;
    s_->cancel.cancel();
    // The worker notices the token, stops reading, and closes the file with what has arrived so far.
}

bool Downloader::start(const DownloadParams& params) {
    if (active()) return false;
    join();
    auto s = std::make_shared<State>();
    s->owner = this;
    s_ = s;
    DownloadParams p = params;
    worker_ = std::thread([s, p]() mutable {
        auto post_done = [&](bool ok, const std::string& text) {
            g_idle_add(deliver, new Job{s, true, ok, 0, 0, text});
        };
        const std::string part = p.path + ".part";
        GError* err = nullptr;
        GstElement* pipe = gst_parse_launch(
            // One thread pushes both streams, so a push must never block: the muxer waits for the other stream's
            // timestamps, and a blocking video push (the old 8 MB limit) then stalls the audio push behind it.
            // Unlimited appsrc queues and unlimited queue elements keep the data flowing (one file is small).
            "appsrc name=vsrc is-live=false format=time block=false max-bytes=0 "
            "caps=video/x-h265,stream-format=byte-stream,alignment=au ! h265parse ! "
            "queue max-size-buffers=0 max-size-bytes=0 max-size-time=0 ! mux. "
            // The audio caps are given by a capsfilter: with the appsrc "caps" property the muxer refused them
            // ("not-negotiated", reproduced here with synthetic data; the capsfilter form gives a valid file).
            "appsrc name=asrc is-live=false format=time block=false max-bytes=0 ! "
            "audio/x-alaw,rate=8000,channels=1 ! "
            "queue max-size-buffers=0 max-size-bytes=0 max-size-time=0 ! mux. "
            "matroskamux name=mux ! filesink name=out",
            &err);
        if (!pipe) {
            std::string m = std::string("Cannot build the save pipeline: ") + (err ? err->message : "?") +
                            " (is gstreamer1.0-plugins-good / -bad installed?)";
            g_clear_error(&err);
            post_done(false, m);
            return;
        }
        g_clear_error(&err);
        GstElement* out = gst_bin_get_by_name(GST_BIN(pipe), "out");
        g_object_set(out, "location", part.c_str(), NULL);
        gst_object_unref(out);
        GstAppSrc* vsrc = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(pipe), "vsrc"));
        GstAppSrc* asrc = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(pipe), "asrc"));
        s->pipe = pipe;
        gst_element_set_state(pipe, GST_STATE_PLAYING);

        xm::AvTimeline tl;
        long long bytes = 0;
        int last_secs = 0;
        gint64 last_post = g_get_monotonic_time();
        bool frames_seen = false;
        std::string fail;
        xm::Connector connector = [&]() {
            auto d = std::make_unique<xm::Dvrip>();
            d->connect(p.host, (uint16_t)p.port, 4000, &s->cancel);
            d->login(p.user, p.password);
            return d;
        };
        try {
            std::vector<xm::RecordingFile> one{p.file};
            xm::fetch_range(connector, one, p.file.begin, p.file.end, 0, [&](const xm::Frame& f, size_t) {
                bytes += (long long)f.payload_len;
                xm::Stamped st;
                if (!tl.stamp(f, st)) return;
                frames_seen = frames_seen || st.video;
                GstAppSrc* src = st.video ? vsrc : asrc;
                GstBuffer* b = gst_buffer_new_allocate(nullptr, f.payload_len, nullptr);
                gst_buffer_fill(b, 0, f.payload, f.payload_len);
                GST_BUFFER_PTS(b) = st.pts_ns;
                GST_BUFFER_DTS(b) = st.pts_ns;
                GST_BUFFER_DURATION(b) = st.dur_ns;
                if (gst_app_src_push_buffer(src, b) != GST_FLOW_OK) s->cancel.cancel();
                if (st.video) last_secs = (int)(st.pts_ns / 1000000000ULL);
                gint64 now = g_get_monotonic_time();
                if (now - last_post > 500000) {
                    last_post = now;
                    g_idle_add(deliver, new Job{s, false, false, bytes, last_secs, ""});
                }
            });
        } catch (const xm::Error& e) {
            if (e.code() != xm::Errc::Cancelled) fail = std::string("Download error: ") + e.what();
        }
        std::fill(p.password.begin(), p.password.end(), '\0');

        bool discarded = !s->live;                     // the window is closing: nothing to keep
        bool cancelled = s->cancel.cancelled();        // the user pressed stop: keep what has been received
        if (!discarded && fail.empty() && !frames_seen) fail = "The camera sent no decodable video for this recording.";
        if (!discarded && fail.empty()) {
            gst_app_src_end_of_stream(vsrc);
            gst_app_src_end_of_stream(asrc);
            GstBus* bus = gst_element_get_bus(pipe);
            GstMessage* m = gst_bus_timed_pop_filtered(bus, 60 * GST_SECOND, (GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
            if (!m) fail = "Timed out while finishing the file.";
            else if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR) {
                GError* ge = nullptr;
                gchar* dbg = nullptr;
                gst_message_parse_error(m, &ge, &dbg);
                fail = std::string("Could not write the file: ") + (ge ? ge->message : "?");
                fprintf(stderr, "ICmon download: %s | %s\n", ge ? ge->message : "?", dbg ? dbg : "");
                g_free(dbg);
                g_clear_error(&ge);
            }
            if (m) gst_message_unref(m);
            gst_object_unref(bus);
        }
        s->pipe = nullptr;
        gst_element_set_state(pipe, GST_STATE_NULL);
        gst_object_unref(vsrc);
        gst_object_unref(asrc);
        gst_object_unref(pipe);

        if (discarded || !fail.empty()) {
            g_remove(part.c_str());
            post_done(false, discarded ? "Download cancelled." : fail);
        } else if (cancelled) {  // stopped by the user: the part received so far is kept, marked in its name
            std::string kept = p.path.substr(0, p.path.size() - 4) + "-partial.mkv";
            if (g_rename(part.c_str(), kept.c_str()) != 0) post_done(false, "Download cancelled.");
            else post_done(true, kept + "  (" + "stopped early, only the part received is saved)");
        } else {
            // The camera's own length of the recording against what arrived: a clearly shorter file is not called finished.
            int expected = (int)(p.file.end - p.file.begin);
            bool short_file = expected > 20 && last_secs < expected * 9 / 10;
            std::string target = p.path;
            if (short_file) target = p.path.substr(0, p.path.size() - 4) + "-incomplete.mkv";
            if (g_rename(part.c_str(), target.c_str()) != 0) post_done(false, "Could not rename the finished file to " + target);
            else if (short_file) {
                fprintf(stderr, "ICmon download: received %d s of %d s\n", last_secs, expected);
                post_done(true, target + "  (incomplete: the camera sent " + std::to_string(last_secs) + " s of " + std::to_string(expected) + " s; try again)");
            } else post_done(true, p.path);
        }
    });
    return true;
}

}  // namespace icmon
