#pragma once
#include <gtk/gtk.h>
#include "SecretStore.h"
#include "SettingsStore.h"
#include <functional>
#include "online/HttpTransport.h"
#include <memory>

namespace metasequoia::linux_ime::account
{
struct SettingsSyncHooks
{
    std::shared_ptr<SettingsStore> store;
    // Main-thread callbacks only, never invoked after the panel is destroyed.
    std::function<bool()> allow;
    std::function<void(bool)> busy;
    std::function<void(const InputSettings &)> applied;
};
GtkWidget *create_account_panel();
GtkWidget *create_account_panel(std::shared_ptr<SecretStore> secrets, std::shared_ptr<online::HttpTransport> transport,
                                SettingsSyncHooks sync = {});
} // namespace metasequoia::linux_ime::account
