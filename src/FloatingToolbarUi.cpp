#include <gtk/gtk.h>
#include "SettingsStore.h"

#include <string>

namespace
{
struct ToolbarState
{
    GtkWidget *window = nullptr;
};

void launch(const char *program)
{
    gchar *argv[] = {const_cast<gchar *>(program), nullptr};
    GError *error = nullptr;
    if (!g_spawn_async(nullptr, argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, &error) &&
        error != nullptr)
    {
        g_warning("Unable to launch %s: %s", program, error->message);
        g_error_free(error);
    }
}

void launch_settings(GtkButton *, gpointer)
{
    launch("metasequoia-ime-settings");
}

void launch_tools(GtkButton *, gpointer)
{
    launch("metasequoia-ime-tools");
}

void launch_voice(GtkButton *, gpointer)
{
    gchar *argv[] = {const_cast<gchar *>("metasequoia-ime-voice"), const_cast<gchar *>("--record"),
                     const_cast<gchar *>("5"), nullptr};
    GError *error = nullptr;
    if (!g_spawn_async(nullptr, argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, &error) &&
        error != nullptr)
    {
        g_warning("Unable to start voice recording: %s", error->message);
        g_error_free(error);
    }
}

void close_toolbar(GtkButton *, gpointer user_data)
{
    auto &state = *static_cast<ToolbarState *>(user_data);
    if (state.window != nullptr)
    {
        gtk_widget_destroy(state.window);
    }
}

GtkWidget *toolbar_button(const char *label, GCallback callback, ToolbarState &state)
{
    GtkWidget *widget = gtk_button_new_with_label(label);
    gtk_widget_set_tooltip_text(widget, label);
    g_signal_connect(widget, "clicked", callback, &state);
    return widget;
}

int check_toolbar()
{
    return gtk_check_version(3, 0, 0) == nullptr ? 0 : 1;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc > 1 && std::string(argv[1]) == "--check")
    {
        return check_toolbar();
    }
    if (!gtk_init_check(&argc, &argv))
    {
        g_printerr("Unable to initialize GTK; use --check for a headless toolbar check.\n");
        return 1;
    }
    const metasequoia::linux_ime::InputSettings settings = metasequoia::linux_ime::SettingsStore().load();
    if (!settings.floating_toolbar_enabled)
    {
        g_printerr("Floating toolbar is disabled in settings.\n");
        return 0;
    }

    ToolbarState state;
    state.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(state.window), "Metasequoia IME");
    gtk_window_set_default_size(GTK_WINDOW(state.window), 420, 52);
    gtk_window_set_resizable(GTK_WINDOW(state.window), FALSE);
    gtk_window_set_keep_above(GTK_WINDOW(state.window), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(state.window), GDK_WINDOW_TYPE_HINT_UTILITY);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(state.window), TRUE);
    g_signal_connect(state.window, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer) { gtk_main_quit(); }), nullptr);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_container_add(GTK_CONTAINER(state.window), box);
    gtk_box_pack_start(GTK_BOX(box), toolbar_button("设置", G_CALLBACK(launch_settings), state), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), toolbar_button("工具", G_CALLBACK(launch_tools), state), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), toolbar_button("语音 5s", G_CALLBACK(launch_voice), state), FALSE, FALSE, 0);
    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start(GTK_BOX(box), separator, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(box), toolbar_button("隐藏", G_CALLBACK(close_toolbar), state), FALSE, FALSE, 0);
    gtk_widget_show_all(state.window);
    gtk_main();
    return 0;
}
