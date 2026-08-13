#pragma once

#include "fh6r/fmod/pe_image.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <unordered_set>

#include <windows.h>

namespace fh6r {

// Temporary runtime RE probe. On the one validated FH6 build, arm x64 hardware
// data breakpoints on the proven interior/exterior camera flag and record only
// value-changing writes. The probe never modifies game state and fails closed
// on every other executable fingerprint.
class CameraReProbe {
public:
    explicit CameraReProbe(const fmod::PEImage& image) noexcept;
    ~CameraReProbe();

    CameraReProbe(const CameraReProbe&) = delete;
    CameraReProbe& operator=(const CameraReProbe&) = delete;

private:
    struct Event {
        DWORD thread_id = 0;
        std::uint32_t previous_value = 0;
        std::uint32_t current_value = 0;
        std::uint8_t slot = 0;
        std::uintptr_t trap_rip = 0;
        std::uint64_t rflags = 0;
        std::uint64_t dr6 = 0;
        std::uint64_t dr7 = 0;
        std::array<std::uint64_t, 16> regs{};
    };

    struct EventSlot {
        volatile LONG ready = 0;
        Event event{};
    };

    static constexpr std::size_t kMaxEvents = 128;

    static LONG CALLBACK vectored_handler(EXCEPTION_POINTERS* pointers) noexcept;
    LONG handle_exception(EXCEPTION_POINTERS* pointers) noexcept;

    void worker(std::stop_token stop) noexcept;
    void arm_new_threads();
    bool arm_thread(DWORD thread_id) noexcept;
    void disarm_all_threads() noexcept;
    void flush_events();

    std::uintptr_t debug_register_value(const CONTEXT& ctx, unsigned slot) const noexcept;
    static void set_debug_register_value(CONTEXT& ctx, unsigned slot, std::uintptr_t value) noexcept;

    const fmod::PEImage* image_ = nullptr;
    std::uintptr_t target_ = 0;
    PVOID veh_handle_ = nullptr;
    std::jthread worker_;
    DWORD worker_thread_id_ = 0;
    std::unordered_set<DWORD> armed_threads_;
    volatile LONG enabled_ = 0;
    volatile LONG last_value_ = 0;
    volatile LONG next_event_ = 0;
    std::array<EventSlot, kMaxEvents> events_{};
    std::size_t next_flush_ = 0;
};

} // namespace fh6r
