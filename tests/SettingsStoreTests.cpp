#include "../src/SettingsStore.h"
#include "../src/SecretStore.h"
#include "../vendor/MetasequoiaImeEngine/core/data_path.h"

#include <sys/stat.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
using metasequoia::FrequencyAdjustmentMode;
using metasequoia::linux_ime::CharacterWidth;
using metasequoia::linux_ime::InputMode;
using metasequoia::linux_ime::InputSettings;
using metasequoia::linux_ime::OnlineSettings;
using metasequoia::linux_ime::PreeditStyle;
using metasequoia::linux_ime::PunctuationLock;
using metasequoia::linux_ime::PunctuationMode;
using metasequoia::linux_ime::SecretKind;
using metasequoia::linux_ime::SecretLookupResult;
using metasequoia::linux_ime::SecretStatus;
using metasequoia::linux_ime::SecretStore;
using metasequoia::linux_ime::SettingsStore;
using metasequoia::linux_ime::TranslationProvider;
using metasequoia::linux_ime::online::AiProvider;

class MemorySecretStore final : public SecretStore
{
  public:
    SecretLookupResult lookup(SecretKind kind, std::string_view provider) const override
    {
        if (!available)
        {
            return {SecretStatus::Unavailable, {}, "Credential service unavailable."};
        }
        const auto found = values.find({kind, std::string(provider)});
        if (found == values.end())
        {
            return {SecretStatus::NotFound, {}, {}};
        }
        return {SecretStatus::Found, found->second, {}};
    }

    bool store(SecretKind kind, std::string_view provider, std::string_view secret, std::string *diagnostic) override
    {
        if (diagnostic != nullptr)
        {
            *diagnostic = available ? "" : "Credential service unavailable.";
        }
        if (!available)
        {
            return false;
        }
        if (fail_store_kind == kind)
        {
            if (become_unavailable_on_store_failure)
            {
                available = false;
            }
            return false;
        }
        values[{kind, std::string(provider)}] = std::string(secret);
        return true;
    }

    bool erase(SecretKind kind, std::string_view provider, std::string *diagnostic) override
    {
        if (diagnostic != nullptr)
        {
            *diagnostic = available ? "" : "Credential service unavailable.";
        }
        if (!available)
        {
            return false;
        }
        values.erase({kind, std::string(provider)});
        return true;
    }

    bool available = true;
    bool become_unavailable_on_store_failure = false;
    std::optional<SecretKind> fail_store_kind;
    std::map<std::pair<SecretKind, std::string>, std::string> values;
};

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
                defaults.punctuation_lock == PunctuationLock::Follow &&
                defaults.character_width == CharacterWidth::Half && defaults.comma_period_paging &&
                defaults.word_to_character && !defaults.bracket_paging && defaults.smart_punctuation &&
                defaults.smart_punctuation_repeat_to_chinese && defaults.paired_punctuation &&
                defaults.preedit_style == PreeditStyle::Raw && defaults.quanpin_helpcode_enabled &&
                defaults.quanpin_helpcode_schema == "lantian" && defaults.shuangpin_helpcode_enabled &&
                defaults.shuangpin_helpcode_schema == "lantian" &&
                defaults.frequency_adjustment_mode == FrequencyAdjustmentMode::Promote &&
                defaults.frequency_trigger_count == 1 && defaults.frequency_linear_step == 1 &&
                defaults.unicode_mode_enabled && defaults.super_jianpin_mode_enabled &&
                defaults.quick_phrase_mode_enabled && defaults.date_time_mode_enabled && defaults.emoji_mode_enabled &&
                defaults.kaomoji_mode_enabled && defaults.temporary_english_mode_enabled &&
                defaults.temporary_japanese_mode_enabled && defaults.mixed_english_candidates_enabled &&
                defaults.mixed_english_minimum_prefix == 2 && defaults.mixed_emoji_candidates_enabled &&
                !defaults.mixed_kaomoji_candidates_enabled,
            "Missing settings did not use defaults.");
    require(warning.empty(), "A missing optional settings file produced a warning.");
    require(defaults.online.cloud_candidates_enabled && !defaults.online.ai.enabled &&
                defaults.online.ai.provider == AiProvider::DeepSeek && defaults.online.ai.token.empty() &&
                defaults.online.ai.endpoint.empty() && defaults.online.ai.model.empty() &&
                defaults.online.ai.prompt.empty() && defaults.online.ai.candidate_limit == 3 &&
                defaults.online.candidate_translations_enabled &&
                defaults.online.translation_provider == TranslationProvider::Local &&
                defaults.online.translation_target_language == "en" && defaults.online.translation_endpoint.empty() &&
                defaults.online.translation_token.empty() && defaults.online.connect_timeout.count() == 2500 &&
                defaults.online.total_timeout.count() == 8000,
            "Missing online settings did not use safe defaults.");

    InputSettings saved;
    saved.mode = InputMode::Direct;
    saved.scheme = SchemeType::Wubi;
    saved.page_size = 3;
    saved.punctuation_mode = PunctuationMode::English;
    saved.punctuation_lock = PunctuationLock::English;
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
    saved.super_jianpin_mode_enabled = false;
    saved.temporary_english_mode_enabled = false;
    saved.temporary_japanese_mode_enabled = false;
    saved.mixed_english_candidates_enabled = true;
    saved.mixed_english_minimum_prefix = 4;
    saved.mixed_emoji_candidates_enabled = true;
    saved.mixed_kaomoji_candidates_enabled = true;
    saved.clipboard_history_enabled = true;
    saved.quick_phrase_mode_enabled = false;
    saved.date_time_mode_enabled = false;
    saved.emoji_mode_enabled = false;
    saved.kaomoji_mode_enabled = false;
    saved.floating_toolbar_enabled = false;
    saved.switch_language_shift = false;
    saved.switch_language_ctrl = true;
    saved.switch_language_ctrl_alt_space = false;
    saved.voice.enabled = true;
    saved.voice.provider = "openai";
    saved.voice.endpoint = "https://voice.example.test/v1/audio/transcriptions";
    saved.voice.model = "whisper-1";
    saved.voice.language = "zh";
    saved.voice.token = "voice-settings-round-trip-secret";
    saved.voice.polish_enabled = true;
    saved.voice.polish_endpoint = "https://voice.example.test/v1/chat/completions";
    saved.voice.polish_model = "polish-model";
    saved.voice.polish_prompt = "请整理文本。";
    saved.online.cloud_candidates_enabled = false;
    saved.online.ai.enabled = true;
    saved.online.ai.provider = AiProvider::OpenAI;
    saved.online.ai.token = "sk-settings-round-trip-secret";
    saved.online.ai.endpoint = "https://example.test/v1/chat/completions";
    saved.online.ai.model = "example-model";
    saved.online.ai.prompt = "Only return the requested candidate.";
    saved.online.ai.candidate_limit = 5;
    saved.online.candidate_translations_enabled = true;
    saved.online.translation_provider = TranslationProvider::DeepLX;
    saved.online.translation_target_language = "ja";
    saved.online.translation_endpoint = "https://translate.example.test/translate";
    saved.online.translation_token = "translation-settings-round-trip-secret";
    saved.online.connect_timeout = std::chrono::milliseconds(1500);
    saved.online.total_timeout = std::chrono::milliseconds(6000);
    std::string error;
    require(store.save(saved, &error) && error.empty(), "Valid settings could not be saved.");
    const InputSettings round_trip = store.load(&warning);
    require(round_trip.mode == saved.mode && round_trip.scheme == saved.scheme &&
                round_trip.page_size == saved.page_size && round_trip.punctuation_mode == saved.punctuation_mode &&
                round_trip.punctuation_lock == saved.punctuation_lock &&
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
                round_trip.super_jianpin_mode_enabled == saved.super_jianpin_mode_enabled &&
                round_trip.temporary_english_mode_enabled == saved.temporary_english_mode_enabled &&
                round_trip.temporary_japanese_mode_enabled == saved.temporary_japanese_mode_enabled &&
                round_trip.mixed_english_candidates_enabled == saved.mixed_english_candidates_enabled &&
                round_trip.mixed_english_minimum_prefix == saved.mixed_english_minimum_prefix &&
                round_trip.mixed_emoji_candidates_enabled == saved.mixed_emoji_candidates_enabled &&
                round_trip.mixed_kaomoji_candidates_enabled == saved.mixed_kaomoji_candidates_enabled &&
                round_trip.clipboard_history_enabled == saved.clipboard_history_enabled &&
                round_trip.quick_phrase_mode_enabled == saved.quick_phrase_mode_enabled &&
                round_trip.date_time_mode_enabled == saved.date_time_mode_enabled &&
                round_trip.emoji_mode_enabled == saved.emoji_mode_enabled &&
                round_trip.kaomoji_mode_enabled == saved.kaomoji_mode_enabled &&
                round_trip.floating_toolbar_enabled == saved.floating_toolbar_enabled &&
                round_trip.switch_language_shift == saved.switch_language_shift &&
                round_trip.switch_language_ctrl == saved.switch_language_ctrl &&
                round_trip.switch_language_ctrl_alt_space == saved.switch_language_ctrl_alt_space &&
                round_trip.voice.enabled == saved.voice.enabled && round_trip.voice.provider == saved.voice.provider &&
                round_trip.voice.endpoint == saved.voice.endpoint && round_trip.voice.model == saved.voice.model &&
                round_trip.voice.language == saved.voice.language &&
                round_trip.voice.polish_enabled == saved.voice.polish_enabled &&
                round_trip.voice.polish_endpoint == saved.voice.polish_endpoint &&
                round_trip.voice.polish_model == saved.voice.polish_model &&
                round_trip.voice.polish_prompt == saved.voice.polish_prompt && round_trip.voice.token.empty(),
            "Settings did not survive a round trip.");
    require(round_trip.online.cloud_candidates_enabled == saved.online.cloud_candidates_enabled &&
                round_trip.online.ai.enabled == saved.online.ai.enabled &&
                round_trip.online.ai.provider == saved.online.ai.provider && round_trip.online.ai.token.empty() &&
                round_trip.online.ai.endpoint == saved.online.ai.endpoint &&
                round_trip.online.ai.model == saved.online.ai.model &&
                round_trip.online.ai.prompt == saved.online.ai.prompt &&
                round_trip.online.ai.candidate_limit == saved.online.ai.candidate_limit &&
                round_trip.online.candidate_translations_enabled == saved.online.candidate_translations_enabled &&
                round_trip.online.translation_provider == saved.online.translation_provider &&
                round_trip.online.translation_target_language == saved.online.translation_target_language &&
                round_trip.online.translation_endpoint == saved.online.translation_endpoint &&
                round_trip.online.translation_token.empty() &&
                round_trip.online.connect_timeout == saved.online.connect_timeout &&
                round_trip.online.total_timeout == saved.online.total_timeout,
            "Non-secret online settings did not survive a round trip or a token leaked through the key file.");
    const std::string saved_contents = read_file(store.config_path());
    require(saved_contents.find(saved.online.ai.token) == std::string::npos &&
                saved_contents.find(saved.online.translation_token) == std::string::npos &&
                saved_contents.find(saved.voice.token) == std::string::npos,
            "An API token was written to config.ini.");

    const auto config_path = store.config_path();
    write_file(config_path, "[input]\n"
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
                            "super-jianpin-mode=false\n"
                            "temporary-english-mode=false\n"
                            "temporary-japanese-mode=false\n"
                            "mixed-english-candidates=true\n"
                            "mixed-english-minimum-prefix=4\n"
                            "mixed-emoji-candidates=true\n"
                            "mixed-kaomoji-candidates=true\n"
                            "future-option=keep-me\n"
                            "\n"
                            "[online]\n"
                            "cloud-enabled=false\n"
                            "connect-timeout-ms=1800\n"
                            "total-timeout-ms=7000\n"
                            "\n"
                            "[ai]\n"
                            "enabled=true\n"
                            "provider=groq\n"
                            "endpoint=https://api.groq.com/openai/v1/chat/completions\n"
                            "model=openai/gpt-oss-120b\n"
                            "prompt=Return one candidate.\n"
                            "candidate-limit=4\n"
                            "token=legacy-ai-plaintext-secret\n"
                            "\n"
                            "[translation]\n"
                            "enabled=true\n"
                            "provider=deeplx\n"
                            "target-language=fr\n"
                            "endpoint=https://translate.example.test/translate\n"
                            "api-key=legacy-translation-plaintext-secret\n"
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
    updated.super_jianpin_mode_enabled = true;
    updated.temporary_english_mode_enabled = true;
    updated.temporary_japanese_mode_enabled = true;
    updated.mixed_english_candidates_enabled = false;
    updated.mixed_english_minimum_prefix = 5;
    updated.mixed_emoji_candidates_enabled = false;
    updated.mixed_kaomoji_candidates_enabled = false;
    updated.online.cloud_candidates_enabled = true;
    updated.online.ai.enabled = false;
    updated.online.ai.provider = AiProvider::SiliconFlow;
    updated.online.ai.endpoint = "https://api.siliconflow.cn/v1/chat/completions";
    updated.online.ai.model = "Qwen/Qwen3-8B";
    updated.online.ai.prompt = "Return concise candidates.";
    updated.online.ai.candidate_limit = 2;
    updated.online.candidate_translations_enabled = false;
    updated.online.translation_provider = TranslationProvider::Local;
    updated.online.translation_target_language = "de";
    updated.online.translation_endpoint.clear();
    updated.online.connect_timeout = std::chrono::milliseconds(2200);
    updated.online.total_timeout = std::chrono::milliseconds(7500);
    require(store.save(updated, &error), "Existing settings could not be replaced.");
    require(inode(config_path) != original_inode,
            "The settings file was modified in place instead of atomically replaced.");
    const std::string preserved = read_file(config_path);
    require(preserved.find("future-option=keep-me") != std::string::npos &&
                preserved.find("[future]") != std::string::npos &&
                preserved.find("value=preserve-me") != std::string::npos,
            "Saving known settings discarded unknown keys.");
    require(preserved.find("legacy-ai-plaintext-secret") == std::string::npos &&
                preserved.find("legacy-translation-plaintext-secret") == std::string::npos,
            "Saving settings preserved a legacy plaintext credential.");
    for (const auto &entry : std::filesystem::directory_iterator(config_path.parent_path()))
    {
        require(entry.path() == config_path, "An atomic settings temporary file was left behind.");
    }

    write_file(config_path, "[input]\n"
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
                            "super-jianpin-mode=unexpected\n"
                            "temporary-english-mode=unexpected\n"
                            "temporary-japanese-mode=unexpected\n"
                            "mixed-english-candidates=unexpected\n"
                            "mixed-english-minimum-prefix=9\n"
                            "mixed-emoji-candidates=unexpected\n"
                            "mixed-kaomoji-candidates=unexpected\n"
                            "\n"
                            "[online]\n"
                            "cloud-enabled=unexpected\n"
                            "connect-timeout-ms=99\n"
                            "total-timeout-ms=30001\n"
                            "\n"
                            "[ai]\n"
                            "enabled=true\n"
                            "provider=unsupported\n"
                            "endpoint=http://insecure.example.test/chat\n"
                            "model=valid-model\n"
                            "prompt=valid prompt\n"
                            "candidate-limit=11\n"
                            "\n"
                            "[translation]\n"
                            "enabled=true\n"
                            "provider=unsupported\n"
                            "target-language=unsupported\n"
                            "endpoint=http://insecure.example.test/translate\n");
    const InputSettings invalid = store.load(&warning);
    require(invalid.mode == InputMode::Ime && invalid.scheme == SchemeType::Quanpin && invalid.page_size == 9 &&
                invalid.punctuation_mode == PunctuationMode::Chinese &&
                invalid.character_width == CharacterWidth::Half && invalid.comma_period_paging &&
                invalid.word_to_character && !invalid.bracket_paging && invalid.smart_punctuation &&
                invalid.smart_punctuation_repeat_to_chinese && invalid.paired_punctuation &&
                invalid.preedit_style == PreeditStyle::Raw && invalid.quanpin_helpcode_enabled &&
                invalid.quanpin_helpcode_schema == "lantian" && invalid.shuangpin_helpcode_enabled &&
                invalid.shuangpin_helpcode_schema == "lantian" &&
                invalid.frequency_adjustment_mode == FrequencyAdjustmentMode::Promote &&
                invalid.frequency_trigger_count == 1 && invalid.frequency_linear_step == 1 &&
                invalid.unicode_mode_enabled && invalid.super_jianpin_mode_enabled &&
                invalid.temporary_english_mode_enabled && invalid.temporary_japanese_mode_enabled &&
                invalid.mixed_english_candidates_enabled && invalid.mixed_english_minimum_prefix == 2 &&
                invalid.mixed_emoji_candidates_enabled && !invalid.mixed_kaomoji_candidates_enabled,
            "Invalid settings did not fall back field by field.");
    require(invalid.online.cloud_candidates_enabled && invalid.online.ai.enabled &&
                invalid.online.ai.provider == AiProvider::DeepSeek && invalid.online.ai.endpoint.empty() &&
                invalid.online.ai.model == "valid-model" && invalid.online.ai.prompt == "valid prompt" &&
                invalid.online.ai.candidate_limit == 3 && invalid.online.candidate_translations_enabled &&
                invalid.online.translation_provider == TranslationProvider::Local &&
                invalid.online.translation_target_language == "en" && invalid.online.translation_endpoint.empty() &&
                invalid.online.connect_timeout.count() == 2500 && invalid.online.total_timeout.count() == 8000,
            "Invalid online settings did not fall back field by field.");
    require(!warning.empty(), "Invalid settings did not produce a diagnostic warning.");

    InputSettings unsupported = saved;
    unsupported.quanpin_helpcode_schema = "unknown";
    require(!store.save(unsupported, &error) && !error.empty(), "An unsupported helpcode schema was written to disk.");
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
    unsupported = saved;
    unsupported.online.ai.provider = static_cast<AiProvider>(99);
    require(!store.save(unsupported, &error) && !error.empty(), "An unsupported AI provider was written to disk.");
    unsupported = saved;
    unsupported.online.ai.endpoint = "http://insecure.example.test/chat";
    require(!store.save(unsupported, &error) && !error.empty(), "An insecure AI endpoint was written to disk.");
    unsupported = saved;
    unsupported.online.ai.candidate_limit = 11;
    require(!store.save(unsupported, &error) && !error.empty(),
            "An out-of-range AI candidate limit was written to disk.");
    unsupported = saved;
    unsupported.online.translation_target_language = "unsupported";
    require(!store.save(unsupported, &error) && !error.empty(),
            "An unsupported translation target language was written to disk.");
    unsupported = saved;
    unsupported.online.connect_timeout = std::chrono::milliseconds(99);
    require(!store.save(unsupported, &error) && !error.empty(), "An invalid connect timeout was written to disk.");
    unsupported = saved;
    unsupported.online.total_timeout = std::chrono::milliseconds(30001);
    require(!store.save(unsupported, &error) && !error.empty(), "An invalid total timeout was written to disk.");

    // A token is concatenated into an Authorization header, and libcurl passes
    // header values through verbatim. Endpoints were rejected at this layer and
    // again before a request; tokens were only ever checked before the request,
    // so a malformed one was stored and the feature then failed with nothing to
    // explain why. Reject it here, where the settings window can show a message.
    MemorySecretStore rejecting_secrets;
    std::string rejection_diagnostic;
    InputSettings header_injection = saved;
    header_injection.voice.token = "sk-abc\r\nX-Injected: yes";
    require(!store.save(header_injection, rejecting_secrets, &rejection_diagnostic) && !rejection_diagnostic.empty(),
            "A voice token containing a header separator was accepted.");
    header_injection = saved;
    header_injection.online.ai.token = "sk-abc\nX-Injected: yes";
    require(!store.save(header_injection, rejecting_secrets, &rejection_diagnostic) && !rejection_diagnostic.empty(),
            "An AI token containing a newline was accepted.");
    header_injection = saved;
    header_injection.online.translation_token = std::string(4097, 'a');
    require(!store.save(header_injection, rejecting_secrets, &rejection_diagnostic) && !rejection_diagnostic.empty(),
            "An overlong translation token was accepted.");
    require(rejecting_secrets.values.empty(), "A rejected credential still reached the secret store.");

    MemorySecretStore secrets;
    std::string secret_diagnostic;
    require(store.save(saved, secrets, &secret_diagnostic) && secret_diagnostic.empty(),
            "Settings and credentials could not be saved together.");
    const InputSettings hydrated = store.load(secrets, &secret_diagnostic);
    require(hydrated.online.ai.enabled && hydrated.online.ai.token == saved.online.ai.token &&
                hydrated.online.translation_token == saved.online.translation_token && hydrated.voice.enabled &&
                hydrated.voice.token == saved.voice.token && secret_diagnostic.empty(),
            "Credentials were not restored from the secret store.");
    const std::string hydrated_contents = read_file(config_path);
    require(hydrated_contents.find(saved.online.ai.token) == std::string::npos &&
                hydrated_contents.find(saved.online.translation_token) == std::string::npos &&
                hydrated_contents.find(saved.voice.token) == std::string::npos &&
                secret_diagnostic.find(saved.online.ai.token) == std::string::npos &&
                secret_diagnostic.find(saved.online.translation_token) == std::string::npos,
            "A credential appeared in config.ini or a diagnostic.");

    MemorySecretStore missing_secrets;
    missing_secrets.values[{SecretKind::TranslationApiToken, "deeplx"}] = saved.online.translation_token;
    const InputSettings missing_ai = store.load(missing_secrets, &secret_diagnostic);
    require(!missing_ai.online.ai.enabled && missing_ai.online.translation_token == saved.online.translation_token &&
                !secret_diagnostic.empty() && secret_diagnostic.find(saved.online.ai.token) == std::string::npos,
            "A missing AI credential did not disable only AI or leaked secret material.");

    MemorySecretStore missing_translation_secrets;
    missing_translation_secrets.values[{SecretKind::AiApiToken, "openai"}] = saved.online.ai.token;
    const InputSettings missing_translation = store.load(missing_translation_secrets, &secret_diagnostic);
    require(missing_translation.online.ai.enabled && !missing_translation.online.candidate_translations_enabled &&
                !secret_diagnostic.empty() && secret_diagnostic.find(saved.online.ai.token) == std::string::npos &&
                secret_diagnostic.find(saved.online.translation_token) == std::string::npos,
            "A missing translation credential did not disable only translation or leaked secret material.");

    MemorySecretStore unavailable_secrets;
    unavailable_secrets.available = false;
    const InputSettings unavailable = store.load(unavailable_secrets, &secret_diagnostic);
    require(!unavailable.online.ai.enabled && !unavailable.online.candidate_translations_enabled &&
                !secret_diagnostic.empty() && secret_diagnostic.find(saved.online.ai.token) == std::string::npos &&
                secret_diagnostic.find(saved.online.translation_token) == std::string::npos,
            "An unavailable secret service did not isolate online providers or exposed a credential.");

    MemorySecretStore rollback_secrets;
    rollback_secrets.values[{SecretKind::AiApiToken, "openai"}] = "old-ai-secret";
    rollback_secrets.values[{SecretKind::TranslationApiToken, "deeplx"}] = "old-translation-secret";
    rollback_secrets.fail_store_kind = SecretKind::TranslationApiToken;
    require(!store.save(saved, rollback_secrets, &secret_diagnostic) &&
                rollback_secrets.values.at({SecretKind::AiApiToken, "openai"}) == "old-ai-secret" &&
                rollback_secrets.values.at({SecretKind::TranslationApiToken, "deeplx"}) == "old-translation-secret" &&
                secret_diagnostic.find(saved.online.ai.token) == std::string::npos &&
                secret_diagnostic.find(saved.online.translation_token) == std::string::npos,
            "A second credential failure did not roll back the first credential update.");
    rollback_secrets.fail_store_kind.reset();

    rollback_secrets.fail_store_kind = SecretKind::TranslationApiToken;
    rollback_secrets.become_unavailable_on_store_failure = true;
    require(!store.save(saved, rollback_secrets, &secret_diagnostic) && !rollback_secrets.available &&
                rollback_secrets.values.at({SecretKind::AiApiToken, "openai"}) == saved.online.ai.token &&
                secret_diagnostic.find("roll back") != std::string::npos &&
                secret_diagnostic.find(saved.online.ai.token) == std::string::npos &&
                secret_diagnostic.find(saved.online.translation_token) == std::string::npos,
            "An unavailable credential service hid a failed credential rollback.");
    rollback_secrets.available = true;
    rollback_secrets.become_unavailable_on_store_failure = false;
    rollback_secrets.fail_store_kind.reset();
    rollback_secrets.values[{SecretKind::AiApiToken, "openai"}] = "old-ai-secret";
    rollback_secrets.values[{SecretKind::TranslationApiToken, "deeplx"}] = "old-translation-secret";

    std::filesystem::remove(config_path);
    std::filesystem::create_directory(config_path);
    require(!store.save(saved, rollback_secrets, &secret_diagnostic) &&
                rollback_secrets.values.at({SecretKind::AiApiToken, "openai"}) == "old-ai-secret" &&
                rollback_secrets.values.at({SecretKind::TranslationApiToken, "deeplx"}) == "old-translation-secret" &&
                secret_diagnostic.find(saved.online.ai.token) == std::string::npos &&
                secret_diagnostic.find(saved.online.translation_token) == std::string::npos,
            "A config-file failure did not roll back credential updates.");
    require(!store.save(saved, &error) && !error.empty(), "A settings replacement failure was not reported.");

    std::filesystem::remove_all(config_home);
    return 0;
}
