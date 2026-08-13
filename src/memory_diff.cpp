#include "fh6r/memory_diff.hpp"
#include "fh6r/log.hpp"
#include "fh6r/safe_mem.hpp"

#include <algorithm>
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
};

MemoryDiff::MemoryDiff(const fmod::PEImage& img) noexcept { impl_ = new Impl{img}; }
MemoryDiff::~MemoryDiff() { delete impl_; impl_ = nullptr; }
void MemoryDiff::capture() noexcept { try { impl_->capture(); } catch (...) {} }
void MemoryDiff::reset() noexcept { try { impl_->reset(); } catch (...) {} }
int MemoryDiff::captures() const noexcept { return impl_->n_captures; }
std::size_t MemoryDiff::region_size() const noexcept { return impl_->region_size; }
std::string MemoryDiff::result_json() const { return impl_->result_json(); }

} // namespace fh6r
