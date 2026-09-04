#include "InputController.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace metasequoia::linux_ime
{
namespace
{
InputOptions options_with_page_size(std::size_t page_size)
{
    InputOptions options;
    options.page_size = page_size;
    return options;
}
} // namespace

ControllerResult::ControllerResult(bool handled_value, std::optional<std::string> commit_value,
                                   std::size_t delete_before_value, std::size_t cursor_left_value)
    : handled(handled_value), commit(std::move(commit_value)), delete_before(delete_before_value),
      cursor_left(cursor_left_value)
{
}

ControllerResult::ControllerResult(KeyResult result)
    : handled(result.handled), commit(std::move(result.commit))
{
}

InputController::InputController(SchemeType scheme_type, InputOptions options)
    : session_(scheme_type), page_size_(options.page_size), punctuation_mode_(options.punctuation_mode),
      character_width_(options.character_width), comma_period_paging_(options.comma_period_paging),
      word_to_character_(options.word_to_character), bracket_paging_(options.bracket_paging),
      smart_punctuation_(options.smart_punctuation),
      smart_punctuation_repeat_to_chinese_(options.smart_punctuation_repeat_to_chinese),
      paired_punctuation_(options.paired_punctuation),
      now_(options.now ? std::move(options.now) : [] { return std::chrono::steady_clock::now(); })
{
    if (page_size_ == 0)
    {
        throw std::invalid_argument("Candidate page size must be greater than zero.");
    }
}

InputController::InputController(SchemeType scheme_type, std::size_t page_size)
    : InputController(scheme_type, options_with_page_size(page_size))
{
}

ControllerResult InputController::handle_key(const FrontendKeyEvent &event)
{
    if (event.key != FrontendKey::Punctuation || event.character != smart_punctuation_history_key_)
    {
        clear_smart_punctuation_history();
    }

    if (event.host_shortcut)
    {
        ControllerResult result = commit_highlighted();
        invalidate_context();
        result.handled = false;
        return result;
    }

    if (event.key == FrontendKey::TogglePunctuation)
    {
        return toggle_punctuation_mode();
    }
    if (event.key == FrontendKey::ToggleWidth)
    {
        return toggle_character_width();
    }

    if (mode_ == InputMode::Ime)
    {
        ControllerResult result;
        switch (event.key)
        {
        case FrontendKey::Character:
            result = session_.handle_character(event.character);
            if (result.handled)
            {
                reset_highlight();
            }
            return result;
        case FrontendKey::Punctuation:
            if (event.character == '\'' && has_composition())
            {
                result = session_.handle_character(event.character);
                if (result.handled)
                {
                    reset_highlight();
                }
                return result;
            }
            if (has_composition())
            {
                if (bracket_paging_ && (event.character == '[' || event.character == ']'))
                {
                    return move_page(event.character == ']');
                }
                if (word_to_character_ && !bracket_paging_ &&
                    (event.character == '[' || event.character == ']'))
                {
                    result = session_.select_candidate_edge(
                        highlighted_candidate_, event.character == '[' ? CandidateEdge::FirstHan : CandidateEdge::LastHan);
                    if (result.handled)
                    {
                        reset_highlight();
                        return result;
                    }
                }
                if (event.character == '-' || event.character == '_')
                {
                    return move_page(false);
                }
                if (event.character == '=' || event.character == '+')
                {
                    return move_page(true);
                }
                if (comma_period_paging_ && (event.character == ',' || event.character == '.'))
                {
                    return move_page(event.character == '.');
                }
            }
            return commit_punctuation(event.character, event.preceding_character);
        case FrontendKey::Backspace:
            result = session_.handle_command(Command::Backspace);
            if (result.handled)
            {
                reset_highlight();
            }
            return result;
        case FrontendKey::Enter:
            result = session_.handle_command(Command::CommitRaw);
            if (result.handled)
            {
                reset_highlight();
            }
            return result;
        case FrontendKey::Escape:
            result = session_.handle_command(Command::Cancel);
            if (result.handled)
            {
                reset_highlight();
            }
            return result;
        case FrontendKey::Space:
            if (has_composition())
            {
                return commit_highlighted();
            }
            break;
        case FrontendKey::Digit:
            if (!has_composition())
            {
                break;
            }
            if (event.digit < 1 || event.digit > 9)
            {
                return {};
            }
            if (event.digit > page_size_)
            {
                return {true, std::nullopt};
            }
            if (const std::size_t absolute_index = page_start() + event.digit - 1;
                absolute_index < candidates().size())
            {
                return select_candidate(absolute_index);
            }
            return {true, std::nullopt};
        case FrontendKey::Up:
            return move_cursor(false);
        case FrontendKey::Down:
            return move_cursor(true);
        case FrontendKey::PageUp:
            return move_page(false);
        case FrontendKey::PageDown:
            return move_page(true);
        case FrontendKey::TogglePunctuation:
        case FrontendKey::ToggleWidth:
            break;
        }
    }

    switch (event.key)
    {
    case FrontendKey::Character:
        return commit_full_width(event.character);
    case FrontendKey::Punctuation:
        return commit_punctuation(event.character, event.preceding_character);
    case FrontendKey::Space:
        return commit_full_width(' ');
    case FrontendKey::Digit:
        return event.digit <= 9 ? commit_full_width(static_cast<char>('0' + event.digit)) : ControllerResult{};
    case FrontendKey::Backspace:
    case FrontendKey::Enter:
    case FrontendKey::Escape:
    case FrontendKey::Up:
    case FrontendKey::Down:
    case FrontendKey::PageUp:
    case FrontendKey::PageDown:
    case FrontendKey::TogglePunctuation:
    case FrontendKey::ToggleWidth:
        return {};
    }
    return {};
}

ControllerResult InputController::select_candidate(std::size_t absolute_index)
{
    ControllerResult result = session_.select_candidate(absolute_index);
    if (result.handled)
    {
        reset_highlight();
    }
    return result;
}

ControllerResult InputController::select_page_candidate(std::size_t page_index)
{
    if (!has_composition() || page_index >= page_size_)
    {
        return {};
    }
    return select_candidate(page_start() + page_index);
}

ControllerResult InputController::set_mode(InputMode mode)
{
    clear_smart_punctuation_history();
    if (mode_ == mode)
    {
        return {};
    }

    ControllerResult result;
    if (mode == InputMode::Direct)
    {
        result = commit_highlighted();
    }
    mode_ = mode;
    result.handled = true;
    reset_highlight();
    return result;
}

ControllerResult InputController::toggle_mode()
{
    return set_mode(mode_ == InputMode::Ime ? InputMode::Direct : InputMode::Ime);
}

ControllerResult InputController::set_punctuation_mode(PunctuationMode mode)
{
    clear_smart_punctuation_history();
    if (punctuation_mode_ == mode)
    {
        return {};
    }
    punctuation_mode_ = mode;
    return {true, std::nullopt};
}

ControllerResult InputController::toggle_punctuation_mode()
{
    return set_punctuation_mode(punctuation_mode_ == PunctuationMode::Chinese ? PunctuationMode::English
                                                                              : PunctuationMode::Chinese);
}

ControllerResult InputController::set_character_width(CharacterWidth width)
{
    clear_smart_punctuation_history();
    if (character_width_ == width)
    {
        return {};
    }
    character_width_ = width;
    return {true, std::nullopt};
}

ControllerResult InputController::toggle_character_width()
{
    return set_character_width(character_width_ == CharacterWidth::Half ? CharacterWidth::Full
                                                                         : CharacterWidth::Half);
}

ControllerResult InputController::switch_scheme(SchemeType scheme_type)
{
    clear_smart_punctuation_history();
    if (session_.scheme() == scheme_type)
    {
        return {};
    }

    ControllerResult result = commit_highlighted();
    session_.switch_scheme(scheme_type);
    result.handled = true;
    reset_highlight();
    return result;
}

void InputController::reset()
{
    session_.handle_command(Command::Cancel);
    reset_highlight();
    invalidate_context();
}

void InputController::invalidate_context()
{
    clear_smart_punctuation_history();
    punctuation_formatter_.reset();
}

InputMode InputController::mode() const
{
    return mode_;
}

PunctuationMode InputController::punctuation_mode() const
{
    return punctuation_mode_;
}

CharacterWidth InputController::character_width() const
{
    return character_width_;
}

bool InputController::comma_period_paging() const
{
    return comma_period_paging_;
}

bool InputController::word_to_character() const
{
    return word_to_character_;
}

bool InputController::bracket_paging() const
{
    return bracket_paging_;
}

bool InputController::smart_punctuation() const
{
    return smart_punctuation_;
}

bool InputController::smart_punctuation_repeat_to_chinese() const
{
    return smart_punctuation_repeat_to_chinese_;
}

bool InputController::paired_punctuation() const
{
    return paired_punctuation_;
}

SchemeType InputController::scheme() const
{
    return session_.scheme();
}

bool InputController::has_composition() const
{
    return session_.has_composition();
}

const std::string &InputController::preedit() const
{
    return session_.preedit();
}

const std::vector<WordItem> &InputController::candidates() const
{
    return session_.candidates();
}

std::size_t InputController::highlighted_candidate() const
{
    return highlighted_candidate_;
}

std::size_t InputController::page_size() const
{
    return page_size_;
}

std::size_t InputController::page_start() const
{
    return highlighted_candidate_ / page_size_ * page_size_;
}

ControllerResult InputController::commit_highlighted()
{
    if (!has_composition())
    {
        return {};
    }

    if (candidates().empty())
    {
        return session_.handle_command(Command::CommitCandidate);
    }
    return select_candidate(highlighted_candidate_);
}

ControllerResult InputController::commit_punctuation(char ascii,
                                                     std::optional<char32_t> preceding_character)
{
    const bool had_composition = has_composition();
    ControllerResult result = commit_highlighted();

    if (result.commit.has_value() && !result.commit->empty())
    {
        const auto last = static_cast<unsigned char>(result.commit->back());
        preceding_character = last <= 0x7f ? std::optional<char32_t>(last) : std::nullopt;
    }

    const auto now = now_();
    const bool can_replace_recent_ascii =
        punctuation_mode_ == PunctuationMode::Chinese && smart_punctuation_ &&
        smart_punctuation_repeat_to_chinese_ && !had_composition && smart_punctuation_history_active_ &&
        smart_punctuation_history_key_ == ascii && preceding_character == static_cast<char32_t>(ascii) &&
        now >= smart_punctuation_history_time_ &&
        now - smart_punctuation_history_time_ <= std::chrono::seconds(2);
    if (can_replace_recent_ascii)
    {
        clear_smart_punctuation_history();
        std::string chinese = punctuation_formatter_.chinese(ascii);
        if (!chinese.empty())
        {
            return {true, std::move(chinese), 1, 0};
        }
    }

    std::string punctuation;
    if (punctuation_mode_ == PunctuationMode::Chinese && is_punctuation(ascii))
    {
        if (smart_punctuation_ && should_keep_ascii_punctuation(ascii, preceding_character))
        {
            punctuation.assign(1, ascii);
        }
        else
        {
            punctuation = punctuation_formatter_.chinese(ascii);
        }
    }
    else if (!had_composition && character_width_ == CharacterWidth::Full)
    {
        punctuation = to_full_width(ascii);
    }
    else if (had_composition)
    {
        punctuation.assign(1, ascii);
    }

    if (punctuation.empty())
    {
        clear_smart_punctuation_history();
        return result;
    }

    const bool committed_smart_ascii = punctuation_mode_ == PunctuationMode::Chinese && smart_punctuation_ &&
                                       punctuation.size() == 1 && punctuation.front() == ascii &&
                                       should_keep_ascii_punctuation(ascii, preceding_character);

    if (paired_punctuation_ && punctuation_mode_ == PunctuationMode::Chinese)
    {
        if (ascii == '"' && punctuation == "”")
        {
            punctuation = "“";
        }
        else if (ascii == '\'' && punctuation == "’")
        {
            punctuation = "‘";
        }
        std::string closing = paired_punctuation_closing(punctuation);
        if (!closing.empty())
        {
            punctuation += closing;
            result.cursor_left = 1;
        }
    }
    if (result.commit.has_value())
    {
        result.commit->append(punctuation);
    }
    else
    {
        result.commit = std::move(punctuation);
    }
    result.handled = true;
    if (committed_smart_ascii)
    {
        smart_punctuation_history_active_ = true;
        smart_punctuation_history_key_ = ascii;
        smart_punctuation_history_time_ = now;
    }
    else
    {
        clear_smart_punctuation_history();
    }
    return result;
}

ControllerResult InputController::commit_full_width(char ascii)
{
    if (character_width_ != CharacterWidth::Full)
    {
        return {};
    }
    std::string converted = to_full_width(ascii);
    if (converted.empty())
    {
        return {};
    }
    return {true, std::move(converted)};
}

ControllerResult InputController::move_cursor(bool forward)
{
    if (!has_composition() || candidates().empty())
    {
        return {};
    }

    if (forward)
    {
        highlighted_candidate_ = std::min(highlighted_candidate_ + 1, candidates().size() - 1);
    }
    else if (highlighted_candidate_ > 0)
    {
        --highlighted_candidate_;
    }
    return {true, std::nullopt};
}

ControllerResult InputController::move_page(bool forward)
{
    if (!has_composition() || candidates().empty())
    {
        return {};
    }

    if (forward)
    {
        highlighted_candidate_ = std::min(highlighted_candidate_ + page_size_, candidates().size() - 1);
    }
    else
    {
        highlighted_candidate_ = highlighted_candidate_ > page_size_ ? highlighted_candidate_ - page_size_ : 0;
    }
    return {true, std::nullopt};
}

void InputController::reset_highlight()
{
    highlighted_candidate_ = 0;
}

void InputController::clear_smart_punctuation_history()
{
    smart_punctuation_history_active_ = false;
    smart_punctuation_history_key_ = 0;
    smart_punctuation_history_time_ = {};
}
} // namespace metasequoia::linux_ime
