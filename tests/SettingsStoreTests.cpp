#include "../src/SettingsStore.h"
#include "../vendor/MetasequoiaImeEngine/core/data_path.h"

#include <sys/stat.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace
{
using metasequoia::linux_ime::InputMode;
using metasequoia::linux_ime::InputSettings;
using metasequoia::linux_ime::CharacterWidth;
using metasequoia::linux_ime::PunctuationMode;
using metasequoia::linux_ime::SettingsStore;

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void write_file(const std::filesystem::path &path, const std::string &contents)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path);
    stream << contents;
    if (!stream)
    {
        throw std::runtime_error("Failed to prepare a settings fixture.");
    }
}

std::string read_file(const std::filesystem::path &path)
{
    std::ifstream stream(path);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

ino_t inode(const std::filesystem::path &path)
{
    struct stat info
    {
    };
    if (stat(metasequoia::path_to_utf8(path).c_str(), &info) != 0)
    {
        throw std::runtime_error("Failed to inspect the settings file.");
    }
    return info.st_ino;
}
} // namespace

int main()
{
    const auto suffix = std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const auto config_home = std::filesystem::temp_directory_path() / ("metasequoia-settings-" + suffix);
    std::filesystem::create_directories(config_home);
    if (setenv("XDG_CONFIG_HOME", metasequoia::path_to_utf8(config_home).c_str(), 1) != 0)
    {
        throw std::runtime_error("Failed to set the test configuration directory.");
    }

    SettingsStore store;
    require(store.config_path() == config_home / "metasequoiaime" / "config.ini",
            "The settings store did not use XDG_CONFIG_HOME.");

    std::string warning;
    const InputSettings defaults = store.load(&warning);
    require(defaults.mode == InputMode::Ime && defaults.scheme == SchemeType::Quanpin && defaults.page_size == 9 &&
                defaults.punctuation_mode == PunctuationMode::Chinese &&
                defaults.character_width == CharacterWidth::Half && !defaults.comma_period_paging,
            "Missing settings did not use defaults.");
    require(warning.empty(), "A missing optional settings file produced a warning.");

    InputSettings saved;
    saved.mode = InputMode::Direct;
    saved.scheme = SchemeType::Wubi;
    saved.page_size = 3;
    saved.punctuation_mode = PunctuationMode::English;
    saved.character_width = CharacterWidth::Full;
    saved.comma_period_paging = true;
    std::string error;
    require(store.save(saved, &error) && error.empty(), "Valid settings could not be saved.");
    const InputSettings round_trip = store.load(&warning);
    require(round_trip.mode == saved.mode && round_trip.scheme == saved.scheme &&
                round_trip.page_size == saved.page_size &&
                round_trip.punctuation_mode == saved.punctuation_mode &&
                round_trip.character_width == saved.character_width &&
                round_trip.comma_period_paging == saved.comma_period_paging,
            "Settings did not survive a round trip.");

    const auto config_path = store.config_path();
    write_file(config_path,
               "[input]\n"
               "mode=ime\n"
               "scheme=shuangpin\n"
               "page-size=5\n"
               "punctuation=english\n"
               "full-width=true\n"
               "comma-period-paging=true\n"
               "future-option=keep-me\n"
               "\n"
               "[future]\n"
               "value=preserve-me\n");
    const ino_t original_inode = inode(config_path);

    InputSettings updated;
    updated.mode = InputMode::Direct;
    updated.scheme = SchemeType::JapaneseRomaji;
    updated.page_size = 7;
    updated.punctuation_mode = PunctuationMode::Chinese;
    updated.character_width = CharacterWidth::Half;
    updated.comma_period_paging = false;
    require(store.save(updated, &error), "Existing settings could not be replaced.");
    require(inode(config_path) != original_inode, "The settings file was modified in place instead of atomically replaced.");
    const std::string preserved = read_file(config_path);
    require(preserved.find("future-option=keep-me") != std::string::npos &&
                preserved.find("[future]") != std::string::npos &&
                preserved.find("value=preserve-me") != std::string::npos,
            "Saving known settings discarded unknown keys.");
    for (const auto &entry : std::filesystem::directory_iterator(config_path.parent_path()))
    {
        require(entry.path() == config_path, "An atomic settings temporary file was left behind.");
    }

    write_file(config_path,
               "[input]\n"
               "mode=unexpected\n"
               "scheme=unsupported\n"
               "page-size=12\n"
               "punctuation=unsupported\n"
               "full-width=unexpected\n"
               "comma-period-paging=unexpected\n");
    const InputSettings invalid = store.load(&warning);
    require(invalid.mode == InputMode::Ime && invalid.scheme == SchemeType::Quanpin && invalid.page_size == 9 &&
                invalid.punctuation_mode == PunctuationMode::Chinese &&
                invalid.character_width == CharacterWidth::Half && !invalid.comma_period_paging,
            "Invalid settings did not fall back field by field.");
    require(!warning.empty(), "Invalid settings did not produce a diagnostic warning.");

    std::filesystem::remove(config_path);
    std::filesystem::create_directory(config_path);
    require(!store.save(saved, &error) && !error.empty(), "A settings replacement failure was not reported.");

    std::filesystem::remove_all(config_home);
    return 0;
}
