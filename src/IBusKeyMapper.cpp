#include "IBusKeyMapper.h"

namespace metasequoia::linux_ime
{
namespace
{
constexpr guint kHostModifierMask = IBUS_CONTROL_MASK | IBUS_MOD1_MASK | IBUS_SUPER_MASK | IBUS_META_MASK;
constexpr guint kHotkeyModifierMask = kHostModifierMask | IBUS_SHIFT_MASK;

IBusKeyTranslation dispatch(FrontendKey key, bool shift_only = false)
{
    IBusKeyTranslation translation;
    translation.disposition = IBusKeyDisposition::Dispatch;
    translation.event.key = key;
    translation.event.shift_only = shift_only;
    return translation;
}

IBusKeyTranslation forward()
{
    IBusKeyTranslation translation;
    translation.disposition = IBusKeyDisposition::Forward;
    translation.event.host_shortcut = true;
    return translation;
}

IBusKeyTranslation punctuation(char character, bool shift_only)
{
    IBusKeyTranslation translation = dispatch(FrontendKey::Punctuation, shift_only);
    translation.event.character = character;
    return translation;
}

bool is_ascii_punctuation(guint keyval)
{
    return (keyval >= '!' && keyval <= '/') || (keyval >= ':' && keyval <= '@') || (keyval >= '[' && keyval <= '`') ||
           (keyval >= '{' && keyval <= '~');
}
} // namespace

bool IBusModeToggleTracker::observe(guint keyval, guint state)
{
    const bool is_shift = keyval == IBUS_Shift_L || keyval == IBUS_Shift_R;
    const bool is_release = (state & IBUS_RELEASE_MASK) != 0;
    if (!is_shift)
    {
        shift_armed_ = false;
        return false;
    }
    if (!is_release)
    {
        shift_armed_ = (state & kHostModifierMask) == 0;
        return false;
    }

    const bool should_toggle = shift_armed_ && (state & kHostModifierMask) == 0;
    shift_armed_ = false;
    return should_toggle;
}

IBusKeyTranslation translate_ibus_key(guint keyval, guint state)
{
    if ((state & IBUS_RELEASE_MASK) != 0)
    {
        return {};
    }

    const guint hotkey_modifiers = state & kHotkeyModifierMask;
    const bool shift_only = hotkey_modifiers == IBUS_SHIFT_MASK;
    if (keyval == IBUS_period && hotkey_modifiers == IBUS_CONTROL_MASK)
    {
        return dispatch(FrontendKey::TogglePunctuation);
    }
    if (keyval == IBUS_space && hotkey_modifiers == (IBUS_CONTROL_MASK | IBUS_SHIFT_MASK))
    {
        return dispatch(FrontendKey::ToggleWidth, shift_only);
    }
    if (keyval == IBUS_E && hotkey_modifiers == (IBUS_CONTROL_MASK | IBUS_SHIFT_MASK))
    {
        return dispatch(FrontendKey::ToggleEnglish);
    }
    if ((state & kHostModifierMask) != 0)
    {
        return forward();
    }

    switch (keyval)
    {
    case IBUS_Shift_L:
    case IBUS_Shift_R:
        return {};
    case IBUS_BackSpace:
        return dispatch(FrontendKey::Backspace, shift_only);
    case IBUS_Return:
    case IBUS_KP_Enter:
        return dispatch(FrontendKey::Enter, shift_only);
    case IBUS_Escape:
        return dispatch(FrontendKey::Escape, shift_only);
    case IBUS_space:
        return dispatch(FrontendKey::Space, shift_only);
    case IBUS_Up:
        return dispatch(FrontendKey::Up, shift_only);
    case IBUS_Down:
        return dispatch(FrontendKey::Down, shift_only);
    case IBUS_Page_Up:
    case IBUS_KP_Page_Up:
    case IBUS_ISO_Left_Tab:
        return dispatch(FrontendKey::PageUp, shift_only);
    case IBUS_Page_Down:
    case IBUS_KP_Page_Down:
    case IBUS_Tab:
        return dispatch(FrontendKey::PageDown, shift_only);
    default:
        break;
    }

    IBusKeyTranslation translation;
    translation.disposition = IBusKeyDisposition::Dispatch;
    translation.event.shift_only = shift_only;
    if (keyval >= '0' && keyval <= '9')
    {
        translation.event.key = FrontendKey::Digit;
        translation.event.digit = keyval - '0';
        return translation;
    }
    if (keyval >= IBUS_KP_1 && keyval <= IBUS_KP_9)
    {
        translation.event.key = FrontendKey::Digit;
        translation.event.digit = keyval - IBUS_KP_1 + 1;
        return translation;
    }
    if ((keyval >= 'a' && keyval <= 'z') || (keyval >= 'A' && keyval <= 'Z'))
    {
        translation.event.key = FrontendKey::Character;
        translation.event.character = static_cast<char>(keyval);
        return translation;
    }
    if (is_ascii_punctuation(keyval))
    {
        return punctuation(static_cast<char>(keyval), shift_only);
    }
    return forward();
}
} // namespace metasequoia::linux_ime
