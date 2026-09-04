#pragma once

#include "SettingsStore.h"

#include <string>
#include <vector>

namespace metasequoia::linux_ime
{
enum class SettingsControl
{
    Boolean,
    Choice,
    Integer,
    Text,
};

struct SettingsUiRow
{
    std::string id;
    std::string label;
    std::string value;
    SettingsControl control = SettingsControl::Text;
    std::vector<std::string> choices;
};

// Platform-neutral model used by the GTK settings application and tests. It
// deliberately excludes credentials; those remain in Secret Service only.
class SettingsUiModel
{
  public:
    explicit SettingsUiModel(InputSettings settings = {});

    const InputSettings &settings() const;
    const std::vector<SettingsUiRow> &rows() const;

    // Apply one user-visible value. The model is updated only after the value
    // has been parsed and validated, so a failed edit cannot partially mutate
    // settings or discard in-memory credentials.
    bool set(const std::string &id, const std::string &value, std::string *error = nullptr);

  private:
    void rebuild_rows();

    InputSettings settings_;
    std::vector<SettingsUiRow> rows_;
};
} // namespace metasequoia::linux_ime
