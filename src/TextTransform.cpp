#include "TextTransform.h"

#include <cstdint>

namespace metasequoia::linux_ime
{
namespace
{
std::string encode_utf8(char32_t code_point)
{
    std::string encoded;
    if (code_point <= 0x7f)
    {
        encoded.push_back(static_cast<char>(code_point));
    }
    else if (code_point <= 0x7ff)
    {
        encoded.push_back(static_cast<char>(0xc0 | (code_point >> 6)));
        encoded.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    }
    else if (code_point <= 0xffff)
    {
        encoded.push_back(static_cast<char>(0xe0 | (code_point >> 12)));
        encoded.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
        encoded.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    }
    else
    {
        encoded.push_back(static_cast<char>(0xf0 | (code_point >> 18)));
        encoded.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
        encoded.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
        encoded.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    }
    return encoded;
}
} // namespace

std::string PunctuationFormatter::chinese(char ascii)
{
    if (ascii == '"')
    {
        const char *value = double_quote_open_ ? "“" : "”";
        double_quote_open_ = !double_quote_open_;
        return value;
    }
    if (ascii == '\'')
    {
        const char *value = single_quote_open_ ? "‘" : "’";
        single_quote_open_ = !single_quote_open_;
        return value;
    }

    switch (ascii)
    {
    case '`':
        return "·";
    case '~':
        return "~";
    case '!':
        return "！";
    case '@':
        return "@";
    case '#':
        return "#";
    case '$':
        return "￥";
    case '%':
        return "%";
    case '^':
        return "……";
    case '&':
        return "&";
    case '*':
        return "*";
    case '(':
        return "（";
    case ')':
        return "）";
    case '_':
        return "——";
    case '[':
        return "【";
    case ']':
        return "】";
    case '{':
        return "{";
    case '}':
        return "}";
    case '\\':
        return "、";
    case ';':
        return "；";
    case ':':
        return "：";
    case ',':
        return "，";
    case '<':
        if (book_title_nesting_++ == 0)
        {
            return "《";
        }
        return "〈";
    case '.':
        return "。";
    case '>':
        if (book_title_nesting_ > 0)
        {
            --book_title_nesting_;
            return book_title_nesting_ == 0 ? "》" : "〉";
        }
        return "》";
    case '?':
        return "？";
    default:
        return {};
    }
}

void PunctuationFormatter::reset()
{
    double_quote_open_ = true;
    single_quote_open_ = true;
    book_title_nesting_ = 0;
}

bool is_punctuation(char ascii)
{
    switch (ascii)
    {
    case '"':
    case '\'':
    case '`':
    case '~':
    case '!':
    case '@':
    case '#':
    case '$':
    case '%':
    case '^':
    case '&':
    case '*':
    case '(':
    case ')':
    case '_':
    case '[':
    case ']':
    case '{':
    case '}':
    case '\\':
    case ';':
    case ':':
    case ',':
    case '<':
    case '.':
    case '>':
    case '?':
        return true;
    default:
        return false;
    }
}

bool should_keep_ascii_punctuation(char ascii, std::optional<char32_t> preceding_character)
{
    if ((ascii != ',' && ascii != '.' && ascii != ':') || !preceding_character.has_value())
    {
        return false;
    }
    const char32_t preceding = *preceding_character;
    return (preceding >= U'0' && preceding <= U'9') || (preceding >= U'A' && preceding <= U'Z') ||
           (preceding >= U'a' && preceding <= U'z');
}

std::string paired_punctuation_closing(std::string_view opening)
{
    if (opening == "“")
    {
        return "”";
    }
    if (opening == "‘")
    {
        return "’";
    }
    if (opening == "【")
    {
        return "】";
    }
    if (opening == "{")
    {
        return "}";
    }
    if (opening == "《")
    {
        return "》";
    }
    if (opening == "〈")
    {
        return "〉";
    }
    if (opening == "（")
    {
        return "）";
    }
    return {};
}

std::string to_full_width(char ascii)
{
    const auto value = static_cast<unsigned char>(ascii);
    if (value == 0x20)
    {
        return encode_utf8(0x3000);
    }
    if (value < 0x21 || value > 0x7e)
    {
        return {};
    }
    return encode_utf8(0xff01 + value - 0x21);
}
} // namespace metasequoia::linux_ime
