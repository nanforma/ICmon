// dvrip.h - DVRIP/Sofia control connection (TCP 34567) used by XM-based cameras.
//
// Layout of a packet (verified against a real XM530V200 camera, firmware V5.04.R02):
//   20-byte header: byte0 = 0xFF, bytes 4..7 session (LE), 8..11 sequence (LE), 14..15 message id (LE),
//                   16..19 payload length (LE); the other bytes are sent as zero.
//   payload: JSON followed by "\n\0" for commands; binary for data packets (message id 1426).
//
// Blocking API meant for a worker thread, never the GTK main thread. Every blocking call polls the
// CancelToken in <=200 ms slices. One Dvrip = one TCP connection; cancel a download by closing it.
//
// SECURITY: the login sends the Sofia hash of the password in clear on the network (the protocol has
// no encryption). The password is not stored in this object.
#pragma once
#include <cstdint>
#include <string>

#include "error.h"
#include "json.h"

namespace xm {

struct Packet {
    uint16_t id = 0;
    std::string payload;
};

class Dvrip {
public:
    Dvrip() = default;
    ~Dvrip() { close(); }
    Dvrip(const Dvrip&) = delete;
    Dvrip& operator=(const Dvrip&) = delete;

    // `cancel` is optional and must outlive the connection.
    void connect(const std::string& host, uint16_t port, int timeout_ms, const CancelToken* cancel = nullptr);
    // Throws Errc::Auth with the camera's Ret code when rejected (203 wrong password, 204 invalid user,
    // 205 locked out, 206 blacklisted). Do not retry blindly: 205 means the camera has locked the account.
    void login(const std::string& user, const std::string& password);
    void close();
    bool connected() const { return fd_ >= 0; }

    // Sends {Name..., SessionID} style JSON command (adds "SessionID" when logged in and absent).
    void send(uint16_t msg_id, Json body);
    // Reads one packet. idle_timeout_ms = max time without ANY byte arriving.
    Packet recv_packet(int idle_timeout_ms);
    // send + one reply; throws Errc::Camera if the reply's Ret is present and not 100.
    Json request(uint16_t msg_id, Json body, int idle_timeout_ms = 8000);

    static Json parse_reply(const Packet& p);  // strips the trailing "\n\0", parses JSON

private:
    void read_exact(void* buf, size_t n, int idle_ms);
    int fd_ = -1;
    uint32_t session_ = 0, seq_ = 0;
    std::string session_str_;
    const CancelToken* cancel_ = nullptr;
};

// Human-readable meaning of a login Ret code (from the public DVRIP table; 203 and 205 seen on a real camera).
std::string describe_ret(int ret);

}  // namespace xm
