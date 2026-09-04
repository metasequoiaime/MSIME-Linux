#include "InputController.h"
#include "IBusKeyMapper.h"

#include <ibus.h>

#include <string>

namespace
{
using metasequoia::linux_ime::FrontendKey;
using metasequoia::linux_ime::IBusKeyDisposition;
using metasequoia::linux_ime::InputController;
using metasequoia::linux_ime::translate_ibus_key;

struct _MetasequoiaEngine
{
    IBusEngine parent;
    InputController *controller = nullptr;
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

void finalize(GObject *object)
{
    auto *engine = METASEQUOIA_ENGINE(object);
    delete engine->controller;
    engine->controller = nullptr;
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
    engine_class->candidate_clicked = candidate_clicked;
    G_OBJECT_CLASS(klass)->finalize = finalize;
}

void metasequoia_engine_init(MetasequoiaEngine *engine)
{
    engine->controller = new InputController(SchemeType::Quanpin);
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
    ibus_main();

    g_object_unref(factory);
    g_object_unref(bus);
    return 0;
}
