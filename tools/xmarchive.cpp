// xmarchive - read the recordings on the camera's SD card over the network (DVRIP), no card removal.
//
//   xmarchive list  --host IP --user U --date YYYY-MM-DD [--from HH:MM:SS] [--to HH:MM:SS] [--port N]
//   xmarchive fetch --host IP --user U --start "YYYY-MM-DD HH:MM:SS" --secs N [--lead S] [--out PREFIX]
//
// Password: environment variable CAM_PASS, otherwise a hidden prompt. It is never printed or stored.
// Times are the CAMERA's clock. `fetch` writes PREFIX.h265 (play with: ffplay -f hevc PREFIX.h265) and
// PREFIX.alaw (audio assumed to be G.711 A-law 8 kHz: NOT verified). Ctrl-C cancels cleanly.
//
// SECURITY: the DVRIP login is not encrypted (see src/xm/dvrip.h). Use it only on a trusted network.
#include <termios.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>

#include "archive.h"
#include <chrono>
#include <thread>

#include "camctl.h"
#include "cli_common.h"

namespace {

using namespace cli;

void usage() {
    std::cerr << "usage:\n"
                 "  xmarchive list  --host IP --user U --date YYYY-MM-DD [--from HH:MM:SS] [--to HH:MM:SS] [--port N]\n"
                 "  xmarchive fetch --host IP --user U --start \"YYYY-MM-DD HH:MM:SS\" --secs N [--lead S] [--out PREFIX] [--port N]\n"
                 "  xmarchive getcfg --host IP --user U --name Camera.WhiteLight   (read-only: prints the camera's answer)\n"
                 "  xmarchive ptz   --host IP --user U --cmd DirectionUp [--ms 500] [--step 5]   (moves, then stops; UNVERIFIED format)\n"
                 "  xmarchive preset --host IP --user U --goto N | --set N                       (UNVERIFIED format)\n"
                 "password: env CAM_PASS or hidden prompt. Times are the camera's clock.\n";
}

int do_list(const Args& a) {
    std::string date = a.get("date");
    if (date.empty()) { usage(); return 2; }
    xm::CamTime b = xm::parse_cam_time(date + " " + a.get("from", "00:00:00"));
    xm::CamTime e = xm::parse_cam_time(date + " " + a.get("to", "23:59:59"));
    xm::Dvrip c;
    std::string pw = get_password(a.get("user", "admin"));
    connect_and_login(c, a, pw);
    auto files = xm::list_files(c, b, e, &g_cancel);
    std::cout << files.size() << " recording(s) between " << xm::format_cam_time(b) << " and " << xm::format_cam_time(e)
              << " (camera clock)\n";
    int i = 0;
    for (const auto& f : files) {
        std::printf("%4d  %s - %s  %5llds  len=0x%llx  %s\n", i++, xm::format_cam_clock(f.begin).c_str(),
                    xm::format_cam_clock(f.end).c_str(), (long long)(f.end - f.begin), (unsigned long long)f.length_raw, f.name.c_str());
    }
    return 0;
}

int do_fetch(const Args& a) {
    if (a.get("start").empty() || a.get("secs").empty()) { usage(); return 2; }
    xm::CamTime start = xm::parse_cam_time(a.get("start"));
    int secs = atoi(a.get("secs").c_str());
    int lead = atoi(a.get("lead", "6").c_str());
    std::string out = a.get("out", "xmarchive_out");
    if (secs <= 0 || secs > 3600 || lead < 0 || lead > 60) { std::cerr << "--secs must be 1..3600 and --lead 0..60\n"; return 2; }

    std::string pw = get_password(a.get("user", "admin"));
    xm::Dvrip c;
    connect_and_login(c, a, pw);
    xm::CamTime end = start + secs;
    auto files = xm::list_files(c, start - 3600, end + 3600, &g_cancel);
    c.close();
    xm::Connector connector = [&]() {
        auto d = std::make_unique<xm::Dvrip>();
        connect_and_login(*d, a, pw);
        return d;
    };

    std::ofstream vid(out + ".h265", std::ios::binary), aud(out + ".alaw", std::ios::binary);
    if (!vid || !aud) { std::cerr << "cannot write " << out << ".h265/.alaw\n"; return 1; }
    size_t vbytes = 0, abytes = 0, dropped = 0, ifr = 0;
    std::string first_i, last_i;
    auto res = xm::fetch_range(connector, files, start, end, lead, [&](const xm::Frame& fr, size_t) {
        if (fr.kind == xm::Kind::IFrame) {
            std::string t = xm::format_cam_time(xm::packed_to_cam_time(fr.time_raw));
            if (first_i.empty()) first_i = t;
            last_i = t;
            ifr++;
        }
        if (fr.kind == xm::Kind::IFrame || fr.kind == xm::Kind::PFrame) {
            if (!fr.after_i) { dropped++; return; }
            vid.write((const char*)fr.payload, fr.payload_len); vbytes += fr.payload_len;
        } else if (fr.kind == xm::Kind::Audio) {
            if (!fr.after_i) { dropped++; return; }
            aud.write((const char*)fr.payload, fr.payload_len); abytes += fr.payload_len;
        }
    });
    std::cout << "requested " << xm::format_cam_time(start) << " .. " << xm::format_cam_time(end) << " (lead " << lead << " s), "
              << res.files_used << " file(s) used\n";
    for (const auto& n : res.notes) std::cout << "NOTE: " << n << "\n";
    std::cout << "downloaded " << res.download.bytes << " bytes in " << res.download.packets << " packets\n"
              << "frames: I=" << res.i_frames << " P=" << res.p_frames << " audio=" << res.audio_frames
              << ", dropped before the first I-frame of each file: " << dropped << "\n";
    if (!first_i.empty()) std::cout << "I-frame times: " << first_i << " .. " << last_i << " (" << ifr << " I-frames)\n";
    if (res.resyncs) std::cout << "WARNING: " << res.resyncs << " resync(s), " << res.skipped_bytes << " byte(s) skipped\n";
    std::cout << "video " << vbytes << " bytes -> " << out << ".h265   audio " << abytes << " bytes -> " << out << ".alaw\n";
    return vbytes ? 0 : 1;
}

int do_getcfg(const Args& a) {
    if (a.get("name").empty()) { usage(); return 2; }
    xm::Dvrip c;
    std::string pw = get_password(a.get("user", "admin"));
    connect_and_login(c, a, pw);
    xm::Json r = icmon::config_get(c, a.get("name"));
    std::cout << r.dump() << "\n";
    return 0;
}

int do_ptz(const Args& a) {
    if (a.get("cmd").empty()) { usage(); return 2; }
    int ms = atoi(a.get("ms", "500").c_str()), step = atoi(a.get("step", "5").c_str());
    xm::Dvrip c;
    std::string pw = get_password(a.get("user", "admin"));
    connect_and_login(c, a, pw);
    xm::Json r1 = c.request(icmon::kMsgPtz, icmon::make_ptz_move(a.get("cmd"), 0, step, true));
    std::cout << "start: " << r1.dump() << "\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    xm::Json r2 = c.request(icmon::kMsgPtz, icmon::make_ptz_move(a.get("cmd"), 0, step, false));
    std::cout << "stop:  " << r2.dump() << "\n";
    return 0;
}

int do_preset(const Args& a) {
    bool go = !a.get("goto").empty(), set = !a.get("set").empty();
    if (go == set) { usage(); return 2; }
    int n = atoi((go ? a.get("goto") : a.get("set")).c_str());
    xm::Dvrip c;
    std::string pw = get_password(a.get("user", "admin"));
    connect_and_login(c, a, pw);
    xm::Json r = c.request(icmon::kMsgPtz, icmon::make_ptz_preset(go ? "GotoPreset" : "SetPreset", 0, n));
    std::cout << r.dump() << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Args a;
    if (!parse_args(argc, argv, a, true) || a.get("host").empty()) { usage(); return 2; }
    install_sigint();
    try {
        if (a.cmd == "list") return do_list(a);
        if (a.cmd == "fetch") return do_fetch(a);
        if (a.cmd == "getcfg") return do_getcfg(a);
        if (a.cmd == "ptz") return do_ptz(a);
        if (a.cmd == "preset") return do_preset(a);
        usage();
        return 2;
    } catch (const xm::Error& e) {
        std::cerr << (e.code() == xm::Errc::Cancelled ? "cancelled" : std::string("error: ") + e.what()) << "\n";
        return e.code() == xm::Errc::Cancelled ? 130 : 1;
    }
}
