#include "ClipboardHistory.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace metasequoia::linux_ime;

namespace
{
struct ToolState
{
    ClipboardHistory history;
    GtkWidget *window = nullptr;
    GtkWidget *clipboard_list = nullptr;
    GtkWidget *clipboard_enabled = nullptr;
    GtkWidget *keyboard_entry = nullptr;
    GtkWidget *handwriting_status = nullptr;
    guint clipboard_poll_id = 0;
};

void show_error(GtkWindow *parent, const std::string &message)
{
    GtkWidget *dialog = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s",
                                               message.c_str());
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

void refresh_clipboard_list(ToolState &state)
{
    if (state.clipboard_list == nullptr)
    {
        return;
    }
    GList *children = gtk_container_get_children(GTK_CONTAINER(state.clipboard_list));
    for (GList *node = children; node != nullptr; node = node->next)
    {
        gtk_widget_destroy(GTK_WIDGET(node->data));
    }
    g_list_free(children);

    std::string error;
    for (const auto &item : state.history.load(&error))
    {
        GtkWidget *button = gtk_button_new_with_label(item.c_str());
        gtk_widget_set_halign(button, GTK_ALIGN_FILL);
        gtk_widget_set_tooltip_text(button, item.c_str());
        g_object_set_data_full(G_OBJECT(button), "clipboard-text", g_strdup(item.c_str()), g_free);
        g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton *button, gpointer) {
                           const auto *text = static_cast<const char *>(g_object_get_data(G_OBJECT(button), "clipboard-text"));
                           if (text != nullptr)
                           {
                               gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), text, -1);
                           }
                       }),
                       nullptr);
        gtk_box_pack_start(GTK_BOX(state.clipboard_list), button, FALSE, FALSE, 2);
    }
    gtk_widget_show_all(state.clipboard_list);
}

gboolean poll_clipboard(gpointer user_data)
{
    auto &state = *static_cast<ToolState *>(user_data);
    if (!state.history.enabled())
    {
        return G_SOURCE_CONTINUE;
    }
    gchar *text = gtk_clipboard_wait_for_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
    if (text != nullptr)
    {
        std::string error;
        if (state.history.add(text, &error))
        {
            refresh_clipboard_list(state);
        }
        g_free(text);
    }
    return G_SOURCE_CONTINUE;
}

void enabled_toggled(GtkToggleButton *button, gpointer user_data)
{
    auto &state = *static_cast<ToolState *>(user_data);
    const bool enabled = gtk_toggle_button_get_active(button);
    std::string error;
    if (!state.history.set_enabled(enabled, &error))
    {
        g_signal_handlers_block_by_func(button, reinterpret_cast<gpointer>(enabled_toggled), user_data);
        gtk_toggle_button_set_active(button, !enabled);
        g_signal_handlers_unblock_by_func(button, reinterpret_cast<gpointer>(enabled_toggled), user_data);
        show_error(GTK_WINDOW(state.window), error.empty() ? "Unable to update clipboard history." : error);
        return;
    }
    refresh_clipboard_list(state);
}

void clear_clipboard(GtkButton *, gpointer user_data)
{
    auto &state = *static_cast<ToolState *>(user_data);
    std::string error;
    if (!state.history.clear(&error))
    {
        show_error(GTK_WINDOW(state.window), error);
        return;
    }
    refresh_clipboard_list(state);
}

void keyboard_key_clicked(GtkButton *button, gpointer user_data)
{
    auto &state = *static_cast<ToolState *>(user_data);
    const char *key = gtk_button_get_label(button);
    if (key == nullptr || state.keyboard_entry == nullptr)
    {
        return;
    }
    if (std::string(key) == "Backspace")
    {
        gtk_editable_delete_text(GTK_EDITABLE(state.keyboard_entry),
                                 std::max(0, gtk_entry_get_text_length(GTK_ENTRY(state.keyboard_entry)) - 1), -1);
    }
    else
    {
        gint position = gtk_entry_get_text_length(GTK_ENTRY(state.keyboard_entry));
        gtk_editable_insert_text(GTK_EDITABLE(state.keyboard_entry), key, -1, &position);
    }
}

void copy_keyboard_text(GtkButton *, gpointer user_data)
{
    auto &state = *static_cast<ToolState *>(user_data);
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), gtk_entry_get_text(GTK_ENTRY(state.keyboard_entry)), -1);
}

gboolean handwriting_draw(GtkWidget *, cairo_t *context, gpointer)
{
    cairo_set_source_rgb(context, 0.95, 0.95, 0.95);
    cairo_paint(context);
    cairo_set_source_rgb(context, 0.2, 0.2, 0.2);
    cairo_set_line_width(context, 2.0);
    cairo_rectangle(context, 1, 1, 398, 198);
    cairo_stroke(context);
    return FALSE;
}

void handwriting_clear(GtkButton *, gpointer user_data)
{
    auto &state = *static_cast<ToolState *>(user_data);
    gtk_label_set_text(GTK_LABEL(state.handwriting_status), "画板已清空；绘制后可交给外部识别后端。");
}

GtkWidget *build_clipboard_tab(ToolState &state)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);
    state.clipboard_enabled = gtk_check_button_new_with_label("Enable clipboard history");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state.clipboard_enabled), state.history.enabled());
    g_signal_connect(state.clipboard_enabled, "toggled", G_CALLBACK(enabled_toggled), &state);
    gtk_box_pack_start(GTK_BOX(box), state.clipboard_enabled, FALSE, FALSE, 0);
    GtkWidget *clear = gtk_button_new_with_label("Clear history");
    g_signal_connect(clear, "clicked", G_CALLBACK(clear_clipboard), &state);
    gtk_box_pack_start(GTK_BOX(box), clear, FALSE, FALSE, 0);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    state.clipboard_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_add(GTK_CONTAINER(scroll), state.clipboard_list);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    refresh_clipboard_list(state);
    return box;
}

GtkWidget *build_keyboard_tab(ToolState &state)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 16);
    state.keyboard_entry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(box), state.keyboard_entry, FALSE, FALSE, 0);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 4);
    const std::vector<std::string> keys{"qwertyuiop", "asdfghjkl", "zxcvbnm", "1234567890", ".,!?"};
    gint row = 0;
    for (const auto &line : keys)
    {
        gint column = 0;
        for (const char key : line)
        {
            std::string label(1, key);
            GtkWidget *button = gtk_button_new_with_label(label.c_str());
            g_signal_connect(button, "clicked", G_CALLBACK(keyboard_key_clicked), &state);
            gtk_grid_attach(GTK_GRID(grid), button, column++, row, 1, 1);
        }
        ++row;
    }
    GtkWidget *backspace = gtk_button_new_with_label("Backspace");
    g_signal_connect(backspace, "clicked", G_CALLBACK(keyboard_key_clicked), &state);
    gtk_grid_attach(GTK_GRID(grid), backspace, 0, row, 3, 1);
    GtkWidget *copy = gtk_button_new_with_label("Copy to clipboard");
    g_signal_connect(copy, "clicked", G_CALLBACK(copy_keyboard_text), &state);
    gtk_grid_attach(GTK_GRID(grid), copy, 3, row, 4, 1);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    return box;
}

GtkWidget *build_handwriting_tab(ToolState &state)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 16);
    GtkWidget *drawing = gtk_drawing_area_new();
    gtk_widget_set_size_request(drawing, 400, 200);
    g_signal_connect(drawing, "draw", G_CALLBACK(handwriting_draw), nullptr);
    gtk_box_pack_start(GTK_BOX(box), drawing, FALSE, FALSE, 0);
    state.handwriting_status = gtk_label_new("手写板已就绪；Linux 识别后端可通过外部服务接入。");
    gtk_label_set_xalign(GTK_LABEL(state.handwriting_status), 0.0F);
    gtk_box_pack_start(GTK_BOX(box), state.handwriting_status, FALSE, FALSE, 0);
    GtkWidget *clear = gtk_button_new_with_label("Clear strokes");
    g_signal_connect(clear, "clicked", G_CALLBACK(handwriting_clear), &state);
    gtk_box_pack_start(GTK_BOX(box), clear, FALSE, FALSE, 0);
    return box;
}

int check_tools()
{
    ClipboardHistory history;
    return history.store_path().empty() ? 1 : 0;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc > 1 && std::string(argv[1]) == "--check")
    {
        return check_tools();
    }
    if (!gtk_init_check(&argc, &argv))
    {
        g_printerr("Unable to initialize GTK; use --check for a headless tools check.\n");
        return 1;
    }
    ToolState state;
    state.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(state.window), "Metasequoia IME Desktop Tools");
    gtk_window_set_default_size(GTK_WINDOW(state.window), 620, 560);
    g_signal_connect(state.window, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer user_data) {
                       auto &state = *static_cast<ToolState *>(user_data);
                       if (state.clipboard_poll_id != 0) g_source_remove(state.clipboard_poll_id);
                       gtk_main_quit();
                   }),
                   &state);
    GtkWidget *notebook = gtk_notebook_new();
    GtkWidget *clipboard = build_clipboard_tab(state);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), clipboard, gtk_label_new("Clipboard"));
    GtkWidget *keyboard = build_keyboard_tab(state);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), keyboard, gtk_label_new("Screen keyboard"));
    GtkWidget *handwriting = build_handwriting_tab(state);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), handwriting, gtk_label_new("Handwriting"));
    gtk_container_add(GTK_CONTAINER(state.window), notebook);
    state.clipboard_poll_id = g_timeout_add(500, poll_clipboard, &state);
    gtk_widget_show_all(state.window);
    gtk_main();
    return 0;
}
