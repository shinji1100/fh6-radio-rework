#include "fh6r/fmod/sig_scanner.hpp"
#include "fh6r/log.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace fh6r::fmod {
namespace {
struct BytePat { std::uint8_t value = 0; bool wild = false; };
using Pattern = std::vector<BytePat>;
Pattern parse_one(std::string_view s) {
    Pattern out;
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
        if (c >= 'A' && c <= 'F') return 10 + c - 'A';
        return -1;
    };
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] == ' ' || s[i] == '\t') { ++i; continue; }
        if (s[i] == '?' && i + 1 < s.size() && s[i + 1] == '?') { out.push_back({0, true}); i += 2; continue; }
        if (i + 1 < s.size()) {
            const int hi = hex(s[i]), lo = hex(s[i + 1]);
            if (hi >= 0 && lo >= 0) { out.push_back({static_cast<std::uint8_t>((hi << 4) | lo), false}); i += 2; continue; }
        }
        ++i;
    }
    return out;
}
std::vector<Pattern> alternatives(std::string_view s) {
    std::vector<Pattern> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '|') { out.push_back(parse_one(s.substr(start, i - start))); start = i + 1; }
    }
    return out;
}
bool match(const std::byte* p, const std::byte* end, const Pattern& pat) noexcept {
    if (pat.empty() || static_cast<std::size_t>(end - p) < pat.size()) return false;
    for (std::size_t i = 0; i < pat.size(); ++i)
        if (!pat[i].wild && std::to_integer<std::uint8_t>(p[i]) != pat[i].value) return false;
    return true;
}
bool match_any(const std::byte* p, const std::byte* end, const std::vector<Pattern>& pats) noexcept {
    for (const auto& x : pats) if (match(p, end, x)) return true;
    return false;
}
std::vector<const std::byte*> anchors(const PEImage& img, std::string_view a) {
    std::vector<const std::byte*> hits;
    const auto n = a.size();
    if (!n || img.rdata_end <= img.rdata || static_cast<std::size_t>(img.rdata_end - img.rdata) < n + 1) return hits;
    for (const std::byte* p = img.rdata; p + n < img.rdata_end; ++p) {
        if (std::memcmp(p, a.data(), n) != 0 || p[n] != std::byte{0}) continue;
        if (p > img.rdata && p[-1] != std::byte{0}) continue;
        hits.push_back(p);
    }
    return hits;
}
std::vector<const std::byte*> leas_to(const PEImage& img, std::vector<const std::byte*> targets) {
    std::vector<const std::byte*> out;
    if (targets.empty()) return out;
    std::ranges::sort(targets);
    for (const std::byte* p = img.text; p + 7 <= img.text_end; ++p) {
        const auto b0 = std::to_integer<std::uint8_t>(p[0]);
        if ((b0 & 0xF8u) != 0x48u) continue; // any REX.W prefix (0x48..0x4F)
        if (std::to_integer<std::uint8_t>(p[1]) != 0x8D || (std::to_integer<std::uint8_t>(p[2]) & 0xC7) != 0x05) continue;
        std::int32_t disp{}; std::memcpy(&disp, p + 3, 4);
        const std::byte* target = p + 7 + disp;
        if (std::ranges::binary_search(targets, target)) out.push_back(p);
    }
    return out;
}
}

std::byte* find_by_anchor(const PEImage& img, std::string_view anchor, std::string_view pattern) noexcept {
    if (!img.valid()) return nullptr;
    auto a = anchors(img, anchor);
    if (a.empty()) { log::warn("[sig] anchor '{}' not found", anchor); return nullptr; }
    auto l = leas_to(img, a);
    const auto pats = alternatives(pattern);
    std::vector<std::byte*> hits;
    for (const std::byte* lea : l) {
        const auto rva = static_cast<std::uint32_t>(lea - img.base);
        auto it = std::ranges::upper_bound(img.function_rvas, rva);
        if (it == img.function_rvas.begin()) continue;
        --it;
        auto* fn = img.base + *it;
        if (match_any(fn, img.text_end, pats) && std::ranges::find(hits, fn) == hits.end()) hits.push_back(fn);
    }
    if (hits.size() != 1) { log::warn("[sig] anchor '{}' candidates={}", anchor, hits.size()); return nullptr; }
    return hits.front();
}

std::byte* find_by_pattern(const PEImage& img, std::string_view pattern) noexcept {
    if (!img.valid()) return nullptr;
    const auto pats = alternatives(pattern);
    std::byte* hit = nullptr;
    int count = 0;
    for (const auto rva : img.function_rvas) {
        auto* fn = img.base + rva;
        if (fn >= img.text && fn < img.text_end && match_any(fn, img.text_end, pats)) {
            hit = fn; if (++count > 1) { log::warn("[sig] direct pattern ambiguous in pdata"); return nullptr; }
        }
    }
    if (hit) return hit;
    count = 0;
    for (auto* p = img.text; p < img.text_end; ++p) {
        if (match_any(p, img.text_end, pats)) { hit = p; if (++count > 1) { log::warn("[sig] direct pattern ambiguous in text"); return nullptr; } }
    }
    if (!hit) log::warn("[sig] direct pattern not found");
    return hit;
}

std::vector<std::byte*> scout_anchor(const PEImage& img, std::string_view anchor) noexcept {
    std::vector<std::byte*> out;
    if (!img.valid()) return out;
    auto a = anchors(img, anchor);
    if (a.empty()) return out;
    auto l = leas_to(img, a);
    for (const std::byte* lea : l) {
        const auto rva = static_cast<std::uint32_t>(lea - img.base);
        auto it = std::ranges::upper_bound(img.function_rvas, rva);
        if (it == img.function_rvas.begin()) continue;
        --it;
        auto* fn = img.base + *it;
        if (std::ranges::find(out, fn) == out.end()) out.push_back(fn);
    }
    return out;
}

std::byte* resolve_by_anchor_unique(const PEImage& img, std::string_view anchor) noexcept {
    const auto cands = scout_anchor(img, anchor);
    if (cands.size() != 1) {
        log::warn("[sig] anchor '{}' candidates={} (need exactly 1)", anchor, cands.size());
        return nullptr;
    }
    return cands.front();
}
} // namespace fh6r::fmod
