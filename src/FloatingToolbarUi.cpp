#include <gio/gio.h>
#include <gtk/gtk.h>
#include "FloatingToolbarUi.h"
#include "SettingsStore.h"
#include "ToolLauncher.h"

#include <string>

namespace
{
struct ToolbarState
{
    GtkWidget *window = nullptr;
    GtkWidget *voice_button = nullptr;
};

void show_message(GtkWindow *parent, GtkMessageType type, const std::string &message)
{
    GtkWidget *dialog = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, type, GTK_BUTTONS_OK, "%s", message.c_str());
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

void spawn(const char *program, const char *argument)
{
    const std::string executable = metasequoia::linux_ime::tool_path(program);
    gchar *argv[] = {const_cast<gchar *>(executable.c_str()), const_cast<gchar *>(argument), nullptr};
    GError *error = nullptr;
    if (!g_spawn_async(nullptr, argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, &error) &&
        error != nullptr)
    {
        g_warning("Unable to launch %s: %s", program, error->message);
        g_error_free(error);
    }
}

void launch(const char *program)
{
    spawn(program, nullptr);
}

void launch_settings(GtkButton *, gpointer)
{
    launch("metasequoia-ime-settings");
}

void launch_tools(GtkButton *, gpointer)
{
    launch("metasequoia-ime-tools");
}

void voice_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    auto &state = *static_cast<ToolbarState *>(user_data);
    GSubprocess *voice = G_SUBPROCESS(source);
    gtk_widget_set_sensitive(state.voice_button, TRUE);
    gchar *standard_output = nullptr;
    gchar *standard_error = nullptr;
    GError *error = nullptr;
    if (!g_subprocess_communicate_utf8_finish(voice, result, &standard_output, &standard_error, &error))
    {
        show_message(GTK_WINDOW(state.window), GTK_MESSAGE_ERROR, error == nullptr ? "语音转写失败。" : error->message);
        g_clear_error(&error);
        return;
    }
    const metasequoia::linux_ime::VoiceToolbarResult outcome = metasequoia::linux_ime::voice_toolbar_result(
        g_subprocess_get_successful(voice) != FALSE, standard_output == nullptr ? "" : standard_output,
        standard_error == nullptr ? "" : standard_error);
    g_free(standard_output);
    g_free(standard_error);
    if (outcome.copied)
    {
        GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        gtk_clipboard_set_text(clipboard, outcome.text.c_str(), -1);
        // The toolbar owns the selection only while it runs, and the hide button ends the process, so hand the
        // transcription to the clipboard manager instead of losing it on the next click.
        gtk_clipboard_set_can_store(clipboard, nullptr, 0);
        gtk_clipboard_store(clipboard);
    }
    show_message(GTK_WINDOW(state.window), outcome.copied ? GTK_MESSAGE_INFO : GTK_MESSAGE_ERROR, outcome.message);
}

void launch_voice(GtkButton *, gpointer user_data)
{
    auto &state = *static_cast<ToolbarState *>(user_data);
    const std::string executable = metasequoia::linux_ime::tool_path("metasequoia-ime-voice");
    GError *error = nullptr;
    // The transcription exists only on the child's stdout and the failure reason only on its stderr, so keep both pipes
    // and the exit status; communicating asynchronously keeps the toolbar drawing while the five seconds of audio are
    // recorded.
    GSubprocess *voice =
        g_subprocess_new(static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE),
                         &error, executable.c_str(), "--record", "5", nullptr);
    if (voice == nullptr)
    {
        show_message(GTK_WINDOW(state.window), GTK_MESSAGE_ERROR,
                     error == nullptr ? "无法启动语音识别工具。" : error->message);
        g_clear_error(&error);
        return;
    }
    gtk_widget_set_sensitive(state.voice_button, FALSE);
    g_subprocess_communicate_utf8_async(voice, nullptr, nullptr, voice_finished, &state);
    // The pending operation holds its own reference until voice_finished has run, and that callback reaches the process
    // through its source object.
    g_object_unref(voice);
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
    state.voice_button = toolbar_button("语音 5s", G_CALLBACK(launch_voice), state);
    gtk_box_pack_start(GTK_BOX(box), state.voice_button, FALSE, FALSE, 0);
    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start(GTK_BOX(box), separator, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(box), toolbar_button("隐藏", G_CALLBACK(close_toolbar), state), FALSE, FALSE, 0);
    gtk_widget_show_all(state.window);
    gtk_main();
    return 0;
}
