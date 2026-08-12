#include "fh6r/audio_ring.hpp"
#include <algorithm>
#include <bit>
#include <cstring>

namespace fh6r {
std::size_t AudioRing::round_pow2(std::size_t n) noexcept {
    if (n < 2048) n = 2048;
    return std::bit_ceil(n);
}
AudioRing::AudioRing(std::size_t capacity_frames)
    : capacity_{round_pow2(capacity_frames)}, mask_{capacity_ - 1},
      data_{std::make_unique<StereoFrame[]>(capacity_)} {}
std::size_t AudioRing::readable_frames() const noexcept {
    const auto w = write_.load(std::memory_order_acquire);
    const auto r = read_.load(std::memory_order_acquire);
    return static_cast<std::size_t>(w - r);
}
std::size_t AudioRing::writable_frames() const noexcept {
    return capacity_ - readable_frames();
}
std::size_t AudioRing::push(const StereoFrame* src, std::size_t frames) noexcept {
    if (!src || frames == 0) return 0;
    const auto w = write_.load(std::memory_order_relaxed);
    const auto r = read_.load(std::memory_order_acquire);
    const auto free = capacity_ - static_cast<std::size_t>(w - r);
    const auto n = std::min(frames, free);
    if (n < frames) {
        overflows_.fetch_add(frames - n, std::memory_order_relaxed);
        // Do not let the consumer later play a stale, full backlog. Only the
        // consumer is allowed to move read_, so ask it to flush on its next
        // callback instead of violating the SPSC ownership rule here.
        request_reset();
    }
    if (!n) return 0;
    const auto off = static_cast<std::size_t>(w) & mask_;
    const auto first = std::min(n, capacity_ - off);
    std::memcpy(data_.get() + off, src, first * sizeof(StereoFrame));
    if (first < n) std::memcpy(data_.get(), src + first, (n - first) * sizeof(StereoFrame));
    write_.store(w + n, std::memory_order_release);
    return n;
}
std::size_t AudioRing::pop(StereoFrame* dst, std::size_t frames) noexcept {
    if (!dst || frames == 0) return 0;
    const auto r = read_.load(std::memory_order_relaxed);
    const auto w = write_.load(std::memory_order_acquire);
    const auto avail = static_cast<std::size_t>(w - r);
    const auto n = std::min(frames, avail);
    if (!n) return 0;
    const auto off = static_cast<std::size_t>(r) & mask_;
    const auto first = std::min(n, capacity_ - off);
    std::memcpy(dst, data_.get() + off, first * sizeof(StereoFrame));
    if (first < n) std::memcpy(dst + first, data_.get(), (n - first) * sizeof(StereoFrame));
    read_.store(r + n, std::memory_order_release);
    return n;
}
bool AudioRing::consumer_apply_reset() noexcept {
    if (!reset_requested_.exchange(false, std::memory_order_acq_rel)) return false;
    read_.store(write_.load(std::memory_order_acquire), std::memory_order_release);
    return true;
}
} // namespace fh6r
