#pragma once

#include "InputController.h"
#include "SecretStore.h"
#include "VoiceInput.h"
#include "online/AiSuggestionProvider.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>

namespace metasequoia::linux_ime
{
enum class TranslationProvider
{
    Local,
    DeepLX,
};

struct OnlineSettings
{
    bool cloud_candidates_enabled = true;
    online::AiSuggestionConfig ai;
    bool candidate_translations_enabled = true;
    TranslationProvider translation_provider = TranslationProvider::Local;
    std::string translation_target_language = "en";
    std::string translation_endpoint;
    std::string translation_token;
    std::chrono::milliseconds connect_timeout{2500};
    std::chrono::milliseconds total_timeout{8000};
};

struct InputSettings
{
    InputMode mode = InputMode::Ime;
    SchemeType scheme = SchemeType::Quanpin;
    std::size_t page_size = 9;
    PunctuationMode punctuation_mode = PunctuationMode::Chinese;
    CharacterWidth character_width = CharacterWidth::Half;
    bool comma_period_paging = true;
    bool word_to_character = true;
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
    bool super_jianpin_mode_enabled = true;
    bool temporary_english_mode_enabled = true;
    bool temporary_japanese_mode_enabled = true;
    bool mixed_english_candidates_enabled = true;
    std::size_t mixed_english_minimum_prefix = 2;
    bool mixed_emoji_candidates_enabled = true;
    bool mixed_kaomoji_candidates_enabled = false;
    bool clipboard_history_enabled = false;
    bool floating_toolbar_enabled = true;
    // Chinese/English toggles, matching the Windows keybindings section. Ctrl+Space is claimed by
    // the desktop rather than by the engine on both platforms, so it is not offered here either.
    bool switch_language_shift = true;
    bool switch_language_ctrl = false;
    bool switch_language_ctrl_alt_space = true;
    VoiceInputConfig voice;
    OnlineSettings online;
};

class SettingsStore
{
  public:
    SettingsStore();
    explicit SettingsStore(std::filesystem::path config_home);

    InputSettings load(std::string *warning = nullptr) const;
    // Credential overloads may block on Secret Service and are not main-loop APIs.
    InputSettings load(const SecretStore &secret_store, std::string *warning = nullptr) const;
    bool save(const InputSettings &settings, std::string *error = nullptr) const;
    bool save(const InputSettings &settings, SecretStore &secret_store, std::string *error = nullptr) const;
    const std::filesystem::path &config_path() const;

  private:
    std::filesystem::path config_path_;
};
} // namespace metasequoia::linux_ime
