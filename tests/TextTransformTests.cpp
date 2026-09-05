#include "../src/TextTransform.h"

#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
using metasequoia::linux_ime::is_punctuation;
using metasequoia::linux_ime::paired_punctuation_closing;
using metasequoia::linux_ime::PunctuationFormatter;
using metasequoia::linux_ime::should_keep_ascii_punctuation;
using metasequoia::linux_ime::to_full_width;

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}
} // namespace

int main()
{
    PunctuationFormatter formatter;
    constexpr std::array<std::pair<char, const char *>, 25> mappings{{
        {'`', "·"},  {'~', "~"},  {'!', "！"}, {'@', "@"},   {'#', "#"},  {'$', "￥"}, {'%', "%"},
        {'^', "……"}, {'&', "&"},  {'*', "*"},  {'(', "（"},  {')', "）"}, {'_', "——"}, {'[', "【"},
        {']', "】"}, {'{', "{"},  {'}', "}"},  {'\\', "、"}, {';', "；"}, {':', "："}, {',', "，"},
        {'<', "《"}, {'.', "。"}, {'>', "》"}, {'?', "？"},
    }};
    for (const auto &[ascii, expected] : mappings)
    {
        require(is_punctuation(ascii), "A Windows-compatible punctuation key was not recognized.");
        require(formatter.chinese(ascii) == expected, "A Windows-compatible punctuation mapping changed.");
    }
    require(!is_punctuation('-') && formatter.chinese('-').empty(),
            "An unmapped punctuation key unexpectedly gained a Chinese form.");

    require(formatter.chinese('"') == "“", "The first double quote was not opening.");
    require(formatter.chinese('"') == "”", "The second double quote was not closing.");
    require(formatter.chinese('\'') == "‘", "The first apostrophe was not opening.");
    require(formatter.chinese('\'') == "’", "The second apostrophe was not closing.");
    formatter.reset();
    require(formatter.chinese('"') == "“" && formatter.chinese('\'') == "‘",
            "Reset did not restore opening quotation marks.");
    require(formatter.chinese('<') == "《" && formatter.chinese('<') == "〈" && formatter.chinese('>') == "〉" &&
                formatter.chinese('>') == "》",
            "Nested book-title marks did not follow the Windows punctuation stack.");
    formatter.reset();
    require(formatter.chinese('<') == "《", "Reset did not restore the outer book-title mark.");

    require(to_full_width(' ') == "　", "ASCII space did not map to U+3000.");
    require(to_full_width('!') == "！", "ASCII punctuation did not map to U+FF01.");
    require(to_full_width('A') == "Ａ", "ASCII uppercase did not map to its full-width form.");
    require(to_full_width('z') == "ｚ", "ASCII lowercase did not map to its full-width form.");
    require(to_full_width('~') == "～", "ASCII tilde did not map to U+FF5E.");
    for (unsigned value = 0x20; value <= 0x7e; ++value)
    {
        require(!to_full_width(static_cast<char>(value)).empty(), "A printable ASCII value lacked a full-width form.");
    }
    require(to_full_width('\n').empty() && to_full_width(static_cast<char>(0x80)).empty(),
            "A non-printable or non-ASCII value gained a full-width form.");

    for (const char punctuation : {',', '.', ':'})
    {
        require(should_keep_ascii_punctuation(punctuation, U'A') && should_keep_ascii_punctuation(punctuation, U'z') &&
                    should_keep_ascii_punctuation(punctuation, U'0') &&
                    should_keep_ascii_punctuation(punctuation, U'9'),
                "Smart punctuation did not preserve ASCII after an ASCII letter or digit.");
    }
    require(!should_keep_ascii_punctuation(',', std::nullopt) && !should_keep_ascii_punctuation('.', U'中') &&
                !should_keep_ascii_punctuation('!', U'A'),
            "Smart punctuation escaped its documented key or context boundary.");

    require(paired_punctuation_closing("“") == "”" && paired_punctuation_closing("‘") == "’" &&
                paired_punctuation_closing("【") == "】" && paired_punctuation_closing("{") == "}" &&
                paired_punctuation_closing("《") == "》" && paired_punctuation_closing("〈") == "〉" &&
                paired_punctuation_closing("（") == "）",
            "A Windows-compatible paired punctuation mapping changed.");
    require(paired_punctuation_closing("。").empty(), "A non-opening punctuation mark gained a closing pair.");
    return 0;
}
