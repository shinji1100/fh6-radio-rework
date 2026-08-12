#pragma once
#include <filesystem>
#include <format>
#include <string_view>
#include <utility>

namespace fh6r::log {
enum class Level { trace, info, warn, error };
void init(const std::filesystem::path& path) noexcept;
void shutdown() noexcept;
void emit(Level level, std::string_view message) noexcept;

template <class... Args>
inline void info(std::format_string<Args...> fmt, Args&&... args) noexcept {
    try { emit(Level::info, std::format(fmt, std::forward<Args>(args)...)); } catch (...) {}
}
template <class... Args>
inline void warn(std::format_string<Args...> fmt, Args&&... args) noexcept {
    try { emit(Level::warn, std::format(fmt, std::forward<Args>(args)...)); } catch (...) {}
}
template <class... Args>
inline void error(std::format_string<Args...> fmt, Args&&... args) noexcept {
    try { emit(Level::error, std::format(fmt, std::forward<Args>(args)...)); } catch (...) {}
}
template <class... Args>
inline void trace(std::format_string<Args...> fmt, Args&&... args) noexcept {
    try { emit(Level::trace, std::format(fmt, std::forward<Args>(args)...)); } catch (...) {}
}
} // namespace fh6r::log
