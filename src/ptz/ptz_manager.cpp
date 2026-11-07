#include "ptz_manager.h"

#include <algorithm>
#include <thread>

namespace ptz {

std::vector<int> default_onvif_ports() { return {8899, 80, 8080, 2020}; }

void PtzManager::refresh_presets(const std::string& id, OnvifTarget target, ReadDone done) {
    {
        std::lock_guard<std::mutex> l(m_);
        auto it = cache_.find(id);
        if (it != cache_.end() && it->second.port > 0) {  // the port that answered last time is tried first
            auto& p = target.ports;
            p.erase(std::remove(p.begin(), p.end(), it->second.port), p.end());
            p.insert(p.begin(), it->second.port);
        }
    }
    std::thread([this, id, target, done]() mutable {
        OnvifResult r = onvif_read_presets(target);
        std::fill(target.password.begin(), target.password.end(), '\0');
        if (r.ok) {
            std::lock_guard<std::mutex> l(m_);
            Entry& e = cache_[id];
            e.port = r.port;
            e.profile_token = r.profile_token;
            e.presets = r.presets;
        }
        if (done) done(r);
    }).detach();
}

void PtzManager::delete_preset(const std::string& id, OnvifTarget target, const std::string& token, RemoveDone done) {
    Entry e;
    bool known = false;
    {
        std::lock_guard<std::mutex> l(m_);
        auto it = cache_.find(id);
        if (it != cache_.end()) {
            e = it->second;
            for (const Preset& p : e.presets) known = known || p.token == token;
        }
    }
    if (!known || e.port <= 0 || e.profile_token.empty()) {
        if (done) done("this position was not read from the camera", "");
        return;
    }
    std::thread([target, e, token, done]() mutable {
        std::string log;
        std::string err = onvif_remove_preset(target, e.port, e.profile_token, token, &log);
        std::fill(target.password.begin(), target.password.end(), '\0');
        if (done) done(err, log);
    }).detach();
}

void PtzManager::save_preset(const std::string& id, OnvifTarget target, const std::string& token, RemoveDone done) {
    Entry e;
    {
        std::lock_guard<std::mutex> l(m_);
        auto it = cache_.find(id);
        if (it != cache_.end()) e = it->second;
    }
    if (e.port <= 0 || e.profile_token.empty()) {
        if (done) done("the camera's presets were not read yet", "");
        return;
    }
    std::thread([target, e, token, done]() mutable {
        std::string log;
        std::string err = onvif_set_preset(target, e.port, e.profile_token, token, token, &log);
        std::fill(target.password.begin(), target.password.end(), '\0');
        if (done) done(err, log);
    }).detach();
}

std::vector<Preset> PtzManager::get_presets(const std::string& id) const {
    std::lock_guard<std::mutex> l(m_);
    auto it = cache_.find(id);
    return it == cache_.end() ? std::vector<Preset>() : it->second.presets;
}

bool PtzManager::token_for(const std::string& id, const std::string& token, Preset* out) const {
    std::lock_guard<std::mutex> l(m_);
    auto it = cache_.find(id);
    if (it == cache_.end()) return false;
    for (const Preset& p : it->second.presets)
        if (p.token == token) { if (out) *out = p; return true; }
    return false;
}

}  // namespace ptz
