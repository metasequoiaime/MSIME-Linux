#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace metasequoia::linux_ime
{
class ClipboardHistory
{
  public:
    static constexpr std::size_t kMaxItems = 50;
    static constexpr std::size_t kMaxChars = 4000;

    ClipboardHistory();
    explicit ClipboardHistory(std::filesystem::path data_directory);

    std::filesystem::path store_path() const;
    bool enabled() const;
    bool set_enabled(bool enabled, std::string *error = nullptr);

    std::vector<std::string> load(std::string *error = nullptr) const;
    // Adds text to the front, removes an older duplicate, and returns false
    // when disabled, empty, invalid, or already the newest item.
    bool add(std::string text, std::string *error = nullptr);
    bool remove(const std::string &text, std::string *error = nullptr);
    bool clear(std::string *error = nullptr);

    static std::string normalize(std::string text);

    // Password managers advertise a marker alongside the text so that clipboard
    // history tools skip it. KeePassXC, Bitwarden, 1Password and Chromium all
    // set x-kde-passwordManagerHint, which became the cross-desktop convention;
    // org.nspasteboard.ConcealedType is the equivalent bridged from macOS.
    // Without this a password copied from a manager is written to disk in
    // plain text and stays there for fifty entries.
    static bool marked_sensitive(const std::vector<std::string> &clipboard_target_names);

  private:
    std::filesystem::path data_directory_;
};
} // namespace metasequoia::linux_ime
