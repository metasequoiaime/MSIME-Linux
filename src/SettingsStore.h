#pragma once

#include "InputController.h"

#include <cstddef>
#include <filesystem>
#include <string>

namespace metasequoia::linux_ime
{
struct InputSettings
{
    InputMode mode = InputMode::Ime;
    SchemeType scheme = SchemeType::Quanpin;
    std::size_t page_size = 9;
};

class SettingsStore
{
  public:
    SettingsStore();
    explicit SettingsStore(std::filesystem::path config_home);

    InputSettings load(std::string *warning = nullptr) const;
    bool save(const InputSettings &settings, std::string *error = nullptr) const;
    const std::filesystem::path &config_path() const;

  private:
    std::filesystem::path config_path_;
};
} // namespace metasequoia::linux_ime
