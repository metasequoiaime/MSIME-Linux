#pragma once

#include <string>

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
};

bool is_punctuation(char ascii);
std::string to_full_width(char ascii);
} // namespace metasequoia::linux_ime
