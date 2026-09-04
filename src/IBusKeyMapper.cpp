#include "IBusKeyMapper.h"

namespace metasequoia::linux_ime
{
namespace
{
constexpr guint kHostModifierMask = IBUS_CONTROL_MASK | IBUS_MOD1_MASK | IBUS_SUPER_MASK | IBUS_META_MASK;

IBusKeyTranslation dispatch(FrontendKey key)
{
    IBusKeyTranslation translation;
    translation.disposition = IBusKeyDisposition::Dispatch;
    translation.event.key = key;
    return translation;
}

IBusKeyTranslation forward()
{
    IBusKeyTranslation translation;
    translation.disposition = IBusKeyDisposition::Forward;
    translation.event.host_shortcut = true;
    return translation;
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
        return dispatch(FrontendKey::Backspace);
    case IBUS_Return:
    case IBUS_KP_Enter:
        return dispatch(FrontendKey::Enter);
    case IBUS_Escape:
        return dispatch(FrontendKey::Escape);
    case IBUS_space:
        return dispatch(FrontendKey::Space);
    case IBUS_Up:
        return dispatch(FrontendKey::Up);
    case IBUS_Down:
        return dispatch(FrontendKey::Down);
    case IBUS_Page_Up:
    case IBUS_KP_Page_Up:
    case IBUS_minus:
    case IBUS_comma:
    case IBUS_ISO_Left_Tab:
        return dispatch(FrontendKey::PageUp);
    case IBUS_Page_Down:
    case IBUS_KP_Page_Down:
    case IBUS_equal:
    case IBUS_period:
    case IBUS_Tab:
        return dispatch(FrontendKey::PageDown);
    default:
        break;
    }

    IBusKeyTranslation translation;
    translation.disposition = IBusKeyDisposition::Dispatch;
    if (keyval >= '1' && keyval <= '9')
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
    if ((keyval >= 'a' && keyval <= 'z') || (keyval >= 'A' && keyval <= 'Z') || keyval == IBUS_apostrophe)
    {
        translation.event.key = FrontendKey::Character;
        translation.event.character = static_cast<char>(keyval);
        return translation;
    }
    return forward();
}
} // namespace metasequoia::linux_ime
