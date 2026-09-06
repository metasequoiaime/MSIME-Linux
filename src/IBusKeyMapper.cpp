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

// The modifier mask cannot be used to recognise a modifier being pressed: X reports the state as it was *before* the
// event, so a Control_L key-down arrives with state == 0 and would otherwise be mistaken for an unknown key and
// forwarded as a host shortcut, which commits the composition. The keysym is the only reliable signal.
bool is_modifier_keysym(guint keyval)
{
    switch (keyval)
    {
    case IBUS_Shift_L:
    case IBUS_Shift_R:
    case IBUS_Control_L:
    case IBUS_Control_R:
    case IBUS_Caps_Lock:
    case IBUS_Shift_Lock:
    case IBUS_Meta_L:
    case IBUS_Meta_R:
    case IBUS_Alt_L:
    case IBUS_Alt_R:
    case IBUS_Super_L:
    case IBUS_Super_R:
    case IBUS_Hyper_L:
    case IBUS_Hyper_R:
    case IBUS_Num_Lock:
    case IBUS_Scroll_Lock:
    case IBUS_Mode_switch:
    case IBUS_ISO_Level3_Shift:
    case IBUS_ISO_Level3_Latch:
    case IBUS_ISO_Level5_Shift:
        return true;
    default:
        return false;
    }
}
} // namespace

void IBusModeToggleTracker::configure(const ModeToggleBindings &bindings)
{
    bindings_ = bindings;
    // A binding turned off mid-composition must not leave a half-pressed key able to toggle later.
    if (!bindings_.shift)
    {
        shift_armed_ = false;
    }
    if (!bindings_.ctrl)
    {
        ctrl_armed_ = false;
    }
    // With the chord binding off the frontend must stop swallowing its auto-repeats, so a chord held across the
    // settings change is no longer ours.
    if (!bindings_.ctrl_alt_space)
    {
        chord_held_ = false;
    }
}

bool IBusModeToggleTracker::observe(guint keyval, guint state)
{
    const bool is_release = (state & IBUS_RELEASE_MASK) != 0;

    if (bindings_.ctrl_alt_space && !is_release && keyval == IBUS_space &&
        (state & kHotkeyModifierMask) == (IBUS_CONTROL_MASK | IBUS_MOD1_MASK))
    {
        // A chord this explicit is never an accidental tap, so it does not disturb the arming of the lone-modifier
        // bindings below. Keyboard auto-repeat re-delivers the press about thirty times a second while the chord is
        // held, and every accepted press flips the language and rewrites the settings file, so only the press that
        // latches the chord toggles.
        const bool first_press = !chord_held_;
        chord_held_ = true;
        return first_press;
    }
    chord_held_ = false;

    const bool is_shift = keyval == IBUS_Shift_L || keyval == IBUS_Shift_R;
    const bool is_ctrl = keyval == IBUS_Control_L || keyval == IBUS_Control_R;
    if (!is_shift && !is_ctrl)
    {
        shift_armed_ = false;
        ctrl_armed_ = false;
        return false;
    }

    // Ctrl is itself part of kHostModifierMask, so its own press reports it as held. Ask only about
    // the modifiers that are not the key being tracked.
    const guint others = is_shift ? kHostModifierMask : (kHostModifierMask & ~IBUS_CONTROL_MASK);
    bool &armed = is_shift ? shift_armed_ : ctrl_armed_;
    const bool enabled = is_shift ? bindings_.shift : bindings_.ctrl;

    if (!is_release)
    {
        // Pressing one tracked modifier disarms the other: Shift+Ctrl is not either toggle.
        shift_armed_ = false;
        ctrl_armed_ = false;
        armed = enabled && (state & others) == 0;
        return false;
    }

    const bool should_toggle = armed && enabled && (state & others) == 0;
    armed = false;
    return should_toggle;
}

bool IBusModeToggleTracker::chord_held() const
{
    return chord_held_;
}

IBusKeyTranslation translate_ibus_key(guint keyval, guint state)
{
    if ((state & IBUS_RELEASE_MASK) != 0)
    {
        return {};
    }
    // Answered before the host-shortcut mask below so that pressing a second modifier while the first is held stays
    // inert as well: no bare modifier press should tear down the composition, and Ignore still lets the key reach the
    // application.
    if (is_modifier_keysym(keyval))
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
    case IBUS_KP_Up:
        return dispatch(FrontendKey::Up, shift_only);
    case IBUS_Down:
    case IBUS_KP_Down:
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
    // Keypad zero has to be in the range for the same reason the main-row zero is dispatched: it is the direct
    // full-width conversion, and forwarding it instead would commit the composition.
    if (keyval >= IBUS_KP_0 && keyval <= IBUS_KP_9)
    {
        translation.event.key = FrontendKey::Digit;
        translation.event.digit = keyval - IBUS_KP_0;
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
