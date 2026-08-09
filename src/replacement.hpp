#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mojie {

// FILTER_ITEM_TEXT values returned through obj.getvalue() use AviUtl2 alias
// escaping. Convert escaped line breaks and tabs before handing the value back
// to the standard Text renderer. A doubled backslash remains a literal one.
[[nodiscard]] std::string DecodeAliasTextValue(std::string_view text);

enum class SizeMode {
    LineHeight,
    Percent,
    Pixels,
};

struct ReplacementRule {
    std::string match_text;
    // Ordered image sequence for one replacement occurrence. When empty,
    // emoji_name is used as a single-image sequence for compatibility.
    std::vector<std::string> emoji_names;
    std::string emoji_name;
    double image_margin = 0.0;
    // Empty means no image-specific ruby override.
    bool ruby_enabled = false;
    std::string ruby_text_override;
    SizeMode size_mode = SizeMode::LineHeight;
    double size_value = 100.0;
    double padding_x = 0.0;
    double padding_y = 0.0;
    int priority = 0;
};

struct AutomaticRubySettings {
    double size_percent = 50.0;
};

struct ReplacementStyleOverride {
    bool enabled = false;
    SizeMode size_mode = SizeMode::LineHeight;
    double size_value = 100.0;
    double padding_x = 0.0;
    double padding_y = 0.0;
};

class ReplacementEngine {
public:
    explicit ReplacementEngine(
        std::vector<ReplacementRule> rules,
        bool normalize_width = false,
        AutomaticRubySettings automatic_ruby = {});

    [[nodiscard]] std::string render(
        std::string_view text,
        double base_size,
        const ReplacementStyleOverride& style_override = {}) const;

private:
    std::vector<ReplacementRule> rules_;
    std::vector<std::wstring> normalized_rule_keys_;
    std::unordered_map<wchar_t, std::vector<std::size_t>> normalized_rule_buckets_;
    bool normalize_width_ = false;
    AutomaticRubySettings automatic_ruby_;
};

} // namespace mojie
