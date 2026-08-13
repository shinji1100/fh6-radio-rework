#include "fh6r/fmod/dsp_bridge.hpp"
#include "fh6r/fmod/sig_scanner.hpp"
#include "fh6r/dsp_contract.hpp"
#include "fh6r/log.hpp"
#include "fh6r/safe_mem.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace fh6r::fmod {
namespace {
struct Sig { const char* anchor; const char* pattern; };
constexpr Sig kSigs[] = {
    {"System::createDSP", "4C 8B DC 56 48 81 EC 70 01 00 00|40 53 55 56 57 41 56 48 81 EC 50 01 00 00"},
    {"DSP::release", "48 89 5C 24 10 57 48 81 EC 50 01 00 00"},
    {"ChannelControl::addDSP", "4C 8B DC 56 48 81 EC 70 01 00 00|40 53 55 56 57 41 56 48 81 EC 50 01 00 00"},
    {"ChannelControl::removeDSP", "48 89 5C 24 18 48 89 74 24 20 57 48 81 EC 50 01 00 00"},
};
constexpr const char* kResolver =
    "48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 20 8B F9 "
    "8B C1 C1 EF 11 49 8B F0 D1 E8 81 E7 FF 0F 00 00 0F B7 E8 4C 8B F2 4C 8B F9";
constexpr const char* kUnlock = "48 8B 89 F0 09 01 00 48 85 C9 0F 85 ?? ?? ?? ?? 33 C0 C3";

DSPBridge* g_bridge = nullptr;
struct FMOD_DSP_DESCRIPTION {
    std::uint32_t pluginsdkversion;
    char name[32];
    std::uint32_t version;
    std::int32_t numinputbuffers;
    std::int32_t numoutputbuffers;
    void* create; void* release; void* reset; void* read; void* process; void* setposition;
    std::int32_t numparameters; std::int32_t padding;
    void* paramdesc; void* setparamfloat; void* setparamint; void* setparambool; void* setparamdata;
    void* getparamfloat; void* getparamint; void* getparambool; void* getparamdata;
    void* shouldiprocess; void* userdata; void* sys_register; void* sys_deregister; void* sys_mix;
};
static_assert(sizeof(FMOD_DSP_DESCRIPTION) == 216);
FMODFns::SystemCreateDSP resolve_create(const PEImage& img) noexcept {
    return reinterpret_cast<FMODFns::SystemCreateDSP>(find_by_anchor(img, kSigs[0].anchor, kSigs[0].pattern));
}
}

bool resolve_fmod_signatures(const PEImage& img, FMODFns& out) noexcept {
    if (!img.valid()) return false;
    out.host_base = img.base;
    out.system_create_dsp = resolve_create(img);
    out.dsp_release = reinterpret_cast<FMODFns::DSPRelease>(find_by_anchor(img, kSigs[1].anchor, kSigs[1].pattern));
    out.add_dsp = reinterpret_cast<FMODFns::AddDSP>(find_by_anchor(img, kSigs[2].anchor, kSigs[2].pattern));
    out.remove_dsp = reinterpret_cast<FMODFns::RemoveDSP>(find_by_anchor(img, kSigs[3].anchor, kSigs[3].pattern));
    out.resolver = reinterpret_cast<FMODFns::HandleResolver>(find_by_pattern(img, kResolver));
    out.unlock = reinterpret_cast<FMODFns::HandleUnlock>(find_by_pattern(img, kUnlock));
    // Phase 1 audio-state probe APIs: resolved by anchor+LEA (one candidate each,
    // confirmed by the runtime scout). Optional to audio injection.
    out.get3d_listener_attrs = reinterpret_cast<FMODFns::SysGet3DListenerAttrs>(
        resolve_by_anchor_unique(img, "System::get3DListenerAttributes"));
    out.get_num_dsps = reinterpret_cast<FMODFns::CCGetNumDSPs>(
        resolve_by_anchor_unique(img, "ChannelControl::getNumDSPs"));
    out.get_dsp = reinterpret_cast<FMODFns::CCGetDSP>(
        resolve_by_anchor_unique(img, "ChannelControl::getDSP"));
    out.dsp_get_type = reinterpret_cast<FMODFns::DSPGetType>(
        resolve_by_anchor_unique(img, "DSP::getType"));
    out.dsp_get_num_parameters = reinterpret_cast<FMODFns::DSPGetNumParameters>(
        resolve_by_anchor_unique(img, "DSP::getNumParameters"));
    out.dsp_get_parameter_float = reinterpret_cast<FMODFns::DSPGetParameterFloat>(
        resolve_by_anchor_unique(img, "DSP::getParameterFloat"));
    out.dsp_get_parameter_int = reinterpret_cast<FMODFns::DSPGetParameterInt>(
        resolve_by_anchor_unique(img, "DSP::getParameterInt"));
    out.dsp_get_info = reinterpret_cast<FMODFns::DSPGetInfo>(
        resolve_by_anchor_unique(img, "DSP::getInfo"));
    out.dsp_get_num_inputs = reinterpret_cast<FMODFns::DSPGetNumInputs>(
        resolve_by_anchor_unique(img, "DSP::getNumInputs"));
    // Cockpit reverb scout: group-tree traversal + convolution IR read. Optional.
    out.get_master_channel_group = reinterpret_cast<FMODFns::SysGetMasterChannelGroup>(
        resolve_by_anchor_unique(img, "System::getMasterChannelGroup"));
    out.group_get_num_groups = reinterpret_cast<FMODFns::GroupGetNumGroups>(
        resolve_by_anchor_unique(img, "ChannelGroup::getNumGroups"));
    out.group_get_group = reinterpret_cast<FMODFns::GroupGetGroup>(
        resolve_by_anchor_unique(img, "ChannelGroup::getGroup"));
    out.group_get_num_channels = reinterpret_cast<FMODFns::GroupGetNumChannels>(
        resolve_by_anchor_unique(img, "ChannelGroup::getNumChannels"));
    out.group_get_channel = reinterpret_cast<FMODFns::GroupGetChannel>(
        resolve_by_anchor_unique(img, "ChannelGroup::getChannel"));
    out.dsp_get_parameter_data = reinterpret_cast<FMODFns::DSPGetParameterData>(
        resolve_by_anchor_unique(img, "DSP::getParameterData"));
    out.dsp_get_parameter_info = reinterpret_cast<FMODFns::DSPGetParameterInfo>(
        resolve_by_anchor_unique(img, "DSP::getParameterInfo"));
    out.channel_get_current_sound = reinterpret_cast<FMODFns::ChannelGetCurrentSound>(
        resolve_by_anchor_unique(img, "Channel::getCurrentSound"));
    out.sound_get_mode = reinterpret_cast<FMODFns::SoundGetMode>(
        resolve_by_anchor_unique(img, "Sound::getMode"));
    log::info("[fmod] create=0x{:X} release=0x{:X} add=0x{:X} remove=0x{:X} resolver=0x{:X} unlock=0x{:X}",
              reinterpret_cast<std::uintptr_t>(out.system_create_dsp),
              reinterpret_cast<std::uintptr_t>(out.dsp_release),
              reinterpret_cast<std::uintptr_t>(out.add_dsp),
              reinterpret_cast<std::uintptr_t>(out.remove_dsp),
              reinterpret_cast<std::uintptr_t>(out.resolver),
              reinterpret_cast<std::uintptr_t>(out.unlock));
    log::info("[fmod] probe listener=0x{:X} getNumDSPs=0x{:X} getDSP=0x{:X} getType=0x{:X} getNumParams=0x{:X} getParamFloat=0x{:X}",
              reinterpret_cast<std::uintptr_t>(out.get3d_listener_attrs),
              reinterpret_cast<std::uintptr_t>(out.get_num_dsps),
              reinterpret_cast<std::uintptr_t>(out.get_dsp),
              reinterpret_cast<std::uintptr_t>(out.dsp_get_type),
              reinterpret_cast<std::uintptr_t>(out.dsp_get_num_parameters),
              reinterpret_cast<std::uintptr_t>(out.dsp_get_parameter_float));
    log::info("[fmod] radio identity getCurrentSound=0x{:X} getMode=0x{:X}",
              reinterpret_cast<std::uintptr_t>(out.channel_get_current_sound),
              reinterpret_cast<std::uintptr_t>(out.sound_get_mode));
    return out.ready();
}

DSPBridge::DSPBridge(AudioRing& ring, FMODFns fns) : ring_{ring}, fns_{fns} { g_bridge = this; }
DSPBridge::~DSPBridge() { detach(); if (g_bridge == this) g_bridge = nullptr; }
void DSPBridge::set_gain(float g) noexcept {
    if (!std::isfinite(g)) return;
    gain_.store(std::clamp(g, 0.0f, 2.0f), std::memory_order_release);
}

bool DSPBridge::validate(std::uint32_t h) const noexcept {
    if (!h || !fns_.resolver) return false;
    void* inst = nullptr;
    std::uint64_t lock_state = 0;
    std::uint32_t rc = ~0u;
    if (!seh_call([&] { rc = fns_.resolver(h, &inst, &lock_state); })) return false;
    if (fns_.unlock && lock_state) seh_call([&] { fns_.unlock(lock_state); });
    return rc == 0 && inst != nullptr;
}
std::uint32_t DSPBridge::live_handle(std::byte* stream) const noexcept {
    if (!stream) return 0;
    std::uint32_t h = 0;
    if (!safe_read(stream + 0x20, h) || !h) return 0;
    return validate(h) ? h : 0;
}
bool DSPBridge::channel_alive(std::byte* stream) const noexcept { return live_handle(stream) != 0; }

void DSPBridge::detach() noexcept {
    if (!dsp_) { handle_.store(0, std::memory_order_release); return; }
    const auto h = handle_.load(std::memory_order_acquire);
    if (h && fns_.remove_dsp) seh_call([&] { fns_.remove_dsp(h, dsp_); });
    if (fns_.dsp_release) seh_call([&] { fns_.dsp_release(dsp_); });
    dsp_ = nullptr;
    handle_.store(0, std::memory_order_release);
    primed_.store(false, std::memory_order_release);
}

void DSPBridge::install(std::uint32_t h) noexcept {
    if (!system_ || !h || !fns_.ready()) return;
    if (!fns_.system_create_dsp && fns_.host_base) fns_.system_create_dsp = resolve_create(parse(fns_.host_base));
    if (!fns_.system_create_dsp) return;
    FMOD_DSP_DESCRIPTION d{};
    std::memcpy(d.name, "FH6 Radio Rework", 17);
    d.version = 1;
    d.numinputbuffers = 1;
    d.numoutputbuffers = 1;
    d.read = reinterpret_cast<void*>(&DSPBridge::read_callback);
    d.userdata = this;
    constexpr std::uint32_t versions[] = {0x00011000u, 0x00011003u, 0x00010000u};
    void* dsp = nullptr;
    std::uint32_t rc = ~0u;
    for (auto v : versions) {
        d.pluginsdkversion = v;
        if (!seh_call([&] { rc = fns_.system_create_dsp(system_, &d, &dsp); })) { dsp = nullptr; continue; }
        if (rc == 0 && dsp) break;
        dsp = nullptr;
    }
    if (!dsp) { log::warn("[fmod] createDSP failed rc={}", rc); return; }
    if (!seh_call([&] { rc = fns_.add_dsp(static_cast<std::uint64_t>(h), 0, dsp); }) || rc != 0) {
        seh_call([&] { fns_.dsp_release(dsp); });
        log::warn("[fmod] addDSP failed rc={}", rc);
        return;
    }
    dsp_ = dsp;
    handle_.store(h, std::memory_order_release);
    ring_.request_reset();
    primed_.store(false, std::memory_order_release);
    log::info("[fmod] DSP attached to channel 0x{:X}", h);
}

void DSPBridge::set_target(const RadioInstance& inst, void* system) noexcept {
    stream_ = inst.radio_stream;
    system_ = system;
}
void DSPBridge::clear_target() noexcept {
    detach();
    stream_ = nullptr;
    system_ = nullptr;
}
void DSPBridge::retarget_if_needed() noexcept {
    if (!stream_ || !system_) return;
    const auto h = live_handle(stream_);
    const auto old = handle_.load(std::memory_order_acquire);
    if (h == old) return;
    if (!h) { if (old) detach(); return; }
    detach();
    install(h);
}
DSPStats DSPBridge::stats() const noexcept {
    DSPStats s;
    s.channel_handle = handle_.load(std::memory_order_relaxed);
    s.attached = s.channel_handle != 0;
    s.callbacks = callbacks_.load(std::memory_order_relaxed);
    s.underrun_frames = underrun_frames_.load(std::memory_order_relaxed);
    s.rebuffer_events = rebuffer_events_.load(std::memory_order_relaxed);
    s.primed = primed_.load(std::memory_order_relaxed);
    s.last_frames = last_frames_.load(std::memory_order_relaxed);
    s.last_channels = last_channels_.load(std::memory_order_relaxed);
    return s;
}

std::uint32_t __stdcall DSPBridge::read_callback(void*, float* /*in*/, float* out,
                                                 std::uint32_t length, std::int32_t in_channels,
                                                 std::int32_t* out_channels) {
    auto* b = g_bridge;
    if (!b || !out || length == 0) return 0;

    // Critical rule: never increase FMOD's channel count. The value supplied in
    // *out_channels describes the buffer FMOD allocated for this callback.
    // The old mod sometimes forced it from 1 to 2 and could write past out_buf.
    const std::int32_t channels = dsp_contract::safe_channel_count(in_channels, out_channels);
    if (channels == 0) return 0;
    if (out_channels) *out_channels = channels;

    std::memset(out, 0, static_cast<std::size_t>(length) * static_cast<std::size_t>(channels) * sizeof(float));

    if (b->ring_.consumer_apply_reset()) {
        b->primed_.store(false, std::memory_order_release);
    }

    // Do not start draining from an empty producer/consumer boundary. A short
    // prebuffer absorbs scheduler jitter; after a real underrun we re-arm it
    // instead of emitting a train of tiny crackles on every callback.
    constexpr std::size_t kPrebufferFrames = 2048; // ~42.7 ms at 48 kHz
    if (!b->primed_.load(std::memory_order_acquire)) {
        if (b->ring_.readable_frames() < kPrebufferFrames) {
            b->callbacks_.fetch_add(1, std::memory_order_relaxed);
            b->last_frames_.store(length, std::memory_order_relaxed);
            b->last_channels_.store(static_cast<std::uint32_t>(channels), std::memory_order_relaxed);
            return 0;
        }
        b->primed_.store(true, std::memory_order_release);
    }

    constexpr std::uint32_t kChunk = 1024;
    StereoFrame frames[kChunk];
    std::uint32_t produced = 0;
    const float gain = b->gain_.load(std::memory_order_relaxed);
    const bool native_stereo = b->native_stereo_.load(std::memory_order_relaxed);

    while (produced < length) {
        const auto want = std::min<std::uint32_t>(kChunk, length - produced);
        const auto got = static_cast<std::uint32_t>(b->ring_.pop(frames, want));
        for (std::uint32_t i = 0; i < got; ++i) {
            const float l = (frames[i].l / 32768.0f) * gain;
            const float r = (frames[i].r / 32768.0f) * gain;
            float* dst = out + (static_cast<std::size_t>(produced + i) * static_cast<std::size_t>(channels));
            // Preserve FMOD's allocated buffer shape. Default mode writes the
            // same mono sample to every channel; native stereo is opt-in only.
            dsp_contract::write_frame(dst, channels, l, r, native_stereo);
        }
        produced += got;
        if (got < want) {
            b->underrun_frames_.fetch_add(want - got, std::memory_order_relaxed);
            b->rebuffer_events_.fetch_add(1, std::memory_order_relaxed);
            b->primed_.store(false, std::memory_order_release);
            break;
        }
    }
    b->callbacks_.fetch_add(1, std::memory_order_relaxed);
    b->last_frames_.store(length, std::memory_order_relaxed);
    b->last_channels_.store(static_cast<std::uint32_t>(channels), std::memory_order_relaxed);
    return 0;
}
} // namespace fh6r::fmod
