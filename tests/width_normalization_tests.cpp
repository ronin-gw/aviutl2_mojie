#include "width_normalization.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

#undef assert
#define assert(expression) \
    ((expression) ? static_cast<void>(0) : throw std::runtime_error("assertion failed: " #expression))

mojie::WidthNormalizedText Normalize(std::string_view input) {
    auto result = mojie::NormalizeWidthForMatch(input);
    assert(result.has_value());
    return std::move(*result);
}

void TestLatinWidthVariantsAndCase() {
    const auto ascii = Normalize("7m");
    assert(Normalize(u8"７ｍ").key == ascii.key);
    assert(Normalize(u8"7ｍ").key == ascii.key);
    assert(Normalize(u8"７m").key == ascii.key);
    assert(Normalize("7M").key != ascii.key);
    assert(Normalize(u8"７Ｍ").key == Normalize("7M").key);
}

void TestKanaAndCombiningMarks() {
    const auto full = Normalize(u8"ガ");
    const auto half = Normalize(u8"ｶﾞ");
    const auto decomposed = Normalize(u8"カ\u3099");
    assert(full.key == half.key);
    assert(full.key == decomposed.key);
    assert(full.key == L"ｶﾞ");

    // One full-width source scalar expands to two comparison units, but only
    // the complete cluster is a legal source-consuming match.
    assert(full.key.size() == 2);
    assert(full.is_cluster_boundary(0));
    assert(!full.is_cluster_boundary(1));
    assert(full.is_cluster_boundary(2));
    assert(!full.source_span_for_key_range(0, 1).has_value());
    const auto full_span = full.source_span_for_key_range(0, 2);
    assert(full_span.has_value());
    assert(full_span->begin == 0);
    assert(full_span->end == std::string(u8"ガ").size());

    const auto half_span = half.source_span_for_key_range(0, 2);
    assert(half_span.has_value());
    assert(half_span->end == std::string(u8"ｶﾞ").size());
    const auto decomposed_span = decomposed.source_span_for_key_range(0, 2);
    assert(decomposed_span.has_value());
    assert(decomposed_span->end == std::string(u8"カ\u3099").size());
}

void TestCompatibilityCharactersAreNotNfkcFolded() {
    assert(Normalize(u8"①").key != Normalize("1").key);
    assert(Normalize(u8"²").key != Normalize("2").key);
    assert(Normalize(u8"Ⅳ").key != Normalize("IV").key);
    assert(Normalize(u8"ﬁ").key != Normalize("fi").key);
    assert(Normalize(u8"㍍").key != Normalize(u8"メートル").key);
}

void TestOriginalUtf8Offsets() {
    const std::string input = u8"A７ｍガZ";
    const auto normalized = Normalize(input);
    assert(normalized.key == L"A7mｶﾞZ");

    const auto width_span = normalized.source_span_for_key_range(1, 3);
    assert(width_span.has_value());
    assert(input.substr(width_span->begin, width_span->end - width_span->begin) == u8"７ｍ");

    const auto kana_span = normalized.source_span_for_key_range(3, 5);
    assert(kana_span.has_value());
    assert(input.substr(kana_span->begin, kana_span->end - kana_span->begin) == u8"ガ");

    const auto all = normalized.source_span_for_key_range(0, normalized.key.size());
    assert(all.has_value());
    assert(all->begin == 0 && all->end == input.size());
}

void TestSupplementaryCharactersRemainMapped() {
    const std::string input = u8"😀７";
    const auto normalized = Normalize(input);
    assert(normalized.key == L"😀7");
    // UTF-16 represents the emoji with two units, neither may be split.
    assert(!normalized.is_cluster_boundary(1));
    const auto emoji = normalized.source_span_for_key_range(0, 2);
    assert(emoji.has_value());
    assert(input.substr(emoji->begin, emoji->end - emoji->begin) == u8"😀");
}

void TestEmptyAndMalformedUtf8() {
    const auto empty = Normalize("");
    assert(empty.key.empty());
    assert(empty.unit_source_spans.empty());
    assert(empty.key_cluster_boundaries.size() == 1);
    assert(empty.is_cluster_boundary(0));

    assert(!mojie::NormalizeWidthForMatch(std::string("\xc0\xaf", 2)).has_value());
    assert(!mojie::NormalizeWidthForMatch(std::string("\xed\xa0\x80", 3)).has_value());
    assert(!mojie::NormalizeWidthForMatch(std::string("\xf4\x90\x80\x80", 4)).has_value());
    assert(!mojie::NormalizeWidthForMatch(std::string("\xe3\x81", 2)).has_value());
}

} // namespace

int main() {
    TestLatinWidthVariantsAndCase();
    TestKanaAndCombiningMarks();
    TestCompatibilityCharactersAreNotNfkcFolded();
    TestOriginalUtf8Offsets();
    TestSupplementaryCharactersRemainMapped();
    TestEmptyAndMalformedUtf8();
    std::cout << "width normalization tests passed\n";
}
