#pragma once

#include "fh6r/fmod/pe_image.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

namespace fh6r {

// Temporary runtime RE probe.
//
// Safety model:
//   * Constructor is dormant: no VEH, no debug registers, no SuspendThread.
//   * A session begins only after camera_re_arm.txt is created in
//     <game>\fh6-radio-rework\.
//   * Existing process threads are armed exactly once for that session.
//   * No periodic thread enumeration/re-arming occurs while armed.
//   * The session auto-disarms after two value-changing hits or 20 seconds.
//   * Threads with any pre-existing enabled hardware breakpoint are skipped.
//   * Original DR0/DR6/DR7 values are restored on disarm.
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

    struct ArmedThread {
        DWORD thread_id = 0;
        std::uint64_t original_dr0 = 0;
        std::uint64_t original_dr6 = 0;
        std::uint64_t original_dr7 = 0;
    };

    static constexpr std::size_t kMaxEvents = 16;

    static LONG CALLBACK vectored_handler(EXCEPTION_POINTERS* pointers) noexcept;
    LONG handle_exception(EXCEPTION_POINTERS* pointers) noexcept;

    void worker(std::stop_token stop) noexcept;
    bool consume_trigger() noexcept;
    bool begin_session() noexcept;
    void end_session(const char* reason) noexcept;

    std::size_t arm_existing_threads_once() noexcept;
    bool arm_thread(DWORD thread_id, ArmedThread& saved) noexcept;
    void disarm_armed_threads() noexcept;
    void flush_events();

    const fmod::PEImage* image_ = nullptr;
    std::uintptr_t target_ = 0;
    std::wstring trigger_path_;

    PVOID veh_handle_ = nullptr;
    std::jthread worker_;
    DWORD worker_thread_id_ = 0;
    std::vector<ArmedThread> armed_threads_;

    volatile LONG enabled_ = 0;
    volatile LONG last_value_ = 0;
    volatile LONG next_event_ = 0;

    std::array<EventSlot, kMaxEvents> events_{};
    std::size_t next_flush_ = 0;
};

} // namespace fh6r
