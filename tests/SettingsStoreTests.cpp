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
using metasequoia::linux_ime::PreeditStyle;
using metasequoia::linux_ime::SettingsStore;
using metasequoia::FrequencyAdjustmentMode;

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
                defaults.character_width == CharacterWidth::Half && !defaults.comma_period_paging &&
                !defaults.word_to_character && !defaults.bracket_paging && defaults.smart_punctuation &&
                defaults.smart_punctuation_repeat_to_chinese && defaults.paired_punctuation &&
                defaults.preedit_style == PreeditStyle::Raw && defaults.quanpin_helpcode_enabled &&
                defaults.quanpin_helpcode_schema == "lantian" && defaults.shuangpin_helpcode_enabled &&
                defaults.shuangpin_helpcode_schema == "lantian" &&
                defaults.frequency_adjustment_mode == FrequencyAdjustmentMode::Promote &&
                defaults.frequency_trigger_count == 1 && defaults.frequency_linear_step == 1 &&
                defaults.unicode_mode_enabled && !defaults.mixed_english_candidates_enabled &&
                defaults.mixed_english_minimum_prefix == 2,
            "Missing settings did not use defaults.");
    require(warning.empty(), "A missing optional settings file produced a warning.");

    InputSettings saved;
    saved.mode = InputMode::Direct;
    saved.scheme = SchemeType::Wubi;
    saved.page_size = 3;
    saved.punctuation_mode = PunctuationMode::English;
    saved.character_width = CharacterWidth::Full;
    saved.comma_period_paging = true;
    saved.word_to_character = true;
    saved.bracket_paging = false;
    saved.smart_punctuation = false;
    saved.smart_punctuation_repeat_to_chinese = false;
    saved.paired_punctuation = false;
    saved.preedit_style = PreeditStyle::Pinyin;
    saved.quanpin_helpcode_enabled = false;
    saved.quanpin_helpcode_schema = "xiaohe";
    saved.shuangpin_helpcode_enabled = false;
    saved.shuangpin_helpcode_schema = "ziranma";
    saved.frequency_adjustment_mode = FrequencyAdjustmentMode::Halve;
    saved.frequency_trigger_count = 3;
    saved.frequency_linear_step = 4;
    saved.unicode_mode_enabled = false;
    saved.mixed_english_candidates_enabled = true;
    saved.mixed_english_minimum_prefix = 4;
    std::string error;
    require(store.save(saved, &error) && error.empty(), "Valid settings could not be saved.");
    const InputSettings round_trip = store.load(&warning);
    require(round_trip.mode == saved.mode && round_trip.scheme == saved.scheme &&
                round_trip.page_size == saved.page_size &&
                round_trip.punctuation_mode == saved.punctuation_mode &&
                round_trip.character_width == saved.character_width &&
                round_trip.comma_period_paging == saved.comma_period_paging &&
                round_trip.word_to_character == saved.word_to_character &&
                round_trip.bracket_paging == saved.bracket_paging &&
                round_trip.smart_punctuation == saved.smart_punctuation &&
                round_trip.smart_punctuation_repeat_to_chinese == saved.smart_punctuation_repeat_to_chinese &&
                round_trip.paired_punctuation == saved.paired_punctuation &&
                round_trip.preedit_style == saved.preedit_style &&
                round_trip.quanpin_helpcode_enabled == saved.quanpin_helpcode_enabled &&
                round_trip.quanpin_helpcode_schema == saved.quanpin_helpcode_schema &&
                round_trip.shuangpin_helpcode_enabled == saved.shuangpin_helpcode_enabled &&
                round_trip.shuangpin_helpcode_schema == saved.shuangpin_helpcode_schema &&
                round_trip.frequency_adjustment_mode == saved.frequency_adjustment_mode &&
                round_trip.frequency_trigger_count == saved.frequency_trigger_count &&
                round_trip.frequency_linear_step == saved.frequency_linear_step &&
                round_trip.unicode_mode_enabled == saved.unicode_mode_enabled &&
                round_trip.mixed_english_candidates_enabled == saved.mixed_english_candidates_enabled &&
                round_trip.mixed_english_minimum_prefix == saved.mixed_english_minimum_prefix,
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
               "word-to-character=true\n"
               "bracket-paging=false\n"
               "smart-punctuation=false\n"
               "smart-punctuation-repeat-to-chinese=false\n"
               "paired-punctuation=false\n"
               "preedit-style=pinyin\n"
               "quanpin-helpcode=false\n"
               "quanpin-helpcode-schema=xiaohe\n"
               "shuangpin-helpcode=false\n"
               "shuangpin-helpcode-schema=ziranma\n"
               "frequency-adjustment=linear\n"
               "frequency-trigger-count=6\n"
               "frequency-linear-step=7\n"
               "unicode-mode=false\n"
               "mixed-english-candidates=true\n"
               "mixed-english-minimum-prefix=4\n"
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
    updated.word_to_character = false;
    updated.bracket_paging = true;
    updated.smart_punctuation = true;
    updated.smart_punctuation_repeat_to_chinese = true;
    updated.paired_punctuation = true;
    updated.preedit_style = PreeditStyle::Hidden;
    updated.quanpin_helpcode_enabled = true;
    updated.quanpin_helpcode_schema = "shouyouplus";
    updated.shuangpin_helpcode_enabled = true;
    updated.shuangpin_helpcode_schema = "shouyou2_0";
    updated.frequency_adjustment_mode = FrequencyAdjustmentMode::Pin;
    updated.frequency_trigger_count = 8;
    updated.frequency_linear_step = 9;
    updated.unicode_mode_enabled = true;
    updated.mixed_english_candidates_enabled = false;
    updated.mixed_english_minimum_prefix = 5;
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
               "comma-period-paging=unexpected\n"
               "word-to-character=unexpected\n"
               "bracket-paging=unexpected\n"
               "smart-punctuation=unexpected\n"
               "smart-punctuation-repeat-to-chinese=unexpected\n"
               "paired-punctuation=unexpected\n"
               "preedit-style=unexpected\n"
               "quanpin-helpcode=unexpected\n"
               "quanpin-helpcode-schema=unsupported\n"
               "shuangpin-helpcode=unexpected\n"
               "shuangpin-helpcode-schema=unsupported\n"
               "frequency-adjustment=unexpected\n"
               "frequency-trigger-count=0\n"
               "frequency-linear-step=11\n"
               "unicode-mode=unexpected\n"
               "mixed-english-candidates=unexpected\n"
               "mixed-english-minimum-prefix=9\n");
    const InputSettings invalid = store.load(&warning);
    require(invalid.mode == InputMode::Ime && invalid.scheme == SchemeType::Quanpin && invalid.page_size == 9 &&
                invalid.punctuation_mode == PunctuationMode::Chinese &&
                invalid.character_width == CharacterWidth::Half && !invalid.comma_period_paging &&
                !invalid.word_to_character && !invalid.bracket_paging && invalid.smart_punctuation &&
                invalid.smart_punctuation_repeat_to_chinese && invalid.paired_punctuation &&
                invalid.preedit_style == PreeditStyle::Raw && invalid.quanpin_helpcode_enabled &&
                invalid.quanpin_helpcode_schema == "lantian" && invalid.shuangpin_helpcode_enabled &&
                invalid.shuangpin_helpcode_schema == "lantian" &&
                invalid.frequency_adjustment_mode == FrequencyAdjustmentMode::Promote &&
                invalid.frequency_trigger_count == 1 && invalid.frequency_linear_step == 1 &&
                invalid.unicode_mode_enabled && !invalid.mixed_english_candidates_enabled &&
                invalid.mixed_english_minimum_prefix == 2,
            "Invalid settings did not fall back field by field.");
    require(!warning.empty(), "Invalid settings did not produce a diagnostic warning.");

    InputSettings unsupported = saved;
    unsupported.quanpin_helpcode_schema = "unknown";
    require(!store.save(unsupported, &error) && !error.empty(),
            "An unsupported helpcode schema was written to disk.");
    unsupported = saved;
    unsupported.frequency_adjustment_mode = static_cast<FrequencyAdjustmentMode>(99);
    require(!store.save(unsupported, &error) && !error.empty(),
            "An unsupported frequency adjustment mode was written to disk.");
    unsupported = saved;
    unsupported.frequency_trigger_count = 11;
    require(!store.save(unsupported, &error) && !error.empty(),
            "An out-of-range frequency trigger count was written to disk.");
    unsupported = saved;
    unsupported.mixed_english_minimum_prefix = 9;
    require(!store.save(unsupported, &error) && !error.empty(),
            "An out-of-range mixed-English minimum prefix was written to disk.");

    std::filesystem::remove(config_path);
    std::filesystem::create_directory(config_path);
    require(!store.save(saved, &error) && !error.empty(), "A settings replacement failure was not reported.");

    std::filesystem::remove_all(config_home);
    return 0;
}
