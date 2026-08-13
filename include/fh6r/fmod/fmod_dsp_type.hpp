#pragma once
#include <cstdint>

// ============================================================================
// FMOD 2.03 Core API type constants.
//
// SOURCE: FMOD 2.03 official documentation (core-api-common.h /
// core-api-dsp.h). These values are NOT inferred from memory. Do not add
// values without verifying them against the FMOD 2.03 docs, or reading them at
// runtime via DSP::getInfo()->name / DSP::getParameterInfo()->type.
//
// Policy (project decision): DSP identification must use triple verification —
// numeric type + getInfo()->name + getParameterInfo() layout — never numeric
// type alone. A human-readable getInfo()->name is the primary signal.
// ============================================================================

namespace fh6r::fmod {

// FMOD_DSP_TYPE — confirmed subset (FMOD 2.03).
enum class DspType : std::int32_t {
    ConvolutionReverb = 28,
    ChannelMix        = 29,
    Transceiver       = 30,
    ObjectPan         = 31,
    MultibandEq       = 32,
};

// FMOD_DSP_PARAMETER_TYPE (FMOD 2.03).
enum class DspParamType : std::int32_t {
    Float = 0,
    Int   = 1,
    Bool  = 2,
    Data  = 3,
};

// FMOD_DSP_PARAMETER_DESC (FMOD 2.03). Only the inline `type` and `name`/`label`
// fields are read here; `description` points elsewhere and is never dereferenced
// (safe-memory discipline).
struct DspParamDesc {
    std::int32_t type = -1;   // DspParamType
    char name[16]{};
    char label[16]{};
    const char* description = nullptr;
};

// FMOD_RESULT: OK == 0.
inline constexpr std::int32_t kFmodOk = 0;

// Human-readable name returned by DSP::getInfo() for the engine's convolution
// reverb. This is the PRIMARY identifier (strongest runtime signal); the
// numeric type (DspType::ConvolutionReverb) is only a cross-check.
inline constexpr const char* kDspNameConvolutionReverb = "FMOD Convolution Reverb";

// FMOD_DSP_CONVOLUTION_REVERB parameter indices (FMOD 2.03 public docs).
inline constexpr std::int32_t kParamIR = 0;      // DATA  (signed 16-bit PCM, channel count in first word)
inline constexpr std::int32_t kParamWet = 1;     // FLOAT (dB, [-80, +10])
inline constexpr std::int32_t kParamDry = 2;     // FLOAT (dB, [-80, +10])
inline constexpr std::int32_t kParamLinked = 3;  // BOOL  (default true)

} // namespace fh6r::fmod
