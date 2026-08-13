#include "fh6r/memory_diff.hpp"
#include "fh6r/log.hpp"
#include "fh6r/safe_mem.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace fh6r {

struct MemoryDiff::Impl {
    std::vector<std::uint8_t> snap[3];
    std::uintptr_t region_base = 0;
    std::size_t region_size = 0;
    int n_captures = 0;

    struct Cand { std::uintptr_t offset; std::uint64_t s1, s2, s3; };
    std::vector<Cand> candidates;

    static constexpr std::array<std::string_view, 6> kViewNames{{
        "dashboard", "cockpit", "chase_near", "chase_far", "hood", "bumper"
    }};
    std::vector<std::uint8_t> view_first[6];
    std::array<int, 6> view_counts{};
    struct ViewCand {
        std::uintptr_t offset = 0;
        std::uint8_t width = 0;
        std::array<std::uint32_t, 6> values{};
        std::uint8_t verified_mask = 0;
        std::uint8_t distinct = 0;
    };
    std::vector<ViewCand> view_candidates;
    bool first_round_analyzed = false;

    explicit Impl(const fmod::PEImage& img) {
        for (const auto& sec : img.sections) {
            std::string name(reinterpret_cast<const char*>(sec.name.data()),
                             sec.name.size());
            if (name.find(".data") != std::string::npos) {
                region_base = reinterpret_cast<std::uintptr_t>(sec.start);
                region_size = static_cast<std::size_t>(sec.end - sec.start);
                break;
            }
        }
        if (region_size == 0)
            log::warn("[memdiff] .data section not found");
        else
            log::info("[memdiff] region .data base=0x{:X} size=0x{:X}", region_base, region_size);
    }

    void capture() {
        if (region_size == 0) return;
        if (n_captures >= 3) reset();
        auto& buf = snap[n_captures];
        buf.resize(region_size);
        const bool ok = seh_call([&] {
            std::memcpy(buf.data(), reinterpret_cast<const void*>(region_base), region_size);
        });
        if (!ok) log::warn("[memdiff] snapshot read fault (partial)");
        ++n_captures;
        log::info("[memdiff] captured snapshot {}/3", n_captures);
        if (n_captures == 3) analyze();
    }

    void reset() {
        for (auto& b : snap) b.clear();
        candidates.clear();
        n_captures = 0;
    }

    int view_index(std::string_view name) const noexcept {
        for (int i = 0; i < static_cast<int>(kViewNames.size()); ++i)
            if (name == kViewNames[static_cast<std::size_t>(i)]) return i;
        return -1;
    }

    bool all_view_counts_at_least(int n) const noexcept {
        return std::ranges::all_of(view_counts, [n](int count) { return count >= n; });
    }

    void analyze_first_view_round() {
        view_candidates.clear();
        if (!all_view_counts_at_least(1)) return;
        for (const auto& snapshot : view_first)
            if (snapshot.size() != region_size) return;

        auto add_candidate = [&](std::size_t offset, std::uint8_t width,
                                 const std::array<std::uint32_t, 6>& values,
                                 std::uint8_t minimum_distinct) {
            auto sorted = values;
            std::ranges::sort(sorted);
            const auto unique_end = std::ranges::unique(sorted).begin();
            const auto distinct = static_cast<std::uint8_t>(unique_end - sorted.begin());
            if (distinct < minimum_distinct) return;
            ViewCand candidate;
            candidate.offset = offset;
            candidate.width = width;
            candidate.values = values;
            candidate.distinct = distinct;
            view_candidates.push_back(candidate);
        };

        // Prefer aligned 32-bit enums; accept only compact values so pointers,
        // counters and floating-point bit patterns cannot become candidates.
        for (std::size_t offset = 0; offset + 4 <= region_size; offset += 4) {
            std::array<std::uint32_t, 6> values{};
            bool compact = true;
            for (int view = 0; view < 6; ++view) {
                std::memcpy(&values[static_cast<std::size_t>(view)],
                            &view_first[view][offset], 4);
                compact = compact && values[static_cast<std::size_t>(view)] <= 64;
            }
            if (compact) add_candidate(offset, 4, values, 5);
        }
        // Byte enums are a fallback. Boolean flags are intentionally excluded:
        // they can only recreate the already-proven two-state split.
        for (std::size_t offset = 0; offset < region_size; ++offset) {
            std::array<std::uint32_t, 6> values{};
            bool compact = true;
            for (int view = 0; view < 6; ++view) {
                values[static_cast<std::size_t>(view)] = view_first[view][offset];
                compact = compact && values[static_cast<std::size_t>(view)] <= 16;
            }
            if (compact) add_candidate(offset, 1, values, 5);
        }

        // Some camera systems may intentionally share an enum value between
        // two related views. Fall back to aligned 3+ state candidates only if
        // no near-six-way candidate exists; this avoids a huge noisy vector.
        if (view_candidates.empty()) {
            for (std::size_t offset = 0; offset + 4 <= region_size; offset += 4) {
                std::array<std::uint32_t, 6> values{};
                bool compact = true;
                for (int view = 0; view < 6; ++view) {
                    std::memcpy(&values[static_cast<std::size_t>(view)],
                                &view_first[view][offset], 4);
                    compact = compact && values[static_cast<std::size_t>(view)] <= 64;
                }
                if (compact) add_candidate(offset, 4, values, 3);
            }
        }

        std::ranges::sort(view_candidates, [](const ViewCand& a, const ViewCand& b) {
            if (a.distinct != b.distinct) return a.distinct > b.distinct;
            if (a.width != b.width) return a.width > b.width;
            return a.offset < b.offset;
        });
        constexpr std::size_t kVerificationCap = 10000;
        if (view_candidates.size() > kVerificationCap)
            view_candidates.resize(kVerificationCap);
        for (auto& snapshot : view_first) {
            snapshot.clear();
            snapshot.shrink_to_fit();
        }
        first_round_analyzed = true;
        log::info("[viewmap] round 1: {} candidate(s) retained for repeat verification",
                  view_candidates.size());
    }

    bool capture_view(std::string_view name) {
        const int view = view_index(name);
        if (view < 0 || region_size == 0 || view_counts[static_cast<std::size_t>(view)] >= 2)
            return false;
        if (!first_round_analyzed && view_counts[static_cast<std::size_t>(view)] == 1)
            return false; // complete every round-1 label before starting round 2

        std::vector<std::uint8_t> snapshot(region_size);
        const bool ok = seh_call([&] {
            std::memcpy(snapshot.data(), reinterpret_cast<const void*>(region_base), region_size);
        });
        if (!ok) {
            log::warn("[viewmap] snapshot read fault for {}", name);
            return false;
        }

        if (!first_round_analyzed) {
            view_first[view] = std::move(snapshot);
            view_counts[static_cast<std::size_t>(view)] = 1;
            log::info("[viewmap] round 1 captured {}", name);
            if (all_view_counts_at_least(1)) analyze_first_view_round();
            return true;
        }

        const std::uint8_t bit = static_cast<std::uint8_t>(1u << view);
        for (auto& candidate : view_candidates) {
            std::uint32_t value = snapshot[candidate.offset];
            if (candidate.width == 4)
                std::memcpy(&value, &snapshot[candidate.offset], 4);
            if (value == candidate.values[static_cast<std::size_t>(view)])
                candidate.verified_mask = static_cast<std::uint8_t>(candidate.verified_mask | bit);
        }
        view_counts[static_cast<std::size_t>(view)] = 2;
        log::info("[viewmap] round 2 captured {}", name);
        if (all_view_counts_at_least(2)) {
            std::erase_if(view_candidates, [](const ViewCand& candidate) {
                return candidate.verified_mask != 0x3Fu;
            });
            constexpr std::size_t kResultCap = 200;
            if (view_candidates.size() > kResultCap) view_candidates.resize(kResultCap);
            log::info("[viewmap] round 2: {} stable multi-view candidate(s)",
                      view_candidates.size());
            for (const auto& candidate : view_candidates) {
                log::info("[viewmap] +0x{:X} width={} distinct={} values={}/{}/{}/{}/{}/{}",
                          candidate.offset, candidate.width, candidate.distinct,
                          candidate.values[0], candidate.values[1], candidate.values[2],
                          candidate.values[3], candidate.values[4], candidate.values[5]);
            }
        }
        return true;
    }

    void reset_view_map() {
        for (auto& snapshot : view_first) {
            snapshot.clear();
            snapshot.shrink_to_fit();
        }
        view_counts.fill(0);
        view_candidates.clear();
        first_round_analyzed = false;
    }

    void analyze() {
        candidates.clear();
        const auto& a = snap[0];
        const auto& b = snap[1];
        const auto& c = snap[2];
        if (a.size() != region_size || b.size() != region_size || c.size() != region_size) return;

        // 4-byte granularity: enums / ints / bools stored as int.
        for (std::size_t o = 0; o + 4 <= region_size; o += 4) {
            std::uint32_t s1 = 0, s2 = 0, s3 = 0;
            std::memcpy(&s1, &a[o], 4);
            std::memcpy(&s2, &b[o], 4);
            std::memcpy(&s3, &c[o], 4);
            if (s1 == s3 && s2 != s1 && (s1 <= 64 || s2 <= 64))
                candidates.push_back({o, s1, s2, s3});
        }
        // 1-byte granularity: bool / byte flags.
        for (std::size_t o = 0; o < region_size; ++o) {
            const std::uint8_t s1 = a[o], s2 = b[o], s3 = c[o];
            if (s1 == s3 && s2 != s1 && s1 <= 1 && s2 <= 1)
                candidates.push_back({o, s1, s2, s3});
        }

        std::sort(candidates.begin(), candidates.end(),
                  [](const Cand& x, const Cand& y) { return x.offset < y.offset; });
        constexpr std::size_t kCap = 400;
        if (candidates.size() > kCap) candidates.resize(kCap);

        log::info("[memdiff] analysis: {} candidate(s)", candidates.size());
        for (const auto& cd : candidates)
            log::info("[memdiff]   +0x{:X} : {} -> {} -> {}", cd.offset, cd.s1, cd.s2, cd.s3);
    }

    std::string result_json() const {
        std::ostringstream o;
        o << "{\"captures\":" << n_captures
          << ",\"region_base\":\"0x" << std::hex << region_base << std::dec << "\""
          << ",\"region_size\":" << region_size
          << ",\"candidates\":[";
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (i) o << ",";
            o << "{\"offset\":\"+0x" << std::hex << candidates[i].offset << std::dec << "\""
              << ",\"s1\":" << candidates[i].s1
              << ",\"s2\":" << candidates[i].s2
              << ",\"s3\":" << candidates[i].s3 << "}";
        }
        o << "]}";
        return o.str();
    }

    std::string view_map_json() const {
        std::ostringstream out;
        out << "{\"phase\":\""
            << (!first_round_analyzed ? "round1" : (all_view_counts_at_least(2) ? "complete" : "round2"))
            << "\",\"counts\":{";
        for (std::size_t i = 0; i < kViewNames.size(); ++i) {
            if (i) out << ',';
            out << '\"' << kViewNames[i] << "\":" << view_counts[i];
        }
        out << "},\"candidates\":[";
        for (std::size_t i = 0; i < view_candidates.size(); ++i) {
            if (i) out << ',';
            const auto& candidate = view_candidates[i];
            out << "{\"offset\":\"+0x" << std::hex << candidate.offset << std::dec
                << "\",\"width\":" << static_cast<int>(candidate.width)
                << ",\"distinct\":" << static_cast<int>(candidate.distinct)
                << ",\"verified_mask\":" << static_cast<int>(candidate.verified_mask)
                << ",\"values\":[";
            for (std::size_t view = 0; view < candidate.values.size(); ++view) {
                if (view) out << ',';
                out << candidate.values[view];
            }
            out << "]}";
        }
        out << "]}";
        return out.str();
    }
};

MemoryDiff::MemoryDiff(const fmod::PEImage& img) noexcept { impl_ = new Impl{img}; }
MemoryDiff::~MemoryDiff() { delete impl_; impl_ = nullptr; }
void MemoryDiff::capture() noexcept { try { impl_->capture(); } catch (...) {} }
void MemoryDiff::reset() noexcept { try { impl_->reset(); } catch (...) {} }
int MemoryDiff::captures() const noexcept { return impl_->n_captures; }
std::size_t MemoryDiff::region_size() const noexcept { return impl_->region_size; }
std::string MemoryDiff::result_json() const { return impl_->result_json(); }
bool MemoryDiff::capture_view(std::string_view name) noexcept {
    try { return impl_->capture_view(name); } catch (...) { return false; }
}
void MemoryDiff::reset_view_map() noexcept { try { impl_->reset_view_map(); } catch (...) {} }
std::string MemoryDiff::view_map_json() const {
    try { return impl_->view_map_json(); } catch (...) { return "{\"error\":\"view map unavailable\"}"; }
}

} // namespace fh6r
