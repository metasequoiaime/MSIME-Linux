#include "DictionaryBootstrap.h"
#include "InputController.h"
#include "IBusKeyMapper.h"
#include "SettingsStore.h"
#include "core/data_path.h"
#include "online/HttpTransport.h"
#include "online/OnlineCandidateService.h"
#include "online/TranslationProvider.h"

#include <ibus.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>

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
using metasequoia::linux_ime::PunctuationMode;
using metasequoia::linux_ime::SettingsStore;
using metasequoia::linux_ime::translate_ibus_key;
using metasequoia::linux_ime::online::AiSuggestionProvider;
using metasequoia::linux_ime::online::CurlHttpTransport;
using metasequoia::linux_ime::online::GoogleCloudProvider;
using metasequoia::linux_ime::online::OnlineCandidateService;
using metasequoia::linux_ime::online::TranslationBackend;
using metasequoia::linux_ime::online::TranslationProvider;
using metasequoia::linux_ime::online::TranslationService;

struct _MetasequoiaEngine
{
    IBusEngine parent;
    InputController *controller = nullptr;
    IBusModeToggleTracker *mode_toggle = nullptr;
    SettingsStore *settings_store = nullptr;
    metasequoia::linux_ime::LibsecretSecretStore *secret_store = nullptr;
    OnlineCandidateService *online_service = nullptr;
    TranslationService *translation_service = nullptr;
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

void update_lookup_table(MetasequoiaEngine *engine);
void refresh_translation(MetasequoiaEngine *engine);

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
    online_request.query.ai_eligible = online_request.query.ai_eligible && engine->online_settings->ai.enabled;
    if (!online_request.query.cloud_eligible && !online_request.query.ai_eligible)
    {
        engine->online_service->clear();
        return;
    }
    engine->online_service->submit(std::move(online_request), {}, engine->online_settings->ai);
}

struct OnlineDelivery
{
    MetasequoiaEngine *engine = nullptr;
    metasequoia::linux_ime::OnlineRequest request;
    std::string candidate;
    CandidateSource source = CandidateSource::CloudSuggestion;
};

gboolean deliver_online(gpointer user_data)
{
    std::unique_ptr<OnlineDelivery> delivery(static_cast<OnlineDelivery *>(user_data));
    auto *engine = delivery->engine;
    if (engine->controller->apply_online_candidate(delivery->request.generation, delivery->request.query,
                                                   std::move(delivery->candidate), delivery->source))
    {
        update_lookup_table(engine);
        refresh_translation(engine);
    }
    g_object_unref(engine);
    return G_SOURCE_REMOVE;
}

void queue_online_result(MetasequoiaEngine *engine, const metasequoia::linux_ime::OnlineRequest &request,
                         std::string candidate, CandidateSource source)
{
    auto delivery = std::make_unique<OnlineDelivery>();
    delivery->engine = static_cast<MetasequoiaEngine *>(g_object_ref(engine));
    delivery->request = request;
    delivery->candidate = std::move(candidate);
    delivery->source = source;
    g_main_context_invoke_full(nullptr, G_PRIORITY_DEFAULT, deliver_online, delivery.release(), nullptr);
}

struct TranslationDelivery
{
    MetasequoiaEngine *engine = nullptr;
    std::uint64_t generation = 0;
    std::vector<std::pair<std::string, std::string>> results;
};

gboolean deliver_translation(gpointer user_data)
{
    std::unique_ptr<TranslationDelivery> delivery(static_cast<TranslationDelivery *>(user_data));
    auto *engine = delivery->engine;
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
    g_object_unref(engine);
    return G_SOURCE_REMOVE;
}

void queue_translation_result(MetasequoiaEngine *engine, std::uint64_t generation,
                              std::vector<std::pair<std::string, std::string>> results)
{
    auto delivery = std::make_unique<TranslationDelivery>();
    delivery->engine = static_cast<MetasequoiaEngine *>(g_object_ref(engine));
    delivery->generation = generation;
    delivery->results = std::move(results);
    g_main_context_invoke_full(nullptr, G_PRIORITY_DEFAULT, deliver_translation, delivery.release(), nullptr);
}

void save_settings(MetasequoiaEngine *engine)
{
    InputSettings settings;
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
    settings.online = *engine->online_settings;
    if (engine->secret_store && engine->settings_store->save(settings, *engine->secret_store, engine->settings_warning))
    {
        engine->settings_warning->clear();
    }
    show_settings_warning(engine);
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
        !engine->controller->has_composition() || engine->controller->candidates().empty())
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

    update_preedit(engine);
    update_lookup_table(engine);
    refresh_online(engine);
    refresh_translation(engine);
}

std::optional<char32_t> preceding_character(IBusEngine *engine)
{
    IBusText *surrounding = nullptr;
    guint cursor = 0;
    guint anchor = 0;
    ibus_engine_get_surrounding_text(engine, &surrounding, &cursor, &anchor);
    if (surrounding == nullptr || cursor == 0 || cursor != anchor)
    {
        return std::nullopt;
    }

    const gchar *contents = ibus_text_get_text(surrounding);
    if (contents == nullptr || !g_utf8_validate(contents, -1, nullptr))
    {
        return std::nullopt;
    }
    const glong length = g_utf8_strlen(contents, -1);
    if (length < 0 || cursor > static_cast<guint>(length))
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

void commit_for_passthrough(MetasequoiaEngine *engine)
{
    metasequoia::linux_ime::FrontendKeyEvent event;
    event.host_shortcut = true;
    const auto result = engine->controller->handle_key(event);
    if (result.commit.has_value())
    {
        apply_result(engine, result);
    }
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
        return FALSE;
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
    commit_for_passthrough(engine);
    engine->controller->invalidate_context();
    ibus_engine_hide_preedit_text(ibus_engine);
    update_lookup_table(engine);
    refresh_online(engine);
    refresh_translation(engine);
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
    engine->controller->invalidate_context();
    ibus_engine_get_surrounding_text(ibus_engine, nullptr, nullptr, nullptr);
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

void finalize(GObject *object)
{
    auto *engine = METASEQUOIA_ENGINE(object);
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
    engine_class->reset = reset;
    engine_class->page_up = page_up;
    engine_class->page_down = page_down;
    engine_class->cursor_up = cursor_up;
    engine_class->cursor_down = cursor_down;
    engine_class->property_activate = property_activate;
    engine_class->candidate_clicked = candidate_clicked;
    G_OBJECT_CLASS(klass)->finalize = finalize;
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
    options.local_modes.temporary_english = settings.temporary_english_mode_enabled;
    options.local_modes.temporary_japanese = settings.temporary_japanese_mode_enabled;
    options.english_input.mixed_candidates = settings.mixed_english_candidates_enabled;
    options.english_input.minimum_prefix = settings.mixed_english_minimum_prefix;
    options.mixed_expressive.emoji_candidates = settings.mixed_emoji_candidates_enabled;
    options.mixed_expressive.kaomoji_candidates = settings.mixed_kaomoji_candidates_enabled;
    engine->controller = new InputController(settings.scheme, options);
    (void)engine->controller->set_mode(settings.mode);
    engine->mode_toggle = new IBusModeToggleTracker();
    initialize_properties(engine);
    update_property_values(engine);

    const auto transport = std::make_shared<CurlHttpTransport>();
    const auto cloud_provider = std::make_shared<GoogleCloudProvider>(transport);
    std::shared_ptr<AiSuggestionProvider> ai_provider;
    if (engine->online_settings->ai.enabled)
    {
        ai_provider = std::make_shared<AiSuggestionProvider>(transport);
    }
    engine->online_service = new OnlineCandidateService(
        cloud_provider,
        [engine](const metasequoia::linux_ime::OnlineRequest &request, std::string candidate, CandidateSource source) {
            queue_online_result(engine, request, std::move(candidate), source);
        },
        std::chrono::milliseconds(500), ai_provider);

    const auto translation_provider = std::make_shared<TranslationProvider>(
        metasequoia::path_to_utf8(metasequoia::data_file_path("english.db")), transport);
    engine->translation_service = new TranslationService(
        translation_provider,
        [engine](std::uint64_t generation, std::vector<std::pair<std::string, std::string>> results) {
            queue_translation_result(engine, generation, std::move(results));
        });
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
    std::string seed_error;
    if (metasequoia::linux_ime::seed_user_data(&seed_error) == 0 && !seed_error.empty())
    {
        g_warning("Unable to seed the user data directory: %s", seed_error.c_str());
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
