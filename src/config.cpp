#include "fh6r/config.hpp"
#include "fh6r/log.hpp"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <cmath>
#include <string_view>

namespace fh6r {
namespace {
std::string trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}
bool parse_bool(std::string_view s, bool fallback) {
    if (s == "1" || s == "true" || s == "on") return true;
    if (s == "0" || s == "false" || s == "off") return false;
    return fallback;
}
}

ConfigStore::ConfigStore(std::filesystem::path path) : path_{std::move(path)} {}
ConfigSnapshot ConfigStore::load() {
    ConfigSnapshot loaded;
    std::ifstream in(path_, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        auto key = trim(line.substr(0, pos));
        auto value = trim(line.substr(pos + 1));
        if (key.empty() || key.starts_with('#') || key.starts_with(';')) continue;
        if (key == "endpoint_id") loaded.endpoint_id = value;
        else if (key == "native_stereo") loaded.native_stereo = parse_bool(value, loaded.native_stereo);
        else if (key == "gain") {
            try {
                const float g = std::stof(value);
                if (std::isfinite(g)) loaded.gain = std::clamp(g, 0.0f, 2.0f);
            } catch (...) {}
        } else if (key == "dashboard_port") {
            try {
                const auto p = std::stoul(value);
                if (p >= 1024 && p <= 65535) loaded.dashboard_port = static_cast<std::uint16_t>(p);
            } catch (...) {}
        }
    }
    {
        std::scoped_lock lk{mu_};
        cfg_ = loaded;
    }
    return loaded;
}
ConfigSnapshot ConfigStore::snapshot() const { std::scoped_lock lk{mu_}; return cfg_; }
void ConfigStore::set_endpoint(std::string endpoint) { std::scoped_lock lk{mu_}; cfg_.endpoint_id = std::move(endpoint); }
void ConfigStore::set_gain(float gain) {
    if (!std::isfinite(gain)) return;
    std::scoped_lock lk{mu_};
    cfg_.gain = std::clamp(gain, 0.0f, 2.0f);
}
void ConfigStore::set_native_stereo(bool enabled) { std::scoped_lock lk{mu_}; cfg_.native_stereo = enabled; }
bool ConfigStore::save() const {
    ConfigSnapshot s;
    { std::scoped_lock lk{mu_}; s = cfg_; }
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    const auto tmp = path_.wstring() + L".tmp";
    std::ofstream out(std::filesystem::path{tmp}, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << "# FH6 Radio Rework v0.1\n"
        << "# endpoint_id blank = Windows default playback device\n"
        << "endpoint_id=" << s.endpoint_id << "\n"
        << "gain=" << s.gain << "\n"
        << "native_stereo=" << (s.native_stereo ? "true" : "false") << "\n"
        << "dashboard_port=" << s.dashboard_port << "\n";
    out.flush();
    if (!out) return false;
    out.close();
    std::filesystem::rename(std::filesystem::path{tmp}, path_, ec);
    if (ec) {
        std::filesystem::remove(path_, ec);
        ec.clear();
        std::filesystem::rename(std::filesystem::path{tmp}, path_, ec);
    }
    if (ec) {
        log::warn("[config] save failed: {}", ec.message());
        return false;
    }
    return true;
}
} // namespace fh6r
