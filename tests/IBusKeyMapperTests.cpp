#include "../src/IBusKeyMapper.h"

#include <ibus.h>

#include <initializer_list>
#include <stdexcept>

namespace
{
using metasequoia::linux_ime::FrontendKey;
using metasequoia::linux_ime::IBusKeyDisposition;
using metasequoia::linux_ime::IBusModeToggleTracker;
using metasequoia::linux_ime::translate_ibus_key;

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void require_key(guint keyval, guint state, FrontendKey expected, const char *message)
{
    const auto translation = translate_ibus_key(keyval, state);
    require(translation.disposition == IBusKeyDisposition::Dispatch && translation.event.key == expected, message);
}

void require_punctuation(guint keyval, guint state, char expected, const char *message)
{
    const auto translation = translate_ibus_key(keyval, state);
    require(translation.disposition == IBusKeyDisposition::Dispatch &&
                translation.event.key == FrontendKey::Punctuation && translation.event.character == expected,
            message);
}
} // namespace

int main()
{
    IBusModeToggleTracker toggle_tracker;
    require(!toggle_tracker.observe(IBUS_Shift_L, 0), "Shift toggled mode on key-down.");
    require(!toggle_tracker.observe(IBUS_Shift_L, 0), "Shift auto-repeat toggled mode.");
    require(toggle_tracker.observe(IBUS_Shift_L, IBUS_RELEASE_MASK | IBUS_SHIFT_MASK),
            "A bare Shift tap did not toggle mode on release.");
    require(!toggle_tracker.observe(IBUS_Shift_L, IBUS_RELEASE_MASK), "A repeated Shift release toggled mode twice.");

    require(!toggle_tracker.observe(IBUS_Shift_R, 0), "Right Shift toggled mode on key-down.");
    require(!toggle_tracker.observe(IBUS_A, IBUS_SHIFT_MASK), "Shift plus a character toggled mode.");
    require(!toggle_tracker.observe(IBUS_Shift_R, IBUS_RELEASE_MASK | IBUS_SHIFT_MASK),
            "Shift used as a modifier toggled mode.");

    require(!toggle_tracker.observe(IBUS_Control_L, 0), "Control toggled mode.");
    require(!toggle_tracker.observe(IBUS_Shift_L, IBUS_CONTROL_MASK), "Control+Shift toggled mode on key-down.");
    require(!toggle_tracker.observe(IBUS_Shift_L, IBUS_RELEASE_MASK | IBUS_CONTROL_MASK | IBUS_SHIFT_MASK),
            "Control+Shift toggled mode on key release.");

    // Ctrl+Alt+Space is on by default and is an ordinary chord, so it toggles on press.
    require(toggle_tracker.observe(IBUS_space, IBUS_CONTROL_MASK | IBUS_MOD1_MASK),
            "Ctrl+Alt+Space did not toggle mode.");
    // Keyboard auto-repeat re-delivers the chord press dozens of times a second while it is held. Only the press that
    // latches it may toggle, but every repeat still belongs to the IME and has to be reported as consumed.
    require(!toggle_tracker.observe(IBUS_space, IBUS_CONTROL_MASK | IBUS_MOD1_MASK),
            "Ctrl+Alt+Space auto-repeat toggled mode again.");
    require(toggle_tracker.chord_held(), "A swallowed chord repeat was not marked for consumption.");
    require(!toggle_tracker.observe(IBUS_space, IBUS_RELEASE_MASK | IBUS_CONTROL_MASK | IBUS_MOD1_MASK),
            "The chord release toggled mode.");
    require(!toggle_tracker.chord_held(), "The chord latch survived its release.");
    require(toggle_tracker.observe(IBUS_space, IBUS_CONTROL_MASK | IBUS_MOD1_MASK),
            "A re-pressed chord did not toggle mode.");
    require(!toggle_tracker.observe(IBUS_space, IBUS_CONTROL_MASK),
            "Ctrl+Space toggled mode; the desktop owns that chord.");
    require(!toggle_tracker.observe(IBUS_space, IBUS_MOD1_MASK | IBUS_SHIFT_MASK), "Alt+Shift+Space toggled mode.");

    // The latch is the discriminator the frontend uses to decide whether to consume the event, so a toggle that came
    // from a different binding must never claim it.
    require(toggle_tracker.observe(IBUS_space, IBUS_CONTROL_MASK | IBUS_MOD1_MASK),
            "Ctrl+Alt+Space did not toggle mode after an unrelated chord.");
    require(toggle_tracker.chord_held(), "The chord latch was not set by the press that toggled mode.");
    require(!toggle_tracker.observe(IBUS_Shift_L, 0), "Shift toggled mode on key-down.");
    require(!toggle_tracker.chord_held(), "A non-chord event left the chord latch set.");
    require(toggle_tracker.observe(IBUS_Shift_L, IBUS_RELEASE_MASK | IBUS_SHIFT_MASK),
            "A bare Shift tap did not toggle mode after a chord.");
    require(!toggle_tracker.chord_held(), "A lone-Shift toggle was reported as the chord.");

    IBusModeToggleTracker without_shift;
    without_shift.configure({false, false, false});
    require(!without_shift.observe(IBUS_Shift_L, 0), "Shift armed while its binding was off.");
    require(!without_shift.observe(IBUS_Shift_L, IBUS_RELEASE_MASK | IBUS_SHIFT_MASK),
            "A disabled Shift binding still toggled mode.");
    require(!without_shift.observe(IBUS_space, IBUS_CONTROL_MASK | IBUS_MOD1_MASK),
            "A disabled Ctrl+Alt+Space binding still toggled mode.");

    IBusModeToggleTracker with_ctrl;
    with_ctrl.configure({false, true, false});
    require(!with_ctrl.observe(IBUS_Control_L, 0), "Control toggled mode on key-down.");
    require(with_ctrl.observe(IBUS_Control_L, IBUS_RELEASE_MASK | IBUS_CONTROL_MASK),
            "A bare Control tap did not toggle mode when its binding was on.");
    require(!with_ctrl.observe(IBUS_A, IBUS_CONTROL_MASK), "Control plus a character toggled mode.");
    require(!with_ctrl.observe(IBUS_Control_L, IBUS_RELEASE_MASK | IBUS_CONTROL_MASK),
            "Control used as a modifier toggled mode on release.");
    require(!with_ctrl.observe(IBUS_Control_L, IBUS_MOD1_MASK), "Alt+Control armed the Control toggle.");
    require(!with_ctrl.observe(IBUS_Control_L, IBUS_RELEASE_MASK | IBUS_CONTROL_MASK | IBUS_MOD1_MASK),
            "Alt+Control toggled mode.");
    require(!with_ctrl.observe(IBUS_Shift_L, IBUS_RELEASE_MASK | IBUS_SHIFT_MASK),
            "Shift toggled mode while only the Control binding was on.");

    // Turning a binding off must not leave an already-pressed key able to toggle on its release.
    IBusModeToggleTracker disarmed;
    require(!disarmed.observe(IBUS_Shift_L, 0), "Shift toggled mode on key-down.");
    disarmed.configure({false, false, true});
    require(!disarmed.observe(IBUS_Shift_L, IBUS_RELEASE_MASK | IBUS_SHIFT_MASK),
            "A Shift held across a settings change toggled mode after its binding was turned off.");

    // Turning the chord binding off while the chord is held has to release the latch too, or the frontend keeps
    // swallowing a chord that is no longer the IME's.
    IBusModeToggleTracker chord_disarmed;
    require(chord_disarmed.observe(IBUS_space, IBUS_CONTROL_MASK | IBUS_MOD1_MASK),
            "Ctrl+Alt+Space did not toggle mode.");
    require(chord_disarmed.chord_held(), "The chord latch was not set by its first press.");
    chord_disarmed.configure({true, false, false});
    require(!chord_disarmed.chord_held(), "The chord latch survived its binding being turned off.");

    const auto shift_press = translate_ibus_key(IBUS_Shift_L, 0);
    require(shift_press.disposition == IBusKeyDisposition::Ignore,
            "A Shift press was treated as a host shortcut and committed the composition.");

    // X reports the modifier state as it was *before* the event, so a bare modifier key-down arrives with state == 0
    // and can only be recognised by its keysym; forwarding it would commit the in-progress composition.
    for (const guint modifier_keyval :
         {IBUS_Control_L, IBUS_Control_R, IBUS_Alt_L, IBUS_Alt_R, IBUS_Super_L, IBUS_Super_R, IBUS_Meta_L, IBUS_Meta_R,
          IBUS_Hyper_L, IBUS_Hyper_R, IBUS_Caps_Lock, IBUS_Shift_Lock, IBUS_Num_Lock, IBUS_Scroll_Lock,
          IBUS_Mode_switch, IBUS_ISO_Level3_Shift, IBUS_ISO_Level3_Latch, IBUS_ISO_Level5_Shift})
    {
        require(translate_ibus_key(modifier_keyval, 0).disposition == IBusKeyDisposition::Ignore,
                "A bare modifier press was treated as a host shortcut and committed the composition.");
    }
    // Pressing a second modifier while the first is held does carry the first one in the mask, so the keysym check has
    // to be answered ahead of the host-shortcut guard rather than inside the switch below it.
    require(translate_ibus_key(IBUS_Alt_L, IBUS_CONTROL_MASK).disposition == IBusKeyDisposition::Ignore,
            "A modifier pressed while another was held committed the composition.");
    require(translate_ibus_key(IBUS_Shift_L, IBUS_CONTROL_MASK).disposition == IBusKeyDisposition::Ignore,
            "Shift pressed while Control was held committed the composition.");
    require(translate_ibus_key(IBUS_Control_R, IBUS_SUPER_MASK).disposition == IBusKeyDisposition::Ignore,
            "Control pressed while Super was held committed the composition.");

    const auto release = translate_ibus_key(IBUS_a, IBUS_RELEASE_MASK);
    require(release.disposition == IBusKeyDisposition::Ignore, "A key release was dispatched.");

    for (const guint modifier : {IBUS_CONTROL_MASK, IBUS_MOD1_MASK, IBUS_SUPER_MASK, IBUS_META_MASK})
    {
        const auto shortcut = translate_ibus_key(IBUS_a, modifier);
        require(shortcut.disposition == IBusKeyDisposition::Forward && shortcut.event.host_shortcut,
                "A modified host shortcut was not forwarded.");
    }
    const auto unknown = translate_ibus_key(IBUS_F1, 0);
    require(unknown.disposition == IBusKeyDisposition::Forward && unknown.event.host_shortcut,
            "An unknown key was not forwarded.");

    require_key(IBUS_period, IBUS_CONTROL_MASK, FrontendKey::TogglePunctuation,
                "Ctrl+period did not toggle punctuation.");
    require_key(IBUS_space, IBUS_CONTROL_MASK | IBUS_SHIFT_MASK, FrontendKey::ToggleWidth,
                "Ctrl+Shift+Space did not toggle character width.");
    require_key(IBUS_E, IBUS_CONTROL_MASK | IBUS_SHIFT_MASK, FrontendKey::ToggleEnglish,
                "Ctrl+Shift+E did not toggle dedicated English mode.");
    const auto extra_modifier_toggle = translate_ibus_key(IBUS_period, IBUS_CONTROL_MASK | IBUS_MOD1_MASK);
    require(extra_modifier_toggle.disposition == IBusKeyDisposition::Forward,
            "Ctrl+Alt+period was mistaken for the punctuation hotkey.");
    const auto extra_width_modifier =
        translate_ibus_key(IBUS_space, IBUS_CONTROL_MASK | IBUS_SHIFT_MASK | IBUS_SUPER_MASK);
    require(extra_width_modifier.disposition == IBusKeyDisposition::Forward,
            "Ctrl+Shift+Super+Space was mistaken for the width hotkey.");
    const auto extra_english_modifier =
        translate_ibus_key(IBUS_E, IBUS_CONTROL_MASK | IBUS_SHIFT_MASK | IBUS_MOD1_MASK);
    require(extra_english_modifier.disposition == IBusKeyDisposition::Forward,
            "Ctrl+Shift+Alt+E was mistaken for the English-mode hotkey.");

    require_key(IBUS_BackSpace, 0, FrontendKey::Backspace, "Backspace mapped incorrectly.");
    require_key(IBUS_Return, 0, FrontendKey::Enter, "Return mapped incorrectly.");
    require_key(IBUS_KP_Enter, 0, FrontendKey::Enter, "Keypad Enter mapped incorrectly.");
    require_key(IBUS_Escape, 0, FrontendKey::Escape, "Escape mapped incorrectly.");
    require_key(IBUS_space, 0, FrontendKey::Space, "Space mapped incorrectly.");
    require_key(IBUS_Up, 0, FrontendKey::Up, "Up mapped incorrectly.");
    require_key(IBUS_Down, 0, FrontendKey::Down, "Down mapped incorrectly.");
    // The NumLock-off keypad arrows have to reach candidate navigation like their main-block twins; forwarding them
    // commits the composition.
    require_key(IBUS_KP_Up, 0, FrontendKey::Up, "Keypad Up mapped incorrectly.");
    require_key(IBUS_KP_Down, 0, FrontendKey::Down, "Keypad Down mapped incorrectly.");

    for (const guint keyval : {IBUS_Page_Up, IBUS_KP_Page_Up, IBUS_ISO_Left_Tab})
    {
        require_key(keyval, keyval == IBUS_ISO_Left_Tab ? IBUS_SHIFT_MASK : 0, FrontendKey::PageUp,
                    "A PageUp alias mapped incorrectly.");
    }
    for (const guint keyval : {IBUS_Page_Down, IBUS_KP_Page_Down, IBUS_Tab})
    {
        require_key(keyval, 0, FrontendKey::PageDown, "A PageDown alias mapped incorrectly.");
    }

    const auto lower = translate_ibus_key(IBUS_a, 0);
    require(lower.disposition == IBusKeyDisposition::Dispatch && lower.event.key == FrontendKey::Character &&
                lower.event.character == 'a' && !lower.event.shift_only,
            "A lowercase composition character mapped incorrectly.");
    const auto upper = translate_ibus_key(IBUS_Z, IBUS_SHIFT_MASK);
    require(upper.disposition == IBusKeyDisposition::Dispatch && upper.event.key == FrontendKey::Character &&
                upper.event.character == 'Z' && upper.event.shift_only,
            "An uppercase composition character mapped incorrectly.");
    const auto caps_lock_upper = translate_ibus_key(IBUS_U, IBUS_LOCK_MASK);
    require(caps_lock_upper.disposition == IBusKeyDisposition::Dispatch &&
                caps_lock_upper.event.key == FrontendKey::Character && caps_lock_upper.event.character == 'U' &&
                !caps_lock_upper.event.shift_only,
            "Caps Lock was mistaken for the Shift-only local-mode modifier.");
    require_punctuation(IBUS_apostrophe, 0, '\'', "Apostrophe did not reach punctuation arbitration.");
    require_punctuation(IBUS_comma, 0, ',', "Comma did not reach punctuation arbitration.");
    require_punctuation(IBUS_period, 0, '.', "Period did not reach punctuation arbitration.");
    require_punctuation(IBUS_minus, 0, '-', "Minus did not reach punctuation arbitration.");
    require_punctuation(IBUS_equal, 0, '=', "Equals did not reach punctuation arbitration.");
    require_punctuation(IBUS_bracketleft, 0, '[', "Left bracket did not reach punctuation arbitration.");
    require_punctuation(IBUS_bracketright, 0, ']', "Right bracket did not reach punctuation arbitration.");
    const auto shifted_punctuation = translate_ibus_key(IBUS_exclam, IBUS_SHIFT_MASK);
    require(shifted_punctuation.disposition == IBusKeyDisposition::Dispatch &&
                shifted_punctuation.event.key == FrontendKey::Punctuation &&
                shifted_punctuation.event.character == '!' && shifted_punctuation.event.shift_only,
            "Shifted punctuation lost its local-mode modifier.");

    const auto keypad_decimal = translate_ibus_key(IBUS_KP_Decimal, 0);
    require(keypad_decimal.disposition == IBusKeyDisposition::Forward && keypad_decimal.event.host_shortcut,
            "Keypad decimal stopped preserving its ASCII application behavior.");

    const auto digit = translate_ibus_key(IBUS_9, 0);
    require(digit.disposition == IBusKeyDisposition::Dispatch && digit.event.key == FrontendKey::Digit &&
                digit.event.digit == 9 && !digit.event.shift_only,
            "A main-keyboard digit mapped incorrectly.");
    const auto shifted_digit = translate_ibus_key(IBUS_1, IBUS_SHIFT_MASK);
    require(shifted_digit.disposition == IBusKeyDisposition::Dispatch &&
                shifted_digit.event.key == FrontendKey::Digit && shifted_digit.event.digit == 1 &&
                shifted_digit.event.shift_only,
            "A Shift-only digit lost the Unicode candidate-selection modifier.");
    const auto keypad_digit = translate_ibus_key(IBUS_KP_1, 0);
    require(keypad_digit.disposition == IBusKeyDisposition::Dispatch && keypad_digit.event.key == FrontendKey::Digit &&
                keypad_digit.event.digit == 1,
            "A keypad digit mapped incorrectly.");
    const auto keypad_nine = translate_ibus_key(IBUS_KP_9, 0);
    require(keypad_nine.disposition == IBusKeyDisposition::Dispatch && keypad_nine.event.key == FrontendKey::Digit &&
                keypad_nine.event.digit == 9,
            "The top of the keypad digit range mapped incorrectly.");
    // Keypad zero carries the same meaning as the main-row zero, direct full-width conversion, so it must dispatch
    // rather than fall through to the host-shortcut tail and commit the composition.
    const auto keypad_zero = translate_ibus_key(IBUS_KP_0, 0);
    require(keypad_zero.disposition == IBusKeyDisposition::Dispatch && keypad_zero.event.key == FrontendKey::Digit &&
                keypad_zero.event.digit == 0,
            "Keypad zero was not dispatched for direct full-width conversion.");
    const auto zero = translate_ibus_key(IBUS_0, 0);
    require(zero.disposition == IBusKeyDisposition::Dispatch && zero.event.key == FrontendKey::Digit &&
                zero.event.digit == 0,
            "Zero was not dispatched for direct full-width conversion.");
    return 0;
}
