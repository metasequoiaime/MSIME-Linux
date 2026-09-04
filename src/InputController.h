#pragma once

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
    Backspace,
    Enter,
    Escape,
    Space,
    Digit,
    Up,
    Down,
    PageUp,
    PageDown,
};

struct FrontendKeyEvent
{
    FrontendKey key = FrontendKey::Character;
    char character = 0;
    unsigned digit = 0;
    bool host_shortcut = false;
};

class InputController
{
  public:
    explicit InputController(SchemeType scheme_type, std::size_t page_size = 9);

    KeyResult handle_key(const FrontendKeyEvent &event);
    KeyResult select_candidate(std::size_t absolute_index);
    KeyResult select_page_candidate(std::size_t page_index);
    KeyResult set_mode(InputMode mode);
    KeyResult switch_scheme(SchemeType scheme_type);
    void reset();

    InputMode mode() const;
    SchemeType scheme() const;
    bool has_composition() const;
    const std::string &preedit() const;
    const std::vector<WordItem> &candidates() const;
    std::size_t highlighted_candidate() const;
    std::size_t page_size() const;
    std::size_t page_start() const;

  private:
    KeyResult commit_highlighted();
    KeyResult move_cursor(bool forward);
    KeyResult move_page(bool forward);
    void reset_highlight();

    InputSession session_;
    InputMode mode_ = InputMode::Ime;
    std::size_t page_size_ = 9;
    std::size_t highlighted_candidate_ = 0;
};
} // namespace metasequoia::linux_ime
