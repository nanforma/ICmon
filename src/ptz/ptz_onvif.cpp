#include "ptz_onvif.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <random>

namespace ptz {

// ------------------------------------------------------------------ SHA-1 and base64 (WS-Security digest)

std::string sha1_raw(const std::string& data) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string m = data;
    uint64_t bits = (uint64_t)data.size() * 8;
    m.push_back((char)0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 7; i >= 0; i--) m.push_back((char)((bits >> (i * 8)) & 0xFF));
    auto rol = [](uint32_t v, int s) { return (v << s) | (v >> (32 - s)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)(uint8_t)m[off + i * 4] << 24) | ((uint32_t)(uint8_t)m[off + i * 4 + 1] << 16) |
                   ((uint32_t)(uint8_t)m[off + i * 4 + 2] << 8) | (uint32_t)(uint8_t)m[off + i * 4 + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::string out;
    for (int i = 0; i < 5; i++)
        for (int s = 24; s >= 0; s -= 8) out.push_back((char)((h[i] >> s) & 0xFF));
    return out;
}

std::string base64_encode(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8) | (uint8_t)in[i + 2];
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += t[v & 63];
    }
    if (i + 1 == in.size()) {
        uint32_t v = (uint8_t)in[i] << 16;
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8);
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += '=';
    }
    return out;
}

// ------------------------------------------------------------------ a small tolerant XML reader

namespace {

struct Node {
    std::string name;  // local name, prefix removed
    std::map<std::string, std::string> attr;  // local names
    std::string text;
    std::vector<Node> kids;
};

std::string local(const std::string& n) {
    size_t c = n.find(':');
    return c == std::string::npos ? n : n.substr(c + 1);
}

std::string unescape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '&') {
            size_t e = s.find(';', i);
            if (e != std::string::npos) {
                std::string k = s.substr(i + 1, e - i - 1);
                if (k == "amp") { o += '&'; i = e; continue; }
                if (k == "lt") { o += '<'; i = e; continue; }
                if (k == "gt") { o += '>'; i = e; continue; }
                if (k == "quot") { o += '"'; i = e; continue; }
                if (k == "apos") { o += '\''; i = e; continue; }
            }
        }
        o += s[i];
    }
    return o;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

bool parse_xml(const std::string& x, Node& root) {
    std::vector<Node*> stack;
    root = Node();
    root.name = "#root";
    stack.push_back(&root);
    size_t i = 0;
    while (i < x.size()) {
        if (x[i] != '<') {
            size_t e = x.find('<', i);
            if (e == std::string::npos) e = x.size();
            stack.back()->text += unescape(x.substr(i, e - i));
            i = e;
            continue;
        }
        if (x.compare(i, 4, "<!--") == 0) { size_t e = x.find("-->", i); if (e == std::string::npos) return false; i = e + 3; continue; }
        if (x.compare(i, 2, "<?") == 0) { size_t e = x.find("?>", i); if (e == std::string::npos) return false; i = e + 2; continue; }
        if (x.compare(i, 9, "<![CDATA[") == 0) {
            size_t e = x.find("]]>", i);
            if (e == std::string::npos) return false;
            stack.back()->text += x.substr(i + 9, e - i - 9);
            i = e + 3;
            continue;
        }
        if (x[i + 1] == '/') {
            size_t e = x.find('>', i);
            if (e == std::string::npos || stack.size() < 2) return false;
            stack.back()->text = trim(stack.back()->text);
            stack.pop_back();
            i = e + 1;
            continue;
        }
        // start tag
        size_t j = i + 1;
        while (j < x.size() && !strchr(" \t\r\n/>", x[j])) j++;
        Node n;
        n.name = local(x.substr(i + 1, j - i - 1));
        bool self_close = false;
        while (j < x.size()) {
            while (j < x.size() && strchr(" \t\r\n", x[j])) j++;
            if (j >= x.size()) return false;
            if (x[j] == '>') { j++; break; }
            if (x[j] == '/') { self_close = true; j++; continue; }
            size_t k = j;
            while (k < x.size() && x[k] != '=' && !strchr(" \t\r\n/>", x[k])) k++;
            std::string an = x.substr(j, k - j);
            while (k < x.size() && strchr(" \t\r\n", x[k])) k++;
            std::string av;
            if (k < x.size() && x[k] == '=') {
                k++;
                while (k < x.size() && strchr(" \t\r\n", x[k])) k++;
                if (k < x.size() && (x[k] == '"' || x[k] == '\'')) {
                    char q = x[k];
                    size_t e = x.find(q, k + 1);
                    if (e == std::string::npos) return false;
                    av = unescape(x.substr(k + 1, e - k - 1));
                    k = e + 1;
                }
            }
            if (an.compare(0, 5, "xmlns") != 0) n.attr[local(an)] = av;
            j = k;
        }
        stack.back()->kids.push_back(std::move(n));
        if (!self_close) stack.push_back(&stack.back()->kids.back());
        i = j;
    }
    return stack.size() == 1;
}

const Node* find_first(const Node& n, const std::string& name) {
    for (const Node& k : n.kids) {
        if (k.name == name) return &k;
        if (const Node* r = find_first(k, name)) return r;
    }
    return nullptr;
}

void find_all(const Node& n, const std::string& name, std::vector<const Node*>& out) {
    for (const Node& k : n.kids) {
        if (k.name == name) out.push_back(&k);  // nested ones of the same name are not wanted here
        else find_all(k, name, out);
    }
}

std::string child_text(const Node& n, const std::string& name) {
    for (const Node& k : n.kids)
        if (k.name == name) return k.text;
    return "";
}

// ------------------------------------------------------------------ HTTP

struct HttpReply {
    bool ok = false;
    int status = 0;
    std::string body, error;
};

int now_ms() {
    return (int)(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() & 0x7fffffff);
}

HttpReply http_post(const std::string& host, int port, const std::string& path, const std::string& action, const std::string& body, int timeout_ms) {
    HttpReply r;
    addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) { r.error = "cannot resolve " + host; return r; }
    int fd = socket(res->ai_family, SOCK_STREAM, 0);
    if (fd < 0) { freeaddrinfo(res); r.error = "socket failed"; return r; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    int c = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (c < 0 && errno != EINPROGRESS) { close(fd); r.error = std::string("connect: ") + strerror(errno); return r; }
    pollfd p{fd, POLLOUT, 0};
    if (poll(&p, 1, timeout_ms) <= 0) { close(fd); r.error = "connect timed out"; return r; }
    int soerr = 0;
    socklen_t sl = sizeof soerr;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
    if (soerr) { close(fd); r.error = std::string("connect: ") + strerror(soerr); return r; }

    std::string req = "POST " + path + " HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) +
                      "\r\nContent-Type: application/soap+xml; charset=utf-8; action=\"" + action + "\"\r\nContent-Length: " +
                      std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    size_t sent = 0;
    int deadline = now_ms() + timeout_ms;
    while (sent < req.size()) {
        pollfd w{fd, POLLOUT, 0};
        if (poll(&w, 1, std::max(1, deadline - now_ms())) <= 0) { close(fd); r.error = "send timed out"; return r; }
        ssize_t n = send(fd, req.data() + sent, req.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) { close(fd); r.error = "send failed"; return r; }
        sent += (size_t)n;
    }
    std::string in;
    deadline = now_ms() + timeout_ms;
    for (;;) {
        pollfd rd{fd, POLLIN, 0};
        int left = deadline - now_ms();
        if (left <= 0 || poll(&rd, 1, left) <= 0) { close(fd); r.error = "answer timed out"; return r; }
        char buf[8192];
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n < 0) { if (errno == EAGAIN || errno == EINTR) continue; close(fd); r.error = "receive failed"; return r; }
        if (n == 0) break;
        in.append(buf, (size_t)n);
        deadline = now_ms() + timeout_ms;
        if (in.size() > (8u << 20)) break;
    }
    close(fd);
    size_t he = in.find("\r\n\r\n");
    if (he == std::string::npos || in.compare(0, 5, "HTTP/") != 0) { r.error = "not an HTTP answer"; return r; }
    std::string head = in.substr(0, he);
    r.status = atoi(head.substr(head.find(' ') + 1).c_str());
    std::string b = in.substr(he + 4);
    std::string lower = head;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return (char)tolower(ch); });
    if (lower.find("transfer-encoding: chunked") != std::string::npos) {
        std::string d;
        size_t pos = 0;
        while (pos < b.size()) {
            size_t e = b.find("\r\n", pos);
            if (e == std::string::npos) break;
            size_t len = strtoul(b.substr(pos, e - pos).c_str(), nullptr, 16);
            if (len == 0) break;
            d += b.substr(e + 2, len);
            pos = e + 2 + len + 2;
        }
        b = d;
    }
    r.body = b;
    r.ok = true;
    return r;
}

// ------------------------------------------------------------------ SOAP

std::string xml_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '"') o += "&quot;";
        else o += c;
    }
    return o;
}

struct Session {
    OnvifTarget t;
    int port = 0;
    long long clock_offset = 0;  // camera UTC minus ours, seconds
    bool digest = true;
    std::string log;
};

std::string iso_utc(time_t v) {
    char b[32];
    tm g{};
    gmtime_r(&v, &g);
    strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &g);
    return b;
}

std::string security_header(Session& s, bool with_auth) {
    if (!with_auth || s.t.user.empty()) return "";
    std::string nonce(16, 0);
    std::random_device rd;
    for (char& ch : nonce) ch = (char)(rd() & 0xFF);
    std::string created = iso_utc(time(nullptr) + (time_t)s.clock_offset);
    std::string pass_el;
    if (s.digest)
        pass_el = "<Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">" +
                  base64_encode(sha1_raw(nonce + created + s.t.password)) + "</Password>";
    else
        pass_el = "<Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordText\">" +
                  xml_escape(s.t.password) + "</Password>";
    return "<Security xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\" "
           "xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" s:mustUnderstand=\"1\"><UsernameToken><Username>" +
           xml_escape(s.t.user) + "</Username>" + pass_el +
           "<Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary\">" +
           base64_encode(nonce) +
           "</Nonce><Created xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\">" + created +
           "</Created></UsernameToken></Security>";
}

struct Answer {
    bool ok = false;
    bool auth_problem = false;
    std::string error;
    Node doc;
};

std::string fault_text(const Node& doc) {
    const Node* f = find_first(doc, "Fault");
    if (!f) return "";
    std::string t;
    if (const Node* x = find_first(*f, "Text")) t = x->text;
    if (t.empty()) t = child_text(*f, "faultstring");
    if (t.empty()) t = "SOAP fault";
    if (const Node* sub = find_first(*f, "Subcode"))
        if (const Node* v = find_first(*sub, "Value")) t += " [" + v->text + "]";
    return t;
}

// One SOAP call. A refused login with the digest is repeated once with the plain-text password.
Answer call(Session& s, const std::string& path, const std::string& action, const std::string& body_xml, bool with_auth = true) {
    for (int attempt = 0; attempt < 2; attempt++) {
        std::string env = "<?xml version=\"1.0\" encoding=\"utf-8\"?><s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"><s:Header>" +
                          security_header(s, with_auth) + "</s:Header><s:Body>" + body_xml + "</s:Body></s:Envelope>";
        HttpReply h = http_post(s.t.host, s.port, path, action, env, s.t.timeout_ms);
        Answer a;
        std::string short_action = action.substr(action.rfind('/') + 1);
        if (!h.ok) {
            s.log += "  " + short_action + " -> " + h.error + "\n";
            a.error = h.error;
            return a;
        }
        bool parsed = parse_xml(h.body, a.doc);
        std::string fault = parsed ? fault_text(a.doc) : "";
        s.log += "  " + short_action + " -> HTTP " + std::to_string(h.status) + (fault.empty() ? "" : ", fault: " + fault) +
                 (parsed ? "" : ", answer is not XML") + "\n";
        bool auth = h.status == 401 || fault.find("uthorized") != std::string::npos || fault.find("uthentication") != std::string::npos ||
                    fault.find("Sender") != std::string::npos;
        if (auth && attempt == 0 && with_auth && s.digest) {
            s.digest = false;
            s.log += "  (the camera refused the digest login; trying the plain-text password token)\n";
            continue;
        }
        if (!parsed) { a.error = "answer is not XML (HTTP " + std::to_string(h.status) + ")"; return a; }
        if (!fault.empty()) { a.error = fault; a.auth_problem = auth; return a; }
        if (h.status != 200) { a.error = "HTTP " + std::to_string(h.status); a.auth_problem = auth; return a; }
        a.ok = true;
        return a;
    }
    return Answer();
}

// Path of an address like http://192.168.1.10:8899/onvif/ptz_service; also the port when it has one.
void split_url(const std::string& u, std::string& path, int& port) {
    size_t p = u.find("://");
    size_t s = p == std::string::npos ? 0 : p + 3;
    size_t slash = u.find('/', s);
    path = slash == std::string::npos ? "/" : u.substr(slash);
    std::string hostport = u.substr(s, slash == std::string::npos ? std::string::npos : slash - s);
    size_t c = hostport.rfind(':');
    port = (c != std::string::npos && hostport.find(']') == std::string::npos) ? atoi(hostport.substr(c + 1).c_str()) : 0;
}

Preset parse_preset(const Node& n) {
    Preset p;
    auto it = n.attr.find("token");
    if (it != n.attr.end()) p.token = it->second;
    p.name = child_text(n, "Name");
    if (const Node* pos = find_first(n, "PTZPosition")) {
        if (const Node* pt = find_first(*pos, "PanTilt")) {
            p.has_position = true;
            auto x = pt->attr.find("x"), y = pt->attr.find("y");
            if (x != pt->attr.end()) p.pan = atof(x->second.c_str());
            if (y != pt->attr.end()) p.tilt = atof(y->second.c_str());
        }
        if (const Node* z = find_first(*pos, "Zoom")) {
            auto x = z->attr.find("x");
            if (x != z->attr.end()) { p.has_position = true; p.zoom = atof(x->second.c_str()); }
        }
    }
    return p;
}

const char* kDevicePath = "/onvif/device_service";

// Finds the port where the device service answers, reads the camera clock and the service addresses.
bool open_session(Session& s, OnvifResult& r) {
    for (int port : s.t.ports) {
        s.port = port;
        s.log += "port " + std::to_string(port) + ":\n";
        Answer a = call(s, kDevicePath, "http://www.onvif.org/ver10/device/wsdl/GetSystemDateAndTime",
                        "<GetSystemDateAndTime xmlns=\"http://www.onvif.org/ver10/device/wsdl\"/>", false);
        if (!a.ok) continue;
        if (const Node* u = find_first(a.doc, "UTCDateTime")) {
            const Node* d = find_first(*u, "Date");
            const Node* t = find_first(*u, "Time");
            if (d && t) {
                tm g{};
                g.tm_year = atoi(child_text(*d, "Year").c_str()) - 1900;
                g.tm_mon = atoi(child_text(*d, "Month").c_str()) - 1;
                g.tm_mday = atoi(child_text(*d, "Day").c_str());
                g.tm_hour = atoi(child_text(*t, "Hour").c_str());
                g.tm_min = atoi(child_text(*t, "Minute").c_str());
                g.tm_sec = atoi(child_text(*t, "Second").c_str());
                s.clock_offset = (long long)timegm(&g) - (long long)time(nullptr);
                s.log += "  camera clock differs by " + std::to_string(s.clock_offset) + " s\n";
            }
        }
        r.port = port;
        return true;
    }
    r.error = "no ONVIF device service answered on port(s) " ;
    for (size_t i = 0; i < s.t.ports.size(); i++) r.error += (i ? ", " : "") + std::to_string(s.t.ports[i]);
    return false;
}

}  // namespace

// ------------------------------------------------------------------ public

OnvifResult onvif_read_presets(const OnvifTarget& t) {
    OnvifResult r;
    Session s;
    s.t = t;
    if (!open_session(s, r)) { r.log = s.log; return r; }

    Answer caps = call(s, kDevicePath, "http://www.onvif.org/ver10/device/wsdl/GetCapabilities",
                       "<GetCapabilities xmlns=\"http://www.onvif.org/ver10/device/wsdl\"><Category>All</Category></GetCapabilities>");
    std::string media_path = "/onvif/media_service", ptz_path = "/onvif/ptz_service";
    if (!caps.ok) {
        r.error = "GetCapabilities failed: " + caps.error;
        r.log = s.log;
        return r;
    }
    auto service_path = [&](const char* group, std::string& path) {
        if (const Node* g = find_first(caps.doc, group)) {
            std::string x = child_text(*g, "XAddr");
            if (!x.empty()) {
                int p;
                split_url(x, path, p);
                s.log += std::string("  ") + group + " service at " + x + "\n";
            }
        }
    };
    service_path("Media", media_path);
    service_path("PTZ", ptz_path);
    r.media_url = media_path;
    r.ptz_url = ptz_path;

    Answer prof = call(s, media_path, "http://www.onvif.org/ver10/media/wsdl/GetProfiles", "<GetProfiles xmlns=\"http://www.onvif.org/ver10/media/wsdl\"/>");
    if (!prof.ok) {
        r.error = "GetProfiles failed: " + prof.error;
        r.log = s.log;
        return r;
    }
    std::vector<const Node*> pn;
    find_all(prof.doc, "Profiles", pn);
    for (const Node* n : pn) {
        Profile p;
        auto it = n->attr.find("token");
        if (it != n->attr.end()) p.token = it->second;
        p.name = child_text(*n, "Name");
        p.has_ptz = find_first(*n, "PTZConfiguration") != nullptr;
        s.log += "  profile " + p.token + " (" + p.name + "), PTZConfiguration: " + (p.has_ptz ? "yes" : "no") + "\n";
        if (!p.token.empty()) r.profiles.push_back(p);
    }
    std::string first_ptz;
    bool any_answer = false;
    std::string last_error;
    for (const Profile& p : r.profiles) {
        if (!p.has_ptz) continue;
        if (first_ptz.empty()) first_ptz = p.token;
        Answer pr = call(s, ptz_path, "http://www.onvif.org/ver20/ptz/wsdl/GetPresets",
                         "<GetPresets xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\"><ProfileToken>" + xml_escape(p.token) + "</ProfileToken></GetPresets>");
        if (!pr.ok) { last_error = pr.error; continue; }
        any_answer = true;
        std::vector<const Node*> list;
        find_all(pr.doc, "Preset", list);
        std::vector<Preset> got;
        for (const Node* n : list) got.push_back(parse_preset(*n));
        s.log += "  GetPresets(" + p.token + "): " + std::to_string(got.size()) + " preset(s)\n";
        for (const Preset& g : got) {
            char pos[96] = "";
            if (g.has_position) snprintf(pos, sizeof pos, " pan=%.3f tilt=%.3f zoom=%.3f", g.pan, g.tilt, g.zoom);
            s.log += "    token=\"" + g.token + "\" name=\"" + g.name + "\"" + pos + "\n";
        }
        if (!got.empty() && r.presets.empty()) { r.presets = got; r.profile_token = p.token; }
        else if (r.profile_token.empty()) r.profile_token = p.token;
    }
    r.log = s.log;
    if (first_ptz.empty()) { r.error = "none of the " + std::to_string(r.profiles.size()) + " media profile(s) has a PTZConfiguration"; return r; }
    if (!any_answer) { r.error = "GetPresets failed: " + last_error; return r; }
    r.ok = true;
    return r;
}

namespace {
// One PTZ-service call on a known port (device service -> PTZ address -> the call).
std::string ptz_action(const OnvifTarget& t, int port, const std::string& action, const std::string& body, std::string* log) {
    Session s;
    OnvifTarget tt = t;
    tt.ports = {port};
    s.t = tt;
    s.port = port;
    OnvifResult r;
    if (!open_session(s, r)) { if (log) *log = s.log; return r.error; }
    Answer caps = call(s, kDevicePath, "http://www.onvif.org/ver10/device/wsdl/GetCapabilities",
                       "<GetCapabilities xmlns=\"http://www.onvif.org/ver10/device/wsdl\"><Category>All</Category></GetCapabilities>");
    std::string ptz_path = "/onvif/ptz_service";
    if (caps.ok)
        if (const Node* g = find_first(caps.doc, "PTZ")) {
            std::string x = child_text(*g, "XAddr");
            int p;
            if (!x.empty()) split_url(x, ptz_path, p);
        }
    Answer a = call(s, ptz_path, "http://www.onvif.org/ver20/ptz/wsdl/" + action, body);
    if (log) *log = s.log;
    return a.ok ? "" : a.error;
}
}  // namespace

std::string onvif_remove_preset(const OnvifTarget& t, int port, const std::string& profile_token, const std::string& preset_token, std::string* log) {
    return ptz_action(t, port, "RemovePreset",
                      "<RemovePreset xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\"><ProfileToken>" + xml_escape(profile_token) +
                          "</ProfileToken><PresetToken>" + xml_escape(preset_token) + "</PresetToken></RemovePreset>",
                      log);
}

std::string onvif_set_preset(const OnvifTarget& t, int port, const std::string& profile_token, const std::string& preset_token,
                             const std::string& name, std::string* log) {
    return ptz_action(t, port, "SetPreset",
                      "<SetPreset xmlns=\"http://www.onvif.org/ver20/ptz/wsdl\"><ProfileToken>" + xml_escape(profile_token) +
                          "</ProfileToken><PresetName>" + xml_escape(name) + "</PresetName><PresetToken>" + xml_escape(preset_token) +
                          "</PresetToken></SetPreset>",
                      log);
}

}  // namespace ptz
