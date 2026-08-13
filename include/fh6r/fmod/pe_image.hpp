#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fh6r::fmod {
struct PESection {
    std::array<char, 9> name{};
    std::byte* start = nullptr;
    std::byte* end = nullptr;
    std::uint32_t characteristics = 0;
    bool readable() const noexcept;
};
struct PEImage {
    std::byte* base = nullptr;
    std::size_t size = 0;
    std::uint32_t time_date_stamp = 0;
    std::uint32_t checksum = 0;
    std::byte* text = nullptr;
    std::byte* text_end = nullptr;
    std::byte* rdata = nullptr;
    std::byte* rdata_end = nullptr;
    std::vector<PESection> sections;
    std::vector<std::uint32_t> function_rvas;
    bool valid() const noexcept { return base && text && rdata && !function_rvas.empty(); }
};
PEImage parse(std::byte* base);
} // namespace fh6r::fmod
