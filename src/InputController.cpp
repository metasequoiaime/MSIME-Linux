#include "InputController.h"

#include <algorithm>
#include <stdexcept>

namespace metasequoia::linux_ime
{
InputController::InputController(SchemeType scheme_type, InputOptions options)
    : session_(scheme_type), page_size_(options.page_size), punctuation_mode_(options.punctuation_mode),
      character_width_(options.character_width), comma_period_paging_(options.comma_period_paging)
{
    if (page_size_ == 0)
    {
        throw std::invalid_argument("Candidate page size must be greater than zero.");
    }
}

InputController::InputController(SchemeType scheme_type, std::size_t page_size)
    : InputController(scheme_type, InputOptions{page_size})
{
}

KeyResult InputController::handle_key(const FrontendKeyEvent &event)
{
    if (event.host_shortcut)
    {
        KeyResult result = commit_highlighted();
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
        KeyResult result;
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
            return commit_punctuation(event.character);
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
        return commit_punctuation(event.character);
    case FrontendKey::Space:
        return commit_full_width(' ');
    case FrontendKey::Digit:
        return event.digit <= 9 ? commit_full_width(static_cast<char>('0' + event.digit)) : KeyResult{};
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

KeyResult InputController::select_candidate(std::size_t absolute_index)
{
    KeyResult result = session_.select_candidate(absolute_index);
    if (result.handled)
    {
        reset_highlight();
    }
    return result;
}

KeyResult InputController::select_page_candidate(std::size_t page_index)
{
    if (!has_composition() || page_index >= page_size_)
    {
        return {};
    }
    return select_candidate(page_start() + page_index);
}

KeyResult InputController::set_mode(InputMode mode)
{
    if (mode_ == mode)
    {
        return {};
    }

    KeyResult result;
    if (mode == InputMode::Direct)
    {
        result = commit_highlighted();
    }
    mode_ = mode;
    result.handled = true;
    reset_highlight();
    return result;
}

KeyResult InputController::toggle_mode()
{
    return set_mode(mode_ == InputMode::Ime ? InputMode::Direct : InputMode::Ime);
}

KeyResult InputController::set_punctuation_mode(PunctuationMode mode)
{
    if (punctuation_mode_ == mode)
    {
        return {};
    }
    punctuation_mode_ = mode;
    return {true, std::nullopt};
}

KeyResult InputController::toggle_punctuation_mode()
{
    return set_punctuation_mode(punctuation_mode_ == PunctuationMode::Chinese ? PunctuationMode::English
                                                                              : PunctuationMode::Chinese);
}

KeyResult InputController::set_character_width(CharacterWidth width)
{
    if (character_width_ == width)
    {
        return {};
    }
    character_width_ = width;
    return {true, std::nullopt};
}

KeyResult InputController::toggle_character_width()
{
    return set_character_width(character_width_ == CharacterWidth::Half ? CharacterWidth::Full
                                                                         : CharacterWidth::Half);
}

KeyResult InputController::switch_scheme(SchemeType scheme_type)
{
    if (session_.scheme() == scheme_type)
    {
        return {};
    }

    KeyResult result = commit_highlighted();
    session_.switch_scheme(scheme_type);
    result.handled = true;
    reset_highlight();
    return result;
}

void InputController::reset()
{
    session_.handle_command(Command::Cancel);
    reset_highlight();
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

KeyResult InputController::commit_highlighted()
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

KeyResult InputController::commit_punctuation(char ascii)
{
    const bool had_composition = has_composition();
    KeyResult result = commit_highlighted();

    std::string punctuation;
    if (punctuation_mode_ == PunctuationMode::Chinese && is_punctuation(ascii))
    {
        punctuation = punctuation_formatter_.chinese(ascii);
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
        return result;
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
    return result;
}

KeyResult InputController::commit_full_width(char ascii)
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

KeyResult InputController::move_cursor(bool forward)
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

KeyResult InputController::move_page(bool forward)
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
} // namespace metasequoia::linux_ime
