#include "SettingsStore.h"

#include "core/data_path.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <cerrno>
#include <cstring>
#include <string_view>
#include <utility>
#include <unistd.h>
#include <vector>

namespace metasequoia::linux_ime
{
namespace
{
constexpr const char *kGroup = "input";
constexpr std::size_t kMinimumPageSize = 3;
constexpr std::size_t kMaximumPageSize = 9;
constexpr int kMinimumFrequencyValue = 1;
constexpr int kMaximumFrequencyValue = 10;
constexpr gint kMinimumEnglishPrefix = 1;
constexpr gint kMaximumEnglishPrefix = 8;

void set_message(std::string *destination, const char *message)
{
    if (destination != nullptr)
    {
        *destination = message;
    }
}

const char *mode_name(InputMode mode)
{
    switch (mode)
    {
    case InputMode::Ime:
        return "ime";
    case InputMode::Direct:
        return "direct";
    }
    return nullptr;
}

const char *scheme_name(SchemeType scheme)
{
    switch (scheme)
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
    return nullptr;
}

const char *punctuation_name(PunctuationMode mode)
{
    switch (mode)
    {
    case PunctuationMode::Chinese:
        return "chinese";
    case PunctuationMode::English:
        return "english";
    }
    return nullptr;
}

const char *preedit_style_name(PreeditStyle style)
{
    switch (style)
    {
    case PreeditStyle::Raw:
        return "raw";
    case PreeditStyle::Pinyin:
        return "pinyin";
    case PreeditStyle::Hidden:
        return "hidden";
    }
    return nullptr;
}

const char *frequency_adjustment_name(FrequencyAdjustmentMode mode)
{
    switch (mode)
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
    return nullptr;
}

bool valid_character_width(CharacterWidth width)
{
    return width == CharacterWidth::Half || width == CharacterWidth::Full;
}

bool write_all(int descriptor, const char *data, std::size_t size)
{
    std::size_t written = 0;
    while (written < size)
    {
        const ssize_t count = ::write(descriptor, data + written, size - written);
        if (count < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        if (count == 0)
        {
            return false;
        }
        written += static_cast<std::size_t>(count);
    }
    return true;
}
} // namespace

SettingsStore::SettingsStore() : SettingsStore(metasequoia::path_from_utf8(g_get_user_config_dir())) {}

SettingsStore::SettingsStore(std::filesystem::path config_home)
    : config_path_(std::move(config_home) / "metasequoiaime" / "config.ini")
{
}

InputSettings SettingsStore::load(std::string *warning) const
{
    set_message(warning, "");
    InputSettings settings;
    GError *read_error = nullptr;
    gchar *contents = nullptr;
    gsize length = 0;
    if (!g_file_get_contents(metasequoia::path_to_utf8(config_path_).c_str(), &contents, &length, &read_error))
    {
        if (g_error_matches(read_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
            g_clear_error(&read_error);
            return settings;
        }
        g_clear_error(&read_error);
        set_message(warning, "Unable to read input settings; defaults were used.");
        return settings;
    }

    GKeyFile *key_file = g_key_file_new();
    GError *parse_error = nullptr;
    if (!g_key_file_load_from_data(key_file, contents, length,
                                   static_cast<GKeyFileFlags>(G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS),
                                   &parse_error))
    {
        g_clear_error(&parse_error);
        g_key_file_unref(key_file);
        g_free(contents);
        set_message(warning, "Unable to parse input settings; defaults were used.");
        return settings;
    }
    g_free(contents);

    bool invalid = false;
    if (g_key_file_has_key(key_file, kGroup, "mode", nullptr))
    {
        gchar *value = g_key_file_get_string(key_file, kGroup, "mode", nullptr);
        if (value != nullptr && std::string_view(value) == "direct")
        {
            settings.mode = InputMode::Direct;
        }
        else if (value == nullptr || std::string_view(value) != "ime")
        {
            invalid = true;
        }
        g_free(value);
    }

    if (g_key_file_has_key(key_file, kGroup, "scheme", nullptr))
    {
        gchar *value = g_key_file_get_string(key_file, kGroup, "scheme", nullptr);
        const std::string_view name = value == nullptr ? std::string_view{} : std::string_view(value);
        if (name == "shuangpin")
        {
            settings.scheme = SchemeType::Shuangpin;
        }
        else if (name == "wubi")
        {
            settings.scheme = SchemeType::Wubi;
        }
        else if (name == "japanese")
        {
            settings.scheme = SchemeType::JapaneseRomaji;
        }
        else if (name != "quanpin")
        {
            invalid = true;
        }
        g_free(value);
    }

    if (g_key_file_has_key(key_file, kGroup, "page-size", nullptr))
    {
        GError *value_error = nullptr;
        const gint value = g_key_file_get_integer(key_file, kGroup, "page-size", &value_error);
        if (value_error == nullptr && value >= static_cast<gint>(kMinimumPageSize) &&
            value <= static_cast<gint>(kMaximumPageSize))
        {
            settings.page_size = static_cast<std::size_t>(value);
        }
        else
        {
            invalid = true;
        }
        g_clear_error(&value_error);
    }

    if (g_key_file_has_key(key_file, kGroup, "punctuation", nullptr))
    {
        gchar *value = g_key_file_get_string(key_file, kGroup, "punctuation", nullptr);
        if (value != nullptr && std::string_view(value) == "english")
        {
            settings.punctuation_mode = PunctuationMode::English;
        }
        else if (value == nullptr || std::string_view(value) != "chinese")
        {
            invalid = true;
        }
        g_free(value);
    }

    if (g_key_file_has_key(key_file, kGroup, "full-width", nullptr))
    {
        GError *value_error = nullptr;
        const gboolean value = g_key_file_get_boolean(key_file, kGroup, "full-width", &value_error);
        if (value_error == nullptr)
        {
            settings.character_width = value ? CharacterWidth::Full : CharacterWidth::Half;
        }
        else
        {
            invalid = true;
        }
        g_clear_error(&value_error);
    }

    if (g_key_file_has_key(key_file, kGroup, "comma-period-paging", nullptr))
    {
        GError *value_error = nullptr;
        const gboolean value = g_key_file_get_boolean(key_file, kGroup, "comma-period-paging", &value_error);
        if (value_error == nullptr)
        {
            settings.comma_period_paging = value;
        }
        else
        {
            invalid = true;
        }
        g_clear_error(&value_error);
    }

    if (g_key_file_has_key(key_file, kGroup, "word-to-character", nullptr))
    {
        GError *value_error = nullptr;
        const gboolean value = g_key_file_get_boolean(key_file, kGroup, "word-to-character", &value_error);
        if (value_error == nullptr)
        {
            settings.word_to_character = value;
        }
        else
        {
            invalid = true;
        }
        g_clear_error(&value_error);
    }

    if (g_key_file_has_key(key_file, kGroup, "bracket-paging", nullptr))
    {
        GError *value_error = nullptr;
        const gboolean value = g_key_file_get_boolean(key_file, kGroup, "bracket-paging", &value_error);
        if (value_error == nullptr)
        {
            settings.bracket_paging = value;
        }
        else
        {
            invalid = true;
        }
        g_clear_error(&value_error);
    }

    if (g_key_file_has_key(key_file, kGroup, "smart-punctuation", nullptr))
    {
        GError *value_error = nullptr;
        const gboolean value = g_key_file_get_boolean(key_file, kGroup, "smart-punctuation", &value_error);
        if (value_error == nullptr)
        {
            settings.smart_punctuation = value;
        }
        else
        {
            invalid = true;
        }
        g_clear_error(&value_error);
    }

    if (g_key_file_has_key(key_file, kGroup, "smart-punctuation-repeat-to-chinese", nullptr))
    {
        GError *value_error = nullptr;
        const gboolean value =
            g_key_file_get_boolean(key_file, kGroup, "smart-punctuation-repeat-to-chinese", &value_error);
        if (value_error == nullptr)
        {
            settings.smart_punctuation_repeat_to_chinese = value;
        }
        else
        {
            invalid = true;
        }
        g_clear_error(&value_error);
    }

    if (g_key_file_has_key(key_file, kGroup, "paired-punctuation", nullptr))
    {
        GError *value_error = nullptr;
        const gboolean value = g_key_file_get_boolean(key_file, kGroup, "paired-punctuation", &value_error);
        if (value_error == nullptr)
        {
            settings.paired_punctuation = value;
        }
        else
        {
            invalid = true;
        }
        g_clear_error(&value_error);
    }

    if (g_key_file_has_key(key_file, kGroup, "preedit-style", nullptr))
    {
        gchar *value = g_key_file_get_string(key_file, kGroup, "preedit-style", nullptr);
        const std::string_view name = value == nullptr ? std::string_view{} : std::string_view(value);
        if (name == "pinyin")
        {
            settings.preedit_style = PreeditStyle::Pinyin;
        }
        else if (name == "hidden")
        {
            settings.preedit_style = PreeditStyle::Hidden;
        }
        else if (name != "raw")
        {
            invalid = true;
        }
        g_free(value);
    }

    if (g_key_file_has_key(key_file, kGroup, "quanpin-helpcode", nullptr))
    {
        GError *value_error = nullptr;
        const gboolean value = g_key_file_get_boolean(key_file, kGroup, "quanpin-helpcode", &value_error);
        if (value_error == nullptr)
        {
            settings.quanpin_helpcode_enabled = value;
        }
        else
        {
            invalid = true;
        }
        g_clear_error(&value_error);
    }

    if (g_key_file_has_key(key_file, kGroup, "quanpin-helpcode-schema", nullptr))
    {
        gchar *value = g_key_file_get_string(key_file, kGroup, "quanpin-helpcode-schema", nullptr);
        if (value != nullptr && InputSession::is_supported_helpcode_schema(value))
        {
            settings.quanpin_helpcode_schema = value;
        }
        else
        {
            invalid = true;
        }
        g_free(value);
    }

    if (g_key_file_has_key(key_file, kGroup, "shuangpin-helpcode", nullptr))
    {
        GError *value_error = nullptr;
        const gboolean value = g_key_file_get_boolean(key_file, kGroup, "shuangpin-helpcode", &value_error);
        if (value_error == nullptr)
        {
            settings.shuangpin_helpcode_enabled = value;
        }
        else
        {
            invalid = true;
        }
        g_clear_error(&value_error);
    }

    if (g_key_file_has_key(key_file, kGroup, "shuangpin-helpcode-schema", nullptr))
    {
        gchar *value = g_key_file_get_string(key_file, kGroup, "shuangpin-helpcode-schema", nullptr);
        if (value != nullptr && InputSession::is_supported_helpcode_schema(value))
        {
            settings.shuangpin_helpcode_schema = value;
        }
        else
        {
            invalid = true;
        }
        g_free(value);
    }

    if (g_key_file_has_key(key_file, kGroup, "frequency-adjustment", nullptr))
    {
        gchar *value = g_key_file_get_string(key_file, kGroup, "frequency-adjustment", nullptr);
        const std::string_view name = value == nullptr ? std::string_view{} : std::string_view(value);
        if (name == "disabled")
        {
            settings.frequency_adjustment_mode = FrequencyAdjustmentMode::Disabled;
        }
        else if (name == "pin")
        {
            settings.frequency_adjustment_mode = FrequencyAdjustmentMode::Pin;
        }
        else if (name == "halve")
        {
            settings.frequency_adjustment_mode = FrequencyAdjustmentMode::Halve;
        }
        else if (name == "linear")
        {
            settings.frequency_adjustment_mode = FrequencyAdjustmentMode::Linear;
        }
        else if (name != "promote")
        {
            invalid = true;
        }
        g_free(value);
    }

    if (g_key_file_has_key(key_file, kGroup, "frequency-trigger-count", nullptr))
    {
        GError *value_error = nullptr;
        const gint value = g_key_file_get_integer(key_file, kGroup, "frequency-trigger-count", &value_error);
        if (value_error == nullptr && value >= kMinimumFrequencyValue && value <= kMaximumFrequencyValue)
        {
            settings.frequency_trigger_count = value;
        }
        else
        {
            invalid = true;
        }
        g_clear_error(&value_error);
    }

    if (g_key_file_has_key(key_file, kGroup, "frequency-linear-step", nullptr))
    {
        GError *value_error = nullptr;
        const gint value = g_key_file_get_integer(key_file, kGroup, "frequency-linear-step", &value_error);
        if (value_error == nullptr && value >= kMinimumFrequencyValue && value <= kMaximumFrequencyValue)
        {
            settings.frequency_linear_step = value;
        }
        else
        {
            invalid = true;
        }
        g_clear_error(&value_error);
    }

    if (g_key_file_has_key(key_file, kGroup, "unicode-mode", nullptr))
    {
        GError *value_error = nullptr;
        const gboolean value = g_key_file_get_boolean(key_file, kGroup, "unicode-mode", &value_error);
        if (value_error == nullptr)
        {
            settings.unicode_mode_enabled = value;
        }
        else
        {
            invalid = true;
        }
        g_clear_error(&value_error);
    }

    if (g_key_file_has_key(key_file, kGroup, "mixed-english-candidates", nullptr))
    {
        GError *value_error = nullptr;
        const gboolean value = g_key_file_get_boolean(key_file, kGroup, "mixed-english-candidates", &value_error);
        if (value_error == nullptr)
        {
            settings.mixed_english_candidates_enabled = value;
        }
        else
        {
            invalid = true;
        }
        g_clear_error(&value_error);
    }

    if (g_key_file_has_key(key_file, kGroup, "mixed-english-minimum-prefix", nullptr))
    {
        GError *value_error = nullptr;
        const gint value = g_key_file_get_integer(key_file, kGroup, "mixed-english-minimum-prefix", &value_error);
        if (value_error == nullptr && value >= kMinimumEnglishPrefix && value <= kMaximumEnglishPrefix)
        {
            settings.mixed_english_minimum_prefix = static_cast<std::size_t>(value);
        }
        else
        {
            invalid = true;
        }
        g_clear_error(&value_error);
    }

    if (g_key_file_has_key(key_file, kGroup, "mixed-emoji-candidates", nullptr))
    {
        GError *value_error = nullptr;
        const gboolean value = g_key_file_get_boolean(key_file, kGroup, "mixed-emoji-candidates", &value_error);
        if (value_error == nullptr)
        {
            settings.mixed_emoji_candidates_enabled = value;
        }
        else
        {
            invalid = true;
        }
        g_clear_error(&value_error);
    }

    if (g_key_file_has_key(key_file, kGroup, "mixed-kaomoji-candidates", nullptr))
    {
        GError *value_error = nullptr;
        const gboolean value = g_key_file_get_boolean(key_file, kGroup, "mixed-kaomoji-candidates", &value_error);
        if (value_error == nullptr)
        {
            settings.mixed_kaomoji_candidates_enabled = value;
        }
        else
        {
            invalid = true;
        }
        g_clear_error(&value_error);
    }

    g_key_file_unref(key_file);
    if (invalid)
    {
        set_message(warning, "Some input settings were invalid; defaults were used for those fields.");
    }
    return settings;
}

bool SettingsStore::save(const InputSettings &settings, std::string *error) const
{
    set_message(error, "");
    const char *mode = mode_name(settings.mode);
    const char *scheme = scheme_name(settings.scheme);
    const char *punctuation = punctuation_name(settings.punctuation_mode);
    const char *preedit_style = preedit_style_name(settings.preedit_style);
    const char *frequency_adjustment = frequency_adjustment_name(settings.frequency_adjustment_mode);
    if (mode == nullptr || scheme == nullptr || punctuation == nullptr || preedit_style == nullptr ||
        frequency_adjustment == nullptr ||
        !valid_character_width(settings.character_width) || settings.page_size < kMinimumPageSize ||
        settings.page_size > kMaximumPageSize ||
        !InputSession::is_supported_helpcode_schema(settings.quanpin_helpcode_schema) ||
        !InputSession::is_supported_helpcode_schema(settings.shuangpin_helpcode_schema) ||
        settings.frequency_trigger_count < kMinimumFrequencyValue ||
        settings.frequency_trigger_count > kMaximumFrequencyValue ||
        settings.frequency_linear_step < kMinimumFrequencyValue ||
        settings.frequency_linear_step > kMaximumFrequencyValue ||
        settings.mixed_english_minimum_prefix < static_cast<std::size_t>(kMinimumEnglishPrefix) ||
        settings.mixed_english_minimum_prefix > static_cast<std::size_t>(kMaximumEnglishPrefix))
    {
        set_message(error, "Input settings were outside the supported range.");
        return false;
    }

    const std::filesystem::path directory = config_path_.parent_path();
    if (g_mkdir_with_parents(metasequoia::path_to_utf8(directory).c_str(), 0700) != 0)
    {
        set_message(error, "Unable to create the input settings directory.");
        return false;
    }

    GKeyFile *key_file = g_key_file_new();
    GError *load_error = nullptr;
    if (!g_key_file_load_from_file(key_file, metasequoia::path_to_utf8(config_path_).c_str(),
                                   static_cast<GKeyFileFlags>(G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS),
                                   &load_error) &&
        !g_error_matches(load_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
    {
        g_clear_error(&load_error);
        g_key_file_unref(key_file);
        set_message(error, "Unable to preserve the existing input settings.");
        return false;
    }
    g_clear_error(&load_error);

    g_key_file_set_string(key_file, kGroup, "mode", mode);
    g_key_file_set_string(key_file, kGroup, "scheme", scheme);
    g_key_file_set_integer(key_file, kGroup, "page-size", static_cast<gint>(settings.page_size));
    g_key_file_set_string(key_file, kGroup, "punctuation", punctuation);
    g_key_file_set_boolean(key_file, kGroup, "full-width", settings.character_width == CharacterWidth::Full);
    g_key_file_set_boolean(key_file, kGroup, "comma-period-paging", settings.comma_period_paging);
    g_key_file_set_boolean(key_file, kGroup, "word-to-character", settings.word_to_character);
    g_key_file_set_boolean(key_file, kGroup, "bracket-paging", settings.bracket_paging);
    g_key_file_set_boolean(key_file, kGroup, "smart-punctuation", settings.smart_punctuation);
    g_key_file_set_boolean(key_file, kGroup, "smart-punctuation-repeat-to-chinese",
                           settings.smart_punctuation_repeat_to_chinese);
    g_key_file_set_boolean(key_file, kGroup, "paired-punctuation", settings.paired_punctuation);
    g_key_file_set_string(key_file, kGroup, "preedit-style", preedit_style);
    g_key_file_set_boolean(key_file, kGroup, "quanpin-helpcode", settings.quanpin_helpcode_enabled);
    g_key_file_set_string(key_file, kGroup, "quanpin-helpcode-schema",
                          settings.quanpin_helpcode_schema.c_str());
    g_key_file_set_boolean(key_file, kGroup, "shuangpin-helpcode", settings.shuangpin_helpcode_enabled);
    g_key_file_set_string(key_file, kGroup, "shuangpin-helpcode-schema",
                          settings.shuangpin_helpcode_schema.c_str());
    g_key_file_set_string(key_file, kGroup, "frequency-adjustment", frequency_adjustment);
    g_key_file_set_integer(key_file, kGroup, "frequency-trigger-count", settings.frequency_trigger_count);
    g_key_file_set_integer(key_file, kGroup, "frequency-linear-step", settings.frequency_linear_step);
    g_key_file_set_boolean(key_file, kGroup, "unicode-mode", settings.unicode_mode_enabled);
    g_key_file_set_boolean(key_file, kGroup, "mixed-english-candidates",
                           settings.mixed_english_candidates_enabled);
    g_key_file_set_integer(key_file, kGroup, "mixed-english-minimum-prefix",
                           static_cast<gint>(settings.mixed_english_minimum_prefix));
    g_key_file_set_boolean(key_file, kGroup, "mixed-emoji-candidates",
                           settings.mixed_emoji_candidates_enabled);
    g_key_file_set_boolean(key_file, kGroup, "mixed-kaomoji-candidates",
                           settings.mixed_kaomoji_candidates_enabled);

    gsize data_size = 0;
    GError *data_error = nullptr;
    gchar *data = g_key_file_to_data(key_file, &data_size, &data_error);
    g_key_file_unref(key_file);
    if (data == nullptr)
    {
        g_clear_error(&data_error);
        set_message(error, "Unable to serialize input settings.");
        return false;
    }

    std::string temporary_template = metasequoia::path_to_utf8(config_path_) + ".tmp.XXXXXX";
    std::vector<char> temporary_path(temporary_template.begin(), temporary_template.end());
    temporary_path.push_back('\0');
    const int descriptor = g_mkstemp(temporary_path.data());
    if (descriptor < 0)
    {
        g_free(data);
        set_message(error, "Unable to create an atomic input settings file.");
        return false;
    }

    const bool wrote = write_all(descriptor, data, data_size);
    g_free(data);
    const bool synced = wrote && fsync(descriptor) == 0;
    const bool closed = close(descriptor) == 0;
    if (!synced || !closed || g_rename(temporary_path.data(), metasequoia::path_to_utf8(config_path_).c_str()) != 0)
    {
        g_unlink(temporary_path.data());
        set_message(error, "Unable to atomically replace input settings.");
        return false;
    }
    return true;
}

const std::filesystem::path &SettingsStore::config_path() const
{
    return config_path_;
}
} // namespace metasequoia::linux_ime
