#include "fh6r/audio_state_probe.hpp"
#include "fh6r/fmod/cockpit_reverb_scout.hpp"
#include "fh6r/log.hpp"
#include "fh6r/safe_mem.hpp"

#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

namespace fh6r {
namespace {
using namespace std::chrono_literals;
constexpr std::int32_t kMaxDSPs = 32;
constexpr std::int32_t kMaxParams = 16;
} // namespace

AudioStateProbe::AudioStateProbe(fmod::DSPBridge& bridge, std::string data_dir)
    : bridge_{bridge}, data_dir_{std::move(data_dir)},
      thread_{[this](std::stop_token s) { run(s); }} {}

AudioStateProbe::~AudioStateProbe() {
    thread_.request_stop();
    if (thread_.joinable()) thread_.join();
}

AudioStateSnapshot AudioStateProbe::snapshot() const {
    std::scoped_lock lk{mu_};
    return snap_;
}

void AudioStateProbe::run(std::stop_token stop) noexcept {
    while (!stop.stop_requested()) {
        sample();
        std::this_thread::sleep_for(1s);
    }
}

void AudioStateProbe::sample() noexcept {
    const auto& fns = bridge_.fns();
    const auto handle = bridge_.channel_handle();
    void* sys = (handle != 0) ? bridge_.system() : nullptr;

    AudioStateSnapshot s;
    s.attached = (handle != 0 && sys != nullptr);
    s.channel_handle = handle;
    s.taken_ms = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    if (!s.attached) {
        std::scoped_lock lk{mu_};
        snap_ = s;
        return;
    }

    // One-shot cockpit reverb scout: the FMOD System pointer is only valid
    // after discovery, so walk the group tree on first attach (up to 3 tries
    // to ride out a lazily-loaded bank) and dump any convolution-reverb IR.
    if (!reverb_scouted_ && reverb_attempts_ < 3) {
        ++reverb_attempts_;
        if (fmod::scout_cockpit_reverb(fns, sys, data_dir_))
            reverb_scouted_ = true;
    }

    // FMOD 3D listener (pos/vel/forward/up). forward/up flip between cockpit
    // and chase, which is itself a candidate interior/exterior signal.
    if (fns.get3d_listener_attrs) {
        ListenerSnap ls;
        seh_call([&] {
            const auto rc = fns.get3d_listener_attrs(sys, 0, &ls.pos, &ls.vel, &ls.fwd, &ls.up);
            ls.valid = (rc == 0);
        });
        if (ls.valid) s.listener = ls;
    }

    // Radio channel DSP chain. Note: getParentGroup is absent from this build,
    // so we enumerate the channel's own DSPs only (no climb to master group).
    if (fns.get_num_dsps && fns.get_dsp) {
        std::int32_t n = 0;
        const bool nok = seh_call([&] {
            fns.get_num_dsps(static_cast<std::uint64_t>(handle), &n);
        });
        if (nok && n > 0) {
            if (n > kMaxDSPs) n = kMaxDSPs;
            s.dsps.reserve(static_cast<std::size_t>(n));
            for (std::int32_t i = 0; i < n; ++i) {
                DSPSnap d;
                void* dsp = nullptr;
                if (!seh_call([&] { fns.get_dsp(static_cast<std::uint64_t>(handle), i, &dsp); }) || !dsp)
                    continue;
                d.valid = true;
                if (fns.dsp_get_type) {
                    std::int32_t t = 0;
                    if (seh_call([&] { fns.dsp_get_type(dsp, &t); })) d.type = t;
                }
                if (fns.dsp_get_num_parameters) {
                    std::int32_t p = 0;
                    if (seh_call([&] { fns.dsp_get_num_parameters(dsp, &p); })) {
                        d.num_params = p;
                        if (p > kMaxParams) p = kMaxParams;
                        if (fns.dsp_get_parameter_float) {
                            for (std::int32_t k = 0; k < p; ++k) {
                                float v = 0;
                                char vs[64]{};
                                seh_call([&] { fns.dsp_get_parameter_float(dsp, k, &v, vs); });
                                d.params[k] = v;
                            }
                        }
                    }
                }
                s.dsps.push_back(std::move(d));
            }
        }
    }

    {
        std::scoped_lock lk{mu_};
        snap_ = s;
    }

    // Log a compact but complete dump so Phase 2 diff can be done from the log
    // alone. Diagnostic build: one line set per sample (~1s) is acceptable.
    log::info("[probe] attached=1 handle=0x{:X} dsps={} listener_valid={}",
              handle, snap_.dsps.size(), snap_.listener.valid);
    if (snap_.listener.valid) {
        const auto& f = snap_.listener.fwd;
        const auto& u = snap_.listener.up;
        const auto& p = snap_.listener.pos;
        log::info("[probe] listener pos=({:.2f},{:.2f},{:.2f}) fwd=({:.2f},{:.2f},{:.2f}) up=({:.2f},{:.2f},{:.2f})",
                  p.x, p.y, p.z, f.x, f.y, f.z, u.x, u.y, u.z);
    }
    for (std::size_t i = 0; i < snap_.dsps.size(); ++i) {
        const auto& d = snap_.dsps[i];
        std::string ps;
        for (std::int32_t k = 0; k < d.num_params && k < kMaxParams; ++k) {
            if (k) ps += ' ';
            ps += std::format("{:.3f}", d.params[k]);
        }
        log::info("[probe] dsp[{}] type={} params={} [{}]", i, d.type, d.num_params, ps);
    }
}

} // namespace fh6r
