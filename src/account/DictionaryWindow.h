#pragma once
#include "AccountSession.h"
#include <gtk/gtk.h>
#include <memory>
namespace metasequoia::linux_ime::account
{
// The owning account panel must outlive the aliased session pointer.
GtkWidget *create_dictionary_window(GtkWindow *parent, std::shared_ptr<AccountSession> session,
                                    std::uint64_t generation);
} // namespace metasequoia::linux_ime::account
