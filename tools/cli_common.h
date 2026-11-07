// cli_common.h - option parsing, hidden password prompt and login shared by the console tools.
#pragma once
#include <termios.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>

#include "dvrip.h"

namespace cli {

inline xm::CancelToken g_cancel;
inline void on_sigint(int) { g_cancel.cancel(); }
inline void install_sigint() {
    struct sigaction sa{};
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, nullptr);
}

struct Args {
    std::string cmd;
    std::map<std::string, std::string> opt;
    std::string get(const std::string& k, const std::string& def = "") const {
        auto it = opt.find(k);
        return it == opt.end() ? def : it->second;
    }
};

// argv[1] is the command when `has_cmd`; the rest are "--key value" pairs.
inline bool parse_args(int argc, char** argv, Args& a, bool has_cmd) {
    int i = 1;
    if (has_cmd) {
        if (argc < 2) return false;
        a.cmd = argv[i++];
    }
    for (; i < argc; i++) {
        std::string k = argv[i];
        if (k.rfind("--", 0) != 0 || i + 1 >= argc) { std::cerr << "bad or incomplete option: " << k << "\n"; return false; }
        a.opt[k.substr(2)] = argv[++i];
    }
    return true;
}

// Password: env CAM_PASS, else a hidden prompt. Never printed.
inline std::string get_password(const std::string& user) {
    if (const char* e = getenv("CAM_PASS")) return e;
    if (!isatty(STDIN_FILENO)) { std::cerr << "no password: set CAM_PASS or run from a terminal\n"; return std::string(); }
    std::cerr << "Password for " << user << " (hidden): " << std::flush;
    termios old{}, noecho{};
    tcgetattr(STDIN_FILENO, &old);
    noecho = old;
    noecho.c_lflag &= ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &noecho);
    std::string pw;
    std::getline(std::cin, pw);
    tcsetattr(STDIN_FILENO, TCSANOW, &old);
    std::cerr << "\n(" << pw.size() << " bytes read)\n";
    return pw;
}

inline void connect_and_login(xm::Dvrip& c, const Args& a, const std::string& pw) {
    for (unsigned char ch : pw)
        if (ch >= 0x80) {
            std::cerr << "refusing to log in: the password contains non-ASCII characters (wrong keyboard layout or a pasted placeholder?).\n"
                         "Failed logins can lock the camera account, so nothing was sent.\n";
            throw xm::Error(xm::Errc::Auth, "password not sent");
        }
    c.connect(a.get("host"), (uint16_t)atoi(a.get("port", "34567").c_str()), 4000, &g_cancel);
    c.login(a.get("user", "admin"), pw);
}

}  // namespace cli
