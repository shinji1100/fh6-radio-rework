#pragma once
#include <atomic>
#include <cstdint>
#include <thread>

namespace fh6r {
class AudioRing;
class ConfigStore;
class WasapiCapture;
class AudioStateProbe;
namespace fmod { class DSPBridge; class Controller; }

namespace http {
class HttpServer {
public:
    HttpServer(std::uint16_t port, ConfigStore& config, AudioRing& ring,
               WasapiCapture& capture, fmod::DSPBridge& dsp, fmod::Controller& controller,
               AudioStateProbe& probe);
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    std::uint16_t port() const noexcept { return port_.load(std::memory_order_acquire); }
private:
    struct Impl;
    Impl* impl_ = nullptr;
    std::atomic<std::uint16_t> port_{0};
};
} // namespace http
} // namespace fh6r
