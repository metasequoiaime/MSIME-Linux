#pragma once

#include "TextTransform.h"
#include "core/input_session.h"

#include <cstddef>
#include <vector>

namespace metasequoia::linux_ime
{
enum class InputMode
{
    Ime,
    Direct,
};

enum class FrontendKey
{
    Character,
    Punctuation,
    Backspace,
    Enter,
    Escape,
    Space,
    Digit,
    Up,
    Down,
    PageUp,
    PageDown,
    TogglePunctuation,
    ToggleWidth,
};

struct FrontendKeyEvent
{
    FrontendKey key = FrontendKey::Character;
    char character = 0;
    unsigned digit = 0;
    bool host_shortcut = false;
};

struct InputOptions
{
    std::size_t page_size = 9;
    PunctuationMode punctuation_mode = PunctuationMode::Chinese;
    CharacterWidth character_width = CharacterWidth::Half;
    bool comma_period_paging = false;
    bool word_to_character = false;
    bool bracket_paging = false;
};

class InputController
{
  public:
    explicit InputController(SchemeType scheme_type, InputOptions options = {});
    InputController(SchemeType scheme_type, std::size_t page_size);

    KeyResult handle_key(const FrontendKeyEvent &event);
    KeyResult select_candidate(std::size_t absolute_index);
    KeyResult select_page_candidate(std::size_t page_index);
    KeyResult set_mode(InputMode mode);
    KeyResult toggle_mode();
    KeyResult set_punctuation_mode(PunctuationMode mode);
    KeyResult toggle_punctuation_mode();
    KeyResult set_character_width(CharacterWidth width);
    KeyResult toggle_character_width();
    KeyResult switch_scheme(SchemeType scheme_type);
    void reset();

    InputMode mode() const;
    PunctuationMode punctuation_mode() const;
    CharacterWidth character_width() const;
    bool comma_period_paging() const;
    bool word_to_character() const;
    bool bracket_paging() const;
    SchemeType scheme() const;
    bool has_composition() const;
    const std::string &preedit() const;
    const std::vector<WordItem> &candidates() const;
    std::size_t highlighted_candidate() const;
    std::size_t page_size() const;
    std::size_t page_start() const;

  private:
    KeyResult commit_highlighted();
    KeyResult commit_punctuation(char ascii);
    KeyResult commit_full_width(char ascii);
    KeyResult move_cursor(bool forward);
    KeyResult move_page(bool forward);
    void reset_highlight();

    InputSession session_;
    InputMode mode_ = InputMode::Ime;
    std::size_t page_size_ = 9;
    std::size_t highlighted_candidate_ = 0;
    PunctuationMode punctuation_mode_ = PunctuationMode::Chinese;
    CharacterWidth character_width_ = CharacterWidth::Half;
    bool comma_period_paging_ = false;
    bool word_to_character_ = false;
    bool bracket_paging_ = false;
    PunctuationFormatter punctuation_formatter_;
};
} // namespace metasequoia::linux_ime
