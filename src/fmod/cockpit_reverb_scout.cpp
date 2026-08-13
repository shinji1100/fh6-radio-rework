#include "fh6r/fmod/cockpit_reverb_scout.hpp"
#include "fh6r/fmod/dsp_bridge.hpp"
#include "fh6r/log.hpp"
#include "fh6r/safe_mem.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fh6r::fmod {
namespace {

// FMOD_DSP_TYPE_CONVOLUTIONREVERB == 32 (matches the FADER=7 we already saw).
constexpr std::int32_t kTypeConvolutionReverb = 32;
constexpr std::int32_t kMaxDepth = 8;
constexpr std::int32_t kMaxGroups = 256;
constexpr std::int32_t kMaxDSPs = 64;
constexpr std::int32_t kMaxChannels = 256;
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

    float wet = 0, dry = 0;
    std::int32_t linked = -1;
    char vs[64]{};
    if (fns.dsp_get_parameter_float) {
        seh_call([&] { fns.dsp_get_parameter_float(dsp, kParamWet, &wet, vs, sizeof(vs)); });
        seh_call([&] { fns.dsp_get_parameter_float(dsp, kParamDry, &dry, vs, sizeof(vs)); });
    }
    // LINKED is a bool (FMOD_BOOL = int); getParameterBool was dead-code-eliminated
    // in this build, so read it via getParameterInt.
    if (fns.dsp_get_parameter_int) {
        seh_call([&] { fns.dsp_get_parameter_int(dsp, kParamLinked, &linked, vs, sizeof(vs)); });
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

    log::info("[cockpit-reverb] DSP #{} wet={:.3f} dry={:.3f} linked={} ir_read={} ir_len={}B",
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

// Dump a single DSP node's identity: address, name, type, channel count and
// input count. Marks interesting nodes (multi-input = likely send return,
// named DSP, 4/8-channel format) so the graph skeleton is readable even though
// the per-connection getters are dead-code-eliminated in this build.
void dump_node(const FMODFns& fns, void* dsp, int index, int depth) noexcept {
    char name[32]{};
    std::uint32_t version = 0;
    std::int32_t channels = -1, cw = -1, ch = -1;
    if (fns.dsp_get_info)
        seh_call([&] { fns.dsp_get_info(dsp, name, &version, &channels, &cw, &ch); });
    name[31] = '\0';

    std::int32_t t = -1;
    if (fns.dsp_get_type) seh_call([&] { fns.dsp_get_type(dsp, &t); });

    std::int32_t ninputs = -1;
    if (fns.dsp_get_num_inputs) seh_call([&] { fns.dsp_get_num_inputs(dsp, &ninputs); });

    std::string flags;
    if (ninputs > 1) flags += " [MULTI-IN]";
    if (t == kTypeConvolutionReverb) flags += " [CONV]";
    if (t == 7) flags += " [FADER]";
    // Case-insensitive keyword highlight for the 2D/3D Music routing question.
    std::string low;
    low.reserve(32);
    for (char c : std::string_view{name})
        low += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto has = [&](const char* kw) { return low.find(kw) != std::string::npos; };
    if (has("radio")) flags += " [RADIO]";
    if (has("music")) flags += " [MUSIC]";
    if (has("3d")) flags += " [3D]";
    if (has("emitter")) flags += " [EMITTER]";
    if (has("panner") || has("pan")) flags += " [PANNER]";
    if (has("lfe")) flags += " [LFE]";
    if (has("blend")) flags += " [BLEND]";
    if (has("spatial")) flags += " [SPATIAL]";
    if (has("fader")) flags += " [FADERNAME]";
    if (has("return")) flags += " [RETURN]";
    if (has("mixer")) flags += " [MIXER]";
    if (has("reverb")) flags += " [REVERB]";
    if (has("cockpit")) flags += " [COCKPIT]";
    if (channels == 4 || channels == 8) flags += " [4/8CH]";

    log::info("[graph] depth={} dsp[{}] addr=0x{:X} type={} ch={} inputs={} name='{}'{}",
              depth, index, reinterpret_cast<std::uintptr_t>(dsp), t, channels, ninputs, name, flags);
}

void walk_group(const FMODFns& fns, void* group, int depth,
                const std::string& out_dir) noexcept {
    if (depth > kMaxDepth || !group) return;

    if (fns.get_num_dsps && fns.get_dsp) {
        std::int32_t n = 0;
        const bool ok = seh_call([&] {
            fns.get_num_dsps(reinterpret_cast<std::uint64_t>(group), &n);
        });
        if (ok && n > 0) {
            if (n > kMaxDSPs) n = kMaxDSPs;
            // Collect this group's DSP chain and check for a convolution node.
            struct Node { void* dsp; std::int32_t type; };
            std::vector<Node> nodes;
            nodes.reserve(static_cast<std::size_t>(n));
            bool has_conv = false;
            for (std::int32_t i = 0; i < n; ++i) {
                void* dsp = nullptr;
                if (!seh_call([&] { fns.get_dsp(reinterpret_cast<std::uint64_t>(group), i, &dsp); }) || !dsp)
                    continue;
                std::int32_t t = -1;
                if (fns.dsp_get_type) seh_call([&] { fns.dsp_get_type(dsp, &t); });
                if (t == kTypeConvolutionReverb) has_conv = true;
                nodes.push_back({dsp, t});
            }
            if (has_conv) {
                log::info("[graph] === group depth={} has convolution reverb ({} DSPs) ===", depth, n);
                for (std::size_t i = 0; i < nodes.size(); ++i) {
                    dump_node(fns, nodes[i].dsp, static_cast<int>(i), depth);
                    if (nodes[i].type == kTypeConvolutionReverb && g_found < kMaxFound) {
                        dump_convolution(fns, nodes[i].dsp, out_dir);
                    }
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

// Dump one channel's full DSP chain (a panner/spatializer can live on the
// channel itself, not on any group).
void dump_channel_chain(const FMODFns& fns, void* ch, int channel_index) noexcept {
    log::info("[radio-route] CHANNEL #{} DSP CHAIN:", channel_index);
    if (fns.get_num_dsps && fns.get_dsp) {
        std::int32_t n = 0;
        if (seh_call([&] { fns.get_num_dsps(reinterpret_cast<std::uint64_t>(ch), &n); }) && n > 0) {
            if (n > kMaxDSPs) n = kMaxDSPs;
            for (std::int32_t i = 0; i < n; ++i) {
                void* dsp = nullptr;
                if (!seh_call([&] { fns.get_dsp(reinterpret_cast<std::uint64_t>(ch), i, &dsp); }) || !dsp) continue;
                dump_node(fns, dsp, i, -1);
            }
        }
    }
}

// Dump the full group lineage (master -> ... -> host) with each group's DSP
// chain. The DFS stack carries the parent info that getParentGroup would have
// given us (it is dead-code-eliminated in this build).
void dump_lineage(const FMODFns& fns, const std::vector<void*>& stack) noexcept {
    log::info("[radio-route] GROUP LINEAGE ({} levels, index 0 = master):", stack.size());
    for (std::size_t d = 0; d < stack.size(); ++d) {
        void* g = stack[d];
        const char* tag = (d + 1 == stack.size()) ? " [HOST]" : "";
        log::info("[radio-route]   depth={} group=0x{:X}{}", d, reinterpret_cast<std::uintptr_t>(g), tag);
        if (fns.get_num_dsps && fns.get_dsp) {
            std::int32_t n = 0;
            if (seh_call([&] { fns.get_num_dsps(reinterpret_cast<std::uint64_t>(g), &n); }) && n > 0) {
                if (n > kMaxDSPs) n = kMaxDSPs;
                for (std::int32_t i = 0; i < n; ++i) {
                    void* dsp = nullptr;
                    if (!seh_call([&] { fns.get_dsp(reinterpret_cast<std::uint64_t>(g), i, &dsp); }) || !dsp) continue;
                    dump_node(fns, dsp, i, static_cast<int>(d));
                }
            }
        }
    }
}

// Locate the RadioStreamFmod channel by finding our injected DSP (named
// "FH6 Radio Rework") on some channel within the group tree. getParentGroup is
// dead-code-eliminated, so instead of climbing up we enumerate every group's
// channels and match on the DSP name, while the DFS stack records lineage.
bool trace_radio(const FMODFns& fns, void* group, int depth,
                 std::vector<void*>& stack) noexcept {
    if (depth > kMaxDepth || !group) return false;

    stack.push_back(group);

    if (fns.group_get_num_channels && fns.group_get_channel) {
        std::int32_t nc = 0;
        if (seh_call([&] { fns.group_get_num_channels(group, &nc); }) && nc > 0) {
            if (nc > kMaxChannels) nc = kMaxChannels;
            for (std::int32_t i = 0; i < nc; ++i) {
                void* ch = nullptr;
                if (!seh_call([&] { fns.group_get_channel(group, i, &ch); }) || !ch) continue;
                std::int32_t nd = 0;
                if (!fns.get_num_dsps ||
                    !seh_call([&] { fns.get_num_dsps(reinterpret_cast<std::uint64_t>(ch), &nd); }) || nd <= 0)
                    continue;
                if (nd > kMaxDSPs) nd = kMaxDSPs;
                for (std::int32_t j = 0; j < nd; ++j) {
                    void* dsp = nullptr;
                    if (!fns.get_dsp ||
                        !seh_call([&] { fns.get_dsp(reinterpret_cast<std::uint64_t>(ch), j, &dsp); }) || !dsp)
                        continue;
                    char name[32]{};
                    if (fns.dsp_get_info) {
                        std::uint32_t ver = 0; std::int32_t chans = -1, cw = -1, chh = -1;
                        seh_call([&] { fns.dsp_get_info(dsp, name, &ver, &chans, &cw, &chh); });
                    }
                    name[31] = '\0';
                    if (std::strcmp(name, "FH6 Radio Rework") == 0) {
                        log::info("[radio-route] FOUND RadioStreamFmod at depth={} channel_index={} dsp_index={}",
                                  depth, i, j);
                        dump_channel_chain(fns, ch, i);
                        dump_lineage(fns, stack);
                        stack.pop_back();
                        return true;
                    }
                }
            }
        }
    }

    if (fns.group_get_num_groups && fns.group_get_group) {
        std::int32_t ng = 0;
        if (seh_call([&] { fns.group_get_num_groups(group, &ng); }) && ng > 0) {
            if (ng > kMaxGroups) ng = kMaxGroups;
            for (std::int32_t i = 0; i < ng; ++i) {
                void* child = nullptr;
                if (!seh_call([&] { fns.group_get_group(group, i, &child); }) || !child) continue;
                if (trace_radio(fns, child, depth + 1, stack)) { stack.pop_back(); return true; }
            }
        }
    }
    stack.pop_back();
    return false;
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

    // RadioStreamFmod-centered trace: find which group hosts our injected DSP
    // and dump that channel's chain + the full ancestor lineage to see the
    // radio's downstream routing.
    std::vector<void*> stack;
    stack.reserve(static_cast<std::size_t>(kMaxDepth) + 1);
    const bool found_radio = trace_radio(fns, master, 0, stack);
    log::info("[radio-route] trace done: radio group {}", found_radio ? "found" : "NOT found");
    return g_found > 0;
}

} // namespace fh6r::fmod
