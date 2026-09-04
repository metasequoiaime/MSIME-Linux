#include "SettingsUiModel.h"

#include <glib.h>

#include <charconv>
#include <cctype>
#include <limits>
#include <string_view>

namespace metasequoia::linux_ime
{
namespace
{
void set_error(std::string *error, const char *message)
{
    if (error != nullptr)
    {
        *error = message;
    }
}

const char *bool_value(bool value)
{
    return value ? "true" : "false";
}

bool parse_bool(std::string_view value, bool &result)
{
    if (value == "true" || value == "1" || value == "yes")
    {
        result = true;
        return true;
    }
    if (value == "false" || value == "0" || value == "no")
    {
        result = false;
        return true;
    }
    return false;
}

bool parse_integer(std::string_view value, int minimum, int maximum, int &result)
{
    if (value.empty())
    {
        return false;
    }
    int parsed = 0;
    const auto conversion = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (conversion.ec != std::errc{} || conversion.ptr != value.data() + value.size() || parsed < minimum ||
        parsed > maximum)
    {
        return false;
    }
    result = parsed;
    return true;
}

bool valid_text(std::string_view value, std::size_t maximum, bool allow_newlines = false)
{
    if (value.size() > maximum || !g_utf8_validate(value.data(), static_cast<gssize>(value.size()), nullptr))
    {
        return false;
    }
    for (const unsigned char character : value)
    {
        if (character == 0 || character == 0x7fU ||
            (character < 0x20U && !(allow_newlines && (character == '\n' || character == '\r' || character == '\t'))))
        {
            return false;
        }
    }
    return true;
}

void add(std::vector<SettingsUiRow> &rows, const char *id, const char *label, std::string value,
         SettingsControl control, std::vector<std::string> choices = {})
{
    rows.push_back({id, label, std::move(value), control, std::move(choices)});
}

std::string mode_value(InputMode value)
{
    return value == InputMode::Ime ? "ime" : "direct";
}

std::string scheme_value(SchemeType value)
{
    switch (value)
    {
    case SchemeType::Quanpin:
        return "quanpin";
    case SchemeType::Shuangpin:
        return "shuangpin";
    case SchemeType::Wubi:
        return "wubi";
    case SchemeType::JapaneseRomaji:
        return "japanese";
    }
    return "quanpin";
}

std::string punctuation_value(PunctuationMode value)
{
    return value == PunctuationMode::Chinese ? "chinese" : "english";
}

std::string width_value(CharacterWidth value)
{
    return value == CharacterWidth::Full ? "full" : "half";
}

std::string preedit_value(PreeditStyle value)
{
    switch (value)
    {
    case PreeditStyle::Raw:
        return "raw";
    case PreeditStyle::Pinyin:
        return "pinyin";
    case PreeditStyle::Hidden:
        return "hidden";
    }
    return "raw";
}

std::string frequency_value(FrequencyAdjustmentMode value)
{
    switch (value)
    {
    case FrequencyAdjustmentMode::Disabled:
        return "disabled";
    case FrequencyAdjustmentMode::Pin:
        return "pin";
    case FrequencyAdjustmentMode::Halve:
        return "halve";
    case FrequencyAdjustmentMode::Linear:
        return "linear";
    case FrequencyAdjustmentMode::Promote:
        return "promote";
    }
    return "disabled";
}

std::string ai_provider_value(online::AiProvider value)
{
    switch (value)
    {
    case online::AiProvider::DeepSeek:
        return "deepseek";
    case online::AiProvider::OpenAI:
        return "openai";
    case online::AiProvider::SiliconFlow:
        return "siliconflow";
    case online::AiProvider::Groq:
        return "groq";
    case online::AiProvider::Custom:
        return "custom";
    }
    return "deepseek";
}

std::string translation_provider_value(TranslationProvider value)
{
    return value == TranslationProvider::DeepLX ? "deeplx" : "local";
}

bool set_choice(std::string_view value, std::initializer_list<std::string_view> choices, std::size_t &index)
{
    std::size_t current = 0;
    for (const auto choice : choices)
    {
        if (value == choice)
        {
            index = current;
            return true;
        }
        ++current;
    }
    return false;
}
} // namespace

SettingsUiModel::SettingsUiModel(InputSettings settings) : settings_(std::move(settings))
{
    rebuild_rows();
}

const InputSettings &SettingsUiModel::settings() const
{
    return settings_;
}

const std::vector<SettingsUiRow> &SettingsUiModel::rows() const
{
    return rows_;
}

void SettingsUiModel::rebuild_rows()
{
    rows_.clear();
    add(rows_, "mode", "Input mode", mode_value(settings_.mode), SettingsControl::Choice, {"ime", "direct"});
    add(rows_, "scheme", "Input scheme", scheme_value(settings_.scheme), SettingsControl::Choice,
        {"quanpin", "shuangpin", "wubi", "japanese"});
    add(rows_, "page-size", "Candidates per page", std::to_string(settings_.page_size), SettingsControl::Integer);
    add(rows_, "punctuation", "Punctuation", punctuation_value(settings_.punctuation_mode), SettingsControl::Choice,
        {"chinese", "english"});
    add(rows_, "width", "Character width", width_value(settings_.character_width), SettingsControl::Choice,
        {"half", "full"});
    add(rows_, "preedit-style", "Preedit style", preedit_value(settings_.preedit_style), SettingsControl::Choice,
        {"raw", "pinyin", "hidden"});
    add(rows_, "comma-period-paging", "Comma/period paging", bool_value(settings_.comma_period_paging),
        SettingsControl::Boolean);
    add(rows_, "word-to-character", "Word to character", bool_value(settings_.word_to_character),
        SettingsControl::Boolean);
    add(rows_, "bracket-paging", "Bracket paging", bool_value(settings_.bracket_paging), SettingsControl::Boolean);
    add(rows_, "smart-punctuation", "Smart punctuation", bool_value(settings_.smart_punctuation),
        SettingsControl::Boolean);
    add(rows_, "smart-punctuation-repeat-to-chinese", "Repeat punctuation to Chinese",
        bool_value(settings_.smart_punctuation_repeat_to_chinese), SettingsControl::Boolean);
    add(rows_, "paired-punctuation", "Paired punctuation", bool_value(settings_.paired_punctuation),
        SettingsControl::Boolean);
    add(rows_, "quanpin-helpcode", "Quanpin helpcode", bool_value(settings_.quanpin_helpcode_enabled),
        SettingsControl::Boolean);
    add(rows_, "quanpin-helpcode-schema", "Quanpin helpcode schema", settings_.quanpin_helpcode_schema,
        SettingsControl::Choice, {"lantian", "ziranma", "shouyou2_0", "shouyouplus", "xiaohe"});
    add(rows_, "shuangpin-helpcode", "Shuangpin helpcode", bool_value(settings_.shuangpin_helpcode_enabled),
        SettingsControl::Boolean);
    add(rows_, "shuangpin-helpcode-schema", "Shuangpin helpcode schema", settings_.shuangpin_helpcode_schema,
        SettingsControl::Choice, {"lantian", "ziranma", "shouyou2_0", "shouyouplus", "xiaohe"});
    add(rows_, "frequency-adjustment", "Frequency adjustment", frequency_value(settings_.frequency_adjustment_mode),
        SettingsControl::Choice, {"disabled", "pin", "halve", "linear", "promote"});
    add(rows_, "frequency-trigger-count", "Frequency trigger count", std::to_string(settings_.frequency_trigger_count),
        SettingsControl::Integer);
    add(rows_, "frequency-linear-step", "Frequency linear step", std::to_string(settings_.frequency_linear_step),
        SettingsControl::Integer);
    add(rows_, "unicode-mode", "Unicode mode", bool_value(settings_.unicode_mode_enabled), SettingsControl::Boolean);
    add(rows_, "super-jianpin-mode", "Super jianpin mode", bool_value(settings_.super_jianpin_mode_enabled),
        SettingsControl::Boolean);
    add(rows_, "temporary-english-mode", "Temporary English mode",
        bool_value(settings_.temporary_english_mode_enabled), SettingsControl::Boolean);
    add(rows_, "temporary-japanese-mode", "Temporary Japanese mode",
        bool_value(settings_.temporary_japanese_mode_enabled), SettingsControl::Boolean);
    add(rows_, "mixed-english-candidates", "Mixed English candidates",
        bool_value(settings_.mixed_english_candidates_enabled), SettingsControl::Boolean);
    add(rows_, "mixed-english-minimum-prefix", "English minimum prefix",
        std::to_string(settings_.mixed_english_minimum_prefix), SettingsControl::Integer);
    add(rows_, "mixed-emoji-candidates", "Mixed Emoji candidates",
        bool_value(settings_.mixed_emoji_candidates_enabled), SettingsControl::Boolean);
    add(rows_, "mixed-kaomoji-candidates", "Mixed kaomoji candidates",
        bool_value(settings_.mixed_kaomoji_candidates_enabled), SettingsControl::Boolean);
    add(rows_, "clipboard-history", "Clipboard history", bool_value(settings_.clipboard_history_enabled),
        SettingsControl::Boolean);
    add(rows_, "floating-toolbar", "Floating toolbar", bool_value(settings_.floating_toolbar_enabled),
        SettingsControl::Boolean);
    add(rows_, "voice-enabled", "Voice input", bool_value(settings_.voice.enabled), SettingsControl::Boolean);
    add(rows_, "voice-provider", "Voice provider", settings_.voice.provider, SettingsControl::Choice,
        {"openai", "siliconflow", "groq", "custom"});
    add(rows_, "voice-endpoint", "Voice endpoint", settings_.voice.endpoint, SettingsControl::Text);
    add(rows_, "voice-model", "Voice model", settings_.voice.model, SettingsControl::Text);
    add(rows_, "voice-language", "Voice language", settings_.voice.language, SettingsControl::Text);
    add(rows_, "voice-polish-enabled", "Voice polish", bool_value(settings_.voice.polish_enabled),
        SettingsControl::Boolean);
    add(rows_, "voice-polish-endpoint", "Voice polish endpoint", settings_.voice.polish_endpoint,
        SettingsControl::Text);
    add(rows_, "voice-polish-model", "Voice polish model", settings_.voice.polish_model, SettingsControl::Text);
    add(rows_, "voice-polish-prompt", "Voice polish prompt", settings_.voice.polish_prompt, SettingsControl::Text);
    add(rows_, "cloud-enabled", "Cloud candidates", bool_value(settings_.online.cloud_candidates_enabled),
        SettingsControl::Boolean);
    add(rows_, "connect-timeout-ms", "Connect timeout (ms)", std::to_string(settings_.online.connect_timeout.count()),
        SettingsControl::Integer);
    add(rows_, "total-timeout-ms", "Total timeout (ms)", std::to_string(settings_.online.total_timeout.count()),
        SettingsControl::Integer);
    add(rows_, "ai-enabled", "AI suggestions", bool_value(settings_.online.ai.enabled), SettingsControl::Boolean);
    add(rows_, "ai-provider", "AI provider", ai_provider_value(settings_.online.ai.provider), SettingsControl::Choice,
        {"deepseek", "openai", "siliconflow", "groq", "custom"});
    add(rows_, "ai-endpoint", "AI endpoint", settings_.online.ai.endpoint, SettingsControl::Text);
    add(rows_, "ai-model", "AI model", settings_.online.ai.model, SettingsControl::Text);
    add(rows_, "ai-prompt", "AI prompt", settings_.online.ai.prompt, SettingsControl::Text);
    add(rows_, "ai-candidate-limit", "AI candidate limit", std::to_string(settings_.online.ai.candidate_limit),
        SettingsControl::Integer);
    add(rows_, "translation-enabled", "Candidate translations",
        bool_value(settings_.online.candidate_translations_enabled), SettingsControl::Boolean);
    add(rows_, "translation-provider", "Translation provider", translation_provider_value(settings_.online.translation_provider),
        SettingsControl::Choice, {"local", "deeplx"});
    add(rows_, "translation-target-language", "Translation target language",
        settings_.online.translation_target_language, SettingsControl::Choice, {"en", "fr", "ja", "es", "ru", "de", "ko"});
    add(rows_, "translation-endpoint", "Translation endpoint", settings_.online.translation_endpoint,
        SettingsControl::Text);
}

bool SettingsUiModel::set(const std::string &id, const std::string &value, std::string *error)
{
    InputSettings candidate = settings_;
    bool parsed = true;
    if (id == "mode")
    {
        if (value == "ime") candidate.mode = InputMode::Ime;
        else if (value == "direct") candidate.mode = InputMode::Direct;
        else parsed = false;
    }
    else if (id == "scheme")
    {
        if (value == "quanpin") candidate.scheme = SchemeType::Quanpin;
        else if (value == "shuangpin") candidate.scheme = SchemeType::Shuangpin;
        else if (value == "wubi") candidate.scheme = SchemeType::Wubi;
        else if (value == "japanese") candidate.scheme = SchemeType::JapaneseRomaji;
        else parsed = false;
    }
    else if (id == "page-size")
    {
        int number = 0; parsed = parse_integer(value, 3, 9, number); candidate.page_size = static_cast<std::size_t>(number);
    }
    else if (id == "punctuation")
    {
        if (value == "chinese") candidate.punctuation_mode = PunctuationMode::Chinese;
        else if (value == "english") candidate.punctuation_mode = PunctuationMode::English;
        else parsed = false;
    }
    else if (id == "width")
    {
        if (value == "half") candidate.character_width = CharacterWidth::Half;
        else if (value == "full") candidate.character_width = CharacterWidth::Full;
        else parsed = false;
    }
    else if (id == "preedit-style")
    {
        if (value == "raw") candidate.preedit_style = PreeditStyle::Raw;
        else if (value == "pinyin") candidate.preedit_style = PreeditStyle::Pinyin;
        else if (value == "hidden") candidate.preedit_style = PreeditStyle::Hidden;
        else parsed = false;
    }
    else if (id == "comma-period-paging" || id == "word-to-character" || id == "bracket-paging" ||
             id == "smart-punctuation" || id == "smart-punctuation-repeat-to-chinese" || id == "paired-punctuation" ||
             id == "quanpin-helpcode" || id == "shuangpin-helpcode" || id == "unicode-mode" ||
             id == "super-jianpin-mode" || id == "temporary-english-mode" || id == "temporary-japanese-mode" ||
             id == "mixed-english-candidates" || id == "mixed-emoji-candidates" || id == "mixed-kaomoji-candidates" ||
             id == "clipboard-history" || id == "floating-toolbar" || id == "voice-enabled" ||
             id == "voice-polish-enabled" || id == "cloud-enabled" || id == "ai-enabled" ||
             id == "translation-enabled")
    {
        bool value_as_bool = false;
        parsed = parse_bool(value, value_as_bool);
        if (parsed)
        {
            if (id == "comma-period-paging") candidate.comma_period_paging = value_as_bool;
            else if (id == "word-to-character") candidate.word_to_character = value_as_bool;
            else if (id == "bracket-paging") candidate.bracket_paging = value_as_bool;
            else if (id == "smart-punctuation") candidate.smart_punctuation = value_as_bool;
            else if (id == "smart-punctuation-repeat-to-chinese") candidate.smart_punctuation_repeat_to_chinese = value_as_bool;
            else if (id == "paired-punctuation") candidate.paired_punctuation = value_as_bool;
            else if (id == "quanpin-helpcode") candidate.quanpin_helpcode_enabled = value_as_bool;
            else if (id == "shuangpin-helpcode") candidate.shuangpin_helpcode_enabled = value_as_bool;
            else if (id == "unicode-mode") candidate.unicode_mode_enabled = value_as_bool;
            else if (id == "super-jianpin-mode") candidate.super_jianpin_mode_enabled = value_as_bool;
            else if (id == "temporary-english-mode") candidate.temporary_english_mode_enabled = value_as_bool;
            else if (id == "temporary-japanese-mode") candidate.temporary_japanese_mode_enabled = value_as_bool;
            else if (id == "mixed-english-candidates") candidate.mixed_english_candidates_enabled = value_as_bool;
            else if (id == "mixed-emoji-candidates") candidate.mixed_emoji_candidates_enabled = value_as_bool;
            else if (id == "mixed-kaomoji-candidates") candidate.mixed_kaomoji_candidates_enabled = value_as_bool;
            else if (id == "clipboard-history") candidate.clipboard_history_enabled = value_as_bool;
            else if (id == "floating-toolbar") candidate.floating_toolbar_enabled = value_as_bool;
            else if (id == "voice-enabled") candidate.voice.enabled = value_as_bool;
            else if (id == "voice-polish-enabled") candidate.voice.polish_enabled = value_as_bool;
            else if (id == "cloud-enabled") candidate.online.cloud_candidates_enabled = value_as_bool;
            else if (id == "ai-enabled") candidate.online.ai.enabled = value_as_bool;
            else if (id == "translation-enabled") candidate.online.candidate_translations_enabled = value_as_bool;
        }
    }
    else if (id == "quanpin-helpcode-schema" || id == "shuangpin-helpcode-schema")
    {
        std::size_t index = 0;
        parsed = set_choice(value, {"lantian", "ziranma", "shouyou2_0", "shouyouplus", "xiaohe"}, index);
        if (parsed)
        {
            if (id == "quanpin-helpcode-schema") candidate.quanpin_helpcode_schema = value;
            else candidate.shuangpin_helpcode_schema = value;
        }
    }
    else if (id == "frequency-adjustment")
    {
        std::size_t index = 0;
        parsed = set_choice(value, {"disabled", "pin", "halve", "linear", "promote"}, index);
        if (parsed) candidate.frequency_adjustment_mode = static_cast<FrequencyAdjustmentMode>(index);
    }
    else if (id == "frequency-trigger-count" || id == "frequency-linear-step")
    {
        int number = 0; parsed = parse_integer(value, 1, 10, number);
        if (parsed)
        {
            if (id == "frequency-trigger-count") candidate.frequency_trigger_count = number;
            else candidate.frequency_linear_step = number;
        }
    }
    else if (id == "mixed-english-minimum-prefix")
    {
        int number = 0; parsed = parse_integer(value, 1, 8, number); candidate.mixed_english_minimum_prefix = static_cast<std::size_t>(number);
    }
    else if (id == "connect-timeout-ms" || id == "total-timeout-ms")
    {
        int number = 0;
        parsed = id == "connect-timeout-ms" ? parse_integer(value, 100, 10000, number)
                                             : parse_integer(value, 500, 30000, number);
        if (parsed)
        {
            if (id == "connect-timeout-ms") candidate.online.connect_timeout = std::chrono::milliseconds(number);
            else candidate.online.total_timeout = std::chrono::milliseconds(number);
        }
    }
    else if (id == "ai-provider")
    {
        std::size_t index = 0;
        parsed = set_choice(value, {"deepseek", "openai", "siliconflow", "groq", "custom"}, index);
        if (parsed) candidate.online.ai.provider = static_cast<online::AiProvider>(index);
    }
    else if (id == "voice-provider")
    {
        std::size_t index = 0;
        parsed = set_choice(value, {"openai", "siliconflow", "groq", "custom"}, index);
        if (parsed) candidate.voice.provider = value;
    }
    else if (id == "ai-candidate-limit")
    {
        int number = 0; parsed = parse_integer(value, 1, 10, number); candidate.online.ai.candidate_limit = static_cast<std::size_t>(number);
    }
    else if (id == "translation-provider")
    {
        if (value == "local") candidate.online.translation_provider = TranslationProvider::Local;
        else if (value == "deeplx") candidate.online.translation_provider = TranslationProvider::DeepLX;
        else parsed = false;
    }
    else if (id == "translation-target-language")
    {
        std::size_t index = 0;
        parsed = set_choice(value, {"en", "fr", "ja", "es", "ru", "de", "ko"}, index);
        if (parsed) candidate.online.translation_target_language = value;
    }
    else if (id == "ai-endpoint" || id == "ai-model" || id == "ai-prompt" || id == "translation-endpoint" ||
             id == "voice-endpoint" || id == "voice-model" || id == "voice-language" ||
             id == "voice-polish-endpoint" || id == "voice-polish-model" || id == "voice-polish-prompt")
    {
        const bool is_endpoint = id == "ai-endpoint" || id == "translation-endpoint" || id == "voice-endpoint" ||
                                 id == "voice-polish-endpoint";
        const bool is_model = id == "ai-model" || id == "voice-model" || id == "voice-polish-model";
        const std::size_t maximum = is_endpoint ? 2048 : (is_model ? 256 : (id == "voice-language" ? 32 : 8192));
        parsed = valid_text(value, maximum, id == "ai-prompt" || id == "voice-polish-prompt") &&
                 (!is_endpoint || value.empty() || value.rfind("https://", 0) == 0);
        if (parsed)
        {
            if (id == "ai-endpoint") candidate.online.ai.endpoint = value;
            else if (id == "ai-model") candidate.online.ai.model = value;
            else if (id == "ai-prompt") candidate.online.ai.prompt = value;
            else if (id == "translation-endpoint") candidate.online.translation_endpoint = value;
            else if (id == "voice-endpoint") candidate.voice.endpoint = value;
            else if (id == "voice-model") candidate.voice.model = value;
            else if (id == "voice-language") candidate.voice.language = value;
            else if (id == "voice-polish-endpoint") candidate.voice.polish_endpoint = value;
            else if (id == "voice-polish-model") candidate.voice.polish_model = value;
            else candidate.voice.polish_prompt = value;
        }
    }
    else
    {
        parsed = false;
    }

    if (!parsed)
    {
        set_error(error, "Invalid value for setting.");
        return false;
    }
    settings_ = std::move(candidate);
    rebuild_rows();
    if (error != nullptr) error->clear();
    return true;
}
} // namespace metasequoia::linux_ime
