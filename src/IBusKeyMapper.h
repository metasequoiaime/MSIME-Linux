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

  private:
    ModeToggleBindings bindings_;
    bool shift_armed_ = false;
    bool ctrl_armed_ = false;
};

IBusKeyTranslation translate_ibus_key(guint keyval, guint state);
} // namespace metasequoia::linux_ime
