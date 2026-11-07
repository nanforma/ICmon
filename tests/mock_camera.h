// TEST-ONLY mock camera. It lives in tests/ and is never linked into production targets.
// It models the DVRIP behaviour that was OBSERVED on one real camera (login, 64-entry query pages,
// 32 KiB data packets, empty packet = end, window starting mid-frame). It proves that our client
// handles those packets; it does not prove anything about other firmware or about the real camera.
#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "camtime.h"
#include "json.h"
#include "sofia_hash.h"
#include "stream_builder.h"

namespace xmtest {

struct MockFile {
    std::string name;
    xm::CamTime begin, end;
};

class MockCamera {
public:
    struct Opts {
        std::string user = "shield", pass = "secret";
        int lockout_after = 3;       // wrong logins before Ret=205
        bool endless = false;        // download never ends (for the cancel test)
        bool brace_fill = false;     // I-frame payload made of '{' so 32 KiB data packets start with 0x7b
        std::vector<MockFile> files;
    };

    explicit MockCamera(Opts o) : o_(std::move(o)) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = 0;
        bind(listen_fd_, (sockaddr*)&sa, sizeof sa);
        listen(listen_fd_, 8);
        socklen_t l = sizeof sa;
        getsockname(listen_fd_, (sockaddr*)&sa, &l);
        port_ = ntohs(sa.sin_port);
        acceptor_ = std::thread([this] { accept_loop(); });
    }
    ~MockCamera() {
        stop_ = true;
        if (acceptor_.joinable()) acceptor_.join();
        for (auto& t : conns_) t.join();
        ::close(listen_fd_);
    }
    uint16_t port() const { return port_; }
    int bad_logins() const { return bad_logins_; }
    std::string last_download_request() { std::lock_guard<std::mutex> g(mu_); return last_dl_; }
    int downloads_started() const { return dl_started_; }
    bool endless_sender_stopped() const { return endless_stopped_; }

private:
    Opts o_;
    int listen_fd_ = -1;
    uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread acceptor_;
    std::vector<std::thread> conns_;
    std::mutex mu_;
    std::string last_dl_;
    std::atomic<int> bad_logins_{0}, dl_started_{0};
    std::atomic<bool> endless_stopped_{false};

    void accept_loop() {
        while (!stop_) {
            pollfd p{listen_fd_, POLLIN, 0};
            if (poll(&p, 1, 50) <= 0) continue;
            int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd >= 0) conns_.emplace_back([this, fd] { serve(fd); ::close(fd); });
        }
    }

    bool rd(int fd, void* buf, size_t n) {
        size_t got = 0;
        while (got < n) {
            if (stop_) return false;
            pollfd p{fd, POLLIN, 0};
            if (poll(&p, 1, 50) <= 0) continue;
            ssize_t k = ::recv(fd, (char*)buf + got, n - got, 0);
            if (k <= 0) return false;
            got += (size_t)k;
        }
        return true;
    }
    bool wr(int fd, uint16_t id, const std::string& payload) {
        std::string h(20, '\0');
        h[0] = (char)0xFF;
        h[14] = (char)(id & 0xff); h[15] = (char)(id >> 8);
        for (int i = 0; i < 4; i++) h[16 + i] = (char)((payload.size() >> (8 * i)) & 0xff);
        std::string all = h + payload;
        size_t off = 0;
        while (off < all.size()) {
            ssize_t k = ::send(fd, all.data() + off, all.size() - off, MSG_NOSIGNAL);
            if (k <= 0) return false;
            off += (size_t)k;
        }
        return true;
    }
    bool wj(int fd, uint16_t id, const xm::Json& j) { return wr(fd, id, j.dump() + "\n" + std::string(1, '\0')); }

    static xm::Json ret(int r) { xm::Json j = xm::Json::object(); j.set("Ret", r); return j; }

    void serve(int fd) {
        bool logged = false;
        for (;;) {
            uint8_t h[20];
            if (!rd(fd, h, 20)) return;
            uint16_t id = (uint16_t)(h[14] | (h[15] << 8));
            uint32_t len = h[16] | (h[17] << 8) | (h[18] << 16) | ((uint32_t)h[19] << 24);
            std::string pl(len, '\0');
            if (len && !rd(fd, &pl[0], len)) return;
            while (!pl.empty() && (pl.back() == '\0' || pl.back() == '\n')) pl.pop_back();
            xm::Json j;
            try { j = xm::Json::parse(pl); } catch (...) { return; }

            if (id == 1000) {
                if (bad_logins_ >= o_.lockout_after) { wj(fd, 1001, ret(205)); continue; }
                if (j["UserName"].str() != o_.user) { bad_logins_++; wj(fd, 1001, ret(204)); continue; }
                if (j["PassWord"].str() != xm::sofia_hash(o_.pass)) { bad_logins_++; wj(fd, 1001, ret(203)); continue; }
                xm::Json r = ret(100);
                r.set("SessionID", "0x00000002").set("ChannelNum", 1);
                logged = true;
                wj(fd, 1001, r);
                continue;
            }
            if (!logged || j["SessionID"].str() != "0x00000002") { wj(fd, id + 1, ret(103)); continue; }
            if (id == 1440) { wj(fd, 1441, query(j)); continue; }
            if (id == 1424) { xm::Json r = ret(100); r.set("Name", "OPPlayBack"); wj(fd, 1425, r); continue; }
            if (id == 1420) {
                const xm::Json& pb = j["OPPlayBack"];
                if (pb["Action"].str() == "DownloadStart") { download(fd, j); }
                else return;  // DownloadStop: the camera side of the exchange is over
                continue;
            }
            wj(fd, id + 1, ret(103));
        }
    }

    xm::Json query(const xm::Json& j) {
        const xm::Json& q = j["OPFileQuery"];
        if (q["Event"].str() != "*") return ret(119);  // observed: the query is refused without "Event":"*"
        xm::CamTime b = xm::parse_cam_time(q["BeginTime"].str()), e = xm::parse_cam_time(q["EndTime"].str());
        std::vector<MockFile> sel;
        for (auto& f : o_.files) if (f.end > b && f.begin < e) sel.push_back(f);
        std::sort(sel.begin(), sel.end(), [](const MockFile& x, const MockFile& y) { return x.begin < y.begin; });
        xm::Json arr = xm::Json::array();
        for (size_t i = 0; i < sel.size() && i < 64; i++) {  // observed: at most 64 entries per reply
            xm::Json e2 = xm::Json::object();
            e2.set("BeginTime", xm::format_cam_time(sel[i].begin)).set("DiskNo", 0).set("EndTime", xm::format_cam_time(sel[i].end))
                .set("FileLength", "0x000053ba").set("FileName", sel[i].name).set("SerialNo", 0);
            arr.push(e2);
        }
        xm::Json r = ret(100);
        r.set("Name", "OPFileQuery").set("OPFileQuery", arr);
        return r;
    }

    void download(int fd, const xm::Json& j) {
        dl_started_++;
        { std::lock_guard<std::mutex> g(mu_); last_dl_ = j.dump(); }
        const xm::Json& pb = j["OPPlayBack"];
        xm::CamTime s = xm::parse_cam_time(pb["StartTime"].str()), e = xm::parse_cam_time(pb["EndTime"].str());
        const MockFile* mf = nullptr;
        for (auto& f : o_.files) if (f.name == pb["Parameter"]["FileName"].str()) mf = &f;
        if (!mf) { wj(fd, 1421, ret(11)); return; }
        // I-frames every 3 s from the file start. Start at the GOP containing s, end after the GOP containing e.
        std::string data;
        data += std::string(100, 'z');  // tail of a frame that began before the window: no marker inside
        data += P(300) + A(320) + P(300) + A(320);  // P-frames whose I-frame is not in the download
        for (xm::CamTime t = mf->begin + ((s - mf->begin) / 3) * 3; t <= e && t < mf->end; t += 3) data += gop(t, nullptr, 40000, o_.brace_fill ? '{' : 'x');
        if (o_.endless) {
            // endless: loop the data until the client goes away
            for (;;) {
                if (stop_) return;
                for (size_t i = 0; i < data.size(); i += 32768)
                    if (!wr(fd, 1426, data.substr(i, 32768))) { endless_stopped_ = true; return; }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        for (size_t i = 0; i < data.size(); i += 32768) wr(fd, 1426, data.substr(i, 32768));
        wr(fd, 1426, std::string());
    }
};

}  // namespace xmtest
