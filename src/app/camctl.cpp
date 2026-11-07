#include "camctl.h"

#include <algorithm>

namespace icmon {

xm::Json make_ptz_body(const std::string& command, int channel, int step, int preset, bool begin_pattern) {
    xm::Json aux = xm::Json::object();
    aux.set("Number", 0).set("Status", "On");
    xm::Json point = xm::Json::object();
    point.set("bottom", 0).set("left", 0).set("right", 0).set("top", 0);
    xm::Json par = xm::Json::object();
    par.set("AUX", aux).set("Channel", channel).set("MenuOpts", "Enter").set("POINT", point)
        .set("Pattern", begin_pattern ? "SetBegin" : "Start").set("Preset", preset).set("Step", step)
        .set("Tour", command.find("Tour") != std::string::npos ? 1 : 0);
    xm::Json p = xm::Json::object();
    p.set("Command", command).set("Parameter", par);
    xm::Json body = xm::Json::object();
    body.set("Name", "OPPTZControl").set("OPPTZControl", p);
    return body;
}

xm::Json make_ptz_move(const std::string& command, int channel, int step, bool start) {
    return make_ptz_body(command, channel, step, start ? 65535 : -1, true);
}

xm::Json make_ptz_preset(const std::string& command, int channel, int preset) {
    return make_ptz_body(command, channel, 0, preset, false);
}

xm::Json config_get(xm::Dvrip& c, const std::string& name) {
    xm::Json body = xm::Json::object();
    body.set("Name", name);
    return c.request(kMsgConfigGet, body);
}

void config_set(xm::Dvrip& c, const std::string& name, const xm::Json& value) {
    xm::Json body = xm::Json::object();
    body.set("Name", name).set(name, value);
    c.request(kMsgConfigSet, body);  // throws Errc::Camera when Ret != 100
}

xm::Json config_object(const xm::Json& reply, const std::string& name) {
    const xm::Json& v = reply[name];
    if (v.is_array()) return v.items().empty() ? xm::Json() : v.items()[0];
    return v;
}

xm::Json config_with(const xm::Json& reply, const std::string& name, const std::string& field, xm::Json value) {
    const xm::Json& v = reply[name];
    if (v.is_array()) {
        xm::Json arr = xm::Json::array();
        for (size_t i = 0; i < v.items().size(); i++) {
            if (i == 0 && v.items()[0].is_object()) { xm::Json o = v.items()[0]; o.set(field, value); arr.push(o); }
            else arr.push(v.items()[i]);
        }
        return arr;
    }
    if (v.is_object()) { xm::Json o = v; o.set(field, value); return o; }
    return xm::Json();
}

CamControl::CamControl(std::string host, int port, std::string user, std::string password)
    : host_(std::move(host)), user_(std::move(user)), password_(std::move(password)), port_(port) {
    th_ = std::thread([this] { run(); });
}

CamControl::~CamControl() {
    {
        std::lock_guard<std::mutex> l(m_);
        stop_ = true;
    }
    cancel_.cancel();
    cv_.notify_all();
    if (th_.joinable()) th_.join();
    std::fill(password_.begin(), password_.end(), '\0');
}

void CamControl::post(std::function<void(xm::Dvrip&)> job, std::function<void(const std::string&)> done) {
    {
        std::lock_guard<std::mutex> l(m_);
        q_.push_back({std::move(job), std::move(done)});
    }
    cv_.notify_one();
}

void CamControl::run() {
    std::unique_ptr<xm::Dvrip> conn;
    for (;;) {
        Item it;
        {
            std::unique_lock<std::mutex> l(m_);
            cv_.wait(l, [this] { return stop_ || !q_.empty(); });
            if (stop_) return;
            it = std::move(q_.front());
            q_.pop_front();
        }
        std::string err;
        for (int attempt = 0; attempt < 2; attempt++) {
            err.clear();
            try {
                if (rejected_) { err = "The camera rejected the login earlier; fix the camera settings first."; break; }
                if (!conn || !conn->connected()) {
                    conn = std::make_unique<xm::Dvrip>();
                    conn->connect(host_, (uint16_t)port_, 4000, &cancel_);
                    conn->login(user_, password_);
                }
                it.job(*conn);
                break;
            } catch (const xm::Error& e) {
                err = e.what();
                if (e.code() == xm::Errc::Auth) { rejected_ = true; conn.reset(); break; }  // never retried
                if (e.code() == xm::Errc::Cancelled) break;
                bool command_refused = e.code() == xm::Errc::Camera;  // a Ret from the camera: the link is fine
                if (command_refused) break;
                conn.reset();  // broken link: log in again once
            }
        }
        if (it.done) it.done(err);
    }
}

}  // namespace icmon
