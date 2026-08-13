#pragma once
#include "fh6r/audio_ring.hpp"
#include "fh6r/cabin_dsp.hpp"
#include "fh6r/fmod/fmod_dsp_type.hpp"
#include "fh6r/fmod/pe_image.hpp"
#include "fh6r/fmod/radio_discovery.hpp"
#include <atomic>
#include <cstdint>
#include <string_view>

namespace fh6r::fmod {
// Minimal FMOD ABI structs needed by the audio-state probe. FMOD_VECTOR is
// {float x,y,z}; FMOD uses a plain 3-float layout on x64.
struct FMOD_VEC { float x = 0; float y = 0; float z = 0; };

struct FMODFns {
    using SystemCreateDSP = std::uint32_t (*)(void*, const void*, void**);
    using DSPRelease = std::uint32_t (*)(void*);
    using AddDSP = std::uint32_t (*)(std::uint64_t, std::int32_t, void*);
    using RemoveDSP = std::uint32_t (*)(std::uint64_t, void*);
    using HandleResolver = std::uint32_t (*)(std::uint32_t, void**, std::uint64_t*);
    using HandleUnlock = std::uint32_t (*)(std::uint64_t);
    // Phase 1 audio-state probe surface (resolved by anchor+LEA, one candidate each).
    using SysGet3DListenerAttrs = std::uint32_t (*)(void*, std::int32_t, FMOD_VEC*, FMOD_VEC*, FMOD_VEC*, FMOD_VEC*);
    using CCGetNumDSPs = std::uint32_t (*)(std::uint64_t, std::int32_t*);
    using CCGetDSP = std::uint32_t (*)(std::uint64_t, std::int32_t, void**);
    using DSPGetType = std::uint32_t (*)(void*, std::int32_t*);
    using DSPGetNumParameters = std::uint32_t (*)(void*, std::int32_t*);
    using DSPGetParameterFloat = std::uint32_t (*)(void*, std::int32_t, float*, char*, std::int32_t);
    using DSPGetParameterInt = std::uint32_t (*)(void*, std::int32_t, std::int32_t*, char*, std::int32_t);
    // DSP graph node introspection (partial: getInput/getOutput and all
    // DSPConnection getters are dead-code-eliminated in this build).
    using DSPGetInfo = std::uint32_t (*)(void*, char*, std::uint32_t*, std::int32_t*, std::int32_t*, std::int32_t*);
    using DSPGetNumInputs = std::uint32_t (*)(void*, std::int32_t*);
    // Cockpit reverb scout: channel-group tree traversal + convolution IR read.
    using SysGetMasterChannelGroup = std::uint32_t (*)(void*, void**);
    using GroupGetNumGroups = std::uint32_t (*)(void*, std::int32_t*);
    using GroupGetGroup = std::uint32_t (*)(void*, std::int32_t, void**);
    using GroupGetNumChannels = std::uint32_t (*)(void*, std::int32_t*);
    using GroupGetChannel = std::uint32_t (*)(void*, std::int32_t, void**);
    using DSPGetParameterData = std::uint32_t (*)(void*, std::int32_t, void**, std::uint32_t*, char*, std::int32_t);
    // DSP::getParameterInfo -> FMOD_DSP_PARAMETER_DESC** (triple-verify the
    // parameter layout: DATA/FLOAT/BOOL type per index).
    using DSPGetParameterInfo = std::uint32_t (*)(void*, std::int32_t, DspParamDesc**);
    // Radio acoustics identity probe. FH6's statically linked Core API uses
    // packed channel/sound handles for these two read-only getters.
    using ChannelGetCurrentSound = std::uint32_t (*)(std::uint64_t, std::uint64_t*);
    using SoundGetMode = std::uint32_t (*)(std::uint64_t, std::uint32_t*);
    SystemCreateDSP system_create_dsp = nullptr;
    DSPRelease dsp_release = nullptr;
    AddDSP add_dsp = nullptr;
    RemoveDSP remove_dsp = nullptr;
    HandleResolver resolver = nullptr;
    HandleUnlock unlock = nullptr;
    SysGet3DListenerAttrs get3d_listener_attrs = nullptr;
    CCGetNumDSPs get_num_dsps = nullptr;
    CCGetDSP get_dsp = nullptr;
    DSPGetType dsp_get_type = nullptr;
    DSPGetNumParameters dsp_get_num_parameters = nullptr;
    DSPGetParameterFloat dsp_get_parameter_float = nullptr;
    DSPGetParameterInt dsp_get_parameter_int = nullptr;
    DSPGetInfo dsp_get_info = nullptr;
    DSPGetNumInputs dsp_get_num_inputs = nullptr;
    SysGetMasterChannelGroup get_master_channel_group = nullptr;
    GroupGetNumGroups group_get_num_groups = nullptr;
    GroupGetGroup group_get_group = nullptr;
    GroupGetNumChannels group_get_num_channels = nullptr;
    GroupGetChannel group_get_channel = nullptr;
    DSPGetParameterData dsp_get_parameter_data = nullptr;
    DSPGetParameterInfo dsp_get_parameter_info = nullptr;
    ChannelGetCurrentSound channel_get_current_sound = nullptr;
    SoundGetMode sound_get_mode = nullptr;
    std::byte* host_base = nullptr;
    bool ready() const noexcept { return host_base && dsp_release && add_dsp && remove_dsp && resolver && unlock; }
    // Probe APIs are optional to audio injection; present iff each anchor resolved uniquely.
    bool probe_ready() const noexcept {
        return get3d_listener_attrs && get_num_dsps && get_dsp && dsp_get_type &&
               dsp_get_num_parameters && dsp_get_parameter_float;
    }
    bool cockpit_reverb_ready() const noexcept {
        return get_master_channel_group && group_get_num_groups && group_get_group &&
               get_num_dsps && get_dsp && dsp_get_type && dsp_get_parameter_data;
    }
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
    // Smoothed per-view parameters currently acting on the audio path.
    CabinDSP::AppliedView applied;
};

class DSPBridge {
public:
    DSPBridge(AudioRing& ring, FMODFns fns);
    ~DSPBridge();
    DSPBridge(const DSPBridge&) = delete;
    DSPBridge& operator=(const DSPBridge&) = delete;

    void set_target(const RadioInstance& inst, void* system) noexcept;
    void clear_target() noexcept;
    void retarget_if_needed() noexcept;
    bool channel_alive(std::byte* stream) const noexcept;
    void detach() noexcept;
    void set_gain(float gain) noexcept;
    void set_native_stereo(bool on) noexcept { native_stereo_.store(on, std::memory_order_release); }
    void set_spatial_audio(bool on) noexcept { spatial_audio_.store(on, std::memory_order_release); }
    void set_binaural(bool on) noexcept { cabin_.set_binaural(on); }
    void set_cabin_mode(CabinMode mode) noexcept { cabin_.set_mode(mode); }
    CabinMode cabin_mode() const noexcept { return cabin_.mode(); }
    bool set_cabin_profile(std::string_view name) noexcept;
    bool set_speaker_layout(std::string_view name) noexcept;
    void set_cabin_wet(float v) noexcept { cabin_.set_cabin_wet(v); }
    void set_driver_offset(float v) noexcept { cabin_.set_driver_offset(v); }
    void set_head_width_m(float v) noexcept { cabin_.set_head_width_m(v); }
    void set_cabin_openness(float v) noexcept { cabin_.set_openness(v); }
    DSPStats stats() const noexcept;

    // Probe access: the live channel handle, the FMOD System pointer and the
    // resolved function table. Valid while a target is attached (handle != 0).
    std::uint32_t channel_handle() const noexcept { return handle_.load(std::memory_order_acquire); }
    void* system() const noexcept { return system_; }
    const FMODFns& fns() const noexcept { return fns_; }

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
    std::atomic<bool> spatial_audio_{true};
    std::atomic<bool> cabin_reset_requested_{false};
    CabinDSP cabin_;
    std::atomic<std::uint64_t> callbacks_{0};
    std::atomic<std::uint64_t> underrun_frames_{0};
    std::atomic<std::uint64_t> rebuffer_events_{0};
    std::atomic<bool> primed_{false};
    std::atomic<std::uint32_t> last_frames_{0};
    std::atomic<std::uint32_t> last_channels_{0};
};
} // namespace fh6r::fmod
