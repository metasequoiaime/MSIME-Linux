#pragma once

#include "TextTransform.h"
#include "core/input_session.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace metasequoia::linux_ime
{
enum class InputMode
{
    Ime,
    Direct,
};

enum class PreeditStyle
{
    Raw,
    Pinyin,
    Hidden,
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
    std::optional<char32_t> preceding_character;

    FrontendKeyEvent() = default;
    FrontendKeyEvent(FrontendKey key_value, char character_value = 0, unsigned digit_value = 0,
                     bool host_shortcut_value = false,
                     std::optional<char32_t> preceding_character_value = std::nullopt)
        : key(key_value), character(character_value), digit(digit_value),
          host_shortcut(host_shortcut_value), preceding_character(preceding_character_value)
    {
    }
};

struct ControllerResult
{
    bool handled = false;
    std::optional<std::string> commit;
    std::optional<std::string> diagnostic;
    std::size_t delete_before = 0;
    std::size_t cursor_left = 0;

    ControllerResult() = default;
    ControllerResult(bool handled, std::optional<std::string> commit, std::size_t delete_before = 0,
                     std::size_t cursor_left = 0, std::optional<std::string> diagnostic = std::nullopt);
    ControllerResult(KeyResult result);
};

struct InputOptions
{
    std::size_t page_size = 9;
    PunctuationMode punctuation_mode = PunctuationMode::Chinese;
    CharacterWidth character_width = CharacterWidth::Half;
    bool comma_period_paging = false;
    bool word_to_character = false;
    bool bracket_paging = false;
    bool smart_punctuation = true;
    bool smart_punctuation_repeat_to_chinese = true;
    bool paired_punctuation = true;
    PreeditStyle preedit_style = PreeditStyle::Raw;
    bool quanpin_helpcode_enabled = true;
    std::string quanpin_helpcode_schema = "lantian";
    bool shuangpin_helpcode_enabled = true;
    std::string shuangpin_helpcode_schema = "lantian";
    FrequencyAdjustmentMode frequency_adjustment_mode = FrequencyAdjustmentMode::Disabled;
    int frequency_trigger_count = 1;
    int frequency_linear_step = 1;
    std::function<std::chrono::steady_clock::time_point()> now;
};

class InputController
{
  public:
    explicit InputController(SchemeType scheme_type, InputOptions options = {});
    InputController(SchemeType scheme_type, std::size_t page_size);

    ControllerResult handle_key(const FrontendKeyEvent &event);
    ControllerResult select_candidate(std::size_t absolute_index);
    ControllerResult select_page_candidate(std::size_t page_index);
    ControllerResult set_mode(InputMode mode);
    ControllerResult toggle_mode();
    ControllerResult set_punctuation_mode(PunctuationMode mode);
    ControllerResult toggle_punctuation_mode();
    ControllerResult set_character_width(CharacterWidth width);
    ControllerResult toggle_character_width();
    ControllerResult switch_scheme(SchemeType scheme_type);
    void reset();
    void invalidate_context();

    InputMode mode() const;
    PunctuationMode punctuation_mode() const;
    CharacterWidth character_width() const;
    bool comma_period_paging() const;
    bool word_to_character() const;
    bool bracket_paging() const;
    bool smart_punctuation() const;
    bool smart_punctuation_repeat_to_chinese() const;
    bool paired_punctuation() const;
    PreeditStyle preedit_style() const;
    bool quanpin_helpcode_enabled() const;
    const std::string &quanpin_helpcode_schema() const;
    bool shuangpin_helpcode_enabled() const;
    const std::string &shuangpin_helpcode_schema() const;
    FrequencyAdjustmentMode frequency_adjustment_mode() const;
    int frequency_trigger_count() const;
    int frequency_linear_step() const;
    SchemeType scheme() const;
    bool has_composition() const;
    const std::string &preedit() const;
    const std::vector<WordItem> &candidates() const;
    std::size_t highlighted_candidate() const;
    std::size_t page_size() const;
    std::size_t page_start() const;

  private:
    ControllerResult commit_highlighted();
    ControllerResult commit_punctuation(char ascii, std::optional<char32_t> preceding_character);
    ControllerResult commit_full_width(char ascii);
    ControllerResult move_cursor(bool forward);
    ControllerResult move_page(bool forward);
    void reset_highlight();
    void clear_smart_punctuation_history();
    void select_active_helpcode_schema();

    InputSession session_;
    InputMode mode_ = InputMode::Ime;
    std::size_t page_size_ = 9;
    std::size_t highlighted_candidate_ = 0;
    PunctuationMode punctuation_mode_ = PunctuationMode::Chinese;
    CharacterWidth character_width_ = CharacterWidth::Half;
    bool comma_period_paging_ = false;
    bool word_to_character_ = false;
    bool bracket_paging_ = false;
    bool smart_punctuation_ = true;
    bool smart_punctuation_repeat_to_chinese_ = true;
    bool paired_punctuation_ = true;
    PreeditStyle preedit_style_ = PreeditStyle::Raw;
    bool quanpin_helpcode_enabled_ = true;
    std::string quanpin_helpcode_schema_ = "lantian";
    bool shuangpin_helpcode_enabled_ = true;
    std::string shuangpin_helpcode_schema_ = "lantian";
    FrequencyAdjustmentMode frequency_adjustment_mode_ = FrequencyAdjustmentMode::Disabled;
    int frequency_trigger_count_ = 1;
    int frequency_linear_step_ = 1;
    std::function<std::chrono::steady_clock::time_point()> now_;
    bool smart_punctuation_history_active_ = false;
    char smart_punctuation_history_key_ = 0;
    std::chrono::steady_clock::time_point smart_punctuation_history_time_{};
    PunctuationFormatter punctuation_formatter_;
};
} // namespace metasequoia::linux_ime
