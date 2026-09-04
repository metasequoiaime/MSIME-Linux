#include "InputController.h"

#include <algorithm>
#include <stdexcept>

namespace metasequoia::linux_ime
{
InputController::InputController(SchemeType scheme_type, std::size_t page_size)
    : session_(scheme_type), page_size_(page_size)
{
    if (page_size_ == 0)
    {
        throw std::invalid_argument("Candidate page size must be greater than zero.");
    }
}

KeyResult InputController::handle_key(const FrontendKeyEvent &event)
{
    if (event.host_shortcut)
    {
        KeyResult result = commit_highlighted();
        result.handled = false;
        return result;
    }

    if (mode_ == InputMode::Direct)
    {
        return {};
    }

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
        return commit_highlighted();
    case FrontendKey::Digit:
    {
        if (!has_composition() || event.digit < 1 || event.digit > 9)
        {
            return {};
        }
        if (event.digit > page_size_)
        {
            return {true, std::nullopt};
        }
        const std::size_t absolute_index = page_start() + event.digit - 1;
        if (absolute_index >= candidates().size())
        {
            return {true, std::nullopt};
        }
        return select_candidate(absolute_index);
    }
    case FrontendKey::Up:
        return move_cursor(false);
    case FrontendKey::Down:
        return move_cursor(true);
    case FrontendKey::PageUp:
        return move_page(false);
    case FrontendKey::PageDown:
        return move_page(true);
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
