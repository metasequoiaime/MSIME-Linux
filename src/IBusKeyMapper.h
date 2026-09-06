#pragma once

#include "InputController.h"

#include <ibus.h>

namespace metasequoia::linux_ime
{
enum class IBusKeyDisposition
{
    Ignore,
    Forward,
    Dispatch,
};

struct IBusKeyTranslation
{
    IBusKeyDisposition disposition = IBusKeyDisposition::Ignore;
    FrontendKeyEvent event;
};

// Which chords toggle Chinese and English. Defaults match the Windows keybindings section, and
// Ctrl+Space is absent for the same reason it is there: the desktop claims it before the engine
// ever sees it.
struct ModeToggleBindings
{
    bool shift = true;
    bool ctrl = false;
    bool ctrl_alt_space = true;
};

// Shift and Ctrl toggle on release, and only when the key was pressed and released alone, so that
// using either as a modifier for some other key does not switch modes. Ctrl+Alt+Space is an
// ordinary chord and toggles on press.
class IBusModeToggleTracker
{
  public:
    void configure(const ModeToggleBindings &bindings);
    bool observe(guint keyval, guint state);
    // True when the last observed event was the Ctrl+Alt+Space chord, including the auto-repeat presses that `observe`
    // swallows after the first one. The frontend has to consume every such event: the chord belongs to the IME, and
    // letting a repeat fall through to `translate_ibus_key` would forward it as a host shortcut.
    bool chord_held() const;

  private:
    ModeToggleBindings bindings_;
    bool shift_armed_ = false;
    bool ctrl_armed_ = false;
    bool chord_held_ = false;
};

IBusKeyTranslation translate_ibus_key(guint keyval, guint state);
} // namespace metasequoia::linux_ime
