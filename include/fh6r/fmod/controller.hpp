#pragma once
#include "fh6r/fmod/dsp_bridge.hpp"
#include "fh6r/fmod/pe_image.hpp"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace fh6r::fmod {
struct ControllerStats {
    bool target_found = false;
    bool dsp_attached = false;
    std::string sound_name;
};

class Controller {
public:
    Controller(DSPBridge& bridge, const PEImage& image);
    ~Controller();
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;
    ControllerStats stats() const;
private:
    void run(std::stop_token stop) noexcept;
    bool discover_target() noexcept;

    DSPBridge& bridge_;
    const PEImage& image_;
    std::jthread thread_;
    mutable std::mutex mu_;
    bool target_found_ = false;
    std::string sound_name_;
};
} // namespace fh6r::fmod
