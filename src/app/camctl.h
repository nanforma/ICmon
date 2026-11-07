// camctl.h - commands to one camera over DVRIP: PTZ, presets, configuration read/write; and CamControl, a small
// worker that keeps one logged-in connection for such commands.
//
// SOURCE OF THE WIRE FORMAT: the message id 1400 and the "OPPTZControl" JSON below are taken from the open-source
// DVRIP clients (python-dvr, dvrip-rs), NOT from capture on this project's camera. They are UNVERIFIED here, and the
// user reported that another program's manual PTZ buttons did not work on this camera while presets did. The same
// holds for the configuration messages (1042 get, 1040 set) and the "Camera.WhiteLight" / "WorkMode" names.
// The functions only build the JSON and report the camera's own Ret code; they invent no result.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "dvrip.h"
#include "json.h"

namespace icmon {

// DVRIP message ids used here (from the open-source clients; see the note above).
constexpr uint16_t kMsgPtz = 1400, kMsgConfigSet = 1040, kMsgConfigGet = 1042;

// Body of an OPPTZControl request. `command` is e.g. DirectionUp, DirectionDown, DirectionLeft, DirectionRight,
// ZoomTile (zoom in), ZoomWide (zoom out), GotoPreset, SetPreset, ClearPreset. Movement: a "begin" message
// (Preset 65535) starts it and the same command with Preset -1 stops it. For presets `preset` is the preset number.
xm::Json make_ptz_body(const std::string& command, int channel, int step, int preset, bool begin_pattern);
// Convenience: pure-movement start / stop.
xm::Json make_ptz_move(const std::string& command, int channel, int step, bool start);
// Preset commands (GotoPreset / SetPreset / ClearPreset) with the preset number.
xm::Json make_ptz_preset(const std::string& command, int channel, int preset);

// Reads one configuration object by name (e.g. "Camera.WhiteLight"). Throws xm::Error. The reply is returned whole.
xm::Json config_get(xm::Dvrip& c, const std::string& name);
// Writes `value` as the configuration `name`. Throws xm::Error when the camera answers with a Ret other than 100.
void config_set(xm::Dvrip& c, const std::string& name, const xm::Json& value);
// Finds the object that holds the configuration `name` in a config_get() reply: reply[name], its first element when
// that is an array, else a null Json. (The exact reply shape is not verified on this camera.)
xm::Json config_object(const xm::Json& reply, const std::string& name);

// Copy of reply[name] (an object, or an array whose first object is changed) with `field` set to `value`; every other
// field and element is kept, so that writing it back changes nothing else. Null Json when reply[name] is neither.
xm::Json config_with(const xm::Json& reply, const std::string& name, const std::string& field, xm::Json value);

// One worker thread, one logged-in connection, jobs run in order. A failed login is remembered and never retried
// (repeated wrong logins lock the camera). A broken connection is re-made once per job (the login worked before).
class CamControl {
public:
    CamControl(std::string host, int port, std::string user, std::string password);
    ~CamControl();
    CamControl(const CamControl&) = delete;
    CamControl& operator=(const CamControl&) = delete;

    // `job` runs on the worker thread with a logged-in connection; it may throw xm::Error. `done` runs on the worker
    // thread afterwards with an empty text on success, else a short error. Callers marshal to the GUI thread.
    void post(std::function<void(xm::Dvrip&)> job, std::function<void(const std::string&)> done);
    bool rejected() const { return rejected_; }

private:
    void run();
    struct Item { std::function<void(xm::Dvrip&)> job; std::function<void(const std::string&)> done; };
    std::string host_, user_, password_;
    int port_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Item> q_;
    bool stop_ = false;
    std::atomic<bool> rejected_{false};
    xm::CancelToken cancel_;
    std::thread th_;
};

}  // namespace icmon
