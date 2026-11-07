#include "dvrip.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

#include "sofia_hash.h"

namespace xm {

namespace {
using Clock = std::chrono::steady_clock;

void put32(uint8_t* h, uint32_t v) { for (int i = 0; i < 4; i++) h[i] = (v >> (8 * i)) & 0xff; }
uint32_t get32(const uint8_t* h) { return h[0] | (h[1] << 8) | (h[2] << 16) | ((uint32_t)h[3] << 24); }
}  // namespace

std::string describe_ret(int ret) {
    switch (ret) {
        case 100: return "OK";
        case 103: return "illegal request or session expired";
        case 119: return "query refused (request incomplete)";
        case 203: return "wrong password";
        case 204: return "invalid user name";
        case 205: return "account locked out after failed logins";
        case 206: return "address blacklisted";
        default: return "unknown code";
    }
}

void Dvrip::connect(const std::string& host, uint16_t port, int timeout_ms, const CancelToken* cancel) {
    close();
    cancel_ = cancel;
    addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    int gai = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (gai != 0 || !res) throw Error(Errc::Connect, "cannot resolve " + host + ": " + gai_strerror(gai));
    std::string last = "no address";
    for (addrinfo* a = res; a; a = a->ai_next) {
        int fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) { last = strerror(errno); continue; }
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int r = ::connect(fd, a->ai_addr, a->ai_addrlen);
        bool ok = (r == 0);
        if (r < 0 && errno == EINPROGRESS) {
            auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
            for (;;) {
                if (cancel_ && cancel_->cancelled()) { ::close(fd); freeaddrinfo(res); throw Error(Errc::Cancelled, "cancelled"); }
                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
                if (left <= 0) { last = "timeout"; break; }
                pollfd p{fd, POLLOUT, 0};
                int pr = poll(&p, 1, (int)std::min<long long>(left, 200));
                if (pr < 0 && errno == EINTR) continue;
                if (pr == 0) continue;
                int err = 0; socklen_t el = sizeof err;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
                if (err) last = strerror(err); else ok = true;
                break;
            }
        } else if (!ok) last = strerror(errno);
        if (!ok) { ::close(fd); continue; }
        fcntl(fd, F_SETFL, fl);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        timeval tv{10, 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        fd_ = fd;
        break;
    }
    freeaddrinfo(res);
    if (fd_ < 0) throw Error(Errc::Connect, "cannot connect to " + host + ":" + std::to_string(port) + " (" + last + ")");
    session_ = 0; seq_ = 0; session_str_.clear();
}

void Dvrip::close() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

void Dvrip::login(const std::string& user, const std::string& password) {
    Json body = Json::object();
    body.set("EncryptType", "MD5").set("LoginType", "DVRIP-Web").set("PassWord", sofia_hash(password)).set("UserName", user);
    send(1000, body);
    Json rep = parse_reply(recv_packet(8000));
    int ret = (int)rep["Ret"].integer(-1);
    if (ret != 100) {
        throw Error(Errc::Auth, "login rejected: Ret=" + std::to_string(ret) + " (" + describe_ret(ret) + ")", ret);
    }
    session_str_ = rep["SessionID"].str();
    if (session_str_.empty()) throw Error(Errc::Protocol, "login reply has no SessionID");
    session_ = (uint32_t)rep["SessionID"].integer(0);
}

void Dvrip::send(uint16_t msg_id, Json body) {
    if (fd_ < 0) throw Error(Errc::Io, "not connected");
    if (!session_str_.empty() && body.is_object() && !body.contains("SessionID")) body.set("SessionID", session_str_);
    std::string payload = body.dump() + "\n";
    payload.push_back('\0');
    std::string pkt(20, '\0');
    uint8_t* h = (uint8_t*)&pkt[0];
    h[0] = 0xFF;
    put32(h + 4, session_);
    put32(h + 8, seq_++);
    h[14] = msg_id & 0xff; h[15] = msg_id >> 8;
    put32(h + 16, (uint32_t)payload.size());
    pkt += payload;
    size_t off = 0;
    while (off < pkt.size()) {
        ssize_t k = ::send(fd_, pkt.data() + off, pkt.size() - off, MSG_NOSIGNAL);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) throw Error(Errc::Io, std::string("send failed: ") + strerror(errno));
        off += (size_t)k;
    }
}

void Dvrip::read_exact(void* buf, size_t n, int idle_ms) {
    uint8_t* out = (uint8_t*)buf;
    size_t got = 0;
    auto deadline = Clock::now() + std::chrono::milliseconds(idle_ms);
    while (got < n) {
        if (cancel_ && cancel_->cancelled()) throw Error(Errc::Cancelled, "cancelled");
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) throw Error(Errc::Timeout, "no data from the camera for " + std::to_string(idle_ms / 1000.0) + " s");
        pollfd p{fd_, POLLIN, 0};
        int pr = poll(&p, 1, (int)std::min<long long>(left, 200));
        if (pr < 0) { if (errno == EINTR) continue; throw Error(Errc::Io, std::string("poll failed: ") + strerror(errno)); }
        if (pr == 0) continue;
        ssize_t k = ::recv(fd_, out + got, n - got, 0);
        if (k == 0) throw Error(Errc::Io, "connection closed by the camera");
        if (k < 0) { if (errno == EINTR || errno == EAGAIN) continue; throw Error(Errc::Io, std::string("recv failed: ") + strerror(errno)); }
        got += (size_t)k;
        deadline = Clock::now() + std::chrono::milliseconds(idle_ms);
    }
}

Packet Dvrip::recv_packet(int idle_timeout_ms) {
    if (fd_ < 0) throw Error(Errc::Io, "not connected");
    uint8_t h[20];
    read_exact(h, sizeof h, idle_timeout_ms);
    if (h[0] != 0xFF) throw Error(Errc::Protocol, "bad packet header (first byte is not 0xFF)");
    Packet p;
    p.id = (uint16_t)(h[14] | (h[15] << 8));
    uint32_t len = get32(h + 16);
    if (len > (16u << 20)) throw Error(Errc::Protocol, "implausible packet length " + std::to_string(len));
    p.payload.resize(len);
    if (len) read_exact(&p.payload[0], len, idle_timeout_ms);
    return p;
}

Json Dvrip::parse_reply(const Packet& p) {
    std::string s = p.payload;
    while (!s.empty() && (s.back() == '\0' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return Json::parse(s);
}

Json Dvrip::request(uint16_t msg_id, Json body, int idle_timeout_ms) {
    send(msg_id, std::move(body));
    Json rep = parse_reply(recv_packet(idle_timeout_ms));
    if (rep.contains("Ret") && rep["Ret"].integer(-1) != 100) {
        int ret = (int)rep["Ret"].integer(-1);
        throw Error(Errc::Camera, "camera answered Ret=" + std::to_string(ret) + " (" + describe_ret(ret) + ")", ret);
    }
    return rep;
}

}  // namespace xm
