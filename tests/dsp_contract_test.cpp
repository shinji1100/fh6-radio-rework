#include "fh6r/dsp_contract.hpp"
#include <cassert>
#include <cmath>

int main() {
    using namespace fh6r::dsp_contract;
    int out = 1;
    assert(safe_channel_count(2, &out) == 1); // never upgrade FMOD's mono buffer
    out = 2;
    assert(safe_channel_count(1, &out) == 2); // use exactly what FMOD says it allocated
    out = 64;
    assert(safe_channel_count(2, &out) == 0);
    assert(safe_channel_count(2, nullptr) == 2);

    float mono[1] = {99.0f};
    write_frame(mono, 1, 1.0f, -0.5f, true);
    assert(std::fabs(mono[0] - 0.25f) < 1e-6f); // cannot write a second channel

    float stereo[2] = {};
    write_frame(stereo, 2, 0.8f, -0.2f, false);
    assert(std::fabs(stereo[0] - 0.3f) < 1e-6f);
    assert(std::fabs(stereo[1] - 0.3f) < 1e-6f);
    write_frame(stereo, 2, 0.8f, -0.2f, true);
    assert(std::fabs(stereo[0] - 0.8f) < 1e-6f);
    assert(std::fabs(stereo[1] + 0.2f) < 1e-6f);
}
