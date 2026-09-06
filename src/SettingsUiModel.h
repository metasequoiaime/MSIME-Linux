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
    // A credential entry. The row never carries the credential: `value` is kSettingsCredentialStored when this run is
    // holding one in memory and empty otherwise, so the window can say that a credential exists without ever rendering
    // it into a widget that X11 and the accessibility bus can read back. A submitted empty value means "keep whatever
    // Secret Service already holds", which is the same reading SettingsStore::save gives an empty token, so an
    // untouched entry preserves the stored credential instead of erasing it.
    Secret,
};

// The only value a Secret row ever carries; see SettingsControl::Secret.
inline constexpr const char *kSettingsCredentialStored = "stored";

enum class SettingsUiSection
{
    Appearance,
    Input,
    Helpcode,
    Shortcuts,
    Dictionary,
    Voice,
    DesktopTools,
    Online,
};

SettingsUiSection settings_section_for_id(const std::string &id);
const char *settings_section_title(SettingsUiSection section);

struct SettingsUiRow
{
    std::string id;
    std::string label;
    std::string value;
    SettingsControl control = SettingsControl::Text;
    std::vector<std::string> choices;
};

// Platform-neutral model used by the GTK settings application and tests. Credentials are write-only here: a Secret row
// accepts a new one and reports whether this run is holding one, but no row ever carries a credential back out, so the
// stored value stays in Secret Service alone.
class SettingsUiModel
{
  public:
    explicit SettingsUiModel(InputSettings settings = {});

    const InputSettings &settings() const;
    const std::vector<SettingsUiRow> &rows() const;

    // Apply one user-visible value. The model is updated only after the value has been parsed and validated, so a
    // failed edit cannot partially mutate settings or discard in-memory credentials. Validation mirrors what
    // SettingsStore accepts on save, so that a rejected value is reported against the row that produced it instead of
    // failing the whole save with an error that names no field. The one edit that does drop a credential is an accepted
    // change of provider, because the credential in memory belongs to the provider it was filed under and the store
    // will not re-file it under another name.
    bool set(const std::string &id, const std::string &value, std::string *error = nullptr);

  private:
    void rebuild_rows();

    InputSettings settings_;
    std::vector<SettingsUiRow> rows_;
};
} // namespace metasequoia::linux_ime
