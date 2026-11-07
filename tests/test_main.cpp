// Automated tests for the protocol layer. Everything camera-like here is SYNTHETIC (tests/mock_camera.h):
// these tests show that the code follows the documented/observed behaviour, not that a real camera does.
// Tests that need real hardware are listed in docs (README, "Tests that need the real camera").
#include <chrono>
#include <clocale>
#include <cstdio>
#include <functional>
#include <random>
#include <thread>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "archive.h"
#include "camera_store.h"
#include "discovery.h"
#include "camctl.h"
#include "rtsp_url.h"
#include "av_timeline.h"
#include "timeline_geometry.h"
#include "mock_camera.h"
#include "sofia_hash.h"
#include "stream_builder.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_fail++; std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static bool throws(xm::Errc code, const std::function<void()>& fn) {
    try { fn(); } catch (const xm::Error& e) { return e.code() == code; } catch (...) { return false; }
    return false;
}

static void test_hash() {
    std::printf("hash\n");
    CHECK(xm::md5_hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(xm::md5_hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(xm::md5_hex(std::string(1000, 'a')) == "cabe45dcc9ae5b66ba86600cca6b8ba8");
    CHECK(xm::sofia_hash("") == "tlJwpbo6");
    CHECK(xm::sofia_hash("secret").size() == 8);
}

static void test_json() {
    std::printf("json\n");
    xm::Json j = xm::Json::parse(R"({"Ret":100,"SessionID":"0x0000001A","a":[1,2,{"b":null}],"s":"x\"y\né","t":true})");
    CHECK(j["Ret"].integer() == 100);
    CHECK(j["SessionID"].integer() == 0x1A);
    CHECK(j["a"].items().size() == 3 && j["a"].items()[2]["b"].is_null());
    CHECK(j["s"].str() == "x\"y\n\xC3\xA9");
    CHECK(j["t"].boolean() && !j["missing"].boolean() && j["missing"].is_null());
    CHECK(xm::Json::parse(j.dump()).dump() == j.dump());
    CHECK(throws(xm::Errc::Protocol, [] { xm::Json::parse("{\"a\":1,}"); }));
    CHECK(throws(xm::Errc::Protocol, [] { xm::Json::parse("{\"a\":1} x"); }));
    try { xm::Json::parse("{\"a\":[1 2]}"); CHECK(false); }
    catch (const xm::Error& e) { CHECK(std::string(e.what()).find("near: [{\"a\":[1 2]}]") != std::string::npos); }
    CHECK(throws(xm::Errc::Protocol, [] { xm::Json::parse("{\"a\":\"unterminated"); }));
}

// Regression: the GUI calls setlocale(LC_ALL, ""); in ru/uk locales the decimal separator is a comma, and a
// locale-dependent strtod() swallowed the comma of  {"AliveInterval" : 30, "ChannelNum" : 1}.
static void test_json_locale() {
    std::printf("json under a comma-decimal locale\n");
    const char* names[] = {"ru_RU.UTF-8", "uk_UA.UTF-8", "de_DE.UTF-8", "ru_RU.utf8", "uk_UA.utf8", "de_DE.utf8"};
    const char* got = nullptr;
    for (const char* n : names) if (setlocale(LC_ALL, n)) { got = n; break; }
    if (!got || std::string(localeconv()->decimal_point) != ",") {
        std::printf("  skipped: no comma-decimal locale installed on this machine (set LOCPATH or install ru_RU.UTF-8)\n");
        setlocale(LC_ALL, "C");
        return;
    }
    std::printf("  using locale %s\n", got);
    xm::Json j = xm::Json::parse(R"({"AliveInterval" : 30, "ChannelNum" : 1, "x" : 1.5, "y" : -2.25e1, "z" : 0})");
    CHECK(j["AliveInterval"].integer() == 30 && j["ChannelNum"].integer() == 1 && j["z"].integer() == 0);
    CHECK(j["x"].integer() == 1 && j.dump().find("1.5") != std::string::npos && j.dump().find(',') != std::string::npos);
    CHECK(xm::Json::parse(j.dump()).dump() == j.dump());
    CHECK(xm::Json::parse(xm::Json(0.125).dump()).dump() == "0.125");
    setlocale(LC_ALL, "C");
}

static void test_time() {
    std::printf("camtime\n");
    xm::CamTime t = xm::parse_cam_time("2026-10-31 00:52:32");
    CHECK(xm::format_cam_time(t) == "2026-10-31 00:52:32");
    CHECK(xm::parse_cam_time("2026-10-31 01:00:00") - t == 3600 - 52 * 60 - 32);
    CHECK(xm::parse_cam_time("2026-11-01 00:00:00") - xm::parse_cam_time("2026-10-31 00:00:00") == 86400);  // no DST jump
    CHECK(xm::packed_to_cam_time(xm::encode_time(xm::from_cam_time(t))) == t);
    xm::Time d = xm::decode_time(0x6abc0c80);  // value checked on a real I-frame
    CHECK(d.year == 2026 && d.month == 10 && d.day == 30 && d.hour == 0 && d.minute == 50 && d.second == 0);
    CHECK(throws(xm::Errc::Protocol, [] { xm::parse_cam_time("2026-10-31"); }));
    CHECK(throws(xm::Errc::Protocol, [] { xm::parse_cam_time("2026-13-31 00:00:00"); }));
}

struct Seen { xm::Kind k; size_t total, plen; uint32_t t; bool after_i; };
static std::vector<Seen> collect_oneshot(const std::string& s, xm::Stats* st) {
    std::vector<Seen> v;
    *st = xm::parse((const uint8_t*)s.data(), s.size(), [&](const xm::Frame& f) { v.push_back({f.kind, f.total, f.payload_len, f.time_raw, f.after_i}); });
    return v;
}
static bool same(const std::vector<Seen>& a, const std::vector<Seen>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (a[i].k != b[i].k || a[i].total != b[i].total || a[i].plen != b[i].plen || a[i].t != b[i].t || a[i].after_i != b[i].after_i) return false;
    return true;
}

static void test_stream_parser() {
    std::printf("stream parser\n");
    using namespace xmtest;
    xm::CamTime t0 = xm::parse_cam_time("2026-10-31 00:52:30");
    std::string s = std::string(77, 'z') + P(300) + A(320) + gop(t0) + gop(t0 + 3) + gop(t0 + 6, nullptr, 500);
    xm::Stats ref;
    auto want = collect_oneshot(s, &ref);
    CHECK(ref.leading_bytes == 77 && ref.i_frames == 3 && ref.p_frames == 1 + 105 && ref.resyncs == 0);

    for (size_t chunk : {size_t(1), size_t(3), size_t(7), size_t(1000), size_t(32768)}) {
        std::vector<Seen> got;
        xm::StreamParser sp([&](const xm::Frame& f) { got.push_back({f.kind, f.total, f.payload_len, f.time_raw, f.after_i}); });
        for (size_t o = 0; o < s.size(); o += chunk) sp.feed((const uint8_t*)s.data() + o, std::min(chunk, s.size() - o));
        auto st = sp.finish();
        CHECK(same(got, want));
        CHECK(st.leading_bytes == 77 && st.resyncs == 0 && st.skipped_bytes == 0 && !st.truncated_tail);
    }
    std::mt19937 rng(7);  // random chunking
    for (int round = 0; round < 20; round++) {
        std::vector<Seen> got;
        xm::StreamParser sp([&](const xm::Frame& f) { got.push_back({f.kind, f.total, f.payload_len, f.time_raw, f.after_i}); });
        for (size_t o = 0; o < s.size();) { size_t n = std::min<size_t>(1 + rng() % 5000, s.size() - o); sp.feed((const uint8_t*)s.data() + o, n); o += n; }
        sp.finish();
        CHECK(same(got, want));
    }
    // cut stream: last frame incomplete is reported, not delivered
    std::string cut = s.substr(0, s.size() - 100);
    xm::StreamParser sp2([](const xm::Frame&) {});
    sp2.feed((const uint8_t*)cut.data(), cut.size());
    auto st2 = sp2.finish();
    CHECK(st2.truncated_tail && st2.tail_bytes > 0);
    // junk inside: one resync
    std::string junk = F9() + I(xm::encode_time(xm::from_cam_time(t0)), 500) + std::string(10, 'J') + P(100) + A(320);
    xm::StreamParser sp3([](const xm::Frame&) {});
    sp3.feed((const uint8_t*)junk.data(), junk.size());
    auto st3 = sp3.finish();
    CHECK(st3.resyncs == 1 && st3.skipped_bytes == 10 && st3.p_frames == 1 && st3.audio_frames == 1);
}

static xmtest::MockCamera::Opts day_files(int n) {
    xmtest::MockCamera::Opts o;
    xm::CamTime day = xm::parse_cam_time("2026-10-31 00:00:00");
    for (int i = 0; i < n; i++) {
        xm::CamTime b = day + i * 300;
        o.files.push_back({"/idea0/2026-10-31/001/" + xm::format_cam_clock(b) + "[R][@1][0].h264", b, b + 300});
    }
    return o;
}

static void test_login() {
    std::printf("login\n");
    xmtest::MockCamera cam(day_files(2));
    { xm::Dvrip c; c.connect("127.0.0.1", cam.port(), 2000); c.login("shield", "secret"); CHECK(c.connected()); }
    { xm::Dvrip c; c.connect("127.0.0.1", cam.port(), 2000);
      try { c.login("shield", "nope"); CHECK(false); } catch (const xm::Error& e) { CHECK(e.code() == xm::Errc::Auth && e.ret() == 203); } }
    { xm::Dvrip c; c.connect("127.0.0.1", cam.port(), 2000);
      try { c.login("admin", "secret"); CHECK(false); } catch (const xm::Error& e) { CHECK(e.ret() == 204); } }
    { xm::Dvrip c; c.connect("127.0.0.1", cam.port(), 2000);   // third failure; now locked
      try { c.login("shield", "nope"); CHECK(false); } catch (const xm::Error& e) { CHECK(e.ret() == 203); } }
    { xm::Dvrip c; c.connect("127.0.0.1", cam.port(), 2000);   // even the right password is refused
      try { c.login("shield", "secret"); CHECK(false); } catch (const xm::Error& e) { CHECK(e.ret() == 205); } }
    xm::Dvrip refused;
    CHECK(throws(xm::Errc::Connect, [&] { refused.connect("127.0.0.1", 1, 500); }));
}

static void test_listing() {
    std::printf("listing\n");
    xmtest::MockCamera cam(day_files(150));
    xm::Dvrip c;
    c.connect("127.0.0.1", cam.port(), 2000);
    c.login("shield", "secret");
    xm::CamTime b = xm::parse_cam_time("2026-10-31 00:00:00"), e = xm::parse_cam_time("2026-10-31 23:59:59");
    auto files = xm::list_files(c, b, e);
    CHECK(files.size() == 150);
    bool sorted = true;
    for (size_t i = 1; i < files.size(); i++) if (files[i].begin <= files[i - 1].begin) sorted = false;
    CHECK(sorted);
    CHECK(xm::list_files(c, b + 300 * 10 + 1, b + 300 * 10 + 2).size() == 1);
    CHECK(xm::list_files(c, b + 86400 * 2, b + 86400 * 2 + 10).empty());
    CHECK(xm::has_files(c, b, e));
    CHECK(!xm::has_files(c, b + 86400 * 2, b + 86400 * 2 + 10));
    CHECK(xm::find_file(files, b + 301) && xm::find_file(files, b + 301)->begin == b + 300);
    CHECK(!xm::find_file(files, b + 300 * 150));
    xm::CancelToken tok; tok.cancel();
    CHECK(throws(xm::Errc::Cancelled, [&] { xm::list_files(c, b, e, &tok); }));
}

static void test_fetch(bool brace) {
    std::printf(brace ? "fetch (data packets that start with '{')\n" : "fetch\n");
    auto o = day_files(10);
    o.brace_fill = brace;
    xmtest::MockCamera cam(o);
    xm::Dvrip c;
    c.connect("127.0.0.1", cam.port(), 2000);
    c.login("shield", "secret");
    xm::CamTime b = xm::parse_cam_time("2026-10-31 00:00:00");
    auto files = xm::list_files(c, b, b + 3000);
    const xm::RecordingFile& f = files[2];  // 00:10:00 - 00:15:00
    xm::CamTime s = f.begin + 150, e = s + 20;
    size_t before_i_p = 0, before_i_a = 0, p = 0, a = 0, ni = 0;
    uint32_t first_t = 0, last_t = 0;
    auto res = xm::fetch_frames(c, f, s, e, [&](const xm::Frame& fr) {
        if (fr.kind == xm::Kind::IFrame) { if (!ni++) first_t = fr.time_raw; last_t = fr.time_raw; }
        if (!fr.after_i && fr.kind == xm::Kind::PFrame) before_i_p++;
        if (!fr.after_i && fr.kind == xm::Kind::Audio) before_i_a++;
        if (fr.kind == xm::Kind::PFrame) p++;
        if (fr.kind == xm::Kind::Audio) a++;
    });
    CHECK(ni == 7);  // I-frames every 3 s at +150,+153..+168 s
    CHECK(xm::packed_to_cam_time(first_t) == s && xm::packed_to_cam_time(last_t) == s + 18);
    CHECK(before_i_p == 2 && before_i_a == 2);
    CHECK(p == 2 + 7 * 35 && a == 2 + 7 * 37);
    CHECK(res.demux.leading_bytes == 100 && res.demux.resyncs == 0 && !res.demux.truncated_tail);
    CHECK(res.download.bytes == 100 + 2 * (308 + 328) + 7 * xmtest::gop(0).size());
    std::string req = cam.last_download_request();
    CHECK(req.find(xm::format_cam_time(s)) != std::string::npos && req.find(xm::format_cam_time(e)) != std::string::npos);
    CHECK(req.find(f.name) != std::string::npos && req.find("\"PlayMode\":\"ByName\"") != std::string::npos);
}

static void test_range() {
    std::printf("range across files\n");
    xmtest::MockCamera cam(day_files(10));
    int connects = 0;
    xm::Connector conn = [&]() {
        auto d = std::make_unique<xm::Dvrip>();
        d->connect("127.0.0.1", cam.port(), 2000);
        d->login("shield", "secret");
        connects++;
        return d;
    };
    std::unique_ptr<xm::Dvrip> c = conn();
    xm::CamTime b = xm::parse_cam_time("2026-10-31 00:00:00");
    auto files = xm::list_files(*c, b, b + 3000);
    c.reset();
    connects = 0;
    // 00:04:50 .. 00:05:20 crosses the border between file 0 (00:00-00:05) and file 1 (00:05-00:10)
    xm::CamTime s = b + 290, e = b + 320;
    std::vector<xm::CamTime> its; std::vector<size_t> origin;
    auto r = xm::fetch_range(conn, files, s, e, 6, [&](const xm::Frame& f, size_t idx) {
        if (f.kind == xm::Kind::IFrame && f.after_i) { its.push_back(xm::packed_to_cam_time(f.time_raw)); origin.push_back(idx); }
    });
    CHECK(r.files_used == 2 && connects == 2);
    CHECK(r.notes.empty() && r.resyncs == 0);
    bool inc = !its.empty();
    for (size_t i = 1; i < its.size(); i++) if (its[i] <= its[i - 1]) inc = false;
    CHECK(inc);
    CHECK(its.front() <= s && its.front() > s - 9);        // the run-up gives an I-frame at or just before the target
    CHECK(its.back() >= e - 3 && its.back() <= e);           // coverage reaches the end of the window
    CHECK(origin.front() == 0 && origin.back() == 1);
    // outside the archive: nothing to download, and it says so
    auto none = xm::fetch_range(conn, files, b + 86400, b + 86410, 6, [](const xm::Frame&, size_t) {});
    CHECK(none.files_used == 0 && !none.notes.empty());
    // a window that sticks out past the end of the archive is reported
    auto tail = xm::fetch_range(conn, files, b + 2990, b + 3100, 0, [](const xm::Frame&, size_t) {});
    CHECK(tail.files_used == 1 && !tail.notes.empty());
}

static void test_timeline() {
    std::printf("av timeline\n");
    using namespace xmtest;
    xm::CamTime t0 = xm::parse_cam_time("2026-10-31 00:52:30");
    // junk + frames before the first I-frame + two GOPs
    // realistic audio density: two audio frames per video frame (72 x 320 B = 2.88 s per 36-frame GOP)
    auto real_gop = [&](xm::CamTime t) {
        std::string g = F9() + I(xm::encode_time(xm::from_cam_time(t)), 5000) + A(320) + A(320);
        for (int k = 0; k < 35; k++) g += P(400) + A(320) + A(320);
        return g;
    };
    std::string s = std::string(50, 'z') + P(300) + A(320) + real_gop(t0) + real_gop(t0 + 3);
    xm::AvTimeline tl(12.5, 8000.0);
    std::vector<xm::Stamped> v;
    std::vector<xm::Kind> kinds;
    xm::StreamParser sp([&](const xm::Frame& f) { xm::Stamped o; if (tl.stamp(f, o)) { v.push_back(o); kinds.push_back(f.kind); } });
    sp.feed((const uint8_t*)s.data(), s.size());
    sp.finish();
    size_t nv = 0, na = 0;
    uint64_t last_a_end = 0;
    bool audio_contiguous = true, video_monotonic = true;
    uint64_t last_v = 0;
    uint64_t first_v_gop2 = 0;
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i].video) {
            nv++;
            if (nv > 1 && v[i].pts_ns < last_v) video_monotonic = false;
            if (nv == 37) first_v_gop2 = v[i].pts_ns;
            last_v = v[i].pts_ns;
        } else {
            na++;
            if (v[i].pts_ns + 2 < last_a_end || v[i].pts_ns > last_a_end + 2) audio_contiguous = false;  // rounding: +-2 ns
            last_a_end = v[i].pts_ns + v[i].dur_ns;
        }
    }
    CHECK(nv == 72 && na == 144);                // frames before the first I-frame are not played
    CHECK(audio_contiguous && video_monotonic);
    CHECK(v.front().video && v.front().pts_ns == 0);
    // GOP 2 is re-anchored to the audio clock: 72 audio frames of 320 bytes (2.88 s) came before it
    CHECK(first_v_gop2 == (uint64_t)(72 * 320 / 8000.0 * 1e9 + 0.5));
    // if audio is much shorter than video, timestamps still never go backwards
    {
        xm::AvTimeline t2(12.5, 8000.0);
        std::string thin = gop(t0) + gop(t0 + 3);   // only ~1.5 s of audio per GOP
        uint64_t prev_end = 0; bool ok = true;
        xm::StreamParser sp2([&](const xm::Frame& f) {
            xm::Stamped o2;
            if (!t2.stamp(f, o2) || !o2.video) return;
            if (o2.pts_ns < prev_end) ok = false;
            prev_end = o2.pts_ns + o2.dur_ns;
        });
        sp2.feed((const uint8_t*)thin.data(), thin.size());
        sp2.finish();
        CHECK(ok);
    }
    xm::Frame info{}; info.kind = xm::Kind::Info; info.after_i = true;
    xm::Stamped o;
    CHECK(!tl.stamp(info, o));
}

static void test_timeline_geometry() {
    std::printf("timeline geometry\n");
    xm::CamTime day = xm::parse_cam_time("2026-10-31 00:00:00");
    auto mk = [&](int a, int b) { xm::RecordingFile f; f.name = "f" + std::to_string(a); f.begin = day + a; f.end = day + b; return f; };
    std::vector<xm::RecordingFile> files = {mk(300, 600), mk(0, 300), mk(900, 1200), mk(-100, 50), mk(86300, 86500)};
    auto segs = icmon::day_segments(files, day);
    // [-100,50] clips to [0,50], merges with [0,300] and [300,600]; 900..1200 is separate; the last one clips to the day end
    CHECK(segs.size() == 3);
    CHECK(segs[0].from == 0 && segs[0].to == 600);
    CHECK(segs[1].from == 900 && segs[1].to == 1200);
    CHECK(segs[2].from == 86300 && segs[2].to == 86400);
    CHECK(icmon::day_segments({}, day).empty());
    CHECK(icmon::second_from_x(14, 14, 1000) == 0);
    CHECK(icmon::second_from_x(514, 14, 1000) == 43200);
    CHECK(icmon::second_from_x(-50, 14, 1000) == 0 && icmon::second_from_x(5000, 14, 1000) == 86400);
    CHECK(icmon::second_from_x(10, 0, 0) == 0);
    CHECK(icmon::x_from_second(43200, 14, 1000) == 514.0);
    CHECK(icmon::clock_text(0) == "00:00:00" && icmon::clock_text(3661) == "01:01:01" && icmon::clock_text(86400) == "00:00:00");
    CHECK(icmon::file_at(files, day, 350) && icmon::file_at(files, day, 350)->name == "f300");
    CHECK(!icmon::file_at(files, day, 700));
}

static void test_cancel() {
    std::printf("cancel\n");
    auto o = day_files(3);
    o.endless = true;
    xmtest::MockCamera cam(o);
    xm::CancelToken tok;
    std::atomic<int> outcome{0};  // 1 = Cancelled, 2 = other
    std::atomic<size_t> frames{0};
    std::thread th([&] {
        try {
            xm::Dvrip c;
            c.connect("127.0.0.1", cam.port(), 2000, &tok);
            c.login("shield", "secret");
            xm::RecordingFile f; f.name = o.files[0].name; f.begin = o.files[0].begin; f.end = o.files[0].end;
            xm::fetch_frames(c, f, f.begin, f.begin + 60, [&](const xm::Frame&) { frames++; });
            outcome = 2;
        } catch (const xm::Error& e) { outcome = e.code() == xm::Errc::Cancelled ? 1 : 2; }
    });
    auto t0 = std::chrono::steady_clock::now();
    while (frames < 100 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(frames >= 100);
    auto tc = std::chrono::steady_clock::now();
    tok.cancel();
    th.join();
    double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - tc).count();
    CHECK(outcome == 1);
    CHECK(took < 1.0);  // cancel is honoured within the 200 ms poll slice, with margin
    for (int i = 0; i < 100 && !cam.endless_sender_stopped(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(cam.endless_sender_stopped());  // the connection was closed: the camera side stopped sending
}

static icmon::CameraConfig sample_cam() {
    icmon::CameraConfig c;
    c.id = "0123456789abcdef"; c.name = "backyard"; c.host = "192.168.0.101"; c.user = "admin";
    c.rtsp_template = icmon::kDefaultRtspTemplate;
    return c;
}

static void test_rtsp_url() {
    std::printf("rtsp url\n");
    icmon::CameraConfig c = sample_cam();
    auto r = icmon::build_rtsp_url("rtsp://{host}:{port}/user={user}&password={password}&channel={channel}&stream={stream}.sdp?real_stream", c, "p&w x", 1);
    CHECK(r.error.empty());
    CHECK(r.url == "rtsp://192.168.0.101:554/user=admin&password=p%26w%20x&channel=1&stream=1.sdp?real_stream");
    CHECK(icmon::percent_encode("aZ09-._~") == "aZ09-._~" && icmon::percent_encode("a b/\xC3\xA9") == "a%20b%2F%C3%A9");
    CHECK(!icmon::build_rtsp_url("http://x/{host}", c, "", 0).error.empty());
    CHECK(!icmon::build_rtsp_url("rtsp://{host}/{bogus}", c, "", 0).error.empty());
    CHECK(!icmon::build_rtsp_url("rtsp://{host", c, "", 0).error.empty());
    c.host = "fe80::1";
    CHECK(icmon::build_rtsp_url("rtsp://{host}:{port}/x", c, "", 0).url == "rtsp://[fe80::1]:554/x");
    c.host = "192.168.0.101";
    CHECK(icmon::build_rtsp_url(icmon::kDefaultRtspTemplate, c, "p@ss", 0).url == "rtsp://admin:p%40ss@192.168.0.101:554/channel=1");
    // The same template without any placeholder is accepted as it is (a user-supplied full address).
    CHECK(icmon::build_rtsp_url("rtsp://10.0.0.5/live", c, "", 0).url == "rtsp://10.0.0.5/live");
}

static void test_camera_store() {
    std::printf("camera store\n");
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / ("icmon_store_test_" + icmon::new_camera_id());
    std::string path = (dir / "sub" / "cameras.json").string();

    icmon::LoadResult none = icmon::load_store(path);  // no file: empty list, not an error
    CHECK(none.error.empty() && none.data.cameras.empty() && none.data.layout == 1);

    icmon::StoreData d;
    d.cameras.push_back(sample_cam());
    icmon::CameraConfig b = sample_cam();
    b.id = icmon::new_camera_id(); b.name = "yard \xD0\xB4\xD0\xB2\xD1\x96\xD1\x80"; b.host = "cam2.lan"; b.dvrip_port = 34568;
    b.use_sub_in_grid = true; b.no_password = true; b.rtsp_template = "rtsp://{host}:{port}/ch{channel}/{stream}";
    b.presets = {1, 3, 7};
    d.cameras.push_back(b);
    d.layout = 4; d.selected_id = b.id; d.media_dir = "/tmp/media \xD1\x84";
    CHECK(icmon::save_store(path, d).empty());
    CHECK(!fs::exists(path + ".tmp"));
    icmon::LoadResult back = icmon::load_store(path);
    CHECK(back.error.empty() && back.notes.empty() && back.data.cameras.size() == 2);
    CHECK(back.data.layout == 4 && back.data.selected_id == b.id && back.data.media_dir == d.media_dir);
    CHECK(icmon::default_media_dir().find("ICmon_media") != std::string::npos);
    {  // grid slots: unique; the list order is kept; a new camera takes the first empty tile and sits between its neighbours
        icmon::StoreData d;
        for (int s : {5, 0, 5, -1}) { icmon::CameraConfig cc; cc.id = "c" + std::to_string(d.cameras.size()); cc.slot = s; d.cameras.push_back(cc); }
        icmon::normalize_slots(d);
        CHECK(d.cameras.size() == 4);
        CHECK(d.cameras[0].id == "c0" && d.cameras[1].id == "c1" && d.cameras[2].id == "c2" && d.cameras[3].id == "c3");
        CHECK(d.cameras[0].slot == 5 && d.cameras[1].slot == 0 && d.cameras[2].slot == 1 && d.cameras[3].slot == 2);
        icmon::CameraConfig n; n.id = "new"; n.slot = icmon::first_free_slot(d);
        CHECK(n.slot == 3);  // slots 0,1,2,5 are taken
        icmon::insert_by_slot(d, n);
        CHECK(d.cameras.size() == 5 && d.cameras[4].id == "new");  // after the last camera with a lower slot (c3, slot 2)
    }
    const icmon::CameraConfig& g = back.data.cameras[1];
    CHECK(g.id == b.id && g.name == b.name && g.host == "cam2.lan" && g.dvrip_port == 34568 && g.rtsp_port == 554);
    CHECK(g.use_sub_in_grid && g.no_password && g.rtsp_template == b.rtsp_template && g.sub_stream == 1);
    CHECK((g.presets == std::vector<int>{1, 3, 7}) && back.data.cameras[0].presets.empty());

    {   // a bad entry and a duplicate id are skipped with a note; the valid one survives; layout 3 falls back to 1
        std::ofstream o(path, std::ios::trunc);
        o << "{\"layout\":3,\"cameras\":[{\"id\":\"a\",\"name\":\"ok\",\"host\":\"h\",\"rtsp_template\":\"rtsp://{host}/x\"},"
             "{\"id\":\"b\",\"name\":\"nohost\",\"rtsp_template\":\"rtsp://x\"},"
             "{\"id\":\"a\",\"name\":\"dup\",\"host\":\"h2\",\"rtsp_template\":\"rtsp://{host}/x\"}]}";
    }
    icmon::LoadResult part = icmon::load_store(path);
    CHECK(part.error.empty() && part.data.cameras.size() == 1 && part.notes.size() == 2 && part.data.layout == 1);

    {   // a damaged file is moved aside so that it is never overwritten by the next save
        std::ofstream o(path, std::ios::trunc);
        o << "{\"cameras\": [oops";
    }
    icmon::LoadResult bad = icmon::load_store(path);
    CHECK(!bad.error.empty() && bad.data.cameras.empty() && !fs::exists(path) && fs::exists(path + ".bad"));

    icmon::CameraConfig v = sample_cam();
    CHECK(icmon::validate_camera(v).empty());
    v.host = "a b"; CHECK(!icmon::validate_camera(v).empty());
    v = sample_cam(); v.dvrip_port = 70000; CHECK(!icmon::validate_camera(v).empty());
    v = sample_cam(); v.name.clear(); CHECK(!icmon::validate_camera(v).empty());
    v = sample_cam(); v.rtsp_template = "rtsp://{nope}"; CHECK(!icmon::validate_camera(v).empty());
    CHECK(icmon::new_camera_id().size() == 16 && icmon::new_camera_id() != icmon::new_camera_id());
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// SYNTHETIC: the reply below is built from the documented format of the open-source dvrip project, not captured
// from a real camera. It proves our code handles that format, not that your camera answers this way.
static std::string fake_discover_reply(const std::string& json, unsigned id = 1531) {
    std::string pl = json + "\n";
    pl.push_back('\0');
    std::string h(20, '\0');
    h[0] = (char)0xFF; h[14] = (char)(id & 0xff); h[15] = (char)(id >> 8);
    for (int i = 0; i < 4; i++) h[16 + i] = (char)((pl.size() >> (8 * i)) & 0xff);
    return h + pl;
}

static void test_discovery() {
    std::printf("discovery\n");
    std::string req = icmon::make_discover_request();
    CHECK(req.size() == 20 && (uint8_t)req[0] == 0xFF && (uint8_t)req[14] == (1530 & 0xff) && (uint8_t)req[15] == (1530 >> 8) &&
          req[16] == 0 && req[4] == 0 && req[8] == 0);

    const std::string good = "{\"Name\":\"\",\"NetWork.NetCommon\":{\"HostIP\":\"0x6500A8C0\",\"HostName\":\"yard\",\"MAC\":\"00:12:34:56:78:9a\","
                             "\"SN\":\"abc123\",\"TCPPort\":34567,\"ChannelNum\":1},\"Ret\":100,\"SessionID\":\"0x0\"}";
    icmon::FoundCamera f; std::string why;
    CHECK(icmon::parse_discover_reply(fake_discover_reply(good), "192.168.0.101", f, why));
    CHECK(f.host == "192.168.0.101" && f.name == "yard" && f.mac == "00:12:34:56:78:9a" && f.serial == "abc123" && f.tcp_port == 34567 && f.channels == 1);
    CHECK(!icmon::parse_discover_reply(req, "1.2.3.4", f, why));                       // our own request is not a reply
    CHECK(!icmon::parse_discover_reply(fake_discover_reply(good, 1530), "1.2.3.4", f, why));
    CHECK(!icmon::parse_discover_reply(fake_discover_reply("{\"Ret\":100}"), "1.2.3.4", f, why));
    CHECK(!icmon::parse_discover_reply(fake_discover_reply("{oops"), "1.2.3.4", f, why));
    CHECK(!icmon::parse_discover_reply("garbage", "1.2.3.4", f, why));

    // Loopback: a fake camera on 127.0.0.1 answers the request; the client sends to it instead of broadcasting.
    int cam = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    CHECK(::bind(cam, (sockaddr*)&a, sizeof a) == 0);
    socklen_t al = sizeof a; getsockname(cam, (sockaddr*)&a, &al);
    std::string seen;
    std::thread th([&] {
        timeval tv{2, 0}; setsockopt(cam, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        char b[256]; sockaddr_in from{}; socklen_t fl = sizeof from;
        ssize_t n = recvfrom(cam, b, sizeof b, 0, (sockaddr*)&from, &fl);
        if (n > 0) {
            seen.assign(b, (size_t)n);
            std::string junk = "not dvrip", r = fake_discover_reply(good);
            sendto(cam, junk.data(), junk.size(), 0, (sockaddr*)&from, fl);  // must be ignored
            sendto(cam, r.data(), r.size(), 0, (sockaddr*)&from, fl);
            sendto(cam, r.data(), r.size(), 0, (sockaddr*)&from, fl);        // duplicate: merged
        }
    });
    icmon::DiscoverOptions o;
    o.timeout_ms = 700; o.destination = "127.0.0.1"; o.bind_port = 0; o.dest_port = ntohs(a.sin_port);
    std::vector<icmon::FoundCamera> got = icmon::discover(o);
    th.join();
    ::close(cam);
    CHECK(seen == req);
    CHECK(got.size() == 1 && got[0].host == "127.0.0.1" && got[0].name == "yard");

    xm::CancelToken tok; tok.cancel();
    CHECK(throws(xm::Errc::Cancelled, [&] { icmon::discover(o, &tok); }));
    // a port that is already taken is reported, not ignored
    int hold = ::socket(AF_INET, SOCK_DGRAM, 0); sockaddr_in h{}; h.sin_family = AF_INET; h.sin_addr.s_addr = htonl(INADDR_ANY);
    ::bind(hold, (sockaddr*)&h, sizeof h); socklen_t hl = sizeof h; getsockname(hold, (sockaddr*)&h, &hl);
    int saved_reuse = 0; (void)saved_reuse;
    o.bind_port = ntohs(h.sin_port);  // SO_REUSEADDR on both sockets would allow sharing; here only the second has it
    // (with SO_REUSEADDR set by discover() the bind may still fail on a socket without it, which is what we check)
    bool threw = false; try { icmon::discover(o); } catch (const xm::Error&) { threw = true; }
    CHECK(threw);
    ::close(hold);
}

static void test_camctl() {
    // Only what we can check without a camera: the JSON we would send, against the documented format.
    xm::Json b = icmon::make_ptz_move("DirectionUp", 0, 5, true);
    CHECK(b["Name"].str() == "OPPTZControl");
    const xm::Json& p = b["OPPTZControl"];
    CHECK(p["Command"].str() == "DirectionUp");
    CHECK(p["Parameter"]["Pattern"].str() == "SetBegin" && p["Parameter"]["Preset"].integer() == 65535);
    CHECK(p["Parameter"]["Step"].integer() == 5 && p["Parameter"]["Tour"].integer() == 0);
    xm::Json e = icmon::make_ptz_move("DirectionUp", 0, 5, false);
    CHECK(e["OPPTZControl"]["Parameter"]["Preset"].integer() == -1);
    xm::Json g = icmon::make_ptz_preset("GotoPreset", 0, 3);
    CHECK(g["OPPTZControl"]["Command"].str() == "GotoPreset" && g["OPPTZControl"]["Parameter"]["Preset"].integer() == 3);
    xm::Json parsed = xm::Json::parse(b.dump());  // survives a round trip
    CHECK(parsed["OPPTZControl"]["Parameter"]["AUX"]["Status"].str() == "On");
    xm::Json rep = xm::Json::parse("{\"Name\":\"Camera.WhiteLight\",\"Ret\":100,\"Camera.WhiteLight\":[{\"WorkMode\":\"Auto\"}]}");
    CHECK(icmon::config_object(rep, "Camera.WhiteLight")["WorkMode"].str() == "Auto");
    xm::Json w = icmon::config_with(rep, "Camera.WhiteLight", "WorkMode", "KeepOpen");
    CHECK(w.is_array() && w.items().size() == 1 && w.items()[0]["WorkMode"].str() == "KeepOpen");
    xm::Json rep3 = xm::Json::parse("{\"X\":[{\"WorkMode\":\"Auto\",\"Level\":3},{\"A\":1}]}");
    xm::Json w3 = icmon::config_with(rep3, "X", "WorkMode", "Close");
    CHECK(w3.items().size() == 2 && w3.items()[0]["Level"].integer() == 3 && w3.items()[1]["A"].integer() == 1 &&
          w3.items()[0]["WorkMode"].str() == "Close");
    CHECK(icmon::config_with(rep3, "missing", "WorkMode", "Close").is_null());
    xm::Json rep2 = xm::Json::parse("{\"Camera.WhiteLight\":{\"WorkMode\":\"Close\"}}");
    CHECK(icmon::config_object(rep2, "Camera.WhiteLight")["WorkMode"].str() == "Close");
}

int main() {
    test_camctl();
    test_hash();
    test_json();
    test_json_locale();
    test_time();
    test_stream_parser();
    test_login();
    test_listing();
    test_fetch(false);
    test_fetch(true);
    test_range();
    test_timeline();
    test_timeline_geometry();
    test_cancel();
    test_rtsp_url();
    test_camera_store();
    test_discovery();
    std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
