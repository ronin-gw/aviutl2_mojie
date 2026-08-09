#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mojie {

struct WidthSourceSpan {
    std::size_t begin = 0;
    std::size_t end = 0;
};

// Comparison-only representation. `key` must never replace the user's text:
// matching ranges are translated back to the original UTF-8 bytes with
// source_span_for_key_range().
struct WidthNormalizedText {
    std::wstring key;

    // One source span per UTF-16 code unit in key. All units emitted from one
    // source cluster have the same span (for example ガ -> ｶﾞ).
    std::vector<WidthSourceSpan> unit_source_spans;

    // key_cluster_boundaries[i] is true when i is a legal match boundary.
    // Its size is key.size() + 1. This prevents a rule for ｶ from consuming
    // only half of the normalized key produced from the single source ガ.
    std::vector<bool> key_cluster_boundaries;

    [[nodiscard]] bool is_cluster_boundary(std::size_t key_offset) const noexcept;

    // Returns the exact original UTF-8 byte range represented by a normalized
    // key range. Both offsets must be cluster boundaries and begin < end.
    [[nodiscard]] std::optional<WidthSourceSpan> source_span_for_key_range(
        std::size_t key_begin,
        std::size_t key_end) const noexcept;
};

// Converts valid UTF-8 to a comparison key using canonical composition (NFC)
// followed by the Japanese Windows half-width mapping. Case is preserved and
// compatibility characters unrelated to width (①, ², Ⅳ, ﬁ, etc.) are not
// folded as they would be by NFKC. Returns nullopt for malformed UTF-8 or a
// Windows normalization failure.
[[nodiscard]] std::optional<WidthNormalizedText> NormalizeWidthForMatch(
    std::string_view utf8);

} // namespace mojie
