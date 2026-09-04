#include "InputController.h"
#include "IBusKeyMapper.h"

#include <ibus.h>

#include <cstring>
#include <string>

namespace
{
using metasequoia::linux_ime::FrontendKey;
using metasequoia::linux_ime::IBusKeyDisposition;
using metasequoia::linux_ime::IBusModeToggleTracker;
using metasequoia::linux_ime::InputController;
using metasequoia::linux_ime::InputMode;
using metasequoia::linux_ime::translate_ibus_key;

struct _MetasequoiaEngine
{
    IBusEngine parent;
    InputController *controller = nullptr;
    IBusModeToggleTracker *mode_toggle = nullptr;
    IBusPropList *properties = nullptr;
    IBusProperty *mode_property = nullptr;
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

    engine->mode_property = create_property("InputMode", PROP_TYPE_TOGGLE, "中", "切换中文/英文（Shift）",
                                            PROP_STATE_CHECKED);
    append_property(engine->properties, engine->mode_property);

    IBusPropList *schemes = ibus_prop_list_new();
    g_object_ref_sink(schemes);
    engine->quanpin_property =
        create_property("Scheme.Quanpin", PROP_TYPE_RADIO, "全拼", "使用全拼", PROP_STATE_CHECKED);
    engine->shuangpin_property =
        create_property("Scheme.Shuangpin", PROP_TYPE_RADIO, "双拼", "使用双拼");
    engine->wubi_property = create_property("Scheme.Wubi", PROP_TYPE_RADIO, "五笔", "使用五笔");
    engine->japanese_property =
        create_property("Scheme.Japanese", PROP_TYPE_RADIO, "日本語", "使用日语罗马字输入");
    append_property(schemes, engine->quanpin_property);
    append_property(schemes, engine->shuangpin_property);
    append_property(schemes, engine->wubi_property);
    append_property(schemes, engine->japanese_property);

    engine->scheme_menu = create_property("Scheme", PROP_TYPE_MENU, "全拼", "切换输入方案",
                                          PROP_STATE_UNCHECKED, schemes);
    g_object_unref(schemes);
    append_property(engine->properties, engine->scheme_menu);
}

void sync_properties(MetasequoiaEngine *engine)
{
    const bool ime_mode = engine->controller->mode() == InputMode::Ime;
    ibus_property_set_label(engine->mode_property, text(ime_mode ? "中" : "英"));
    ibus_property_set_symbol(engine->mode_property, text(ime_mode ? "中" : "英"));
    ibus_property_set_state(engine->mode_property, ime_mode ? PROP_STATE_CHECKED : PROP_STATE_UNCHECKED);

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

    ibus_engine_update_property(IBUS_ENGINE(engine), engine->mode_property);
    ibus_engine_update_property(IBUS_ENGINE(engine), engine->scheme_menu);
    ibus_engine_update_property(IBUS_ENGINE(engine), engine->quanpin_property);
    ibus_engine_update_property(IBUS_ENGINE(engine), engine->shuangpin_property);
    ibus_engine_update_property(IBUS_ENGINE(engine), engine->wubi_property);
    ibus_engine_update_property(IBUS_ENGINE(engine), engine->japanese_property);
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
        IBusText *text = ibus_text_new_from_string(candidate.word.c_str());
        ibus_lookup_table_append_candidate(table, text);
    }

    if (!engine->controller->candidates().empty())
    {
        (void)ibus_lookup_table_set_cursor_pos(
            table, static_cast<guint>(engine->controller->highlighted_candidate()));
    }

    const gboolean visible = engine->controller->has_composition() && !engine->controller->candidates().empty();
    ibus_engine_update_lookup_table(IBUS_ENGINE(engine), table, visible);
}

void apply_result(MetasequoiaEngine *engine, const metasequoia::KeyResult &result)
{
    if (result.commit.has_value())
    {
        IBusText *text = ibus_text_new_from_string(result.commit->c_str());
        ibus_engine_commit_text(IBUS_ENGINE(engine), text);
        update_preedit(engine);
        update_lookup_table(engine);
        return;
    }

    update_preedit(engine);
    update_lookup_table(engine);
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
        apply_result(engine, engine->controller->toggle_mode());
        sync_properties(engine);
        return FALSE;
    }

    const auto translation = translate_ibus_key(keyval, state);
    if (translation.disposition == IBusKeyDisposition::Ignore)
    {
        return FALSE;
    }
    if (translation.disposition == IBusKeyDisposition::Forward)
    {
        commit_for_passthrough(engine);
        return FALSE;
    }

    const auto result = engine->controller->handle_key(translation.event);
    if (!result.handled)
    {
        commit_for_passthrough(engine);
        return FALSE;
    }

    apply_result(engine, result);
    return TRUE;
}

void focus_in(IBusEngine *ibus_engine)
{
    auto *engine = METASEQUOIA_ENGINE(ibus_engine);
    ibus_engine_register_properties(ibus_engine, engine->properties);
    update_preedit(engine);
    update_lookup_table(engine);
}

void focus_out(IBusEngine *ibus_engine)
{
    auto *engine = METASEQUOIA_ENGINE(ibus_engine);
    commit_for_passthrough(engine);
    ibus_engine_hide_preedit_text(ibus_engine);
    update_lookup_table(engine);
}

void reset(IBusEngine *ibus_engine)
{
    auto *engine = METASEQUOIA_ENGINE(ibus_engine);
    engine->controller->reset();
    update_preedit(engine);
    update_lookup_table(engine);
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
    metasequoia::KeyResult result;
    if (std::strcmp(property_name, "InputMode") == 0)
    {
        result = engine->controller->set_mode(property_state == PROP_STATE_CHECKED ? InputMode::Ime
                                                                                   : InputMode::Direct);
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
    sync_properties(engine);
}

void finalize(GObject *object)
{
    auto *engine = METASEQUOIA_ENGINE(object);
    delete engine->controller;
    engine->controller = nullptr;
    delete engine->mode_toggle;
    engine->mode_toggle = nullptr;
    g_clear_object(&engine->properties);
    G_OBJECT_CLASS(metasequoia_engine_parent_class)->finalize(object);
}

void metasequoia_engine_class_init(MetasequoiaEngineClass *klass)
{
    auto *engine_class = IBUS_ENGINE_CLASS(klass);
    engine_class->process_key_event = process_key_event;
    engine_class->focus_in = focus_in;
    engine_class->focus_out = focus_out;
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
    engine->controller = new InputController(SchemeType::Quanpin);
    engine->mode_toggle = new IBusModeToggleTracker();
    initialize_properties(engine);
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
