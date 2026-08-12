#include "fh6r/audio_ring.hpp"
#include <cassert>
#include <cstdint>
#include <thread>
#include <vector>

int main() {
    using fh6r::AudioRing; using fh6r::StereoFrame;
    {
        AudioRing ring{2048};
        std::vector<StereoFrame> in(3000), out(2048);
        for (std::size_t i=0;i<in.size();++i) in[i] = {static_cast<std::int16_t>(i), static_cast<std::int16_t>(-static_cast<int>(i))};
        assert(ring.push(in.data(), in.size()) == 2048);
        assert(ring.overflow_count() == 952);
        assert(ring.pop(out.data(), out.size()) == out.size());
        for (std::size_t i=0;i<out.size();++i) assert(out[i].l == in[i].l && out[i].r == in[i].r);
        assert(ring.readable_frames() == 0);
    }
    {
        constexpr std::size_t total = 200000;
        AudioRing ring{4096};
        std::thread producer([&]{
            for (std::size_t i=0;i<total;) {
                StereoFrame f{static_cast<std::int16_t>(i & 0x7fff), static_cast<std::int16_t>((i*3) & 0x7fff)};
                if (ring.push(&f,1)==1) ++i; else std::this_thread::yield();
            }
        });
        for (std::size_t i=0;i<total;) {
            StereoFrame f{};
            if (ring.pop(&f,1)==1) {
                assert(f.l == static_cast<std::int16_t>(i & 0x7fff));
                assert(f.r == static_cast<std::int16_t>((i*3) & 0x7fff));
                ++i;
            } else std::this_thread::yield();
        }
        producer.join();
    }
    {
        // Control/producer threads only request a reset. The consumer owns
        // read_ and applies it at a safe callback boundary.
        AudioRing ring{2048};
        StereoFrame frames[8]{};
        for (int i=0; i<8; ++i) frames[i] = {static_cast<std::int16_t>(i), static_cast<std::int16_t>(-i)};
        assert(ring.push(frames, 8) == 8);
        ring.request_reset();
        assert(ring.readable_frames() == 8);
        assert(ring.consumer_apply_reset());
        assert(ring.readable_frames() == 0);
        assert(!ring.consumer_apply_reset());
    }
}
