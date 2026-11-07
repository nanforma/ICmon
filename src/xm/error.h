// error.h - error type and cancellation token shared by the whole protocol layer (no GUI dependencies).
#pragma once
#include <atomic>
#include <stdexcept>
#include <string>

namespace xm {

enum class Errc {
    Connect,    // TCP connection could not be made
    Io,         // connection broke / send failed
    Timeout,    // nothing received for too long
    Cancelled,  // CancelToken was set
    Protocol,   // reply does not look like the documented/observed protocol
    Auth,       // login rejected (ret() holds the camera's Ret code)
    Camera,     // camera answered with a Ret code other than 100
};

class Error : public std::runtime_error {
public:
    Error(Errc c, const std::string& msg, int ret = 0) : std::runtime_error(msg), code_(c), ret_(ret) {}
    Errc code() const { return code_; }
    int ret() const { return ret_; }  // camera Ret code, 0 if not applicable
private:
    Errc code_;
    int ret_;
};

// Set from any thread; blocking calls in the protocol layer poll it in <=200 ms slices.
class CancelToken {
public:
    void cancel() { flag_.store(true); }
    bool cancelled() const { return flag_.load(); }
private:
    std::atomic<bool> flag_{false};
};

}  // namespace xm
