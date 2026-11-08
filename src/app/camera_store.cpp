#include <algorithm>
#include "camera_store.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <sstream>

#include "error.h"
#include "json.h"
#include "rtsp_url.h"

namespace icmon {

namespace fs = std::filesystem;

std::string validate_camera(const CameraConfig& c) {
    if (c.name.empty()) return "Enter a name for the camera.";
    if (c.host.empty()) return "Enter the camera address.";
    if (c.host.find_first_of(" \t/@") != std::string::npos) return "The camera address must be a host name or an IP address.";
    if (c.dvrip_port < 1 || c.dvrip_port > 65535) return "The archive port must be a number from 1 to 65535.";
    if (c.rtsp_port < 1 || c.rtsp_port > 65535) return "The RTSP port must be a number from 1 to 65535.";
    if (c.channel < 0 || c.channel > 64 || c.main_stream < 0 || c.main_stream > 16 || c.sub_stream < 0 || c.sub_stream > 16)
        return "Channel and stream numbers are out of range.";
    if (c.user.empty()) return "Enter the user name.";
    UrlResult u = build_rtsp_url(c.rtsp_template, c, "", c.main_stream);  // checks the template only
    if (!u.error.empty()) return u.error;
    return "";
}

std::string new_camera_id() {
    std::random_device rd;
    std::string s;
    for (int i = 0; i < 16; i++) s.push_back("0123456789abcdef"[rd() & 15]);
    return s;
}

std::string default_media_dir() {
    const char* h = std::getenv("HOME");
    return std::string(h && *h ? h : ".") + "/Downloads/ICmon_media";
}

std::string default_config_dir() {
    const char* x = std::getenv("XDG_CONFIG_HOME");
    const char* h = std::getenv("HOME");
    std::string base = (x && *x) ? std::string(x) : std::string(h && *h ? h : ".") + "/.config";
    return base + "/ICmon";
}

namespace {

xm::Json to_json(const CameraConfig& c) {
    xm::Json o = xm::Json::object();
    o.set("id", c.id).set("name", c.name).set("host", c.host).set("user", c.user);
    o.set("dvrip_port", c.dvrip_port).set("rtsp_port", c.rtsp_port).set("channel", c.channel);
    o.set("main_stream", c.main_stream).set("sub_stream", c.sub_stream);
    o.set("use_sub_in_grid", c.use_sub_in_grid).set("no_password", c.no_password).set("rtsp_template", c.rtsp_template);
    o.set("ptz_flip_h", c.ptz_flip_h).set("ptz_flip_v", c.ptz_flip_v).set("slot", c.slot);
    xm::Json pr = xm::Json::array();
    for (int n : c.presets) pr.push(n);
    o.set("presets", std::move(pr));
    return o;
}

CameraConfig from_json(const xm::Json& j) {
    CameraConfig c;
    c.id = j["id"].str();
    c.name = j["name"].str();
    c.host = j["host"].str();
    c.user = j["user"].str(c.user);
    c.dvrip_port = (int)j["dvrip_port"].integer(c.dvrip_port);
    c.rtsp_port = (int)j["rtsp_port"].integer(c.rtsp_port);
    c.channel = (int)j["channel"].integer(c.channel);
    c.main_stream = (int)j["main_stream"].integer(c.main_stream);
    c.sub_stream = (int)j["sub_stream"].integer(c.sub_stream);
    c.use_sub_in_grid = j["use_sub_in_grid"].boolean();
    c.no_password = j["no_password"].boolean();
    c.rtsp_template = j["rtsp_template"].str();
    c.ptz_flip_h = j["ptz_flip_h"].boolean(false);
    c.ptz_flip_v = j["ptz_flip_v"].boolean(false);
    c.slot = (int)j["slot"].integer(-1);
    for (const xm::Json& p : j["presets"].items()) {
        int n = (int)p.integer(0);
        if (n >= 1 && n <= 255 && std::find(c.presets.begin(), c.presets.end(), n) == c.presets.end()) c.presets.push_back(n);
    }
    return c;
}

}  // namespace

int first_free_slot(const StoreData& d) {
    for (int s = 0;; s++) {
        bool used = false;
        for (const CameraConfig& c : d.cameras) used = used || c.slot == s;
        if (!used) return s;
    }
}

void normalize_slots(StoreData& d) {
    std::set<int> used;
    for (CameraConfig& c : d.cameras) {
        if (c.slot >= 0 && used.insert(c.slot).second) continue;
        c.slot = -1;
    }
    for (CameraConfig& c : d.cameras) {
        if (c.slot >= 0) continue;
        int s = 0;
        while (used.count(s)) s++;
        c.slot = s;
        used.insert(s);
    }
}

void insert_by_slot(StoreData& d, const CameraConfig& c) {
    size_t pos = 0;
    for (size_t i = 0; i < d.cameras.size(); i++)
        if (d.cameras[i].slot < c.slot) pos = i + 1;
    d.cameras.insert(d.cameras.begin() + (long)pos, c);
}

LoadResult load_store(const std::string& path) {
    LoadResult r;
    std::error_code ec;
    if (!fs::exists(path, ec)) return r;
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    if (!in && !in.eof()) { r.error = "Cannot read " + path; return r; }
    try {
        xm::Json j = xm::Json::parse(ss.str());
        if (!j.is_object() || !j["cameras"].is_array()) throw xm::Error(xm::Errc::Protocol, "not a camera list");
        std::set<std::string> ids;
        int n = 0;
        for (const xm::Json& e : j["cameras"].items()) {
            n++;
            CameraConfig c = from_json(e);
            if (c.id.empty()) c.id = new_camera_id();
            std::string why = validate_camera(c);
            if (why.empty() && !ids.insert(c.id).second) why = "duplicate id";
            if (!why.empty()) { r.notes.push_back("Camera entry " + std::to_string(n) + " skipped: " + why); continue; }
            r.data.cameras.push_back(std::move(c));
        }
        int layout = (int)j["layout"].integer(1);
        r.data.layout = (layout == 2 || layout == 4 || layout == 16) ? layout : 1;
        r.data.selected_id = j["selected_id"].str();
        r.data.media_dir = j["media_dir"].str();
        r.data.language = j["language"].str();
        r.data.start_maximized = j["start_maximized"].boolean(false);
        r.data.hw_decoding = j["hw_decoding"].boolean(false);
        normalize_slots(r.data);
    } catch (const xm::Error& e) {
        r.data = StoreData();
        r.error = std::string("The camera list ") + path + " is damaged (" + e.what() + "). It was renamed to .bad";
        fs::rename(path, path + ".bad", ec);  // keep it for the user, and never overwrite it by saving
    }
    return r;
}

bool read_hw_decoding(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    try {
        return xm::Json::parse(ss.str())["hw_decoding"].boolean(false);
    } catch (const xm::Error&) {
        return false;
    }
}

std::string read_language(const std::string& path) {
    std::ifstream in(path);
    if (!in) return "";
    std::stringstream ss;
    ss << in.rdbuf();
    try {
        return xm::Json::parse(ss.str())["language"].str();
    } catch (const xm::Error&) {
        return "";
    }
}

std::string save_store(const std::string& path, const StoreData& d) {
    xm::Json j = xm::Json::object();
    j.set("version", 1).set("layout", d.layout).set("selected_id", d.selected_id).set("media_dir", d.media_dir).set("language", d.language).set("start_maximized", d.start_maximized).set("hw_decoding", d.hw_decoding);
    xm::Json arr = xm::Json::array();
    for (const CameraConfig& c : d.cameras) arr.push(to_json(c));
    j.set("cameras", std::move(arr));

    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    if (ec) return "Cannot create " + fs::path(path).parent_path().string() + ": " + ec.message();
    std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << j.dump() << "\n";
        out.flush();
        if (!out) return "Cannot write " + tmp;
    }
    fs::rename(tmp, path, ec);
    if (ec) return "Cannot replace " + path + ": " + ec.message();
    return "";
}

}  // namespace icmon
