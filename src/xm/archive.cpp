#include "archive.h"

#include <algorithm>
#include <cstdlib>
#include <set>

namespace xm {

namespace {

constexpr size_t kPageSize = 64;  // observed: the camera never returns more than 64 entries per reply

// A command reply starts with '{', is printable ASCII and ends with '}'. Binary data packets can also
// start with '{', so this is only ever applied before the first data byte has arrived.
bool looks_like_json(const std::string& p) {
    if (p.empty() || p[0] != '{') return false;
    size_t n = p.size();
    while (n > 0 && (p[n - 1] == '\0' || p[n - 1] == '\n' || p[n - 1] == ' ')) n--;
    if (n == 0 || p[n - 1] != '}') return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)p[i];
        if ((ch < 0x20 && ch != '\n' && ch != '\r' && ch != '\t') || ch >= 0x7f) return false;
    }
    return true;
}

Json playback_body(const char* action, const RecordingFile& f, CamTime start, CamTime end) {
    Json param = Json::object();
    param.set("FileName", f.name).set("PlayMode", "ByName").set("StreamType", 0).set("TransMode", "TCP").set("Value", 0);
    Json pb = Json::object();
    pb.set("Action", action).set("StartTime", format_cam_time(start)).set("EndTime", format_cam_time(end)).set("Parameter", param);
    Json body = Json::object();
    body.set("Name", "OPPlayBack").set("OPPlayBack", pb);
    return body;
}

}  // namespace

std::vector<RecordingFile> list_files(Dvrip& c, CamTime begin, CamTime end, const CancelToken* cancel) {
    std::vector<RecordingFile> out;
    std::set<std::string> seen;
    CamTime from = begin;
    for (int page = 0; page < 10000; page++) {  // hard stop: a misbehaving camera must not loop us forever
        if (cancel && cancel->cancelled()) throw Error(Errc::Cancelled, "cancelled");
        Json q = Json::object();
        q.set("BeginTime", format_cam_time(from)).set("Channel", 0).set("DriverTypeMask", "0x0000FFFF")
            .set("EndTime", format_cam_time(end)).set("Event", "*").set("StreamType", "0x00000000").set("Type", "h264");
        Json body = Json::object();
        body.set("Name", "OPFileQuery").set("OPFileQuery", q);
        Json rep = c.request(1440, body);
        const Json& arr = rep["OPFileQuery"];
        size_t count = arr.is_array() ? arr.items().size() : 0;
        CamTime last_begin = from;
        size_t added = 0;
        for (const Json& e : arr.items()) {
            RecordingFile f;
            f.name = e["FileName"].str();
            if (f.name.empty()) continue;
            f.begin = parse_cam_time(e["BeginTime"].str());
            f.end = parse_cam_time(e["EndTime"].str());
            f.length_raw = (uint64_t)e["FileLength"].integer(0);
            f.disk = (int)e["DiskNo"].integer(0);
            f.serial = (int)e["SerialNo"].integer(0);
            last_begin = std::max(last_begin, f.begin);
            if (seen.insert(f.name).second) { out.push_back(f); added++; }
        }
        if (count < kPageSize) break;  // short page: that was the last one
        // Full page: continue from the last BeginTime. If nothing new came back, step one second forward
        // so the same page is not requested again.
        from = (added == 0 || last_begin <= from) ? from + 1 : last_begin;
        if (from > end) break;
    }
    std::sort(out.begin(), out.end(), [](const RecordingFile& a, const RecordingFile& b) {
        return a.begin != b.begin ? a.begin < b.begin : a.name < b.name;
    });
    return out;
}

bool has_files(Dvrip& c, CamTime begin, CamTime end, const CancelToken* cancel) {
    if (cancel && cancel->cancelled()) throw Error(Errc::Cancelled, "cancelled");
    Json q = Json::object();
    q.set("BeginTime", format_cam_time(begin)).set("Channel", 0).set("DriverTypeMask", "0x0000FFFF")
        .set("EndTime", format_cam_time(end)).set("Event", "*").set("StreamType", "0x00000000").set("Type", "h264");
    Json body = Json::object();
    body.set("Name", "OPFileQuery").set("OPFileQuery", q);
    Json rep = c.request(1440, body);
    const Json& arr = rep["OPFileQuery"];
    if (!arr.is_array()) return false;
    for (const Json& e : arr.items())
        if (!e["FileName"].str().empty()) return true;
    return false;
}

const RecordingFile* find_file(const std::vector<RecordingFile>& files, CamTime t) {
    for (const auto& f : files)
        if (f.begin <= t && t < f.end) return &f;
    return nullptr;
}

DownloadStats download_window(Dvrip& c, const RecordingFile& f, CamTime start, CamTime end, const DataSink& sink,
                              int idle_timeout_ms) {
    DownloadStats st;
    c.send(1424, playback_body("Claim", f, start, end));
    Json claim = Dvrip::parse_reply(c.recv_packet(idle_timeout_ms));
    if (claim.contains("Ret") && claim["Ret"].integer(-1) != 100) {
        int ret = (int)claim["Ret"].integer(-1);
        throw Error(Errc::Camera, "download refused: Ret=" + std::to_string(ret) + " (" + describe_ret(ret) + ")", ret);
    }
    c.send(1420, playback_body("DownloadStart", f, start, end));
    bool got_data = false;
    for (;;) {
        Packet p = c.recv_packet(idle_timeout_ms);
        st.packets++;
        if (p.payload.empty()) break;  // an empty data packet ends the download
        if (!got_data && looks_like_json(p.payload)) {  // control reply before the data starts
            Json j = Dvrip::parse_reply(p);
            if (j.contains("Ret") && j["Ret"].integer(-1) != 100) {
                int ret = (int)j["Ret"].integer(-1);
                throw Error(Errc::Camera, "download failed: Ret=" + std::to_string(ret) + " (" + describe_ret(ret) + ")", ret);
            }
            continue;
        }
        got_data = true;
        st.bytes += p.payload.size();
        if (!sink((const uint8_t*)p.payload.data(), p.payload.size())) { st.stopped_by_sink = true; break; }
    }
    try { c.send(1420, playback_body("DownloadStop", f, start, end)); } catch (const Error&) {}  // best effort
    return st;
}

FetchResult fetch_frames(Dvrip& c, const RecordingFile& f, CamTime start, CamTime end,
                         const std::function<void(const Frame&)>& on_frame, int idle_timeout_ms) {
    StreamParser parser(on_frame);
    FetchResult r;
    r.download = download_window(c, f, start, end,
                                 [&](const uint8_t* d, size_t n) { parser.feed(d, n); return true; }, idle_timeout_ms);
    r.demux = parser.finish();
    return r;
}

RangeResult fetch_range(const Connector& connect, const std::vector<RecordingFile>& files, CamTime start, CamTime end,
                        int lead_secs, const std::function<void(const Frame&, size_t file_index)>& on_frame,
                        int idle_timeout_ms) {
    std::vector<const RecordingFile*> sel;
    for (const auto& f : files)
        if (f.end > start && f.begin < end) sel.push_back(&f);
    std::sort(sel.begin(), sel.end(), [](const RecordingFile* a, const RecordingFile* b) { return a->begin < b->begin; });
    RangeResult r;
    for (size_t i = 0; i < sel.size(); i++) {
        const RecordingFile& f = *sel[i];
        if (i > 0 && f.begin != sel[i - 1]->end)
            r.notes.push_back("gap in the archive between " + format_cam_time(sel[i - 1]->end) + " and " + format_cam_time(f.begin));
        CamTime ws = (i == 0) ? std::max(f.begin, start - lead_secs) : f.begin;
        CamTime we = std::min(f.end, end);
        std::unique_ptr<Dvrip> c = connect();
        FetchResult fr = fetch_frames(*c, f, ws, we, [&](const Frame& fm) { on_frame(fm, i); }, idle_timeout_ms);
        r.files_used++;
        r.download.bytes += fr.download.bytes;
        r.download.packets += fr.download.packets;
        r.i_frames += fr.demux.i_frames; r.p_frames += fr.demux.p_frames; r.audio_frames += fr.demux.audio_frames;
        r.resyncs += fr.demux.resyncs; r.skipped_bytes += fr.demux.skipped_bytes;
    }
    if (sel.empty()) r.notes.push_back("no recording overlaps the requested time range");
    else if (sel.front()->begin > start) r.notes.push_back("the archive starts only at " + format_cam_time(sel.front()->begin));
    if (!sel.empty() && sel.back()->end < end) r.notes.push_back("the archive ends at " + format_cam_time(sel.back()->end) + ", before the requested end");
    return r;
}

}  // namespace xm
