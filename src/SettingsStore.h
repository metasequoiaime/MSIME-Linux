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
    PunctuationMode punctuation_mode = PunctuationMode::Chinese;
    CharacterWidth character_width = CharacterWidth::Half;
    bool comma_period_paging = false;
    bool word_to_character = false;
    bool bracket_paging = false;
    bool smart_punctuation = true;
    bool smart_punctuation_repeat_to_chinese = true;
    bool paired_punctuation = true;
    PreeditStyle preedit_style = PreeditStyle::Raw;
    bool quanpin_helpcode_enabled = true;
    std::string quanpin_helpcode_schema = "lantian";
    bool shuangpin_helpcode_enabled = true;
    std::string shuangpin_helpcode_schema = "lantian";
    FrequencyAdjustmentMode frequency_adjustment_mode = FrequencyAdjustmentMode::Promote;
    int frequency_trigger_count = 1;
    int frequency_linear_step = 1;
    bool unicode_mode_enabled = true;
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
