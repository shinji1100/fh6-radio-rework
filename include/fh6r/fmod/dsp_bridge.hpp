#pragma once
#include "fh6r/audio_ring.hpp"
#include "fh6r/fmod/pe_image.hpp"
#include "fh6r/fmod/radio_discovery.hpp"
#include <atomic>
#include <cstdint>

namespace fh6r::fmod {
struct FMODFns {
    using SystemCreateDSP = std::uint32_t (*)(void*, const void*, void**);
    using DSPRelease = std::uint32_t (*)(void*);
    using AddDSP = std::uint32_t (*)(std::uint64_t, std::int32_t, void*);
    using RemoveDSP = std::uint32_t (*)(std::uint64_t, void*);
    using HandleResolver = std::uint32_t (*)(std::uint32_t, void**, std::uint64_t*);
    using HandleUnlock = std::uint32_t (*)(std::uint64_t);
    SystemCreateDSP system_create_dsp = nullptr;
    DSPRelease dsp_release = nullptr;
    AddDSP add_dsp = nullptr;
    RemoveDSP remove_dsp = nullptr;
    HandleResolver resolver = nullptr;
    HandleUnlock unlock = nullptr;
    std::byte* host_base = nullptr;
    bool ready() const noexcept { return host_base && dsp_release && add_dsp && remove_dsp && resolver && unlock; }
};
bool resolve_fmod_signatures(const PEImage& img, FMODFns& out) noexcept;

struct DSPStats {
    bool attached = false;
    std::uint32_t channel_handle = 0;
    std::uint64_t callbacks = 0;
    std::uint64_t underrun_frames = 0;
    std::uint64_t rebuffer_events = 0;
    bool primed = false;
    std::uint32_t last_frames = 0;
    std::uint32_t last_channels = 0;
};

class DSPBridge {
public:
    DSPBridge(AudioRing& ring, FMODFns fns);
    ~DSPBridge();
    DSPBridge(const DSPBridge&) = delete;
    DSPBridge& operator=(const DSPBridge&) = delete;

    void set_target(const RadioInstance& inst, void* system) noexcept;
    void retarget_if_needed() noexcept;
    bool channel_alive(std::byte* stream) const noexcept;
    void detach() noexcept;
    void set_gain(float gain) noexcept;
    void set_native_stereo(bool on) noexcept { native_stereo_.store(on, std::memory_order_release); }
    DSPStats stats() const noexcept;

    static std::uint32_t __stdcall read_callback(void*, float*, float*, std::uint32_t,
                                                  std::int32_t, std::int32_t*);
private:
    bool validate(std::uint32_t handle) const noexcept;
    std::uint32_t live_handle(std::byte* stream) const noexcept;
    void install(std::uint32_t handle) noexcept;

    AudioRing& ring_;
    FMODFns fns_;
    void* system_ = nullptr;
    void* dsp_ = nullptr;
    std::byte* stream_ = nullptr;
    std::atomic<std::uint32_t> handle_{0};
    std::atomic<float> gain_{1.0f};
    std::atomic<bool> native_stereo_{false};
    std::atomic<std::uint64_t> callbacks_{0};
    std::atomic<std::uint64_t> underrun_frames_{0};
    std::atomic<std::uint64_t> rebuffer_events_{0};
    std::atomic<bool> primed_{false};
    std::atomic<std::uint32_t> last_frames_{0};
    std::atomic<std::uint32_t> last_channels_{0};
};
} // namespace fh6r::fmod
