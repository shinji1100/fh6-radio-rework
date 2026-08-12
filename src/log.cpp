#include "fh6r/log.hpp"
#include <chrono>
#include <cstdio>
#include <mutex>

namespace fh6r::log {
namespace {
std::FILE* g_file = nullptr;
std::mutex g_mu;
constexpr std::string_view name(Level l) noexcept {
    switch (l) {
        case Level::trace: return "TRACE";
        case Level::info: return "INFO";
        case Level::warn: return "WARN";
        case Level::error: return "ERROR";
    }
    return "?";
}
}

void init(const std::filesystem::path& path) noexcept {
    std::scoped_lock lk{g_mu};
    if (g_file) std::fclose(g_file);
    g_file = _wfopen(path.c_str(), L"a");
}
void shutdown() noexcept {
    std::scoped_lock lk{g_mu};
    if (!g_file) return;
    std::fflush(g_file);
    std::fclose(g_file);
    g_file = nullptr;
}
void emit(Level level, std::string_view message) noexcept {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto t = system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    char ts[32]{};
    std::snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d.%03lld",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<long long>(ms));
    std::scoped_lock lk{g_mu};
    if (!g_file) return;
    const auto lvl = name(level);
    std::fprintf(g_file, "%s %-5.*s %.*s\n", ts, static_cast<int>(lvl.size()), lvl.data(),
                 static_cast<int>(message.size()), message.data());
    std::fflush(g_file);
}
} // namespace fh6r::log
