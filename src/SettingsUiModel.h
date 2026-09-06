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

// The id of the Boolean row that asks for the credential of `credential_id` to be forgotten. A Secret row cannot
// express that on its own: an empty entry has to mean "keep what is stored", or an untouched form would erase a
// credential on every save, which leaves the window with no way to say "forget this token" at all. The gesture is a
// row of its own rather than a sentinel typed into the entry, so it is labelled, reversible before saving, and cannot
// be arrived at by accident. Ticking it drops the credential this run holds in memory, and entering a new credential
// unticks it, so the two rows never contradict each other whichever order the user touches them in.
std::string settings_credential_clear_id(const std::string &credential_id);

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
    // Whether the configuration being edited has any use for this value. A credential row is the case this exists for:
    // the translation token is never read while the provider is 本地, and the AI and voice tokens are never read while
    // their features are off, so showing those rows invites a user to type a credential that will be filed under a
    // provider with no use for it. The model still builds, parses and reports a hidden row -- only the window skips it
    // -- so hiding one cannot silently drop a setting, and a value already stored keeps whatever it had.
    bool visible = true;
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
