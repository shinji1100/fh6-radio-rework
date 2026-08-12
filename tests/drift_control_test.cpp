#include "fh6r/drift_control.hpp"
#include <cassert>
#include <cmath>
int main() {
    using fh6r::drift::resample_step;
    const auto nominal = resample_step(48000,48000,4096,16384);
    assert(std::abs(nominal - 1.0) < 1e-12);
    assert(resample_step(48000,48000,12000,16384) > 1.0); // full-ish: produce fewer frames
    assert(resample_step(48000,48000,0,16384) < 1.0);     // low: produce more frames
    assert(resample_step(96000,48000,4096,16384) == 2.0);
    assert(resample_step(48000,48000,16384,16384) <= 1.0050000001);
    assert(resample_step(48000,48000,0,16384) >= 0.9949999999);
}
