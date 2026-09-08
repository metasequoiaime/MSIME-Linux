#pragma once
#include <gtk/gtk.h>
#include "SecretStore.h"
#include "online/HttpTransport.h"
#include <memory>

namespace metasequoia::linux_ime::account
{
GtkWidget *create_account_panel();
GtkWidget *create_account_panel(std::shared_ptr<SecretStore> secrets, std::shared_ptr<online::HttpTransport> transport);
} // namespace metasequoia::linux_ime::account
