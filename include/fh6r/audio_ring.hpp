#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace fh6r {
struct StereoFrame { std::int16_t l = 0; std::int16_t r = 0; };

class AudioRing {
public:
    explicit AudioRing(std::size_t capacity_frames = 16384);
    std::size_t capacity_frames() const noexcept { return capacity_; }
    std::size_t readable_frames() const noexcept;
    std::size_t writable_frames() const noexcept;
    std::size_t push(const StereoFrame* src, std::size_t frames) noexcept;
    std::size_t pop(StereoFrame* dst, std::size_t frames) noexcept;
    // Request a flush from any control/producer thread. The actual read-index
    // move is performed only by the consumer, preserving the SPSC contract.
    void request_reset() noexcept { reset_requested_.store(true, std::memory_order_release); }
    bool consumer_apply_reset() noexcept;
    std::uint64_t overflow_count() const noexcept { return overflows_.load(std::memory_order_relaxed); }
private:
    static std::size_t round_pow2(std::size_t n) noexcept;
    std::size_t capacity_ = 0;
    std::size_t mask_ = 0;
    std::unique_ptr<StereoFrame[]> data_;
    alignas(64) std::atomic<std::uint64_t> write_{0};
    alignas(64) std::atomic<std::uint64_t> read_{0};
    alignas(64) std::atomic<std::uint64_t> overflows_{0};
    alignas(64) std::atomic<bool> reset_requested_{false};
};
} // namespace fh6r
