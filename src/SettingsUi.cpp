#include "SettingsStore.h"
#include "SettingsUiModel.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>

using namespace metasequoia::linux_ime;

namespace
{
struct AppState
{
    SettingsStore store;
    SettingsUiModel model;
    GtkWidget *grid = nullptr;
    std::unordered_map<std::string, GtkWidget *> editors;
};

GtkWidget *make_editor(const SettingsUiRow &row)
{
    switch (row.control)
    {
    case SettingsControl::Boolean:
    {
        GtkWidget *button = gtk_check_button_new();
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), row.value == "true");
        return button;
    }
    case SettingsControl::Choice:
    {
        GtkWidget *combo = gtk_combo_box_text_new();
        for (const auto &choice : row.choices)
        {
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), choice.c_str());
        }
        const auto found = std::find(row.choices.begin(), row.choices.end(), row.value);
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo),
                                 found == row.choices.end() ? 0 : static_cast<gint>(found - row.choices.begin()));
        return combo;
    }
    case SettingsControl::Integer:
    {
        double minimum = 1;
        double maximum = 10;
        if (row.id == "page-size")
        {
            minimum = 3;
            maximum = 9;
        }
        else if (row.id == "connect-timeout-ms")
        {
            minimum = 100;
            maximum = 10000;
        }
        else if (row.id == "total-timeout-ms")
        {
            minimum = 500;
            maximum = 30000;
        }
        else if (row.id == "mixed-english-minimum-prefix")
        {
            minimum = 1;
            maximum = 8;
        }
        else if (row.id == "ai-candidate-limit")
        {
            minimum = 1;
            maximum = 10;
        }
        GtkWidget *spin = gtk_spin_button_new_with_range(minimum, maximum, 1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), std::strtod(row.value.c_str(), nullptr));
        return spin;
    }
    case SettingsControl::Text:
        return gtk_entry_new();
    }
    return gtk_entry_new();
}

std::string editor_value(const SettingsUiRow &row, GtkWidget *editor)
{
    switch (row.control)
    {
    case SettingsControl::Boolean:
        return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(editor)) ? "true" : "false";
    case SettingsControl::Choice:
    {
        const gchar *value = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(editor));
        std::string result = value == nullptr ? std::string{} : value;
        g_free(const_cast<gchar *>(value));
        return result;
    }
    case SettingsControl::Integer:
        return std::to_string(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(editor)));
    case SettingsControl::Text:
        return gtk_entry_get_text(GTK_ENTRY(editor));
    }
    return {};
}

void show_message(GtkWindow *parent, GtkMessageType type, const std::string &message)
{
    GtkWidget *dialog = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, type, GTK_BUTTONS_OK, "%s", message.c_str());
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

void build_form(AppState &state, GtkWidget *container)
{
    if (state.grid != nullptr)
    {
        gtk_widget_destroy(state.grid);
    }
    state.editors.clear();
    state.grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(state.grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(state.grid), 16);
    gtk_widget_set_margin_start(state.grid, 20);
    gtk_widget_set_margin_end(state.grid, 20);
    gtk_widget_set_margin_top(state.grid, 20);
    gtk_widget_set_margin_bottom(state.grid, 20);

    gint row_index = 0;
    for (const auto &row : state.model.rows())
    {
        GtkWidget *label = gtk_label_new(row.label.c_str());
        gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
        GtkWidget *editor = make_editor(row);
        if (row.control == SettingsControl::Text)
        {
            gtk_entry_set_text(GTK_ENTRY(editor), row.value.c_str());
            gtk_widget_set_hexpand(editor, TRUE);
        }
        gtk_grid_attach(GTK_GRID(state.grid), label, 0, row_index, 1, 1);
        gtk_grid_attach(GTK_GRID(state.grid), editor, 1, row_index, 1, 1);
        state.editors.emplace(row.id, editor);
        ++row_index;
    }
    gtk_container_add(GTK_CONTAINER(container), state.grid);
    gtk_widget_show_all(container);
}

void save_clicked(GtkButton *, gpointer user_data)
{
    auto &state = *static_cast<AppState *>(user_data);
    std::string error;
    const auto rows = state.model.rows();
    for (const auto &row : rows)
    {
        const auto editor = state.editors.find(row.id);
        if (editor == state.editors.end() || !state.model.set(row.id, editor_value(row, editor->second), &error))
        {
            show_message(GTK_WINDOW(gtk_widget_get_toplevel(state.grid)), GTK_MESSAGE_ERROR,
                         error.empty() ? "Unable to apply setting." : error);
            return;
        }
    }
    if (!state.store.save(state.model.settings(), &error))
    {
        show_message(GTK_WINDOW(gtk_widget_get_toplevel(state.grid)), GTK_MESSAGE_ERROR, error);
        return;
    }
    show_message(GTK_WINDOW(gtk_widget_get_toplevel(state.grid)), GTK_MESSAGE_INFO,
                 "Settings saved. Restart the IBus engine if a running session does not pick up the change.");
}

void reset_clicked(GtkButton *, gpointer user_data)
{
    auto &state = *static_cast<AppState *>(user_data);
    std::string warning;
    state.model = SettingsUiModel(state.store.load(&warning));
    build_form(state, gtk_widget_get_parent(state.grid));
    if (!warning.empty())
    {
        show_message(GTK_WINDOW(gtk_widget_get_toplevel(state.grid)), GTK_MESSAGE_WARNING, warning);
    }
}

GtkWidget *build_window(AppState &state)
{
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Metasequoia IME Settings");
    gtk_window_set_default_size(GTK_WINDOW(window), 760, 820);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(outer), scroll, TRUE, TRUE, 0);
    build_form(state, scroll);

    GtkWidget *buttons = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout(GTK_BUTTON_BOX(buttons), GTK_BUTTONBOX_END);
    GtkWidget *reset = gtk_button_new_with_label("Reload");
    GtkWidget *save = gtk_button_new_with_label("Save");
    gtk_container_add(GTK_CONTAINER(buttons), reset);
    gtk_container_add(GTK_CONTAINER(buttons), save);
    gtk_box_pack_end(GTK_BOX(outer), buttons, FALSE, FALSE, 12);
    g_signal_connect(reset, "clicked", G_CALLBACK(reset_clicked), &state);
    g_signal_connect(save, "clicked", G_CALLBACK(save_clicked), &state);
    gtk_container_add(GTK_CONTAINER(window), outer);
    return window;
}

int check_settings()
{
    SettingsStore store;
    std::string warning;
    const auto settings = store.load(&warning);
    SettingsUiModel model(settings);
    return model.rows().empty() ? 1 : 0;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc > 1 && std::string(argv[1]) == "--check")
    {
        return check_settings();
    }
    if (!gtk_init_check(&argc, &argv))
    {
        g_printerr("Unable to initialize GTK; use --check for a headless configuration check.\n");
        return 1;
    }

    AppState state;
    std::string warning;
    state.model = SettingsUiModel(state.store.load(&warning));
    GtkWidget *window = build_window(state);
    gtk_widget_show_all(window);
    if (!warning.empty())
    {
        show_message(GTK_WINDOW(window), GTK_MESSAGE_WARNING, warning);
    }
    gtk_main();
    return 0;
}
