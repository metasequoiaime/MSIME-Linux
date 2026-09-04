#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace metasequoia::linux_ime
{
enum class PunctuationMode
{
    Chinese,
    English,
};

enum class CharacterWidth
{
    Half,
    Full,
};

class PunctuationFormatter
{
  public:
    std::string chinese(char ascii);
    void reset();

  private:
    bool double_quote_open_ = true;
    bool single_quote_open_ = true;
    int book_title_nesting_ = 0;
};

bool is_punctuation(char ascii);
bool should_keep_ascii_punctuation(char ascii, std::optional<char32_t> preceding_character);
std::string paired_punctuation_closing(std::string_view opening);
std::string to_full_width(char ascii);
} // namespace metasequoia::linux_ime
