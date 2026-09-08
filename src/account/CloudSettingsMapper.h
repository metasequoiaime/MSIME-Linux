#pragma once
#include "BackendAccountClient.h"
#include "SettingsUiModel.h"

namespace metasequoia::linux_ime::account
{
struct SettingsChange
{
    std::string field;
    PreferenceValue before, after;
};
struct SettingsPreview
{
    InputSettings settings;
    std::vector<SettingsChange> changes;
    std::vector<std::string> unsupported_fields;
};
// Pure mapping only. Callers own account/revision checks, review and persistence.
class CloudSettingsMapper
{
  public:
    static std::string field_label(const std::string &field);
    static std::map<std::string, PreferenceValue> export_settings(const InputSettings &local);
    static Preferences prepare_upload(const InputSettings &local, const Preferences &remote);
    static SettingsPreview prepare_download(const InputSettings &local, const Preferences &remote);
};
} // namespace metasequoia::linux_ime::account
