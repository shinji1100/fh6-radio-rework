#pragma once
#include "fh6r/audio_ring.hpp"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fh6r {
struct AudioDeviceInfo {
    std::string id;
    std::string name;
    bool is_default = false;
};

struct CaptureStats {
    bool worker_alive = false;
    bool running = false;
    std::uint32_t input_rate = 0;
    std::uint16_t input_channels = 0;
    std::uint64_t packets = 0;
    std::uint64_t frames_captured = 0;
    std::uint64_t discontinuities = 0;
    std::uint64_t event_timeouts = 0;
    std::string device_name;
    std::string last_error;
};

std::vector<AudioDeviceInfo> enumerate_audio_devices();

class WasapiCapture {
public:
    explicit WasapiCapture(AudioRing& ring);
    ~WasapiCapture();
    WasapiCapture(const WasapiCapture&) = delete;
    WasapiCapture& operator=(const WasapiCapture&) = delete;

    void set_endpoint(std::string endpoint);
    std::string endpoint() const;
    bool start();
    void stop() noexcept;
    bool restart();
    CaptureStats stats() const;

private:
    void run() noexcept;
    void set_error(std::string msg) noexcept;

    AudioRing& ring_;
    mutable std::mutex mu_;
    std::string endpoint_;
    std::string device_name_;
    std::string last_error_;
    std::thread thread_;
    void* stop_event_ = nullptr; // HANDLE, kept opaque in the header

    std::atomic<bool> worker_alive_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::uint32_t> input_rate_{0};
    std::atomic<std::uint16_t> input_channels_{0};
    std::atomic<std::uint64_t> packets_{0};
    std::atomic<std::uint64_t> frames_captured_{0};
    std::atomic<std::uint64_t> discontinuities_{0};
    std::atomic<std::uint64_t> event_timeouts_{0};
};
} // namespace fh6r
