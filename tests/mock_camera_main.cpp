// TEST-ONLY: runs the mock camera as a process so tests/e2e_xmarchive.sh can drive the real xmarchive binary.
// Prints "PORT <n>" and serves until stdin closes.
#include <cstdio>
#include <iostream>
#include <string>

#include "mock_camera.h"

int main() {
    xmtest::MockCamera::Opts o;
    xm::CamTime day = xm::parse_cam_time("2026-10-31 00:00:00");
    for (int i = 0; i < 150; i++) {  // 150 five-minute files: forces three query pages
        xm::CamTime b = day + i * 300;
        std::string n = "/idea0/2026-10-31/001/" + xm::format_cam_clock(b) + "[R][@1][0].h264";
        o.files.push_back({n, b, b + 300});
    }
    xmtest::MockCamera cam(o);
    std::printf("PORT %u\n", cam.port());
    std::fflush(stdout);
    std::string line;
    while (std::getline(std::cin, line)) {}
    return 0;
}
