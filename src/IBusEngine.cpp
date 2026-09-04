#include "core/input_session.h"

#include <ibus.h>
#include <ibuskeysyms.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <string>

namespace
{
constexpr guint kCandidatePageSize = 9;
constexpr guint kModifierMask = IBUS_CONTROL_MASK | IBUS_MOD1_MASK | IBUS_SUPER_MASK | IBUS_META_MASK;

struct _MetasequoiaEngine
{
    IBusEngine parent;
    metasequoia::InputSession *session = nullptr;
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
    const std::string &preedit = engine->session->preedit();
    IBusText *text = ibus_text_new_from_string(preedit.c_str());
    ibus_engine_update_preedit_text(IBUS_ENGINE(engine), text, static_cast<guint>(preedit.size()), !preedit.empty());
}

void update_lookup_table(MetasequoiaEngine *engine)
{
    IBusLookupTable *table = ibus_lookup_table_new(kCandidatePageSize, 0, TRUE, TRUE);
    for (const WordItem &candidate : engine->session->candidates())
    {
        IBusText *text = ibus_text_new_from_string(candidate.word.c_str());
        ibus_lookup_table_append_candidate(table, text);
    }

    const gboolean visible = engine->session->has_composition() && !engine->session->candidates().empty();
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

void commit_leading_candidate(MetasequoiaEngine *engine)
{
    if (!engine->session->has_composition())
    {
        return;
    }
    const auto result = engine->session->handle_command(metasequoia::Command::CommitCandidate);
    if (result.handled)
    {
        apply_result(engine, result);
    }
}

gboolean process_key_event(IBusEngine *ibus_engine, guint keyval, guint keycode, guint state)
{
    (void)keycode;
    auto *engine = METASEQUOIA_ENGINE(ibus_engine);

    if ((state & IBUS_RELEASE_MASK) != 0)
    {
        return FALSE;
    }

    if ((state & kModifierMask) != 0)
    {
        commit_leading_candidate(engine);
        return FALSE;
    }

    metasequoia::KeyResult result;
    switch (keyval)
    {
    case IBUS_BackSpace:
        result = engine->session->handle_command(metasequoia::Command::Backspace);
        break;
    case IBUS_Return:
    case IBUS_KP_Enter:
        result = engine->session->handle_command(metasequoia::Command::CommitRaw);
        break;
    case IBUS_Escape:
        result = engine->session->handle_command(metasequoia::Command::Cancel);
        break;
    case IBUS_space:
        result = engine->session->handle_command(metasequoia::Command::CommitCandidate);
        break;
    default:
        if (keyval >= '1' && keyval <= '9')
        {
            result = engine->session->select_candidate(keyval - '1');
        }
        else if ((keyval >= 'a' && keyval <= 'z') || (keyval >= 'A' && keyval <= 'Z') || keyval == '\'')
        {
            result = engine->session->handle_character(static_cast<char>(keyval));
        }
        break;
    }

    if (!result.handled)
    {
        commit_leading_candidate(engine);
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
    commit_leading_candidate(engine);
    ibus_engine_hide_preedit_text(ibus_engine);
    update_lookup_table(engine);
}

void reset(IBusEngine *ibus_engine)
{
    auto *engine = METASEQUOIA_ENGINE(ibus_engine);
    engine->session->handle_command(metasequoia::Command::Cancel);
    update_preedit(engine);
    update_lookup_table(engine);
}

void candidate_clicked(IBusEngine *ibus_engine, guint index, guint button, guint state)
{
    (void)button;
    (void)state;
    auto *engine = METASEQUOIA_ENGINE(ibus_engine);
    const auto result = engine->session->select_candidate(index);
    if (result.handled)
    {
        apply_result(engine, result);
    }
}

void finalize(GObject *object)
{
    auto *engine = METASEQUOIA_ENGINE(object);
    delete engine->session;
    engine->session = nullptr;
    G_OBJECT_CLASS(metasequoia_engine_parent_class)->finalize(object);
}

void metasequoia_engine_class_init(MetasequoiaEngineClass *klass)
{
    auto *engine_class = IBUS_ENGINE_CLASS(klass);
    engine_class->process_key_event = process_key_event;
    engine_class->focus_in = focus_in;
    engine_class->focus_out = focus_out;
    engine_class->reset = reset;
    engine_class->candidate_clicked = candidate_clicked;
    G_OBJECT_CLASS(klass)->finalize = finalize;
}

void metasequoia_engine_init(MetasequoiaEngine *engine)
{
    engine->session = new metasequoia::InputSession(SchemeType::Quanpin);
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
