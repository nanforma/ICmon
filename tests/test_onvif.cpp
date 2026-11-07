// Tests of the ONVIF client against a FAKE device service. The fake exists only here: it checks the WS-Security digest,
// has a first media profile without PTZ and a second with PTZ, and answers GetPresets / RemovePreset.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <thread>

#include "ptz_manager.h"
#include "ptz_onvif.h"

using namespace ptz;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static std::string between(const std::string& s, const std::string& a, const std::string& b) {
    size_t i = s.find(a);
    if (i == std::string::npos) return "";
    i += a.size();
    size_t e = s.find(b, i);
    return e == std::string::npos ? "" : s.substr(i, e - i);
}

static std::string b64decode(const std::string& in) {
    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        int d;
        if (c >= 'A' && c <= 'Z') d = c - 'A';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
        else if (c >= '0' && c <= '9') d = c - '0' + 52;
        else if (c == '+') d = 62;
        else if (c == '/') d = 63;
        else continue;
        val = (val << 6) + d;
        bits += 6;
        if (bits >= 0) { out.push_back((char)((val >> bits) & 0xFF)); bits -= 8; }
    }
    return out;
}

struct Fake {
    int fd = -1, port = 0;
    std::atomic<bool> stop{false};
    std::set<std::string> presets{"1", "2", "5"};
    std::thread th;
    void start() {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(fd, (sockaddr*)&a, sizeof a);
        socklen_t l = sizeof a;
        getsockname(fd, (sockaddr*)&a, &l);
        port = ntohs(a.sin_port);
        listen(fd, 8);
        th = std::thread([this] {
            while (!stop) {
                int c = accept(fd, nullptr, nullptr);
                if (c < 0) break;
                std::string in;
                char b[4096];
                size_t need = 0;
                for (;;) {
                    ssize_t n = recv(c, b, sizeof b, 0);
                    if (n <= 0) break;
                    in.append(b, (size_t)n);
                    size_t he = in.find("\r\n\r\n");
                    if (he != std::string::npos) {
                        if (!need) need = he + 4 + (size_t)atoi(between(in, "Content-Length: ", "\r\n").c_str());
                        if (in.size() >= need) break;
                    }
                }
                std::string out = answer(in);
                std::string http = "HTTP/1.1 200 OK\r\nContent-Type: application/soap+xml\r\nContent-Length: " + std::to_string(out.size()) +
                                   "\r\nConnection: close\r\n\r\n" + out;
                send(c, http.data(), http.size(), 0);
                close(c);
            }
        });
    }
    ~Fake() {
        stop = true;
        shutdown(fd, SHUT_RDWR);
        close(fd);
        if (th.joinable()) th.join();
    }
    static std::string env(const std::string& body) {
        return "<?xml version=\"1.0\"?><env:Envelope xmlns:env=\"http://www.w3.org/2003/05/soap-envelope\"><env:Body>" + body + "</env:Body></env:Envelope>";
    }
    bool authorised(const std::string& req) {
        std::string user = between(req, "<Username>", "</Username>");
        std::string dig = between(req, "PasswordDigest\">", "</Password>");
        std::string nonce = b64decode(between(req, "Base64Binary\">", "</Nonce>"));
        std::string created = between(req, "wssecurity-utility-1.0.xsd\">", "</Created>");
        return user == "admin" && !dig.empty() && dig == base64_encode(sha1_raw(nonce + created + "secret"));
    }
    std::string answer(const std::string& req) {
        std::string url = "http://127.0.0.1:" + std::to_string(port);
        if (req.find("GetSystemDateAndTime") != std::string::npos)
            return env("<tds:GetSystemDateAndTimeResponse xmlns:tds=\"x\"><tds:SystemDateAndTime><tt:UTCDateTime xmlns:tt=\"y\"><tt:Time><tt:Hour>10</tt:Hour><tt:Minute>0</tt:Minute><tt:Second>0</tt:Second></tt:Time><tt:Date><tt:Year>2026</tt:Year><tt:Month>11</tt:Month><tt:Day>5</tt:Day></tt:Date></tt:UTCDateTime></tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>");
        if (!authorised(req))
            return env("<env:Fault><env:Code><env:Value>env:Sender</env:Value><env:Subcode><env:Value>ter:NotAuthorized</env:Value></env:Subcode></env:Code><env:Reason><env:Text>Sender not Authorized</env:Text></env:Reason></env:Fault>");
        if (req.find("GetCapabilities") != std::string::npos)
            return env("<tds:GetCapabilitiesResponse xmlns:tds=\"x\"><tds:Capabilities><tt:Media xmlns:tt=\"y\"><tt:XAddr>" + url + "/onvif/media_service</tt:XAddr></tt:Media><tt:PTZ xmlns:tt=\"y\"><tt:XAddr>" + url + "/onvif/ptz_service</tt:XAddr></tt:PTZ></tds:Capabilities></tds:GetCapabilitiesResponse>");
        if (req.find("GetProfiles") != std::string::npos)
            return env("<trt:GetProfilesResponse xmlns:trt=\"m\" xmlns:tt=\"y\"><trt:Profiles token=\"Prof_video\"><tt:Name>video only</tt:Name></trt:Profiles>"
                       "<trt:Profiles token=\"Prof_ptz\"><tt:Name>ptz</tt:Name><tt:PTZConfiguration token=\"ptzcfg\"><tt:Name>c</tt:Name></tt:PTZConfiguration></trt:Profiles></trt:GetProfilesResponse>");
        if (req.find("GetPresets") != std::string::npos) {
            if (req.find("<ProfileToken>Prof_ptz</ProfileToken>") == std::string::npos) return env("<tptz:GetPresetsResponse xmlns:tptz=\"p\"/>");
            std::string l;
            for (const std::string& p : presets)
                l += "<tptz:Preset token=\"" + p + "\"><tt:Name xmlns:tt=\"y\">Pos " + p + "</tt:Name><tt:PTZPosition xmlns:tt=\"y\"><tt:PanTilt x=\"0.5\" y=\"-0.25\"/><tt:Zoom x=\"0.1\"/></tt:PTZPosition></tptz:Preset>";
            return env("<tptz:GetPresetsResponse xmlns:tptz=\"p\">" + l + "</tptz:GetPresetsResponse>");
        }
        if (req.find("<SetPreset") != std::string::npos) {
            presets.insert(between(req, "<PresetToken>", "</PresetToken>"));
            return env("<tptz:SetPresetResponse xmlns:tptz=\"p\"><tptz:PresetToken>x</tptz:PresetToken></tptz:SetPresetResponse>");
        }
        if (req.find("RemovePreset") != std::string::npos) {
            presets.erase(between(req, "<PresetToken>", "</PresetToken>"));
            return env("<tptz:RemovePresetResponse xmlns:tptz=\"p\"/>");
        }
        return env("<env:Fault><env:Reason><env:Text>unknown</env:Text></env:Reason></env:Fault>");
    }
};

int main() {
    CHECK(base64_encode(sha1_raw("abc")) == "qZk+NkcGgWq6PiVxeFDCbJzQ2J0=");  // SHA-1("abc") = a9993e36...
    CHECK(base64_encode("Man") == "TWFu" && base64_encode("Ma") == "TWE=" && base64_encode("M") == "TQ==");

    Fake f;
    f.start();
    OnvifTarget t;
    t.host = "127.0.0.1";
    t.user = "admin";
    t.password = "secret";
    t.ports = {1, f.port};  // the first port refuses: it is skipped
    t.timeout_ms = 2000;
    OnvifResult r = onvif_read_presets(t);
    if (!r.ok) printf("%s\n%s\n", r.error.c_str(), r.log.c_str());
    CHECK(r.ok);
    CHECK(r.port == f.port);
    CHECK(r.profiles.size() == 2);
    CHECK(!r.profiles[0].has_ptz && r.profiles[1].has_ptz);
    CHECK(r.profile_token == "Prof_ptz");  // the profile with a PTZConfiguration, not the first one
    CHECK(r.presets.size() == 3);
    if (r.presets.size() == 3) {
        CHECK(r.presets[2].token == "5" && r.presets[2].name == "Pos 5");
        CHECK(r.presets[0].has_position && r.presets[0].pan == 0.5 && r.presets[0].tilt == -0.25 && r.presets[0].zoom == 0.1);
    }
    CHECK(r.log.find("token=\"1\"") != std::string::npos);

    OnvifTarget bad = t;
    bad.password = "wrong";
    OnvifResult rb = onvif_read_presets(bad);
    CHECK(!rb.ok);
    CHECK(rb.error.find("Not Authorized") != std::string::npos || rb.error.find("not Authorized") != std::string::npos);

    // manager: asynchronous refresh, cache, delete
    PtzManager m;
    std::atomic<bool> got{false};
    OnvifResult mr;
    m.refresh_presets("cam", t, [&](const OnvifResult& x) { mr = x; got = true; });
    for (int i = 0; i < 200 && !got; i++) usleep(20000);
    CHECK(got && mr.ok && m.get_presets("cam").size() == 3);
    CHECK(m.token_for("cam", "2"));
    CHECK(!m.token_for("cam", "9"));
    std::atomic<bool> del{false};
    std::string derr = "x";
    m.delete_preset("cam", t, "2", [&](const std::string& e, const std::string&) { derr = e; del = true; });
    for (int i = 0; i < 200 && !del; i++) usleep(20000);
    CHECK(del && derr.empty());
    CHECK(f.presets.count("2") == 0 && f.presets.size() == 2);
    std::atomic<bool> del2{false};
    std::string derr2;
    m.delete_preset("cam", t, "77", [&](const std::string& e, const std::string&) { derr2 = e; del2 = true; });
    CHECK(del2 && !derr2.empty());  // an unknown token is never sent

    std::atomic<bool> sv{false};
    std::string serr = "x";
    m.save_preset("cam", t, "7", [&](const std::string& e, const std::string&) { serr = e; sv = true; });
    for (int i = 0; i < 200 && !sv; i++) usleep(20000);
    CHECK(sv && serr.empty() && f.presets.count("7") == 1);

    printf(failures ? "FAILED\n" : "all ONVIF tests passed\n");
    return failures ? 1 : 0;
}
