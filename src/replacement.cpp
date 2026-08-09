#include "replacement.hpp"

#include "width_normalization.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>
#include <utility>

namespace mojie {

std::string DecodeAliasTextValue(std::string_view text) {
    std::string output;
    output.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        const char current = text[index];
        if (current == '\r') {
            if (index + 1 < text.size() && text[index + 1] == '\n') {
                ++index;
            }
            output.push_back('\n');
            ++index;
            continue;
        }
        if (current != '\\' || index + 1 >= text.size()) {
            output.push_back(current);
            ++index;
            continue;
        }

        const char escaped = text[index + 1];
        if (escaped == '\\') {
            output.push_back('\\');
            index += 2;
        } else if (escaped == 'n') {
            output.push_back('\n');
            index += 2;
        } else if (escaped == 'r') {
            // Alias data may contain either \\n or \\r\\n. Normalize both to LF.
            if (index + 3 < text.size() && text[index + 2] == '\\' &&
                text[index + 3] == 'n') {
                index += 4;
            } else {
                index += 2;
            }
            output.push_back('\n');
        } else if (escaped == 't') {
            output.push_back('\t');
            index += 2;
        } else {
            output.push_back('\\');
            ++index;
        }
    }
    return output;
}

namespace {

bool IsValidUtf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i]);
        std::size_t continuation_count = 0;
        unsigned int code_point = 0;
        if (first <= 0x7f) {
            ++i;
            continue;
        }
        if (first >= 0xc2 && first <= 0xdf) {
            continuation_count = 1;
            code_point = first & 0x1f;
        } else if (first >= 0xe0 && first <= 0xef) {
            continuation_count = 2;
            code_point = first & 0x0f;
        } else if (first >= 0xf0 && first <= 0xf4) {
            continuation_count = 3;
            code_point = first & 0x07;
        } else {
            return false;
        }
        if (i + continuation_count >= text.size()) {
            return false;
        }
        for (std::size_t j = 1; j <= continuation_count; ++j) {
            const auto byte = static_cast<unsigned char>(text[i + j]);
            if ((byte & 0xc0) != 0x80) {
                return false;
            }
            code_point = (code_point << 6) | (byte & 0x3f);
        }
        if ((continuation_count == 2 && code_point < 0x800) ||
            (continuation_count == 3 && code_point < 0x10000) ||
            code_point > 0x10ffff ||
            (code_point >= 0xd800 && code_point <= 0xdfff)) {
            return false;
        }
        i += continuation_count + 1;
    }
    return true;
}

bool IsSafeEmojiName(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char value) {
        const auto c = static_cast<unsigned char>(value);
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    });
}

bool IsSafeRubyText(std::string_view text) {
    if (text.empty() || !IsValidUtf8(text) ||
        text.find_first_of("<>\r\n") != std::string_view::npos ||
        text.find("\xc2\x85") != std::string_view::npos ||
        text.find("\xe2\x80\xa8") != std::string_view::npos ||
        text.find("\xe2\x80\xa9") != std::string_view::npos) {
        return false;
    }
    return std::none_of(text.begin(), text.end(), [](char value) {
        const auto byte = static_cast<unsigned char>(value);
        return byte <= 0x1f || byte == 0x7f;
    });
}

std::string FormatNumber(double value) {
    if (std::abs(value) < 0.0000000001) {
        value = 0.0;
    }
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(12) << std::defaultfloat << value;
    return stream.str();
}

std::string FormatSignedNumber(double value) {
    if (value >= 0) {
        return "+" + FormatNumber(value);
    }
    return FormatNumber(value);
}

std::string_view Trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

std::optional<double> ParseNumber(std::string_view value) {
    value = Trim(value);
    if (value.empty()) {
        return std::nullopt;
    }
    std::string owned(value);
    std::istringstream stream(owned);
    stream.imbue(std::locale::classic());
    double result = 0.0;
    stream >> result;
    if (!stream || !stream.eof() || !std::isfinite(result)) {
        return std::nullopt;
    }
    return result;
}

struct InlineOverrides {
    std::optional<double> size_percent;
    std::optional<double> padding_x;
    std::optional<double> padding_y;
    std::optional<std::string_view> ruby_text;
    std::optional<double> ruby_size_percent;
    std::size_t consumed = 0;
};

enum class InlineParseResult {
    NotPresent,
    Parsed,
    Invalid,
};

InlineParseResult ParseInlineRuby(
    std::string_view suffix,
    InlineOverrides& overrides,
    std::size_t& consumed) {
    if (suffix.empty() || suffix.front() != '#') {
        return InlineParseResult::NotPresent;
    }
    const auto ruby_end = suffix.find('#', 1);
    if (ruby_end == std::string_view::npos) {
        return InlineParseResult::Invalid;
    }
    const auto ruby = suffix.substr(1, ruby_end - 1);
    if (!IsSafeRubyText(ruby)) {
        return InlineParseResult::Invalid;
    }

    std::size_t parsed_length = ruby_end + 1;
    std::optional<double> parsed_size;

    // A second, number-only field is the optional ruby-size percentage.
    // Only a number-looking next byte starts that field; this lets a following
    // image-style block (`#ruby#[80]`) remain unambiguous.
    const auto size_candidate = suffix.substr(parsed_length);
    const bool starts_size = !size_candidate.empty() &&
        ((size_candidate.front() >= '0' && size_candidate.front() <= '9') ||
         size_candidate.front() == '+' || size_candidate.front() == '-' ||
         size_candidate.front() == '.');
    if (starts_size) {
        const auto size_end = suffix.find('#', parsed_length);
        if (size_end == std::string_view::npos || size_end == parsed_length) {
            return InlineParseResult::Invalid;
        }
        const auto size = ParseNumber(suffix.substr(parsed_length, size_end - parsed_length));
        if (!size || *size <= 0.0 || *size > 10000.0) {
            return InlineParseResult::Invalid;
        }
        parsed_size = *size;
        parsed_length = size_end + 1;
    }
    overrides.ruby_text = ruby;
    overrides.ruby_size_percent = parsed_size;
    consumed = parsed_length;
    return InlineParseResult::Parsed;
}

InlineParseResult ParseInlineImageStyle(
    std::string_view suffix,
    InlineOverrides& overrides,
    std::size_t& consumed) {
    if (suffix.empty() || suffix.front() != '[') {
        return InlineParseResult::NotPresent;
    }
    const auto end = suffix.find(']', 1);
    if (end == std::string_view::npos) {
        return InlineParseResult::Invalid;
    }

    const auto fields = suffix.substr(1, end - 1);
    std::optional<double> values[3];
    std::size_t field_count = 0;
    std::size_t begin = 0;
    while (begin <= fields.size()) {
        if (field_count == 3) {
            return InlineParseResult::Invalid;
        }
        const auto comma = fields.find(',', begin);
        const auto field = fields.substr(
            begin,
            comma == std::string_view::npos ? fields.size() - begin : comma - begin);
        values[field_count] = ParseNumber(field);
        if (!values[field_count]) {
            return InlineParseResult::Invalid;
        }
        ++field_count;
        if (comma == std::string_view::npos) {
            break;
        }
        begin = comma + 1;
    }
    if (field_count == 0 || !values[0] || *values[0] <= 0.0 ||
        *values[0] > 10000.0) {
        return InlineParseResult::Invalid;
    }
    if ((field_count >= 2 && std::abs(*values[1]) > 10000.0) ||
        (field_count >= 3 && std::abs(*values[2]) > 10000.0)) {
        return InlineParseResult::Invalid;
    }

    overrides.size_percent = *values[0];
    if (field_count >= 2) {
        overrides.padding_x = *values[1];
    }
    if (field_count >= 3) {
        overrides.padding_y = *values[2];
    }
    consumed = end + 1;
    return InlineParseResult::Parsed;
}

InlineOverrides ParseInlineOverrides(std::string_view suffix) {
    InlineOverrides overrides;
    bool saw_ruby = false;
    bool saw_image_style = false;
    for (;;) {
        const auto remaining = suffix.substr(overrides.consumed);
        std::size_t consumed = 0;
        InlineParseResult result = InlineParseResult::NotPresent;
        if (!saw_ruby) {
            result = ParseInlineRuby(remaining, overrides, consumed);
            if (result == InlineParseResult::Parsed) {
                saw_ruby = true;
            }
        }
        if (result == InlineParseResult::NotPresent && !saw_image_style) {
            result = ParseInlineImageStyle(remaining, overrides, consumed);
            if (result == InlineParseResult::Parsed) {
                saw_image_style = true;
            }
        }
        if (result != InlineParseResult::Parsed) {
            break;
        }
        overrides.consumed += consumed;
    }
    return overrides;
}

void UpdateSizeState(std::string_view tag, double base_size, double& current_size) {
    if (tag.size() < 3 || tag[0] != '<' || tag[1] != 's' || tag.back() != '>') {
        return;
    }
    auto argument = tag.substr(2, tag.size() - 3);
    const auto comma = argument.find(',');
    if (comma != std::string_view::npos) {
        argument = argument.substr(0, comma);
    }
    argument = Trim(argument);
    if (argument.empty()) {
        current_size = base_size;
        return;
    }

    const char operation = argument.front();
    if (operation == '*') {
        if (const auto number = ParseNumber(argument.substr(1)); number && *number > 0.0) {
            current_size *= *number;
        }
        return;
    }
    if (operation == '+' || operation == '-') {
        if (const auto number = ParseNumber(argument)) {
            current_size = std::max(0.000001, current_size + *number);
        }
        return;
    }
    if (const auto number = ParseNumber(argument); number && *number > 0.0) {
        current_size = *number;
    }
}

void UpdateCharacterSpacingState(
    std::string_view tag,
    std::optional<double>& current_spacing) {
    if (tag.size() < 4 || tag.substr(0, 3) != "<gw" || tag.back() != '>') {
        return;
    }
    const auto argument = Trim(tag.substr(3, tag.size() - 4));
    if (argument.empty()) {
        current_spacing.reset();
        return;
    }
    if (const auto number = ParseNumber(argument); number && std::isfinite(*number)) {
        current_spacing = *number;
    }
}

std::optional<std::size_t> ProtectedBlockEnd(std::string_view text, std::size_t begin) {
    if (text[begin] != '<') {
        return std::nullopt;
    }
    if (text.substr(begin, 3) == "<//") {
        const auto end = text.find("//>", begin + 3);
        return end == std::string_view::npos ? text.size() : end + 3;
    }
    if (text.substr(begin, 2) == "<?") {
        const auto end = text.find("?>", begin + 2);
        return end == std::string_view::npos ? text.size() : end + 2;
    }
    const auto end = text.find('>', begin + 1);
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    return end + 1;
}

struct RenderedGlyphParts {
    std::string prefix;
    std::string glyph;
    std::string suffix;
};

std::vector<RenderedGlyphParts> RenderRuleParts(
    const ReplacementRule& rule,
    double current_size,
    const std::optional<double>& current_character_spacing,
    const ReplacementStyleOverride& style_override,
    const InlineOverrides& inline_overrides) {
    SizeMode size_mode =
        style_override.enabled ? style_override.size_mode : rule.size_mode;
    double size_value =
        style_override.enabled ? style_override.size_value : rule.size_value;
    double padding_x =
        style_override.enabled ? style_override.padding_x : rule.padding_x;
    double padding_y =
        style_override.enabled ? style_override.padding_y : rule.padding_y;
    if (inline_overrides.size_percent) {
        size_mode = SizeMode::Percent;
        size_value = *inline_overrides.size_percent;
    }
    if (inline_overrides.padding_x) {
        padding_x = *inline_overrides.padding_x;
    }
    if (inline_overrides.padding_y) {
        padding_y = *inline_overrides.padding_y;
    }
    std::optional<double> target_size;
    if (size_mode == SizeMode::Percent && size_value > 0.0 &&
        std::isfinite(size_value) && std::abs(size_value - 100.0) > 0.0000001) {
        target_size = current_size * size_value / 100.0;
    } else if (size_mode == SizeMode::Pixels && size_value > 0.0 &&
               std::isfinite(size_value)) {
        target_size = size_value;
    }

    std::vector<RenderedGlyphParts> rendered;
    rendered.reserve(rule.emoji_names.size());
    for (const auto& emoji_name : rule.emoji_names) {
        RenderedGlyphParts parts;
        if (target_size) {
            parts.prefix += "<s" + FormatNumber(*target_size) + ">";
        }
        if (padding_x != 0.0 || padding_y != 0.0) {
            // AviUtl2 has no per-glyph box padding. Relative positioning is the
            // native, signed approximation: X/Y move only this generated emoji.
            parts.prefix += "<p" + FormatSignedNumber(padding_x) + "," +
                      FormatSignedNumber(padding_y) + ">";
        }
        parts.glyph = "<&" + emoji_name + ">";
        if (padding_x != 0.0 || padding_y != 0.0) {
            // Undo only this glyph's relative offset so every member of a
            // composition keeps the same style and natural text flow.
            parts.suffix += "<p" + FormatSignedNumber(-padding_x) + "," +
                      FormatSignedNumber(-padding_y) + ">";
        }
        if (target_size) {
            parts.suffix += "<s" + FormatNumber(current_size) + ">";
        }
        rendered.push_back(std::move(parts));
    }
    if (rendered.size() >= 2 && rule.image_margin > 0.0 &&
        rule.image_margin <= 10000.0 && std::isfinite(rule.image_margin)) {
        // AviUtl2's native character-spacing control applies between the
        // generated emoji glyphs. Reset it immediately after the sequence so
        // following user text keeps its normal spacing.
        rendered[1].prefix.insert(
            0, "<gw" + FormatNumber(rule.image_margin) + ">");
        rendered.back().glyph += current_character_spacing
            ? "<gw" + FormatNumber(*current_character_spacing) + ">"
            : "<gw>";
    }
    return rendered;
}

std::string RenderRule(
    const ReplacementRule& rule,
    double current_size,
    const std::optional<double>& current_character_spacing,
    const ReplacementStyleOverride& style_override,
    const InlineOverrides& inline_overrides,
    std::string_view matched_source,
    const AutomaticRubySettings& automatic_ruby) {
    auto parts = RenderRuleParts(
        rule, current_size, current_character_spacing, style_override, inline_overrides);
    const bool has_inline_ruby = inline_overrides.ruby_text.has_value();
    const bool has_image_ruby = !rule.ruby_text_override.empty();
    const bool ruby_enabled = has_inline_ruby || rule.ruby_enabled;
    const std::string_view ruby_text = has_inline_ruby
        ? *inline_overrides.ruby_text
        : (has_image_ruby ? std::string_view(rule.ruby_text_override) : matched_source);
    const double ruby_size_percent = inline_overrides.ruby_size_percent
        ? *inline_overrides.ruby_size_percent
        : automatic_ruby.size_percent;

    const bool render_ruby = ruby_enabled && IsSafeRubyText(ruby_text) &&
        ruby_size_percent > 0.0 && ruby_size_percent <= 10000.0 &&
        std::isfinite(ruby_size_percent);

    std::string output;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        output += parts[index].prefix;
        if (render_ruby && index == 0) {
            output += "</>";
        }
        output += parts[index].glyph;
        if (render_ruby && index + 1 == parts.size()) {
            output += "<!" + FormatNumber(ruby_size_percent / 100.0) +
                "+>" + std::string(ruby_text) + "</>";
        }
        output += parts[index].suffix;
    }
    return output;
}

struct PendingOccurrence {
    const ReplacementRule* rule = nullptr;
    InlineOverrides overrides;
    std::string_view matched_source;
};

class OccurrenceRenderer {
public:
    OccurrenceRenderer(
        std::string& output,
        double current_size,
        const std::optional<double>& current_character_spacing,
        const ReplacementStyleOverride& style_override,
        const AutomaticRubySettings& automatic_ruby)
        : output_(output),
          current_size_(current_size),
          current_character_spacing_(current_character_spacing),
          style_override_(style_override),
          automatic_ruby_(automatic_ruby) {}

    void add(
        const ReplacementRule& rule,
        const InlineOverrides& overrides,
        std::string_view matched_source) {
        PendingOccurrence occurrence{&rule, overrides, matched_source};
        pending_.push_back(std::move(occurrence));
        if (overrides.ruby_text) {
            // An inline ruby belongs to the consecutive image run ending at
            // this occurrence. Close that run immediately so later adjacent
            // images begin a new ruby grouping candidate.
            flush();
        }
    }

    void add_literal(std::string_view literal) {
        flush();
        output_.append(literal.data(), literal.size());
    }

    void flush() {
        const double trailing_ruby_size = !pending_.empty() &&
                pending_.back().overrides.ruby_size_percent
            ? *pending_.back().overrides.ruby_size_percent
            : automatic_ruby_.size_percent;
        const bool can_group = pending_.size() >= 2 &&
            pending_.back().overrides.ruby_text.has_value() &&
            trailing_ruby_size > 0.0 && trailing_ruby_size <= 10000.0 &&
            std::isfinite(trailing_ruby_size) &&
            std::none_of(
                pending_.begin(),
                pending_.end() - 1,
                [](const PendingOccurrence& occurrence) {
                    return occurrence.overrides.ruby_text.has_value();
                });
        if (can_group) {
            // The final occurrence terminates this consecutive run with an
            // inline ruby assignment. Preserve every image's prefix/suffix controls,
            // but put the native ruby base start immediately before the first
            // glyph and its annotation immediately after the final glyph.
            const auto& last = pending_.back();
            bool ruby_started = false;
            for (std::size_t occurrence_index = 0;
                 occurrence_index < pending_.size(); ++occurrence_index) {
                const auto& occurrence = pending_[occurrence_index];
                auto parts = RenderRuleParts(
                    *occurrence.rule,
                    current_size_,
                    current_character_spacing_,
                    style_override_,
                    occurrence.overrides);
                for (std::size_t glyph_index = 0;
                     glyph_index < parts.size(); ++glyph_index) {
                    output_ += parts[glyph_index].prefix;
                    if (!ruby_started) {
                        output_ += "</>";
                        ruby_started = true;
                    }
                    output_ += parts[glyph_index].glyph;
                    if (occurrence_index + 1 == pending_.size() &&
                        glyph_index + 1 == parts.size()) {
                        output_ += "<!" +
                            FormatNumber(trailing_ruby_size / 100.0) +
                            "+>" + std::string(*last.overrides.ruby_text) +
                            "</>";
                    }
                    output_ += parts[glyph_index].suffix;
                }
            }
            pending_.clear();
            return;
        }
        for (const auto& occurrence : pending_) {
            output_ += RenderRule(
                *occurrence.rule,
                current_size_,
                current_character_spacing_,
                style_override_,
                occurrence.overrides,
                occurrence.matched_source,
                automatic_ruby_);
        }
        pending_.clear();
    }

private:
    std::string& output_;
    double current_size_;
    std::optional<double> current_character_spacing_;
    const ReplacementStyleOverride& style_override_;
    const AutomaticRubySettings& automatic_ruby_;
    std::vector<PendingOccurrence> pending_;
};

} // namespace

ReplacementEngine::ReplacementEngine(
    std::vector<ReplacementRule> rules,
    bool normalize_width,
    AutomaticRubySettings automatic_ruby)
    : normalize_width_(normalize_width),
      automatic_ruby_(automatic_ruby) {
    rules_.reserve(rules.size());
    for (auto& rule : rules) {
        if (rule.emoji_names.empty() && IsSafeEmojiName(rule.emoji_name)) {
            rule.emoji_names.push_back(rule.emoji_name);
        }
        const bool has_safe_images = !rule.emoji_names.empty() &&
            std::all_of(
                rule.emoji_names.begin(), rule.emoji_names.end(),
                [](const std::string& name) { return IsSafeEmojiName(name); });
        if (!rule.match_text.empty() && IsValidUtf8(rule.match_text) &&
            has_safe_images) {
            std::wstring normalized_key;
            if (normalize_width_) {
                const auto normalized = NormalizeWidthForMatch(rule.match_text);
                if (!normalized || normalized->key.empty()) {
                    continue;
                }
                normalized_key = normalized->key;
            }
            rules_.push_back(std::move(rule));
            normalized_rule_keys_.push_back(std::move(normalized_key));
            if (normalize_width_) {
                const std::size_t index = rules_.size() - 1;
                normalized_rule_buckets_[normalized_rule_keys_.back().front()].push_back(index);
            }
        }
    }
}

std::string ReplacementEngine::render(
    std::string_view text,
    double base_size,
    const ReplacementStyleOverride& style_override) const {
    if (!std::isfinite(base_size) || base_size <= 0.0) {
        base_size = 1.0;
    }
    double current_size = base_size;
    std::optional<double> current_character_spacing;
    std::string output;
    output.reserve(text.size());

    auto render_plain_exact = [&](std::string_view plain) {
        OccurrenceRenderer renderer(
            output, current_size, current_character_spacing,
            style_override, automatic_ruby_);
        for (std::size_t offset = 0; offset < plain.size();) {
            const ReplacementRule* best = nullptr;
            std::size_t best_index = 0;
            for (std::size_t index = 0; index < rules_.size(); ++index) {
                const auto& candidate = rules_[index];
                if (candidate.match_text.size() > plain.size() - offset ||
                    plain.compare(offset, candidate.match_text.size(), candidate.match_text) != 0) {
                    continue;
                }
                if (best == nullptr || candidate.match_text.size() > best->match_text.size() ||
                    (candidate.match_text.size() == best->match_text.size() &&
                     (candidate.priority > best->priority ||
                      (candidate.priority == best->priority && index > best_index)))) {
                    best = &candidate;
                    best_index = index;
                }
            }
            if (best != nullptr) {
                const std::size_t match_end = offset + best->match_text.size();
                const auto inline_overrides = ParseInlineOverrides(plain.substr(match_end));
                renderer.add(
                    *best,
                    inline_overrides,
                    plain.substr(offset, best->match_text.size()));
                offset = match_end + inline_overrides.consumed;
            } else {
                renderer.add_literal(plain.substr(offset, 1));
                ++offset;
            }
        }
        renderer.flush();
    };

    auto render_plain = [&](std::string_view plain) {
        if (!normalize_width_) {
            render_plain_exact(plain);
            return;
        }
        const auto normalized = NormalizeWidthForMatch(plain);
        if (!normalized || normalized->key.empty()) {
            render_plain_exact(plain);
            return;
        }

        const std::size_t output_begin = output.size();
        OccurrenceRenderer renderer(
            output, current_size, current_character_spacing,
            style_override, automatic_ruby_);
        for (std::size_t key_offset = 0; key_offset < normalized->key.size();) {
            const ReplacementRule* best = nullptr;
            std::size_t best_index = 0;
            std::size_t best_key_length = 0;
            const auto bucket = normalized_rule_buckets_.find(
                normalized->key[key_offset]);
            const std::vector<std::size_t> empty_bucket;
            const auto& candidates = bucket == normalized_rule_buckets_.end()
                ? empty_bucket
                : bucket->second;
            for (const std::size_t index : candidates) {
                const auto& candidate_key = normalized_rule_keys_[index];
                if (candidate_key.empty() ||
                    candidate_key.size() > normalized->key.size() - key_offset) {
                    continue;
                }
                const std::size_t key_end = key_offset + candidate_key.size();
                if (!normalized->is_cluster_boundary(key_end) ||
                    normalized->key.compare(
                        key_offset, candidate_key.size(), candidate_key) != 0) {
                    continue;
                }
                const auto& candidate = rules_[index];
                if (best == nullptr || candidate_key.size() > best_key_length ||
                    (candidate_key.size() == best_key_length &&
                     (candidate.priority > best->priority ||
                      (candidate.priority == best->priority && index > best_index)))) {
                    best = &candidate;
                    best_index = index;
                    best_key_length = candidate_key.size();
                }
            }

            if (best != nullptr) {
                const std::size_t key_end = key_offset + best_key_length;
                const auto source_span = normalized->source_span_for_key_range(
                    key_offset, key_end);
                if (source_span) {
                    const auto inline_overrides = ParseInlineOverrides(
                        plain.substr(source_span->end));
                    renderer.add(
                        *best,
                        inline_overrides,
                        plain.substr(
                            source_span->begin,
                            source_span->end - source_span->begin));
                    const std::size_t consumed_source_end =
                        source_span->end + inline_overrides.consumed;
                    key_offset = key_end;
                    while (key_offset < normalized->unit_source_spans.size() &&
                           normalized->unit_source_spans[key_offset].begin <
                               consumed_source_end) {
                        ++key_offset;
                    }
                    continue;
                }
            }

            std::size_t next_boundary = key_offset + 1;
            while (next_boundary <= normalized->key.size() &&
                   !normalized->is_cluster_boundary(next_boundary)) {
                ++next_boundary;
            }
            const auto source_span = normalized->source_span_for_key_range(
                key_offset, next_boundary);
            if (!source_span) {
                renderer.flush();
                output.resize(output_begin);
                render_plain_exact(plain);
                return;
            }
            renderer.add_literal(plain.substr(
                source_span->begin,
                source_span->end - source_span->begin));
            key_offset = next_boundary;
        }
        renderer.flush();
    };

    std::size_t plain_begin = 0;
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        if (text[cursor] != '<') {
            ++cursor;
            continue;
        }
        const auto protected_end = ProtectedBlockEnd(text, cursor);
        if (!protected_end) {
            ++cursor;
            continue;
        }
        render_plain(text.substr(plain_begin, cursor - plain_begin));
        const auto tag = text.substr(cursor, *protected_end - cursor);
        output.append(tag.data(), tag.size());
        if (tag.substr(0, 3) != "<//" && tag.substr(0, 2) != "<?") {
            UpdateSizeState(tag, base_size, current_size);
            UpdateCharacterSpacingState(tag, current_character_spacing);
        }
        cursor = *protected_end;
        plain_begin = cursor;
    }
    render_plain(text.substr(plain_begin));
    return output;
}

} // namespace mojie
