#include "width_normalization.hpp"

#include <windows.h>

#include <cstdint>
#include <limits>
#include <utility>

namespace mojie {
namespace {

struct Scalar {
    std::uint32_t value = 0;
    std::size_t byte_begin = 0;
    std::size_t byte_end = 0;
};

bool DecodeUtf8(std::string_view input, std::vector<Scalar>& output) {
    for (std::size_t offset = 0; offset < input.size();) {
        const std::size_t begin = offset;
        const auto first = static_cast<unsigned char>(input[offset++]);
        std::uint32_t value = 0;
        std::size_t continuation_count = 0;
        if (first <= 0x7f) {
            value = first;
        } else if (first >= 0xc2 && first <= 0xdf) {
            value = first & 0x1f;
            continuation_count = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            value = first & 0x0f;
            continuation_count = 2;
        } else if (first >= 0xf0 && first <= 0xf4) {
            value = first & 0x07;
            continuation_count = 3;
        } else {
            return false;
        }
        if (continuation_count > input.size() - offset) {
            return false;
        }
        for (std::size_t index = 0; index < continuation_count; ++index) {
            const auto byte = static_cast<unsigned char>(input[offset++]);
            if ((byte & 0xc0) != 0x80) {
                return false;
            }
            value = (value << 6) | (byte & 0x3f);
        }
        if ((continuation_count == 1 && value < 0x80) ||
            (continuation_count == 2 && value < 0x800) ||
            (continuation_count == 3 && value < 0x10000) ||
            value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
            return false;
        }
        output.push_back({value, begin, offset});
    }
    return true;
}

void AppendUtf16(std::uint32_t value, std::wstring& output) {
    if (value <= 0xffff) {
        output.push_back(static_cast<wchar_t>(value));
        return;
    }
    value -= 0x10000;
    output.push_back(static_cast<wchar_t>(0xd800 + (value >> 10)));
    output.push_back(static_cast<wchar_t>(0xdc00 + (value & 0x3ff)));
}

bool IsVariationSelector(std::uint32_t value) {
    return (value >= 0xfe00 && value <= 0xfe0f) ||
           (value >= 0xe0100 && value <= 0xe01ef);
}

bool IsCombiningScalar(std::uint32_t value) {
    if (value == 0xff9e || value == 0xff9f || IsVariationSelector(value)) {
        return true;
    }
    std::wstring encoded;
    AppendUtf16(value, encoded);
    std::vector<WORD> types(encoded.size());
    if (!GetStringTypeW(
            CT_CTYPE3, encoded.data(), static_cast<int>(encoded.size()), types.data())) {
        return false;
    }
    for (const WORD type : types) {
        // C3_DIACRITIC/C3_VOWELMARK are also reported for some precomposed or
        // spacing characters. Only NONSPACING is safe as a continuation here;
        // the half-width voiced marks are handled explicitly above.
        if ((type & C3_NONSPACING) != 0) {
            return true;
        }
    }
    return false;
}

std::optional<std::wstring> NormalizeNfc(std::wstring_view input) {
    if (input.empty()) {
        return std::wstring{};
    }
    if (input.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return std::nullopt;
    }
    const int input_size = static_cast<int>(input.size());
    const int required = NormalizeString(
        NormalizationC, input.data(), input_size, nullptr, 0);
    if (required <= 0) {
        return std::nullopt;
    }
    std::wstring output(static_cast<std::size_t>(required), L'\0');
    const int written = NormalizeString(
        NormalizationC, input.data(), input_size, output.data(), required);
    if (written <= 0) {
        return std::nullopt;
    }
    output.resize(static_cast<std::size_t>(written));
    return output;
}

std::optional<std::wstring> MapToHalfWidth(std::wstring_view input) {
    if (input.empty()) {
        return std::wstring{};
    }
    if (input.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return std::nullopt;
    }
    const int input_size = static_cast<int>(input.size());
    const int required = LCMapStringEx(
        L"ja-JP", LCMAP_HALFWIDTH, input.data(), input_size,
        nullptr, 0, nullptr, nullptr, 0);
    if (required <= 0) {
        return std::nullopt;
    }
    std::wstring output(static_cast<std::size_t>(required), L'\0');
    const int written = LCMapStringEx(
        L"ja-JP", LCMAP_HALFWIDTH, input.data(), input_size,
        output.data(), required, nullptr, nullptr, 0);
    if (written <= 0) {
        return std::nullopt;
    }
    output.resize(static_cast<std::size_t>(written));
    return output;
}

} // namespace

bool WidthNormalizedText::is_cluster_boundary(std::size_t key_offset) const noexcept {
    return key_offset < key_cluster_boundaries.size() &&
           key_cluster_boundaries[key_offset];
}

std::optional<WidthSourceSpan> WidthNormalizedText::source_span_for_key_range(
    std::size_t key_begin,
    std::size_t key_end) const noexcept {
    if (key_begin >= key_end || key_end > key.size() ||
        unit_source_spans.size() != key.size() ||
        !is_cluster_boundary(key_begin) || !is_cluster_boundary(key_end)) {
        return std::nullopt;
    }
    return WidthSourceSpan{
        unit_source_spans[key_begin].begin,
        unit_source_spans[key_end - 1].end,
    };
}

std::optional<WidthNormalizedText> NormalizeWidthForMatch(std::string_view utf8) {
    std::vector<Scalar> scalars;
    scalars.reserve(utf8.size());
    if (!DecodeUtf8(utf8, scalars)) {
        return std::nullopt;
    }

    WidthNormalizedText result;
    result.key_cluster_boundaries.push_back(true);
    for (std::size_t begin = 0; begin < scalars.size();) {
        std::size_t end = begin + 1;
        while (end < scalars.size() && IsCombiningScalar(scalars[end].value)) {
            ++end;
        }

        std::wstring cluster;
        for (std::size_t index = begin; index < end; ++index) {
            AppendUtf16(scalars[index].value, cluster);
        }
        auto nfc = NormalizeNfc(cluster);
        if (!nfc) {
            return std::nullopt;
        }
        auto folded = MapToHalfWidth(*nfc);
        if (!folded || folded->empty()) {
            return std::nullopt;
        }

        const WidthSourceSpan span{
            scalars[begin].byte_begin,
            scalars[end - 1].byte_end,
        };
        result.key.append(*folded);
        result.unit_source_spans.insert(
            result.unit_source_spans.end(), folded->size(), span);
        result.key_cluster_boundaries.insert(
            result.key_cluster_boundaries.end(), folded->size(), false);
        result.key_cluster_boundaries.back() = true;
        begin = end;
    }
    return result;
}

} // namespace mojie
