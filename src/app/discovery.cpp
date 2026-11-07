#include "discovery.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include "dvrip.h"
#include "json.h"

namespace icmon {

std::string make_discover_request() {
    std::string p(20, '\0');
    p[0] = (char)0xFF;
    p[14] = (char)(1530 & 0xff);
    p[15] = (char)(1530 >> 8);
    return p;  // session 0, sequence 0, payload length 0
}

bool parse_discover_reply(const std::string& d, const std::string& sender_ip, FoundCamera& out, std::string& why) {
    if (d.size() <= 20 || (uint8_t)d[0] != 0xFF) { why = "not a DVRIP packet"; return false; }
    unsigned id = (uint8_t)d[14] | ((uint8_t)d[15] << 8);
    if (id != 1531) { why = "message id " + std::to_string(id) + " is not a discovery reply"; return false; }
    uint32_t len = 0;
    for (int i = 0; i < 4; i++) len |= (uint32_t)(uint8_t)d[16 + i] << (8 * i);
    if (len == 0 || 20 + (size_t)len > d.size()) { why = "payload length does not match the datagram"; return false; }
    try {
        xm::Packet pk;
        pk.id = (uint16_t)id;
        pk.payload = d.substr(20, len);
        xm::Json j = xm::Dvrip::parse_reply(pk);
        const xm::Json& n = j["NetWork.NetCommon"];
        if (!n.is_object()) { why = "no NetWork.NetCommon in the reply"; return false; }
        FoundCamera c;
        c.host = sender_ip;
        c.name = n["HostName"].str();
        c.mac = n["MAC"].str();
        c.serial = n["SN"].str();
        c.tcp_port = (int)n["TCPPort"].integer(34567);
        if (c.tcp_port < 1 || c.tcp_port > 65535) c.tcp_port = 34567;
        c.channels = (int)n["ChannelNum"].integer(0);
        out = std::move(c);
        return true;
    } catch (const xm::Error& e) {
        why = e.what();
        return false;
    }
}

std::vector<FoundCamera> discover(const DiscoverOptions& opt, const xm::CancelToken* cancel) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) throw xm::Error(xm::Errc::Io, std::string("cannot open a UDP socket: ") + strerror(errno));
    struct Closer { int fd; ~Closer() { ::close(fd); } } closer{fd};
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    sockaddr_in me{};
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = htonl(INADDR_ANY);
    me.sin_port = htons((uint16_t)opt.bind_port);
    if (::bind(fd, (sockaddr*)&me, sizeof me) < 0)
        throw xm::Error(xm::Errc::Io, "cannot listen on UDP port " + std::to_string(opt.bind_port) + ": " + strerror(errno) +
                                               " (another program may be using it)");
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t)opt.dest_port);
    if (inet_pton(AF_INET, opt.destination.c_str(), &to.sin_addr) != 1)
        throw xm::Error(xm::Errc::Io, "bad broadcast address " + opt.destination);
    std::string req = make_discover_request();
    if (::sendto(fd, req.data(), req.size(), 0, (sockaddr*)&to, sizeof to) < 0)
        throw xm::Error(xm::Errc::Io, std::string("cannot send the search request: ") + strerror(errno));

    std::vector<FoundCamera> found;
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(opt.timeout_ms);
    for (;;) {
        if (cancel && cancel->cancelled()) throw xm::Error(xm::Errc::Cancelled, "cancelled");
        long long left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
        if (left <= 0) break;
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, (int)std::min<long long>(left, 200)) <= 0) continue;
        char buf[4096];
        sockaddr_in from{};
        socklen_t fl = sizeof from;
        ssize_t n = ::recvfrom(fd, buf, sizeof buf, 0, (sockaddr*)&from, &fl);
        if (n <= 0) continue;
        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
        FoundCamera c;
        std::string why;
        if (!parse_discover_reply(std::string(buf, (size_t)n), ip, c, why)) continue;  // our own request or other traffic
        bool dup = std::any_of(found.begin(), found.end(), [&](const FoundCamera& f) { return f.host == c.host; });
        if (!dup) found.push_back(std::move(c));
    }
    return found;
}

}  // namespace icmon
