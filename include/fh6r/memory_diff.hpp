#pragma once
#include "fh6r/fmod/pe_image.hpp"
#include <string>
#include <string_view>

namespace fh6r {

// Read-only memory diff to locate the interior/exterior (camera) state.
//
// Snapshots the game's writable global-data section (.data) and finds values
// that correlate with the camera view using the "return-to-baseline" filter:
// capture three snapshots (cockpit -> chase -> cockpit), then report offsets
// where S1 == S3 and S2 != S1 — a value that changed when the camera changed
// and changed back when the camera changed back (so timers / frame counters /
// physics, which never return, are filtered out).
//
// Entirely read-only: it only reads committed memory and writes nothing to the
// game. No code or data is modified, so it cannot crash the game.
class MemoryDiff {
public:
    explicit MemoryDiff(const fmod::PEImage& img) noexcept;
    ~MemoryDiff();
    void capture() noexcept;        // capture next snapshot; analyzes on the 3rd
    void reset() noexcept;
    int captures() const noexcept;
    std::size_t region_size() const noexcept;
    std::string result_json() const; // {"captures", "region_base", "candidates":[{offset,s1,s2,s3}]}

    // Six-view mapper. Capture every named view once for round 1, then once
    // again for round 2. Only candidates reproducing the same value map in
    // both rounds survive.
    bool capture_view(std::string_view name) noexcept;
    void reset_view_map() noexcept;
    std::string view_map_json() const;
private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace fh6r
