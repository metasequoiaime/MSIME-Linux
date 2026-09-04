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
} // namespace

int main()
{
    IBusModeToggleTracker toggle_tracker;
    require(!toggle_tracker.observe(IBUS_Shift_L, 0), "Shift toggled mode on key-down.");
    require(!toggle_tracker.observe(IBUS_Shift_L, 0), "Shift auto-repeat toggled mode.");
    require(toggle_tracker.observe(IBUS_Shift_L, IBUS_RELEASE_MASK | IBUS_SHIFT_MASK),
            "A bare Shift tap did not toggle mode on release.");
    require(!toggle_tracker.observe(IBUS_Shift_L, IBUS_RELEASE_MASK),
            "A repeated Shift release toggled mode twice.");

    require(!toggle_tracker.observe(IBUS_Shift_R, 0), "Right Shift toggled mode on key-down.");
    require(!toggle_tracker.observe(IBUS_A, IBUS_SHIFT_MASK), "Shift plus a character toggled mode.");
    require(!toggle_tracker.observe(IBUS_Shift_R, IBUS_RELEASE_MASK | IBUS_SHIFT_MASK),
            "Shift used as a modifier toggled mode.");

    require(!toggle_tracker.observe(IBUS_Control_L, 0), "Control toggled mode.");
    require(!toggle_tracker.observe(IBUS_Shift_L, IBUS_CONTROL_MASK), "Control+Shift toggled mode on key-down.");
    require(!toggle_tracker.observe(IBUS_Shift_L, IBUS_RELEASE_MASK | IBUS_CONTROL_MASK | IBUS_SHIFT_MASK),
            "Control+Shift toggled mode on key release.");

    const auto shift_press = translate_ibus_key(IBUS_Shift_L, 0);
    require(shift_press.disposition == IBusKeyDisposition::Ignore,
            "A Shift press was treated as a host shortcut and committed the composition.");

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

    require_key(IBUS_BackSpace, 0, FrontendKey::Backspace, "Backspace mapped incorrectly.");
    require_key(IBUS_Return, 0, FrontendKey::Enter, "Return mapped incorrectly.");
    require_key(IBUS_KP_Enter, 0, FrontendKey::Enter, "Keypad Enter mapped incorrectly.");
    require_key(IBUS_Escape, 0, FrontendKey::Escape, "Escape mapped incorrectly.");
    require_key(IBUS_space, 0, FrontendKey::Space, "Space mapped incorrectly.");
    require_key(IBUS_Up, 0, FrontendKey::Up, "Up mapped incorrectly.");
    require_key(IBUS_Down, 0, FrontendKey::Down, "Down mapped incorrectly.");

    for (const guint keyval : {IBUS_Page_Up, IBUS_KP_Page_Up, IBUS_minus, IBUS_comma, IBUS_ISO_Left_Tab})
    {
        require_key(keyval, keyval == IBUS_ISO_Left_Tab ? IBUS_SHIFT_MASK : 0, FrontendKey::PageUp,
                    "A PageUp alias mapped incorrectly.");
    }
    for (const guint keyval : {IBUS_Page_Down, IBUS_KP_Page_Down, IBUS_equal, IBUS_period, IBUS_Tab})
    {
        require_key(keyval, 0, FrontendKey::PageDown, "A PageDown alias mapped incorrectly.");
    }

    const auto lower = translate_ibus_key(IBUS_a, 0);
    require(lower.disposition == IBusKeyDisposition::Dispatch && lower.event.key == FrontendKey::Character &&
                lower.event.character == 'a',
            "A lowercase composition character mapped incorrectly.");
    const auto upper = translate_ibus_key(IBUS_Z, IBUS_SHIFT_MASK);
    require(upper.disposition == IBusKeyDisposition::Dispatch && upper.event.key == FrontendKey::Character &&
                upper.event.character == 'Z',
            "An uppercase composition character mapped incorrectly.");
    const auto apostrophe = translate_ibus_key(IBUS_apostrophe, 0);
    require(apostrophe.disposition == IBusKeyDisposition::Dispatch && apostrophe.event.character == '\'',
            "Apostrophe mapped incorrectly.");

    const auto digit = translate_ibus_key(IBUS_9, 0);
    require(digit.disposition == IBusKeyDisposition::Dispatch && digit.event.key == FrontendKey::Digit &&
                digit.event.digit == 9,
            "A main-keyboard digit mapped incorrectly.");
    const auto keypad_digit = translate_ibus_key(IBUS_KP_1, 0);
    require(keypad_digit.disposition == IBusKeyDisposition::Dispatch &&
                keypad_digit.event.key == FrontendKey::Digit && keypad_digit.event.digit == 1,
            "A keypad digit mapped incorrectly.");
    return 0;
}
