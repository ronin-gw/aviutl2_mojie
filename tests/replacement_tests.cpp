#include "replacement.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

#undef assert
#define assert(expression) \
    ((expression) ? static_cast<void>(0) : throw std::runtime_error("assertion failed: " #expression))

using mojie::ReplacementEngine;
using mojie::ReplacementRule;
using mojie::ReplacementStyleOverride;
using mojie::AutomaticRubySettings;
using mojie::SizeMode;

void TestAliasTextEscapes() {
    assert(mojie::DecodeAliasTextValue("first\\nsecond") == "first\nsecond");
    assert(mojie::DecodeAliasTextValue("first\\r\\nsecond") == "first\nsecond");
    assert(mojie::DecodeAliasTextValue("first\r\nsecond") == "first\nsecond");
    assert(mojie::DecodeAliasTextValue("column\\tvalue") == "column\tvalue");
    assert(mojie::DecodeAliasTextValue("literal\\\\n") == "literal\\n");
    assert(mojie::DecodeAliasTextValue("unknown\\q") == "unknown\\q");
    assert(mojie::DecodeAliasTextValue("trailing\\") == "trailing\\");
    assert(mojie::DecodeAliasTextValue("\\nfirst\\n\\nlast\\n") ==
           "\nfirst\n\nlast\n");
}

ReplacementRule Rule(std::string text, std::string emoji, int priority = 0) {
    ReplacementRule rule;
    rule.match_text = std::move(text);
    rule.emoji_name = std::move(emoji);
    rule.priority = priority;
    return rule;
}

ReplacementRule Composition(
    std::string text,
    std::vector<std::string> emojis,
    int priority = 0) {
    ReplacementRule rule;
    rule.match_text = std::move(text);
    rule.emoji_names = std::move(emojis);
    rule.priority = priority;
    return rule;
}

ReplacementRule WithRuby(ReplacementRule rule, std::string text = {}) {
    rule.ruby_enabled = true;
    rule.ruby_text_override = std::move(text);
    return rule;
}

void TestDecodedMultilineRendering() {
    ReplacementEngine engine({Rule(u8"猫", "cat"), Rule("dog", "dog")});
    const std::string decoded = mojie::DecodeAliasTextValue(
        u8"猫\\ndog\\n<// 猫\\ndog //>\\n<? 猫\\ndog ?>");
    assert(engine.render(decoded, 32) ==
           u8"<&cat>\n<&dog>\n<// 猫\ndog //>\n<? 猫\ndog ?>");
}

void TestUtf8AndLiteralCaseSensitivity() {
    ReplacementEngine engine({Rule(u8"猫", "cat"), Rule("Cat", "upper")});
    assert(engine.render(u8"猫とcatとCat", 32) == u8"<&cat>とcatと<&upper>");
}

void TestLongestThenPriorityThenLaterRule() {
    auto low = Rule("abc", "low", 1);
    auto high = Rule("abc", "high", 2);
    ReplacementEngine engine({Rule("ab", "short", 100), low, high, Rule("xy", "first"),
                              Rule("xy", "later")});
    assert(engine.render("abc xy ab", 32) == "<&high> <&later> <&short>");
}

void TestProtectedRegionsAndBoundaries() {
    ReplacementEngine engine({Rule("cat", "cat"), Rule("ab", "cross")});
    const std::string input =
        "cat <&cat> <#cat> <? cat > still script ?> <// cat > still comment //> a<#fff>b";
    const std::string expected =
        "<&cat> <&cat> <#cat> <? cat > still script ?> <// cat > still comment //> a<#fff>b";
    assert(engine.render(input, 32) == expected);
}

void TestSizeAndSignedPadding() {
    auto percent = Rule("half", "half");
    percent.size_mode = SizeMode::Percent;
    percent.size_value = 50;
    percent.padding_x = -3;
    percent.padding_y = 4;

    auto pixels = Rule("px", "pixel");
    pixels.size_mode = SizeMode::Pixels;
    pixels.size_value = 20;
    pixels.padding_x = 1.5;
    pixels.padding_y = -0.25;

    ReplacementEngine engine({percent, pixels});
    assert(engine.render("half px", 40) ==
           "<s20><p-3,+4><&half><p+3,-4><s40> "
           "<s20><p+1.5,-0.25><&pixel><p-1.5,+0.25><s40>");
}

void TestExistingSizeStateIsRestored() {
    auto pixels = Rule("icon", "icon");
    pixels.size_mode = SizeMode::Pixels;
    pixels.size_value = 16;
    ReplacementEngine engine({pixels});
    assert(engine.render("<s50>icon<s*2>icon<s-20>icon<s>icon", 30) ==
           "<s50><s16><&icon><s50><s*2><s16><&icon><s100><s-20><s16><&icon><s80>"
           "<s><s16><&icon><s30>");
}

void TestObjectStyleOverrideAppliesToEveryImage() {
    auto first = Rule("a", "a");
    first.size_mode = SizeMode::Pixels;
    first.size_value = 12;
    auto second = Rule("b", "b");
    second.padding_x = 9;
    ReplacementStyleOverride style;
    style.enabled = true;
    style.size_mode = SizeMode::Percent;
    style.size_value = 50;
    style.padding_x = -1.5;
    style.padding_y = 2;
    ReplacementEngine engine({first, second});
    assert(engine.render("a b", 40, style) ==
           "<s20><p-1.5,+2><&a><p+1.5,-2><s40> "
           "<s20><p-1.5,+2><&b><p+1.5,-2><s40>");
}

void TestObjectStyleOverridePreservesFlowForThreeImages() {
    ReplacementStyleOverride style;
    style.enabled = true;
    style.padding_x = 4;
    style.padding_y = -3;
    ReplacementEngine engine({Rule("a", "a"), Rule("b", "b"), Rule("c", "c")});
    assert(engine.render("abc", 40, style) ==
           "<p+4,-3><&a><p-4,+3>"
           "<p+4,-3><&b><p-4,+3>"
           "<p+4,-3><&c><p-4,+3>");
}

void TestCompositionRendersOrderedImagesAndKeepsLegacyRules() {
    ReplacementEngine engine({
        Composition("combo", {"left", "middle", "right"}),
        Rule("single", "legacy"),
    });
    assert(engine.render("combo single", 40) ==
           "<&left><&middle><&right> <&legacy>");
}

void TestCompositionImageMarginUsesNativeSpacing() {
    auto composition = Composition("combo", {"left", "middle", "right"});
    composition.image_margin = 6.5;
    ReplacementEngine engine({composition});
    assert(engine.render("combo", 40) ==
           "<&left><gw6.5><&middle><&right><gw>");
    assert(engine.render("<gw2>combo tail", 40) ==
           "<gw2><&left><gw6.5><&middle><&right><gw2> tail");

    auto single = Composition("single", {"only"});
    single.image_margin = 6.5;
    ReplacementEngine single_engine({single});
    assert(single_engine.render("single", 40) == "<&only>");
}

void TestCompositionAppliesStyleToEveryImage() {
    auto composition = Composition("x", {"left", "right"});
    composition.size_mode = SizeMode::Pixels;
    composition.size_value = 12;
    composition.padding_x = 1;
    composition.padding_y = 2;
    ReplacementEngine engine({composition});

    assert(engine.render("x", 40) ==
           "<s12><p+1,+2><&left><p-1,-2><s40>"
           "<s12><p+1,+2><&right><p-1,-2><s40>");

    ReplacementStyleOverride object;
    object.enabled = true;
    object.size_mode = SizeMode::Percent;
    object.size_value = 50;
    object.padding_x = -1;
    object.padding_y = 3;
    assert(engine.render("x", 40, object) ==
           "<s20><p-1,+3><&left><p+1,-3><s40>"
           "<s20><p-1,+3><&right><p+1,-3><s40>");

    assert(engine.render("x[75,4,-5]", 40, object) ==
           "<s30><p+4,-5><&left><p-4,+5><s40>"
           "<s30><p+4,-5><&right><p-4,+5><s40>");
}

void TestCompositionRubyUsesWholeSequenceAsOneBase() {
    auto image_ruby = Composition("x", {"left", "right"});
    image_ruby.image_margin = 5;
    image_ruby = WithRuby(std::move(image_ruby), u8"画像");
    ReplacementEngine engine(
        {image_ruby}, false, AutomaticRubySettings{40});

    assert(engine.render("x", 40) ==
           u8"</><&left><gw5><&right><gw><!0.4+>画像</>");
    assert(engine.render(u8"x#本文#60#", 40) ==
           u8"</><&left><gw5><&right><gw><!0.6+>本文</>");

    ReplacementEngine automatic(
        {WithRuby(Composition("x", {"left", "right"}))},
        false,
        AutomaticRubySettings{35});
    assert(automatic.render("x", 40) ==
           "</><&left><&right><!0.35+>x</>");

    ReplacementEngine named_aliases({
        WithRuby(Composition("primary", {"left", "right"}), u8"合成名"),
        WithRuby(Composition("alias", {"left", "right"}), u8"合成名"),
    }, false, AutomaticRubySettings{35});
    assert(named_aliases.render("primary alias", 40) ==
           u8"</><&left><&right><!0.35+>合成名</> "
           u8"</><&left><&right><!0.35+>合成名</>");
    assert(named_aliases.render(u8"alias#明示指定#", 40) ==
           u8"</><&left><&right><!0.35+>明示指定</>");
}

void TestCompositionIsOneOccurrenceInAdjacentRubyGroup() {
    ReplacementEngine engine({
        Composition("x", {"left", "right"}),
        Rule("y", "tail"),
    });
    assert(engine.render(u8"xy#全体#50#", 40) ==
           u8"</><&left><&right><&tail><!0.5+>全体</>");
    assert(engine.render(u8"xx#反復#", 40) ==
           u8"</><&left><&right><&left><&right><!0.5+>反復</>");
}

void TestCompositionPreservesMatchingRulesAndValidation() {
    ReplacementEngine priority({
        Composition("ab", {"low_a", "low_b"}, 1),
        Composition("ab", {"high_a", "high_b"}, 2),
        Composition("a", {"short_a", "short_b"}, 100),
    });
    assert(priority.render("ab a", 40) ==
           "<&high_a><&high_b> <&short_a><&short_b>");

    ReplacementEngine normalized(
        {Composition("7m", {"tile", "marker"})}, true);
    assert(normalized.render(u8"７ｍ 7ｍ", 40) ==
           "<&tile><&marker> <&tile><&marker>");

    auto invalid = Composition("bad", {"safe", "bad>name"});
    invalid.emoji_name = "legacy_is_not_a_fallback";
    ReplacementEngine validated({invalid, Composition("ok", {"safe", "safe_2"})});
    assert(validated.render("bad ok", 40) ==
           "bad <&safe><&safe_2>");
}

void TestAutomaticRubyUsesMatchedSourceText() {
    ReplacementEngine engine(
        {WithRuby(Rule("7m", "tile"))}, false, AutomaticRubySettings{45});
    assert(engine.render("7m", 40) ==
           "</><&tile><!0.45+>7m</>");

    ReplacementEngine aliases(
        {WithRuby(Rule("7m", "tile")), WithRuby(Rule("distance", "tile"))},
        false,
        AutomaticRubySettings{30});
    assert(aliases.render("distance", 40) ==
           "</><&tile><!0.3+>distance</>");

    ReplacementEngine disabled(
        {Rule("7m", "tile")}, false, AutomaticRubySettings{45});
    assert(disabled.render("7m", 40) == "<&tile>");

    ReplacementEngine unsafe_source(
        {WithRuby(Rule("\t", "tab"))}, false, AutomaticRubySettings{50});
    assert(unsafe_source.render("\t", 40) == "<&tab>");
}

void TestInlineRubyOverride() {
    ReplacementEngine engine(
        {Rule("a", "a")}, false, AutomaticRubySettings{30});

    assert(engine.render(u8"a#上書き#", 40) ==
           u8"</><&a><!0.3+>上書き</>");
    assert(engine.render(u8"a#上書き#62.5#", 40) ==
           u8"</><&a><!0.625+>上書き</>");

    ReplacementEngine automatic_off(
        {Rule("a", "a")}, false, AutomaticRubySettings{40});
    assert(automatic_off.render(u8"a#明示#", 40) ==
           u8"</><&a><!0.4+>明示</>");
}

void TestImageRubyOverridePrecedence() {
    auto image_ruby = Rule("a", "a");
    image_ruby = WithRuby(std::move(image_ruby), u8"画像指定");
    ReplacementEngine automatic_on(
        {image_ruby, WithRuby(Rule("b", "b"))}, false, AutomaticRubySettings{37.5});
    assert(automatic_on.render(u8"ab", 40) ==
           u8"</><&a><!0.375+>画像指定</></><&b><!0.375+>b</>");
    assert(automatic_on.render(u8"a#本文指定#62.5#", 40) ==
           u8"</><&a><!0.625+>本文指定</>");

    ReplacementEngine automatic_off(
        {image_ruby, Rule("b", "b")}, false, AutomaticRubySettings{45});
    assert(automatic_off.render("ab", 40) ==
           u8"</><&a><!0.45+>画像指定</><&b>");

    auto text_without_checkbox = Rule("x", "x");
    text_without_checkbox.ruby_text_override = u8"表示しない";
    ReplacementEngine unchecked(
        {text_without_checkbox}, false, AutomaticRubySettings{45});
    assert(unchecked.render("x", 40) == "<&x>");

    // A trailing inline annotation covers the entire adjacent image run and
    // suppresses both image-specific and automatic annotations within it.
    assert(automatic_on.render(u8"ab#全体#", 40) ==
           u8"</><&a><&b><!0.375+>全体</>");
}

void TestInlineImageStyleAritiesAndPrecedence() {
    auto rule = Rule("a", "a");
    rule.size_mode = SizeMode::Pixels;
    rule.size_value = 12;
    rule.padding_x = 1;
    rule.padding_y = 2;
    ReplacementEngine engine({rule});

    assert(engine.render("a[80]", 40) ==
           "<s32><p+1,+2><&a><p-1,-2><s40>");
    assert(engine.render("a[80,5]", 40) ==
           "<s32><p+5,+2><&a><p-5,-2><s40>");
    assert(engine.render("a[80,5,-6.5]", 40) ==
           "<s32><p+5,-6.5><&a><p-5,+6.5><s40>");

    ReplacementStyleOverride object;
    object.enabled = true;
    object.size_mode = SizeMode::Percent;
    object.size_value = 50;
    object.padding_x = 7;
    object.padding_y = 8;
    assert(engine.render("a[80,5]", 40, object) ==
           "<s32><p+5,+8><&a><p-5,-8><s40>");
}

void TestInlineOverridesCanBeCombinedInEitherOrder() {
    ReplacementEngine engine({Rule("a", "a")});
    const std::string expected =
        u8"<s32><p+5,-2></><&a><!0.5+>ルビ</><p-5,+2><s40>";
    assert(engine.render(u8"a#ルビ#50#[80,5,-2]", 40) == expected);
    assert(engine.render(u8"a[80,5,-2]#ルビ#50#", 40) == expected);
}

void TestTrailingInlineRubyGroupsConsecutiveImages() {
    auto first = Rule("a", "a");
    first.size_mode = SizeMode::Percent;
    first.size_value = 80;
    first.padding_x = 1;
    auto second = Rule("b", "b");
    second.padding_y = -2;
    ReplacementEngine engine({first, second, Rule("c", "c")});

    assert(engine.render(u8"abc#まとめ#40#", 50) ==
           u8"<s40><p+1,+0></><&a><p-1,+0><s50>"
           u8"<p+0,-2><&b><p+0,+2>"
           u8"<&c><!0.4+>まとめ</>");

    // Per-occurrence image overrides remain part of the combined ruby base,
    // and #...# / [...] may still appear in either order.
    const std::string expected =
        u8"<s30><p+3,+4></><&a><p-3,-4><s50>"
        u8"<s35><p-5,-2><&b><!0.625+>まとめ</>"
        u8"<p+5,+2><s50>";
    assert(engine.render(u8"a[60,3,4]b#まとめ#62.5#[70,-5]", 50) ==
           expected);
    assert(engine.render(u8"a[60,3,4]b[70,-5]#まとめ#62.5#", 50) ==
           expected);
}

void TestGroupedRubyBoundariesAndPrecedence() {
    ReplacementEngine engine(
        {WithRuby(Rule("a", "a")), WithRuby(Rule("b", "b")),
         WithRuby(Rule("c", "c"))},
        false,
        AutomaticRubySettings{30});

    assert(engine.render("abc", 40) ==
           "</><&a><!0.3+>a</>"
           "</><&b><!0.3+>b</>"
           "</><&c><!0.3+>c</>");

    // The explicit group annotation suppresses automatic per-image ruby.
    assert(engine.render(u8"abc#明示#", 40) ==
           u8"</><&a><&b><&c><!0.3+>明示</>");

    // A literal boundary flushes automatic ruby before a clean grouped run.
    assert(engine.render(u8"a bc#明示#", 40) ==
           u8"</><&a><!0.3+>a</> "
           u8"</><&b><&c><!0.3+>明示</>");

    // An inline ruby terminates the consecutive group ending at that image.
    // Ordinary characters, line breaks and protected control blocks also
    // break image adjacency.
    assert(engine.render(u8"a#前#bc#後#", 40) ==
           u8"</><&a><!0.3+>前</></><&b><&c><!0.3+>後</>");
    assert(engine.render(u8"ab#中間#c", 40) ==
           u8"</><&a><&b><!0.3+>中間</>"
           u8"</><&c><!0.3+>c</>");

    ReplacementEngine no_config({Rule("a", "a"), Rule("b", "b"), Rule("c", "c")});
    assert(no_config.render(u8"ab#中間#c", 40) ==
           u8"</><&a><&b><!0.5+>中間</><&c>");
    assert(no_config.render("a#A#bc#C#", 40) ==
           "</><&a><!0.5+>A</></><&b><&c><!0.5+>C</>");
    assert(engine.render(u8"a b#後#", 40) ==
           u8"</><&a><!0.3+>a</> </><&b><!0.3+>後</>");
    assert(engine.render(u8"a\nb#後#", 40) ==
           u8"</><&a><!0.3+>a</>\n</><&b><!0.3+>後</>");
    assert(engine.render(u8"a<#fff>b#後#", 40) ==
           u8"</><&a><!0.3+>a</><#fff></><&b><!0.3+>後</>");
}

void TestGroupedRubyRejectsInvalidInheritedSize() {
    ReplacementEngine engine(
        {Rule("a", "a"), Rule("b", "b")},
        false,
        AutomaticRubySettings{0});
    assert(engine.render(u8"ab#ルビ#", 40) == "<&a><&b>");
}

void TestGroupedRubyWithObjectStyleAndWidthNormalization() {
    ReplacementStyleOverride style;
    style.enabled = true;
    style.size_mode = SizeMode::Percent;
    style.size_value = 50;
    style.padding_x = 2;
    style.padding_y = -1;
    ReplacementEngine engine({Rule("7m", "tile"), Rule("8m", "eight")}, true);

    assert(engine.render(u8"７ｍ８ｍ#距離#45#", 40, style) ==
           u8"<s20><p+2,-1></><&tile><p-2,+1><s40>"
           u8"<s20><p+2,-1><&eight><!0.45+>距離</>"
           u8"<p-2,+1><s40>");
}

void TestDuplicateAndMalformedInlineSyntaxRemainsText() {
    ReplacementEngine engine({Rule("a", "a")});
    assert(engine.render(u8"a#一#50##二#60#", 40) ==
           u8"</><&a><!0.5+>一</>#二#60#");
    assert(engine.render("a[80][70]", 40) ==
           "<s32><&a><s40>[70]");
    assert(engine.render("a#unfinished", 40) == "<&a>#unfinished");
    assert(engine.render("a#ruby#50", 40) == "<&a>#ruby#50");
    assert(engine.render("a#ruby#0#", 40) == "<&a>#ruby#0#");
    assert(engine.render("a[]", 40) == "<&a>[]");
    assert(engine.render("a[80,]", 40) == "<&a>[80,]");
    assert(engine.render("a[80,1,2,3]", 40) == "<&a>[80,1,2,3]");
    assert(engine.render("a[80,10001,0]", 40) == "<&a>[80,10001,0]");
    assert(engine.render("a[80", 40) == "<&a>[80");
    assert(engine.render(u8"a#x\u2028y#", 40) == u8"<&a>#x\u2028y#");
}

void TestInlineSyntaxWithNormalizedMatchAndProtectedBlocks() {
    ReplacementEngine engine({Rule("7m", "tile")}, true);
    assert(engine.render(u8"７ｍ#ななめーとる#45#[75,2,3]", 40) ==
           u8"<s30><p+2,+3></><&tile><!0.45+>ななめーとる</>"
           u8"<p-2,-3><s40>");
    assert(engine.render(u8"<#７ｍ#ルビ#>[80] ７ｍ[80]", 40) ==
           u8"<#７ｍ#ルビ#>[80] <s32><&tile><s40>");
}

void TestWidthNormalizedMatching() {
    ReplacementEngine normalized({Rule("7", "seven"), Rule("7m", "tile")}, true);
    assert(normalized.render(u8"７ｍ 7ｍ ７m", 40) ==
           "<&tile> <&tile> <&tile>");
    assert(normalized.render(u8"Ｍ ｍ", 40) == u8"Ｍ ｍ");

    ReplacementEngine exact({Rule("7m", "tile")}, false);
    assert(exact.render(u8"７ｍ 7m", 40) == u8"７ｍ <&tile>");

    ReplacementEngine kana({Rule(u8"ガ", "ga")}, true);
    assert(kana.render(u8"ｶﾞX カ\u3099Y ガZ", 40) ==
           "<&ga>X <&ga>Y <&ga>Z");
    assert(kana.render(u8"①1 <#７ｍ> ７ｍ", 40) ==
           u8"①1 <#７ｍ> ７ｍ");

    ReplacementEngine automatic(
        {WithRuby(Rule("7m", "tile"))},
        true,
        AutomaticRubySettings{35});
    assert(automatic.render(u8"７ｍ 7ｍ ７m", 40) ==
           u8"</><&tile><!0.35+>７ｍ</> "
           u8"</><&tile><!0.35+>7ｍ</> "
           u8"</><&tile><!0.35+>７m</>");

    ReplacementEngine automatic_kana(
        {WithRuby(Rule(u8"ガ", "ga"))},
        true,
        AutomaticRubySettings{40});
    assert(automatic_kana.render(u8"ｶﾞ カ\u3099", 40) ==
           u8"</><&ga><!0.4+>ｶﾞ</> </><&ga><!0.4+>カ\u3099</>");
}

void TestInvalidRulesCannotInjectControls() {
    ReplacementEngine engine({Rule("x", "bad>name"), Rule("", "empty"), Rule("ok", "safe_1")});
    assert(engine.render("x ok", 32) == "x <&safe_1>");
}

void TestMalformedControlStartRemainsLiteralText() {
    ReplacementEngine engine({Rule("cat", "cat")});
    assert(engine.render("before <broken cat", 32) == "before <broken <&cat>");
    assert(engine.render("<? cat", 32) == "<? cat");
    assert(engine.render("<// cat", 32) == "<// cat");
}

} // namespace

int main() try {
    TestAliasTextEscapes();
    TestDecodedMultilineRendering();
    TestUtf8AndLiteralCaseSensitivity();
    TestLongestThenPriorityThenLaterRule();
    TestProtectedRegionsAndBoundaries();
    TestSizeAndSignedPadding();
    TestExistingSizeStateIsRestored();
    TestObjectStyleOverrideAppliesToEveryImage();
    TestObjectStyleOverridePreservesFlowForThreeImages();
    TestCompositionRendersOrderedImagesAndKeepsLegacyRules();
    TestCompositionImageMarginUsesNativeSpacing();
    TestCompositionAppliesStyleToEveryImage();
    TestCompositionRubyUsesWholeSequenceAsOneBase();
    TestCompositionIsOneOccurrenceInAdjacentRubyGroup();
    TestCompositionPreservesMatchingRulesAndValidation();
    TestAutomaticRubyUsesMatchedSourceText();
    TestInlineRubyOverride();
    TestImageRubyOverridePrecedence();
    TestInlineImageStyleAritiesAndPrecedence();
    TestInlineOverridesCanBeCombinedInEitherOrder();
    TestTrailingInlineRubyGroupsConsecutiveImages();
    TestGroupedRubyBoundariesAndPrecedence();
    TestGroupedRubyWithObjectStyleAndWidthNormalization();
    TestGroupedRubyRejectsInvalidInheritedSize();
    TestDuplicateAndMalformedInlineSyntaxRemainsText();
    TestInlineSyntaxWithNormalizedMatchAndProtectedBlocks();
    TestWidthNormalizedMatching();
    TestInvalidRulesCannotInjectControls();
    TestMalformedControlStartRemainsLiteralText();
    std::cout << "replacement tests passed\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
