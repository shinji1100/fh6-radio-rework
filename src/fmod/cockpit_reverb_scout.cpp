#include "fh6r/fmod/cockpit_reverb_scout.hpp"
#include "fh6r/fmod/dsp_bridge.hpp"
#include "fh6r/log.hpp"
#include "fh6r/safe_mem.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace fh6r::fmod {
namespace {

// FMOD_DSP_TYPE_CONVOLUTIONREVERB == 32 (matches the FADER=7 we already saw).
constexpr std::int32_t kTypeConvolutionReverb = 32;
constexpr std::int32_t kMaxDepth = 8;
constexpr std::int32_t kMaxGroups = 256;
constexpr std::int32_t kMaxDSPs = 64;
constexpr int kMaxFound = 8;

// FMOD convolution reverb parameter indices (public FMOD docs):
//   0 = IR data, 1 = Wet, 2 = Dry, 3 = Linked
constexpr std::int32_t kParamIR = 0;
constexpr std::int32_t kParamWet = 1;
constexpr std::int32_t kParamDry = 2;
constexpr std::int32_t kParamLinked = 3;

int g_found = 0;

void dump_convolution(const FMODFns& fns, void* dsp, const std::string& out_dir) noexcept {
    const int seq = g_found++;

    float wet = 0, dry = 0, linked = 0;
    if (fns.dsp_get_parameter_float) {
        char vs[64]{};
        seh_call([&] { fns.dsp_get_parameter_float(dsp, kParamWet, &wet, vs); });
        seh_call([&] { fns.dsp_get_parameter_float(dsp, kParamDry, &dry, vs); });
        seh_call([&] { fns.dsp_get_parameter_float(dsp, kParamLinked, &linked, vs); });
    }

    void* data = nullptr;
    std::uint32_t length = 0;
    char valuestr[64]{};
    bool read_ok = false;
    if (fns.dsp_get_parameter_data) {
        read_ok = seh_call([&] {
            fns.dsp_get_parameter_data(dsp, kParamIR, &data, &length, valuestr, sizeof(valuestr));
        });
    }

    log::info("[cockpit-reverb] DSP #{} wet={:.3f} dry={:.3f} linked={:.3f} ir_read={} ir_len={}B",
              seq, wet, dry, linked, read_ok ? "ok" : "fail", length);

    if (!read_ok || !data || length <= 2) {
        log::warn("[cockpit-reverb] DSP #{} IR unreadable (len={})", seq, length);
        return;
    }

    // First int16 = channel count, then deinterleaved int16 PCM.
    std::int16_t channels = 0;
    std::memcpy(&channels, data, 2);
    const std::uint32_t pcm_bytes = length - 2;
    const std::uint32_t frames = (channels > 0) ? pcm_bytes / (2u * static_cast<std::uint32_t>(channels)) : 0;
    const double duration_ms = static_cast<double>(frames) / 48.0; // 48 kHz

    log::info("[cockpit-reverb]   channels={} frames={} duration={:.2f} ms",
              channels, frames, duration_ms);

    // Persist the raw IR for offline acoustic analysis (FFT, RT60, L/R diff).
    std::error_code ec;
    std::filesystem::path p = std::filesystem::path(out_dir) /
        ("cockpit_ir_" + std::to_string(seq) + ".raw");
    FILE* f = std::fopen(p.string().c_str(), "wb");
    if (f) {
        std::fwrite(data, 1, length, f);
        std::fclose(f);
        log::info("[cockpit-reverb]   IR written to {}", p.string());
    } else {
        log::warn("[cockpit-reverb]   could not write IR to {}", p.string());
    }

    // Energy profile + onset location. Bands: 0-5 / 5-20 / 20-50 / 50-100 ms.
    const std::int16_t* pcm = reinterpret_cast<const std::int16_t*>(
        static_cast<const std::byte*>(data) + 2);
    constexpr std::uint32_t b0 = 5 * 48, b1 = 20 * 48, b2 = 50 * 48, b3 = 100 * 48;
    double e[4] = {0, 0, 0, 0};
    double peak = 0;
    std::uint32_t peak_idx = 0;
    const std::uint32_t nframes = std::min<std::uint32_t>(frames, b3);
    for (std::uint32_t i = 0; i < nframes; ++i) {
        double acc = 0;
        for (std::int32_t c = 0; c < channels; ++c) {
            const double s = static_cast<double>(pcm[i * channels + c]) / 32768.0;
            acc += s * s;
            if (std::fabs(s) > peak) { peak = std::fabs(s); peak_idx = i; }
        }
        if (i < b0) e[0] += acc;
        else if (i < b1) e[1] += acc;
        else if (i < b2) e[2] += acc;
        else e[3] += acc;
    }
    const double total = e[0] + e[1] + e[2] + e[3];
    log::info("[cockpit-reverb]   peak_at={:.2f}ms peak={:.3f} energy 0-5={:.1f}% 5-20={:.1f}% 20-50={:.1f}% 50-100={:.1f}%",
              static_cast<double>(peak_idx) / 48.0, peak,
              total > 0 ? 100.0 * e[0] / total : 0.0,
              total > 0 ? 100.0 * e[1] / total : 0.0,
              total > 0 ? 100.0 * e[2] / total : 0.0,
              total > 0 ? 100.0 * e[3] / total : 0.0);
}

void walk_group(const FMODFns& fns, void* group, int depth,
                const std::string& out_dir) noexcept {
    if (depth > kMaxDepth || g_found >= kMaxFound || !group) return;

    if (fns.get_num_dsps && fns.get_dsp) {
        std::int32_t n = 0;
        const bool ok = seh_call([&] {
            fns.get_num_dsps(reinterpret_cast<std::uint64_t>(group), &n);
        });
        if (ok && n > 0) {
            if (n > kMaxDSPs) n = kMaxDSPs;
            for (std::int32_t i = 0; i < n; ++i) {
                void* dsp = nullptr;
                if (!seh_call([&] { fns.get_dsp(reinterpret_cast<std::uint64_t>(group), i, &dsp); }) || !dsp)
                    continue;
                std::int32_t t = -1;
                if (fns.dsp_get_type) seh_call([&] { fns.dsp_get_type(dsp, &t); });
                if (t == kTypeConvolutionReverb) {
                    log::info("[cockpit-reverb] found convolution DSP at depth={} dsp_index={}", depth, i);
                    dump_convolution(fns, dsp, out_dir);
                }
            }
        }
    }

    if (fns.group_get_num_groups && fns.group_get_group) {
        std::int32_t ng = 0;
        const bool ok = seh_call([&] { fns.group_get_num_groups(group, &ng); });
        if (ok && ng > 0) {
            if (ng > kMaxGroups) ng = kMaxGroups;
            for (std::int32_t i = 0; i < ng; ++i) {
                void* child = nullptr;
                if (!seh_call([&] { fns.group_get_group(group, i, &child); }) || !child)
                    continue;
                walk_group(fns, child, depth + 1, out_dir);
            }
        }
    }
}

} // namespace

bool scout_cockpit_reverb(const FMODFns& fns, void* system,
                          const std::string& out_dir) noexcept {
    if (!system || !fns.cockpit_reverb_ready()) {
        log::warn("[cockpit-reverb] not ready (system={} ready={})",
                  system != nullptr, fns.cockpit_reverb_ready());
        return false;
    }

    g_found = 0;
    void* master = nullptr;
    const bool ok = seh_call([&] { fns.get_master_channel_group(system, &master); });
    if (!ok || !master) {
        log::warn("[cockpit-reverb] getMasterChannelGroup failed");
        return false;
    }

    log::info("[cockpit-reverb] walking group tree from master=0x{:X}",
              reinterpret_cast<std::uintptr_t>(master));
    walk_group(fns, master, 0, out_dir);
    log::info("[cockpit-reverb] done: {} convolution DSP(s) found", g_found);
    return g_found > 0;
}

} // namespace fh6r::fmod
