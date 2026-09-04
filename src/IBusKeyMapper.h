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

class IBusModeToggleTracker
{
  public:
    bool observe(guint keyval, guint state);

  private:
    bool shift_armed_ = false;
};

IBusKeyTranslation translate_ibus_key(guint keyval, guint state);
} // namespace metasequoia::linux_ime
