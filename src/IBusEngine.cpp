#include "DictionaryBootstrap.h"
#include "InputController.h"
#include "IBusKeyMapper.h"
#include "SettingsStore.h"

#include "common/helpcode_utils.h"
#include "core/data_path.h"
#include "online/HttpTimeouts.h"
#include "online/HttpTransport.h"
#include "online/OnlineCandidateService.h"
#include "online/TranslationProvider.h"

#include <gio/gio.h>
#include <ibus.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace
{
using metasequoia::linux_ime::CharacterWidth;
using metasequoia::linux_ime::ControllerResult;
using metasequoia::linux_ime::FrontendKey;
using metasequoia::linux_ime::IBusKeyDisposition;
using metasequoia::linux_ime::IBusModeToggleTracker;
using metasequoia::linux_ime::InputController;
using metasequoia::linux_ime::InputMode;
using metasequoia::linux_ime::InputOptions;
using metasequoia::linux_ime::InputSettings;
using metasequoia::linux_ime::PunctuationLock;
using metasequoia::linux_ime::PunctuationMode;
using metasequoia::linux_ime::SettingsStore;
using metasequoia::linux_ime::translate_ibus_key;
using metasequoia::linux_ime::online::AiSuggestionProvider;
using metasequoia::linux_ime::online::CurlHttpTransport;
using metasequoia::linux_ime::online::GoogleCloudProvider;
using metasequoia::linux_ime::online::HttpTimeouts;
using metasequoia::linux_ime::online::OnlineCandidateService;
using metasequoia::linux_ime::online::TranslationBackend;
using metasequoia::linux_ime::online::TranslationProvider;
using metasequoia::linux_ime::online::TranslationService;

struct _MetasequoiaEngine;

// The worker threads reach the engine only through this handle. Taking a GObject reference from a
// worker cannot be made safe: the services are stopped inside finalize, which GObject runs once the
// reference count has already reached zero, so the reference would be taken on a dying object.
// finalize clears the handle first, and the main-loop delivery drops the result when it finds the
// handle empty.
struct DeliveryHandle
{
    _MetasequoiaEngine *engine = nullptr;
};

struct _MetasequoiaEngine
{
    IBusEngine parent;
    InputController *controller = nullptr;
    IBusModeToggleTracker *mode_toggle = nullptr;
    InputMode default_mode = InputMode::Ime;
    bool show_quanpin_helpcode = true;
    bool show_shuangpin_helpcode = true;
    SettingsStore *settings_store = nullptr;
    // The settings as they were read from disk. Persisting starts from this copy so the keys the
    // engine does not own -- the utility toggles, the keybindings, the voice provider, everything
    // else the settings window writes -- survive a save triggered from the key path.
    InputSettings *settings = nullptr;
    guint settings_save_source = 0;
    // Watches config.ini so an edit made in the settings window -- a separate process -- reaches the running engine
    // instead of being overwritten by the next save of the copy above.
    GFileMonitor *settings_monitor = nullptr;
    // What config.ini looked like the last time this engine wrote it or adopted it, so a change event can be told apart
    // from the echo of the engine's own save. Zero means the file could not be stat'ed.
    gint64 settings_file_modified = 0;
    guint64 settings_file_size = 0;
    metasequoia::linux_ime::LibsecretSecretStore *secret_store = nullptr;
    OnlineCandidateService *online_service = nullptr;
    TranslationService *translation_service = nullptr;
    std::shared_ptr<DeliveryHandle> *delivery_handle = nullptr;
    metasequoia::linux_ime::OnlineSettings *online_settings = nullptr;
    std::map<std::string, std::string> *translation_glosses = nullptr;
    std::string *settings_warning = nullptr;
    IBusPropList *properties = nullptr;
    IBusProperty *mode_property = nullptr;
    IBusProperty *punctuation_property = nullptr;
    IBusProperty *character_width_property = nullptr;
    IBusProperty *scheme_menu = nullptr;
    IBusProperty *quanpin_property = nullptr;
    IBusProperty *shuangpin_property = nullptr;
    IBusProperty *wubi_property = nullptr;
    IBusProperty *japanese_property = nullptr;
};

struct _MetasequoiaEngineClass
{
    IBusEngineClass parent;
};

using MetasequoiaEngine = _MetasequoiaEngine;
using MetasequoiaEngineClass = _MetasequoiaEngineClass;

#define METASEQUOIA_TYPE_ENGINE (metasequoia_engine_get_type())
#define METASEQUOIA_ENGINE(object) (reinterpret_cast<MetasequoiaEngine *>(object))

G_DEFINE_TYPE(MetasequoiaEngine, metasequoia_engine, IBUS_TYPE_ENGINE)

IBusText *text(const char *value)
{
    return ibus_text_new_from_string(value);
}

const char *scheme_label(SchemeType scheme)
{
    switch (scheme)
    {
    case SchemeType::Quanpin:
        return "全拼";
    case SchemeType::Shuangpin:
        return "双拼";
    case SchemeType::Wubi:
        return "五笔";
    case SchemeType::JapaneseRomaji:
        return "日本語";
    }
    return "全拼";
}

IBusProperty *create_property(const char *key, IBusPropType type, const char *label, const char *tooltip,
                              IBusPropState state = PROP_STATE_UNCHECKED, IBusPropList *sub_properties = nullptr)
{
    IBusProperty *property =
        ibus_property_new(key, type, text(label), nullptr, text(tooltip), TRUE, TRUE, state, sub_properties);
    g_object_ref_sink(property);
    return property;
}

void append_property(IBusPropList *list, IBusProperty *property)
{
    ibus_prop_list_append(list, property);
    g_object_unref(property);
}

void initialize_properties(MetasequoiaEngine *engine)
{
    engine->properties = ibus_prop_list_new();
    g_object_ref_sink(engine->properties);

    engine->mode_property =
        create_property("InputMode", PROP_TYPE_TOGGLE, "中", "切换中文/英文（Shift）", PROP_STATE_CHECKED);
    append_property(engine->properties, engine->mode_property);

    engine->punctuation_property =
        create_property("Punctuation", PROP_TYPE_TOGGLE, "中标", "切换中文/英文标点（Ctrl+.）", PROP_STATE_CHECKED);
    append_property(engine->properties, engine->punctuation_property);

    engine->character_width_property =
        create_property("CharacterWidth", PROP_TYPE_TOGGLE, "半", "切换全角/半角（Ctrl+Shift+Space）");
    append_property(engine->properties, engine->character_width_property);

    IBusPropList *schemes = ibus_prop_list_new();
    g_object_ref_sink(schemes);
    engine->quanpin_property =
        create_property("Scheme.Quanpin", PROP_TYPE_RADIO, "全拼", "使用全拼", PROP_STATE_CHECKED);
    engine->shuangpin_property = create_property("Scheme.Shuangpin", PROP_TYPE_RADIO, "双拼", "使用双拼");
    engine->wubi_property = create_property("Scheme.Wubi", PROP_TYPE_RADIO, "五笔", "使用五笔");
    engine->japanese_property = create_property("Scheme.Japanese", PROP_TYPE_RADIO, "日本語", "使用日语罗马字输入");
    append_property(schemes, engine->quanpin_property);
    append_property(schemes, engine->shuangpin_property);
    append_property(schemes, engine->wubi_property);
    append_property(schemes, engine->japanese_property);

    engine->scheme_menu =
        create_property("Scheme", PROP_TYPE_MENU, "全拼", "切换输入方案", PROP_STATE_UNCHECKED, schemes);
    g_object_unref(schemes);
    append_property(engine->properties, engine->scheme_menu);
}

void update_property_values(MetasequoiaEngine *engine)
{
    const bool ime_mode = engine->controller->mode() == InputMode::Ime;
    ibus_property_set_label(engine->mode_property, text(ime_mode ? "中" : "英"));
    ibus_property_set_symbol(engine->mode_property, text(ime_mode ? "中" : "英"));
    ibus_property_set_state(engine->mode_property, ime_mode ? PROP_STATE_CHECKED : PROP_STATE_UNCHECKED);

    const bool chinese_punctuation = engine->controller->punctuation_mode() == PunctuationMode::Chinese;
    ibus_property_set_label(engine->punctuation_property, text(chinese_punctuation ? "中标" : "英标"));
    ibus_property_set_symbol(engine->punctuation_property, text(chinese_punctuation ? "中标" : "英标"));
    ibus_property_set_state(engine->punctuation_property,
                            chinese_punctuation ? PROP_STATE_CHECKED : PROP_STATE_UNCHECKED);

    const bool full_width = engine->controller->character_width() == CharacterWidth::Full;
    ibus_property_set_label(engine->character_width_property, text(full_width ? "全" : "半"));
    ibus_property_set_symbol(engine->character_width_property, text(full_width ? "全" : "半"));
    ibus_property_set_state(engine->character_width_property, full_width ? PROP_STATE_CHECKED : PROP_STATE_UNCHECKED);

    const SchemeType active_scheme = engine->controller->scheme();
    ibus_property_set_label(engine->scheme_menu, text(scheme_label(active_scheme)));
    ibus_property_set_symbol(engine->scheme_menu, text(scheme_label(active_scheme)));
    ibus_property_set_state(engine->quanpin_property,
                            active_scheme == SchemeType::Quanpin ? PROP_STATE_CHECKED : PROP_STATE_UNCHECKED);
    ibus_property_set_state(engine->shuangpin_property,
                            active_scheme == SchemeType::Shuangpin ? PROP_STATE_CHECKED : PROP_STATE_UNCHECKED);
    ibus_property_set_state(engine->wubi_property,
                            active_scheme == SchemeType::Wubi ? PROP_STATE_CHECKED : PROP_STATE_UNCHECKED);
    ibus_property_set_state(engine->japanese_property,
                            active_scheme == SchemeType::JapaneseRomaji ? PROP_STATE_CHECKED : PROP_STATE_UNCHECKED);
}

void sync_properties(MetasequoiaEngine *engine)
{
    update_property_values(engine);
    ibus_engine_update_property(IBUS_ENGINE(engine), engine->mode_property);
    ibus_engine_update_property(IBUS_ENGINE(engine), engine->punctuation_property);
    ibus_engine_update_property(IBUS_ENGINE(engine), engine->character_width_property);
    ibus_engine_update_property(IBUS_ENGINE(engine), engine->scheme_menu);
    ibus_engine_update_property(IBUS_ENGINE(engine), engine->quanpin_property);
    ibus_engine_update_property(IBUS_ENGINE(engine), engine->shuangpin_property);
    ibus_engine_update_property(IBUS_ENGINE(engine), engine->wubi_property);
    ibus_engine_update_property(IBUS_ENGINE(engine), engine->japanese_property);
}

void show_settings_warning(MetasequoiaEngine *engine)
{
    if (engine->settings_warning->empty())
    {
        ibus_engine_hide_auxiliary_text(IBUS_ENGINE(engine));
        return;
    }
    ibus_engine_update_auxiliary_text(IBUS_ENGINE(engine), text(engine->settings_warning->c_str()), TRUE);
}

// Only dictionary candidates carry a helpcode. Emoji, kaomoji, English words and quick phrases are
// matched by something other than a pinyin reading, and a kaomoji containing a Chinese character
// would otherwise pick up a hint for that one character.
bool has_helpcode_hint(const WordItem &candidate)
{
    return candidate.source == CandidateSource::Database || candidate.source == CandidateSource::UserDatabase;
}

// A hint is only shown when the scheme in use has helpcodes enabled and its display switch is on,
// which is how the two Windows settings compose.
bool show_helpcode_hint(const MetasequoiaEngine *engine)
{
    switch (engine->controller->scheme())
    {
    case SchemeType::Quanpin:
        return engine->show_quanpin_helpcode && engine->controller->quanpin_helpcode_enabled();
    case SchemeType::Shuangpin:
        return engine->show_shuangpin_helpcode && engine->controller->shuangpin_helpcode_enabled();
    default:
        return false;
    }
}

void update_lookup_table(MetasequoiaEngine *engine);
void refresh_translation(MetasequoiaEngine *engine);
std::string preceding_context(IBusEngine *engine);

void refresh_online(MetasequoiaEngine *engine)
{
    if (!engine->online_service)
    {
        return;
    }
    const auto request = engine->controller->online_request();
    if (!request.has_value())
    {
        engine->online_service->clear();
        return;
    }
    auto online_request = *request;
    online_request.query.cloud_eligible =
        online_request.query.cloud_eligible && engine->online_settings->cloud_candidates_enabled;
    // An enabled provider whose credential could not be read stays enabled in the configuration and
    // is only inactive for this run, so the request has to be gated on both.
    online_request.query.ai_eligible = online_request.query.ai_eligible && engine->online_settings->ai.enabled &&
                                       engine->online_settings->ai_credential_available;
    if (!online_request.query.cloud_eligible && !online_request.query.ai_eligible)
    {
        engine->online_service->clear();
        return;
    }
    // The AI prompt asks the model to rank by the text in front of the caret and PRIVACY.md declares
    // that context as part of what an enabled AI provider receives, so it is read here rather than
    // sent empty. Nothing is read for the cloud-only path, which never sees more than the spelling.
    std::string context;
    if (online_request.query.ai_eligible)
    {
        context = preceding_context(IBUS_ENGINE(engine));
    }
    engine->online_service->submit(std::move(online_request), std::move(context), engine->online_settings->ai);
}

struct OnlineDelivery
{
    std::shared_ptr<DeliveryHandle> handle;
    metasequoia::linux_ime::OnlineRequest request;
    std::string candidate;
    CandidateSource source = CandidateSource::CloudSuggestion;
};

gboolean deliver_online(gpointer user_data)
{
    std::unique_ptr<OnlineDelivery> delivery(static_cast<OnlineDelivery *>(user_data));
    auto *engine = delivery->handle->engine;
    // Only finalize clears the handle, and it runs on this same main loop, so a non-null engine here
    // stays alive for the rest of the callback.
    if (engine == nullptr)
    {
        return G_SOURCE_REMOVE;
    }
    if (engine->controller->apply_online_candidate(delivery->request.generation, delivery->request.query,
                                                   std::move(delivery->candidate), delivery->source))
    {
        update_lookup_table(engine);
        refresh_translation(engine);
    }
    return G_SOURCE_REMOVE;
}

void queue_online_result(std::shared_ptr<DeliveryHandle> handle, const metasequoia::linux_ime::OnlineRequest &request,
                         std::string candidate, CandidateSource source)
{
    auto delivery = std::make_unique<OnlineDelivery>();
    delivery->handle = std::move(handle);
    delivery->request = request;
    delivery->candidate = std::move(candidate);
    delivery->source = source;
    g_main_context_invoke_full(nullptr, G_PRIORITY_DEFAULT, deliver_online, delivery.release(), nullptr);
}

struct TranslationDelivery
{
    std::shared_ptr<DeliveryHandle> handle;
    std::uint64_t generation = 0;
    std::vector<std::pair<std::string, std::string>> results;
};

gboolean deliver_translation(gpointer user_data)
{
    std::unique_ptr<TranslationDelivery> delivery(static_cast<TranslationDelivery *>(user_data));
    auto *engine = delivery->handle->engine;
    if (engine == nullptr)
    {
        return G_SOURCE_REMOVE;
    }
    if (delivery->generation == engine->controller->online_generation())
    {
        engine->translation_glosses->clear();
        for (const auto &entry : delivery->results)
        {
            const auto found =
                std::find_if(engine->controller->candidates().begin(), engine->controller->candidates().end(),
                             [&](const WordItem &candidate) { return candidate.word == entry.first; });
            if (found != engine->controller->candidates().end())
            {
                (*engine->translation_glosses)[entry.first] = entry.second;
            }
        }
        update_lookup_table(engine);
    }
    return G_SOURCE_REMOVE;
}

void queue_translation_result(std::shared_ptr<DeliveryHandle> handle, std::uint64_t generation,
                              std::vector<std::pair<std::string, std::string>> results)
{
    auto delivery = std::make_unique<TranslationDelivery>();
    delivery->handle = std::move(handle);
    delivery->generation = generation;
    delivery->results = std::move(results);
    g_main_context_invoke_full(nullptr, G_PRIORITY_DEFAULT, deliver_translation, delivery.release(), nullptr);
}

// Long enough to collapse a burst -- a held Ctrl+. auto-repeating, or a user flipping modes back and
// forth -- into a single write, short enough that a toggle is on disk well before the engine goes
// away.
constexpr guint kSettingsSaveDelayMs = 500;

struct SettingsFileStamp
{
    gint64 modified = 0;
    guint64 size = 0;
};

// Identity of the settings file as it is on disk right now. Modification time and size together, because a settings
// window save and an engine save write the same keys in the same order and can differ only in one value. A file that
// cannot be stat'ed reports a zero stamp, which never compares equal to a remembered one.
SettingsFileStamp settings_file_stamp(const std::filesystem::path &path)
{
    SettingsFileStamp stamp;
    std::error_code code;
    const auto modified = std::filesystem::last_write_time(path, code);
    if (code)
    {
        return stamp;
    }
    const auto size = std::filesystem::file_size(path, code);
    if (code)
    {
        return stamp;
    }
    stamp.modified = static_cast<gint64>(modified.time_since_epoch().count());
    stamp.size = static_cast<guint64>(size);
    return stamp;
}

void remember_settings_file(MetasequoiaEngine *engine)
{
    const SettingsFileStamp stamp = settings_file_stamp(engine->settings_store->config_path());
    engine->settings_file_modified = stamp.modified;
    engine->settings_file_size = stamp.size;
}

void write_settings(MetasequoiaEngine *engine)
{
    // The plaintext overload on purpose: nothing reachable from the key path or the property menu
    // changes a credential, and SettingsStore documents the credential overload as blocking on
    // Secret Service, which must not happen while the IBus main loop is delivering keys.
    if (engine->settings_store->save(*engine->settings, engine->settings_warning))
    {
        engine->settings_warning->clear();
    }
    // Unconditionally, and after the failed save as well: the store writes a temporary file and renames it over the
    // target, so a save that reported an error may still have replaced the file, and the monitor must not read anything
    // this process produced back as somebody else's edit.
    remember_settings_file(engine);
}

gboolean flush_settings(gpointer user_data)
{
    auto *engine = static_cast<MetasequoiaEngine *>(user_data);
    engine->settings_save_source = 0;
    write_settings(engine);
    show_settings_warning(engine);
    return G_SOURCE_REMOVE;
}

// Makes a coalesced save durable at the moments the engine may never get another main-loop turn: the client has gone
// away at focus-out, the user has switched input methods at disable, and finalize is the last turn of all. Only what
// save_settings already captured is written, so this cannot persist anything the user did not ask for; it only stops
// the debounce window from being the difference between a toggle surviving a logout and not. The warning is
// deliberately not published here -- there is no client to show it to at any of these points, and focus_in shows
// whatever the last write left behind.
void flush_pending_settings(MetasequoiaEngine *engine)
{
    if (engine->settings_save_source == 0)
    {
        return;
    }
    g_source_remove(engine->settings_save_source);
    engine->settings_save_source = 0;
    write_settings(engine);
}

// Updates the retained settings with everything the controller owns and schedules the write. The
// fields the engine does not own keep the values that were loaded from disk, so a save from a mode
// toggle no longer reverts what the settings window stored.
void save_settings(MetasequoiaEngine *engine)
{
    InputSettings &settings = *engine->settings;
    settings.mode = engine->controller->mode();
    settings.scheme = engine->controller->scheme();
    settings.page_size = engine->controller->page_size();
    settings.punctuation_mode = engine->controller->punctuation_mode();
    settings.character_width = engine->controller->character_width();
    settings.comma_period_paging = engine->controller->comma_period_paging();
    settings.word_to_character = engine->controller->word_to_character();
    settings.bracket_paging = engine->controller->bracket_paging();
    settings.smart_punctuation = engine->controller->smart_punctuation();
    settings.smart_punctuation_repeat_to_chinese = engine->controller->smart_punctuation_repeat_to_chinese();
    settings.paired_punctuation = engine->controller->paired_punctuation();
    settings.preedit_style = engine->controller->preedit_style();
    settings.quanpin_helpcode_enabled = engine->controller->quanpin_helpcode_enabled();
    settings.quanpin_helpcode_schema = engine->controller->quanpin_helpcode_schema();
    settings.shuangpin_helpcode_enabled = engine->controller->shuangpin_helpcode_enabled();
    settings.shuangpin_helpcode_schema = engine->controller->shuangpin_helpcode_schema();
    settings.frequency_adjustment_mode = engine->controller->frequency_adjustment_mode();
    settings.frequency_trigger_count = engine->controller->frequency_trigger_count();
    settings.frequency_linear_step = engine->controller->frequency_linear_step();
    settings.unicode_mode_enabled = engine->controller->unicode_mode_enabled();
    settings.super_jianpin_mode_enabled = engine->controller->super_jianpin_mode_enabled();
    settings.temporary_english_mode_enabled = engine->controller->temporary_english_mode_enabled();
    settings.temporary_japanese_mode_enabled = engine->controller->temporary_japanese_mode_enabled();
    settings.mixed_english_candidates_enabled = engine->controller->mixed_english_candidates_enabled();
    settings.mixed_english_minimum_prefix = engine->controller->mixed_english_minimum_prefix();
    settings.mixed_emoji_candidates_enabled = engine->controller->mixed_emoji_candidates_enabled();
    settings.mixed_kaomoji_candidates_enabled = engine->controller->mixed_kaomoji_candidates_enabled();
    // settings.online is deliberately left as it was read from disk rather than taken from the
    // running copy in engine->online_settings, which carries the hydrated credentials and the
    // runtime record of a keyring that could not be read.

    if (engine->settings_save_source == 0)
    {
        engine->settings_save_source = g_timeout_add(kSettingsSaveDelayMs, flush_settings, engine);
    }
}

void update_preedit(MetasequoiaEngine *engine)
{
    const std::string &preedit = engine->controller->preedit();
    IBusText *text = ibus_text_new_from_string(preedit.c_str());
    ibus_engine_update_preedit_text(IBUS_ENGINE(engine), text, static_cast<guint>(preedit.size()), !preedit.empty());
}

void update_lookup_table(MetasequoiaEngine *engine)
{
    IBusLookupTable *table = ibus_lookup_table_new(static_cast<guint>(engine->controller->page_size()), 0, TRUE, FALSE);
    for (const WordItem &candidate : engine->controller->candidates())
    {
        std::string display = candidate.word;
        if (show_helpcode_hint(engine) && has_helpcode_hint(candidate))
        {
            // The controller selects the schema for the active scheme before every key, so the
            // globally selected one is already the right one to compute against here.
            display += HelpcodeUtils::compute_helpcodes(candidate.word);
        }
        if (const auto found = engine->translation_glosses->find(candidate.word);
            found != engine->translation_glosses->end())
        {
            display += " · ";
            display += found->second;
        }
        IBusText *text = ibus_text_new_from_string(display.c_str());
        ibus_lookup_table_append_candidate(table, text);
    }

    if (!engine->controller->candidates().empty())
    {
        (void)ibus_lookup_table_set_cursor_pos(table, static_cast<guint>(engine->controller->highlighted_candidate()));
    }

    const gboolean visible = engine->controller->has_composition() && !engine->controller->candidates().empty();
    ibus_engine_update_lookup_table(IBUS_ENGINE(engine), table, visible);
}

void refresh_translation(MetasequoiaEngine *engine)
{
    if (!engine->translation_service || !engine->online_settings->candidate_translations_enabled ||
        !engine->online_settings->translation_credential_available || !engine->controller->has_composition() ||
        engine->controller->candidates().empty())
    {
        if (!engine->translation_glosses->empty())
        {
            engine->translation_glosses->clear();
            update_lookup_table(engine);
        }
        if (engine->translation_service)
        {
            engine->translation_service->clear();
        }
        return;
    }

    metasequoia::linux_ime::online::TranslationRequest request;
    request.generation = engine->controller->online_generation();
    request.config.enabled = true;
    request.config.backend =
        engine->online_settings->translation_provider == metasequoia::linux_ime::TranslationProvider::DeepLX
            ? TranslationBackend::DeepLX
            : TranslationBackend::Local;
    request.config.endpoint = engine->online_settings->translation_endpoint;
    request.config.token = engine->online_settings->translation_token;
    request.config.target_language = engine->online_settings->translation_target_language;
    for (const auto &candidate : engine->controller->candidates())
    {
        request.candidates.push_back(candidate.word);
    }
    if (!engine->translation_glosses->empty())
    {
        engine->translation_glosses->clear();
        update_lookup_table(engine);
    }
    engine->translation_service->submit(std::move(request));
}

void apply_result(MetasequoiaEngine *engine, const ControllerResult &result)
{
    if (result.diagnostic.has_value())
    {
        g_warning("%s", result.diagnostic->c_str());
    }
    if (result.delete_before > 0)
    {
        const auto count = static_cast<guint>(
            std::min<std::size_t>(result.delete_before, static_cast<std::size_t>(std::numeric_limits<gint>::max())));
        ibus_engine_delete_surrounding_text(IBUS_ENGINE(engine), -static_cast<gint>(count), count);
    }
    if (result.commit.has_value())
    {
        IBusText *text = ibus_text_new_from_string(result.commit->c_str());
        ibus_engine_commit_text(IBUS_ENGINE(engine), text);
    }
    for (std::size_t index = 0; index < result.cursor_left; ++index)
    {
        ibus_engine_forward_key_event(IBUS_ENGINE(engine), IBUS_Left, 0, 0);
    }
    // Typing over a closing mark the controller inserted earlier commits nothing, so without this the
    // keystroke would be swallowed and the caret would stay behind the mark.
    for (std::size_t index = 0; index < result.cursor_right; ++index)
    {
        ibus_engine_forward_key_event(IBUS_ENGINE(engine), IBUS_Right, 0, 0);
    }

    update_preedit(engine);
    update_lookup_table(engine);
    refresh_online(engine);
    refresh_translation(engine);
}

// The client's surrounding text once it is known to be usable: present, valid UTF-8, with the caret
// past the start of it and no selection. Returns nullptr otherwise, which includes every client that
// does not implement the capability at all. On success *cursor holds the caret offset in characters.
const gchar *validated_surrounding_text(IBusEngine *engine, guint *cursor)
{
    IBusText *surrounding = nullptr;
    guint anchor = 0;
    *cursor = 0;
    ibus_engine_get_surrounding_text(engine, &surrounding, cursor, &anchor);
    if (surrounding == nullptr || *cursor == 0 || *cursor != anchor)
    {
        return nullptr;
    }

    const gchar *contents = ibus_text_get_text(surrounding);
    if (contents == nullptr || !g_utf8_validate(contents, -1, nullptr))
    {
        return nullptr;
    }
    const glong length = g_utf8_strlen(contents, -1);
    if (length < 0 || *cursor > static_cast<guint>(length))
    {
        return nullptr;
    }
    return contents;
}

std::optional<char32_t> preceding_character(IBusEngine *engine)
{
    guint cursor = 0;
    const gchar *contents = validated_surrounding_text(engine, &cursor);
    if (contents == nullptr)
    {
        return std::nullopt;
    }

    const gchar *cursor_pointer = g_utf8_offset_to_pointer(contents, cursor);
    const gchar *previous_pointer = g_utf8_find_prev_char(contents, cursor_pointer);
    if (previous_pointer == nullptr)
    {
        return std::nullopt;
    }
    const gunichar value =
        g_utf8_get_char_validated(previous_pointer, static_cast<gssize>(cursor_pointer - previous_pointer));
    if (value == static_cast<gunichar>(-1) || value == static_cast<gunichar>(-2))
    {
        return std::nullopt;
    }
    return static_cast<char32_t>(value);
}

// Bounded to the characters that plausibly disambiguate the next word: the provider caps the field
// at 2048 bytes anyway, and a shorter window keeps a whole paragraph of the user's document out of
// the request.
constexpr glong kContextCharacters = 32;

std::string preceding_context(IBusEngine *engine)
{
    guint cursor = 0;
    const gchar *contents = validated_surrounding_text(engine, &cursor);
    if (contents == nullptr)
    {
        return {};
    }

    const glong end_offset = static_cast<glong>(cursor);
    const glong start_offset = std::max<glong>(0, end_offset - kContextCharacters);
    const gchar *start = g_utf8_offset_to_pointer(contents, start_offset);
    const gchar *end = g_utf8_offset_to_pointer(contents, end_offset);
    return std::string(start, static_cast<std::size_t>(end - start));
}

void commit_for_passthrough(MetasequoiaEngine *engine)
{
    metasequoia::linux_ime::FrontendKeyEvent event;
    event.host_shortcut = true;
    const auto result = engine->controller->handle_key(event);
    // The host_shortcut branch always ends the composition, even in the cases that produce no text
    // to commit, so the preedit and the lookup table have to be resynchronised either way or the
    // client keeps drawing a composition the engine no longer has.
    apply_result(engine, result);
}

gboolean process_key_event(IBusEngine *ibus_engine, guint keyval, guint keycode, guint state)
{
    (void)keycode;
    auto *engine = METASEQUOIA_ENGINE(ibus_engine);

    if (engine->mode_toggle->observe(keyval, state))
    {
        const auto result = engine->controller->toggle_mode();
        apply_result(engine, result);
        save_settings(engine);
        sync_properties(engine);
        // The chord is the IME's own key and reporting it as unhandled would run the application's
        // binding for it as well. The Shift and Ctrl bindings instead toggle on the modifier
        // release, which the client still has to see.
        return engine->mode_toggle->chord_held() ? TRUE : FALSE;
    }
    if (engine->mode_toggle->chord_held())
    {
        // The auto-repeat presses that follow the chord toggle. They must not reach
        // translate_ibus_key, which would read the held modifiers as a host shortcut and commit the
        // composition once per repeat.
        return TRUE;
    }

    auto translation = translate_ibus_key(keyval, state);
    if (translation.disposition == IBusKeyDisposition::Ignore)
    {
        return FALSE;
    }
    if (translation.disposition == IBusKeyDisposition::Forward)
    {
        commit_for_passthrough(engine);
        return FALSE;
    }

    if (translation.event.key == FrontendKey::Punctuation)
    {
        translation.event.preceding_character = preceding_character(ibus_engine);
    }

    const auto result = engine->controller->handle_key(translation.event);
    if (!result.handled)
    {
        commit_for_passthrough(engine);
        return FALSE;
    }

    apply_result(engine, result);
    if (translation.event.key == FrontendKey::TogglePunctuation || translation.event.key == FrontendKey::ToggleWidth)
    {
        save_settings(engine);
        sync_properties(engine);
    }
    return TRUE;
}

void focus_in(IBusEngine *ibus_engine)
{
    auto *engine = METASEQUOIA_ENGINE(ibus_engine);
    engine->controller->invalidate_context();
    ibus_engine_register_properties(ibus_engine, engine->properties);
    update_preedit(engine);
    update_lookup_table(engine);
    show_settings_warning(engine);
    refresh_online(engine);
    refresh_translation(engine);
}

void focus_out(IBusEngine *ibus_engine)
{
    auto *engine = METASEQUOIA_ENGINE(ibus_engine);
    // GTK may already have switched its input context when this asynchronous
    // notification arrives. Committing here can type into the newly focused
    // widget; cancel the old composition and invalidate its pending requests.
    engine->controller->reset();
    ibus_engine_hide_preedit_text(ibus_engine);
    update_lookup_table(engine);
    refresh_online(engine);
    refresh_translation(engine);
    flush_pending_settings(engine);
}

void reset(IBusEngine *ibus_engine)
{
    auto *engine = METASEQUOIA_ENGINE(ibus_engine);
    engine->controller->reset();
    update_preedit(engine);
    update_lookup_table(engine);
    refresh_online(engine);
    refresh_translation(engine);
}

void enable(IBusEngine *ibus_engine)
{
    auto *engine = METASEQUOIA_ENGINE(ibus_engine);
    // enable fires when this input method is activated, including when the user switches back
    // to it from another one, and not on window focus changes. That is the moment the Windows
    // default_ime_mode describes, so the mode resets here rather than being carried over.
    const auto mode_result = engine->controller->set_mode(engine->default_mode);
    if (mode_result.handled)
    {
        apply_result(engine, mode_result);
        sync_properties(engine);
    }
    engine->controller->invalidate_context();
    ibus_engine_get_surrounding_text(ibus_engine, nullptr, nullptr, nullptr);
}

void disable(IBusEngine *ibus_engine)
{
    // The counterpart of enable: the user has switched to another input method, and the daemon is
    // free to tear this engine down -- or the whole process, on the way out of a session -- without
    // ever running the pending timer.
    flush_pending_settings(METASEQUOIA_ENGINE(ibus_engine));
}

void candidate_clicked(IBusEngine *ibus_engine, guint index, guint button, guint state)
{
    (void)button;
    (void)state;
    auto *engine = METASEQUOIA_ENGINE(ibus_engine);
    const auto result = engine->controller->select_page_candidate(index);
    if (result.handled)
    {
        apply_result(engine, result);
    }
}

void navigate(IBusEngine *ibus_engine, FrontendKey key)
{
    auto *engine = METASEQUOIA_ENGINE(ibus_engine);
    const auto result = engine->controller->handle_key({key});
    if (result.handled)
    {
        apply_result(engine, result);
    }
}

void page_up(IBusEngine *ibus_engine)
{
    navigate(ibus_engine, FrontendKey::PageUp);
}

void page_down(IBusEngine *ibus_engine)
{
    navigate(ibus_engine, FrontendKey::PageDown);
}

void cursor_up(IBusEngine *ibus_engine)
{
    navigate(ibus_engine, FrontendKey::Up);
}

void cursor_down(IBusEngine *ibus_engine)
{
    navigate(ibus_engine, FrontendKey::Down);
}

void property_activate(IBusEngine *ibus_engine, const gchar *property_name, guint property_state)
{
    auto *engine = METASEQUOIA_ENGINE(ibus_engine);
    ControllerResult result;
    if (std::strcmp(property_name, "InputMode") == 0)
    {
        result =
            engine->controller->set_mode(property_state == PROP_STATE_CHECKED ? InputMode::Ime : InputMode::Direct);
    }
    else if (std::strcmp(property_name, "Punctuation") == 0)
    {
        result = engine->controller->set_punctuation_mode(
            property_state == PROP_STATE_CHECKED ? PunctuationMode::Chinese : PunctuationMode::English);
    }
    else if (std::strcmp(property_name, "CharacterWidth") == 0)
    {
        result = engine->controller->set_character_width(property_state == PROP_STATE_CHECKED ? CharacterWidth::Full
                                                                                              : CharacterWidth::Half);
    }
    else if (property_state == PROP_STATE_CHECKED && std::strcmp(property_name, "Scheme.Quanpin") == 0)
    {
        result = engine->controller->switch_scheme(SchemeType::Quanpin);
    }
    else if (property_state == PROP_STATE_CHECKED && std::strcmp(property_name, "Scheme.Shuangpin") == 0)
    {
        result = engine->controller->switch_scheme(SchemeType::Shuangpin);
    }
    else if (property_state == PROP_STATE_CHECKED && std::strcmp(property_name, "Scheme.Wubi") == 0)
    {
        result = engine->controller->switch_scheme(SchemeType::Wubi);
    }
    else if (property_state == PROP_STATE_CHECKED && std::strcmp(property_name, "Scheme.Japanese") == 0)
    {
        result = engine->controller->switch_scheme(SchemeType::JapaneseRomaji);
    }
    else
    {
        return;
    }

    apply_result(engine, result);
    if (result.handled)
    {
        save_settings(engine);
    }
    sync_properties(engine);
}

// The online workers take a reference on the engine before handing a result to the main loop, so they must be
// joined while that reference can still be taken. GLib runs dispose with the count still held; by finalize it has
// already reached zero, g_object_ref returns NULL there, and the delivery would dereference a null engine.
void dispose(GObject *object)
{
    auto *engine = METASEQUOIA_ENGINE(object);
    if (engine->online_service != nullptr)
    {
        engine->online_service->stop();
    }
    if (engine->translation_service != nullptr)
    {
        engine->translation_service->stop();
    }
    G_OBJECT_CLASS(metasequoia_engine_parent_class)->dispose(object);
}

void finalize(GObject *object)
{
    auto *engine = METASEQUOIA_ENGINE(object);
    // Before anything is torn down: a worker may already be inside its callback, and a delivery it
    // queues from now on has to find an empty handle instead of a half-destroyed engine.
    if (engine->delivery_handle != nullptr)
    {
        (*engine->delivery_handle)->engine = nullptr;
    }
    // Cancelled before anything is freed: an event that is already queued would otherwise reach a callback whose
    // controller and settings are being torn down under it.
    if (engine->settings_monitor != nullptr)
    {
        g_file_monitor_cancel(engine->settings_monitor);
        g_clear_object(&engine->settings_monitor);
    }
    // A deferred save still has to reach disk, and this is the last moment the settings it would
    // write are alive.
    flush_pending_settings(engine);
    delete engine->delivery_handle;
    engine->delivery_handle = nullptr;
    delete engine->online_service;
    engine->online_service = nullptr;
    delete engine->translation_service;
    engine->translation_service = nullptr;
    delete engine->online_settings;
    engine->online_settings = nullptr;
    delete engine->translation_glosses;
    engine->translation_glosses = nullptr;
    delete engine->secret_store;
    engine->secret_store = nullptr;
    delete engine->controller;
    engine->controller = nullptr;
    delete engine->mode_toggle;
    engine->mode_toggle = nullptr;
    delete engine->settings_store;
    engine->settings_store = nullptr;
    delete engine->settings;
    engine->settings = nullptr;
    delete engine->settings_warning;
    engine->settings_warning = nullptr;
    g_clear_object(&engine->properties);
    G_OBJECT_CLASS(metasequoia_engine_parent_class)->finalize(object);
}

void metasequoia_engine_class_init(MetasequoiaEngineClass *klass)
{
    auto *engine_class = IBUS_ENGINE_CLASS(klass);
    engine_class->process_key_event = process_key_event;
    engine_class->focus_in = focus_in;
    engine_class->focus_out = focus_out;
    engine_class->enable = enable;
    engine_class->disable = disable;
    engine_class->reset = reset;
    engine_class->page_up = page_up;
    engine_class->page_down = page_down;
    engine_class->cursor_up = cursor_up;
    engine_class->cursor_down = cursor_down;
    engine_class->property_activate = property_activate;
    engine_class->candidate_clicked = candidate_clicked;
    G_OBJECT_CLASS(klass)->dispose = dispose;
    G_OBJECT_CLASS(klass)->finalize = finalize;
}

InputOptions build_input_options(const InputSettings &settings)
{
    InputOptions options;
    options.page_size = settings.page_size;
    options.punctuation_mode = settings.punctuation_mode;
    options.character_width = settings.character_width;
    options.comma_period_paging = settings.comma_period_paging;
    options.word_to_character = settings.word_to_character;
    options.bracket_paging = settings.bracket_paging;
    options.smart_punctuation = settings.smart_punctuation;
    options.smart_punctuation_repeat_to_chinese = settings.smart_punctuation_repeat_to_chinese;
    options.paired_punctuation = settings.paired_punctuation;
    options.preedit_style = settings.preedit_style;
    options.quanpin_helpcode_enabled = settings.quanpin_helpcode_enabled;
    options.quanpin_helpcode_schema = settings.quanpin_helpcode_schema;
    options.shuangpin_helpcode_enabled = settings.shuangpin_helpcode_enabled;
    options.shuangpin_helpcode_schema = settings.shuangpin_helpcode_schema;
    options.frequency_adjustment_mode = settings.frequency_adjustment_mode;
    options.frequency_trigger_count = settings.frequency_trigger_count;
    options.frequency_linear_step = settings.frequency_linear_step;
    options.local_modes.unicode = settings.unicode_mode_enabled;
    options.local_modes.super_jianpin = settings.super_jianpin_mode_enabled;
    options.local_modes.quick_phrase = settings.quick_phrase_mode_enabled;
    options.local_modes.date_time = settings.date_time_mode_enabled;
    options.local_modes.emoji = settings.emoji_mode_enabled;
    options.local_modes.kaomoji = settings.kaomoji_mode_enabled;
    options.local_modes.temporary_english = settings.temporary_english_mode_enabled;
    options.local_modes.temporary_japanese = settings.temporary_japanese_mode_enabled;
    options.english_input.mixed_candidates = settings.mixed_english_candidates_enabled;
    options.english_input.minimum_prefix = settings.mixed_english_minimum_prefix;
    options.mixed_expressive.emoji_candidates = settings.mixed_emoji_candidates_enabled;
    options.mixed_expressive.kaomoji_candidates = settings.mixed_kaomoji_candidates_enabled;
    return options;
}

// A controller that reflects `settings` exactly. InputController takes its options once, at construction, and exposes
// no way to hand it new ones, so this is what a settings reload has to go through as well.
InputController *create_controller(const InputSettings &settings)
{
    auto *controller = new InputController(settings.scheme, build_input_options(settings));
    (void)controller->set_mode(settings.mode);
    // After set_mode, which early-returns when the mode is already the default and so would not
    // apply the lock itself.
    controller->set_punctuation_lock(settings.punctuation_lock);
    if (settings.punctuation_lock == PunctuationLock::Follow)
    {
        // Follow recomputes the punctuation from the language, which is right on a language switch
        // but not here: restoring a session is not a switch, so the persisted punctuation -- a
        // manual toggle or the settings window's choice -- has to be put back.
        (void)controller->set_punctuation_mode(settings.punctuation_mode);
    }
    return controller;
}

// The settings the engine itself reads rather than the controller. mode_toggle has to exist already.
void adopt_engine_settings(MetasequoiaEngine *engine, const InputSettings &settings)
{
    engine->default_mode = settings.default_mode;
    engine->show_quanpin_helpcode = settings.show_quanpin_helpcode_in_candidates;
    engine->show_shuangpin_helpcode = settings.show_shuangpin_helpcode_in_candidates;
    engine->mode_toggle->configure(
        {settings.switch_language_shift, settings.switch_language_ctrl, settings.switch_language_ctrl_alt_space});
}

// Takes the settings file as the newer truth after somebody else has written it. Two things depend on this: the
// retained copy the engine saves from the key path is no longer stale, so a save stops reverting what the settings
// window just stored, and everything the reloaded file changes takes effect now rather than at the next login.
void reload_settings(MetasequoiaEngine *engine)
{
    std::string warning;
    // The plaintext overload for the same reason write_settings uses it: the credential-aware load blocks on Secret
    // Service, and this runs on the IBus main loop.
    InputSettings settings = engine->settings_store->load(&warning);
    if (warning.empty())
    {
        warning = metasequoia::linux_ime::describe_unusable_dictionary();
    }
    *engine->settings_warning = warning;

    // Credentials are never in config.ini -- they live in Secret Service, and the lookup that reads them is the
    // blocking call this path must not make -- so what the running configuration already holds is carried over. A token
    // is only carried while the provider it was fetched for is still the selected one; after a switch it belongs to
    // somebody else's endpoint. A copy rather than a reference: the running configuration is overwritten a few lines
    // below, and reading the old values out of the object being replaced would depend on the order of these statements.
    const metasequoia::linux_ime::OnlineSettings live = *engine->online_settings;
    metasequoia::linux_ime::OnlineSettings online = settings.online;
    // A non-empty token is the only proof this process actually holds an AI credential: the hydration happens once, at
    // startup, so AI that was off then has no token now and no worker either, and it stays inactive until the engine is
    // restarted rather than sending an unauthenticated request.
    const bool ai_credential_still_applies = !live.ai.token.empty() && online.ai.provider == live.ai.provider;
    online.ai.token = ai_credential_still_applies ? live.ai.token : std::string();
    online.ai_credential_available = ai_credential_still_applies && live.ai_credential_available;
    // Translation is the provider where having no credential is a supported configuration -- a self-hosted DeepLX
    // endpoint may accept unauthenticated requests, which is why load() only clears this flag when the credential
    // service could not be reached -- so a provider switch drops the token that belonged to the other backend without
    // also blocking the request.
    const bool translation_credential_still_applies = online.translation_provider == live.translation_provider;
    online.translation_token = translation_credential_still_applies ? live.translation_token : std::string();
    online.translation_credential_available =
        translation_credential_still_applies ? live.translation_credential_available : true;

    // The retained copy stays what is on disk, credentials excluded, exactly as it is at startup.
    *engine->settings = settings;
    *engine->online_settings = online;
    // connect_timeout and total_timeout are deliberately not re-applied: the providers capture them when they are
    // constructed, and replacing the services would mean joining workers that may be inside a request, which the main
    // loop cannot wait for. They take effect at the next start.

    // Nothing the controller reads can be changed after it exists, so a new page size, scheme or helpcode schema means
    // a new controller. The composition in progress belongs to the old one and is committed first, the way focus_out
    // ends one.
    commit_for_passthrough(engine);
    delete engine->controller;
    engine->controller = create_controller(settings);
    adopt_engine_settings(engine, settings);
    engine->translation_glosses->clear();

    update_preedit(engine);
    update_lookup_table(engine);
    sync_properties(engine);
    show_settings_warning(engine);
    refresh_online(engine);
    refresh_translation(engine);
}

// The engine writes this file itself, and every one of those writes comes back as an event; adopting them would rebuild
// the controller after every mode toggle. Which write it was is decided by the remembered stamp rather than by the
// event type, because a save arrives as a temporary file renamed over the target and surfaces as anything from CREATED
// to CHANGES_DONE_HINT depending on the monitor backend.
void settings_file_changed(GFileMonitor *monitor, GFile *file, GFile *other_file, GFileMonitorEvent event_type,
                           gpointer user_data)
{
    (void)monitor;
    (void)file;
    (void)other_file;
    (void)event_type;
    auto *engine = static_cast<MetasequoiaEngine *>(user_data);
    const SettingsFileStamp stamp = settings_file_stamp(engine->settings_store->config_path());
    if (stamp.modified == 0)
    {
        // No readable file at this instant: it was deleted, or this is the moment between a writer's temporary file and
        // its rename. There is nothing to adopt, and the rename raises another event with the file back in place.
        return;
    }
    if (stamp.modified == engine->settings_file_modified && stamp.size == engine->settings_file_size)
    {
        return;
    }
    engine->settings_file_modified = stamp.modified;
    engine->settings_file_size = stamp.size;
    reload_settings(engine);
}

void metasequoia_engine_init(MetasequoiaEngine *engine)
{
    engine->settings_store = new SettingsStore();
    engine->settings_warning = new std::string();
    engine->secret_store = new metasequoia::linux_ime::LibsecretSecretStore();
    InputSettings settings = engine->settings_store->load(engine->settings_warning);
    // Reuse the settings warning channel: without this a missing or empty
    // dictionary just yields no candidates, which looks like the input method
    // being broken rather than its data being absent.
    if (engine->settings_warning->empty())
    {
        *engine->settings_warning = metasequoia::linux_ime::describe_unusable_dictionary();
    }
    // Retained before the credential hydration below, so that what the engine writes back is what
    // was on disk. The hydrated copy additionally carries the live tokens and the runtime flags that
    // record a keyring which could not be read, and neither belongs in a save made from the key path.
    engine->settings = new InputSettings(settings);
    // Avoid synchronously waking Secret Service for the common offline configuration. If an
    // online provider is explicitly enabled, hydrate its credentials before starting workers.
    if (settings.online.ai.enabled ||
        (settings.online.candidate_translations_enabled &&
         settings.online.translation_provider == metasequoia::linux_ime::TranslationProvider::DeepLX))
    {
        settings = engine->settings_store->load(*engine->secret_store, engine->settings_warning);
    }
    engine->online_settings = new metasequoia::linux_ime::OnlineSettings(settings.online);
    engine->translation_glosses = new std::map<std::string, std::string>();
    engine->controller = create_controller(settings);
    engine->mode_toggle = new IBusModeToggleTracker();
    adopt_engine_settings(engine, settings);
    initialize_properties(engine);
    update_property_values(engine);

    // The configured deadlines reach curl only through the providers, each of which takes a copy when it is
    // constructed. A provider built without them falls back to the built-in defaults without saying so, which is what
    // left connect-timeout-ms and total-timeout-ms as settings that changed nothing.
    const HttpTimeouts timeouts{engine->online_settings->connect_timeout, engine->online_settings->total_timeout};
    const auto transport = std::make_shared<CurlHttpTransport>();
    const auto cloud_provider = std::make_shared<GoogleCloudProvider>(transport, timeouts);
    std::shared_ptr<AiSuggestionProvider> ai_provider;
    if (engine->online_settings->ai.enabled && engine->online_settings->ai_credential_available)
    {
        ai_provider = std::make_shared<AiSuggestionProvider>(transport, false, timeouts);
    }
    const auto handle = std::make_shared<DeliveryHandle>();
    handle->engine = engine;
    engine->delivery_handle = new std::shared_ptr<DeliveryHandle>(handle);
    engine->online_service = new OnlineCandidateService(
        cloud_provider,
        [handle](const metasequoia::linux_ime::OnlineRequest &request, std::string candidate, CandidateSource source) {
            queue_online_result(handle, request, std::move(candidate), source);
        },
        std::chrono::milliseconds(500), ai_provider);

    const auto translation_provider = std::make_shared<TranslationProvider>(
        metasequoia::path_to_utf8(metasequoia::data_file_path("english.db")), transport, timeouts);
    engine->translation_service = new TranslationService(
        translation_provider,
        [handle](std::uint64_t generation, std::vector<std::pair<std::string, std::string>> results) {
            queue_translation_result(handle, generation, std::move(results));
        });

    // Last, so an event arriving on the next main-loop turn finds a fully built engine. The settings window is a
    // separate process: without this watch the engine would go on writing the copy it read here over whatever the user
    // has changed since.
    remember_settings_file(engine);
    GFile *config_file = g_file_new_for_path(metasequoia::path_to_utf8(engine->settings_store->config_path()).c_str());
    GError *monitor_error = nullptr;
    engine->settings_monitor = g_file_monitor_file(config_file, G_FILE_MONITOR_NONE, nullptr, &monitor_error);
    g_object_unref(config_file);
    if (engine->settings_monitor != nullptr)
    {
        g_signal_connect(engine->settings_monitor, "changed", G_CALLBACK(settings_file_changed), engine);
    }
    else
    {
        g_warning("Unable to watch the input settings file: %s",
                  monitor_error != nullptr ? monitor_error->message : "unknown error");
    }
    g_clear_error(&monitor_error);
}

void bus_disconnected(IBusBus *bus, gpointer user_data)
{
    (void)bus;
    (void)user_data;
    ibus_quit();
}
} // namespace

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    // A packaged install leaves the dictionaries in a system directory that the
    // engine never reads. Seed the per-user directory before anything opens a
    // database, or the engine creates an empty one and produces no candidates.
    // Seeding attempts every file even after one of them fails, so what comes back is a partial success: `seeded`
    // counts what was placed and every file that could not be comes back separately. Both halves matter. Reporting only
    // the first failure would hide an unreadable helpcode set behind an unreadable database, and a non-zero count is
    // not success either -- the run that copies the main dictionary and then fails on the others leaves an installation
    // that produces Chinese candidates but no Emoji, kaomoji or English ones, with nothing said about why.
    std::vector<std::string> seed_errors;
    const std::size_t seeded = metasequoia::linux_ime::seed_user_data(&seed_errors);
    for (const std::string &failure : seed_errors)
    {
        g_warning("Unable to seed the user data directory: %s", failure.c_str());
    }
    if (!seed_errors.empty())
    {
        g_warning("The user data directory is incomplete: %zu file(s) seeded, %zu could not be.", seeded,
                  seed_errors.size());
    }
    ibus_init();
    IBusBus *bus = ibus_bus_new();
    if (bus == nullptr || !ibus_bus_is_connected(bus))
    {
        g_printerr("Unable to connect to the IBus daemon.\n");
        if (bus != nullptr)
        {
            g_object_unref(bus);
        }
        return 1;
    }

    IBusFactory *factory = ibus_factory_new(ibus_bus_get_connection(bus));
    ibus_factory_add_engine(factory, "metasequoiaime", METASEQUOIA_TYPE_ENGINE);
    g_signal_connect(bus, "disconnected", G_CALLBACK(bus_disconnected), nullptr);
    constexpr guint kRequestFlags = IBUS_BUS_NAME_FLAG_REPLACE_EXISTING | IBUS_BUS_NAME_FLAG_ALLOW_REPLACEMENT;
    if (ibus_bus_request_name(bus, "com.houko.inputmethod.MetasequoiaImeLinux", kRequestFlags) == 0)
    {
        g_printerr("Unable to register the Metasequoia IME component name.\n");
        g_object_unref(factory);
        g_object_unref(bus);
        return 1;
    }
    ibus_main();

    g_object_unref(factory);
    g_object_unref(bus);
    return 0;
}
