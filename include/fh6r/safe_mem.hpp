#pragma once
#include <windows.h>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string>

namespace fh6r {
inline bool is_readable(const void* addr, std::size_t size) noexcept {
    if (!addr || !size) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD)) return false;
    constexpr DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                         PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & ok) == 0) return false;
    const auto* p = static_cast<const std::byte*>(addr);
    const auto* end = static_cast<const std::byte*>(mbi.BaseAddress) + mbi.RegionSize;
    return p + size <= end;
}

template <class T>
inline bool safe_read(const void* addr, T& out) noexcept {
    if (!is_readable(addr, sizeof(T))) return false;
    std::memcpy(&out, addr, sizeof(T));
    return true;
}

inline std::optional<std::string> safe_read_msvc_string(const void* addr,
                                                        std::size_t max_size = 4096) noexcept {
    struct Header { std::byte sbo[16]; std::uint64_t size; std::uint64_t cap; } h{};
    if (!safe_read(addr, h) || h.size > max_size) return std::nullopt;
    if (h.cap >= 16) {
        const void* data = nullptr;
        std::memcpy(&data, h.sbo, sizeof(data));
        if (!data || !is_readable(data, static_cast<std::size_t>(h.size))) return std::nullopt;
        std::string out(static_cast<std::size_t>(h.size), '\0');
        std::memcpy(out.data(), data, out.size());
        return out;
    }
    if (h.size > 16) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(h.sbo), static_cast<std::size_t>(h.size));
}

template <class Fn>
inline bool seh_call(Fn&& fn) noexcept {
    __try { fn(); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace fh6r
