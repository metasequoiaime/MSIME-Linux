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

IBusKeyTranslation translate_ibus_key(guint keyval, guint state);
} // namespace metasequoia::linux_ime
