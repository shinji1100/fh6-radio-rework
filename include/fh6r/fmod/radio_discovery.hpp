#pragma once
#include "fh6r/fmod/pe_image.hpp"
#include <string>
#include <vector>

namespace fh6r::fmod {
struct RadioInstance {
    std::byte* refcount_obj = nullptr;
    std::byte* radio_stream = nullptr;
    std::byte* fmod_sound = nullptr;
    std::byte* sample_props_body = nullptr;
    std::string sound_name;
};
struct DiscoveryResult {
    std::byte* vtable = nullptr;
    std::vector<RadioInstance> instances;
};
void* resolve_fmod_system(const PEImage& img, std::byte* radio_stream) noexcept;
DiscoveryResult discover_radio_instances(const PEImage& img) noexcept;
} // namespace fh6r::fmod
