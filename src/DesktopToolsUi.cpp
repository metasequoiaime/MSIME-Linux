#include "ClipboardHistory.h"
#include "HandwritingRecognizer.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace metasequoia::linux_ime;

namespace
{
struct HandwritingOutcome
{
    std::vector<std::string> candidates;
    std::string error;
};

struct ToolState
{
    ClipboardHistory history;
    GtkWidget *window = nullptr;
    GtkWidget *clipboard_list = nullptr;
    GtkWidget *clipboard_enabled = nullptr;
    GtkWidget *keyboard_entry = nullptr;
    GtkWidget *handwriting_canvas = nullptr;
    GtkWidget *handwriting_candidates = nullptr;
    GtkWidget *handwriting_status = nullptr;
    std::vector<HandwritingStroke> handwriting_strokes;
    bool handwriting_drawing = false;
    guint clipboard_poll_id = 0;
    guint handwriting_debounce_id = 0;
    // Recognition runs on a worker thread, so the outcome is handed back under the mutex and the worker is joined
    // before ToolState dies.
    std::thread handwriting_worker;
    std::mutex handwriting_mutex;
    HandwritingOutcome handwriting_outcome;
    bool handwriting_running = false;
    bool handwriting_restart = false;
    bool handwriting_stale = false;
    // Set from the "destroy" handler, which can run re-entrantly from inside a blocking clipboard wait; every widget
    // pointer above is dangling once it is true.
    bool shutting_down = false;
};

void show_error(GtkWidget *parent, const std::string &message)
{
    GtkWidget *dialog = gtk_message_dialog_new(parent == nullptr ? nullptr : GTK_WINDOW(parent), GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", message.c_str());
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
                             const auto *text =
                                 static_cast<const char *>(g_object_get_data(G_OBJECT(button), "clipboard-text"));
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

// Ask what the owner is offering before asking for the text: a password manager marks its content so history tools
// leave it alone. An owner that will not describe itself is treated as concealed rather than as safe, because the
// marker only ever arrives as a target name, so a failed TARGETS query is exactly the case where a marker would go
// unseen.
bool clipboard_content_is_recordable(GtkClipboard *clipboard)
{
    GdkAtom *targets = nullptr;
    gint target_count = 0;
    if (!gtk_clipboard_wait_for_targets(clipboard, &targets, &target_count))
    {
        return false;
    }
    std::vector<std::string> names;
    names.reserve(static_cast<std::size_t>(target_count));
    for (gint index = 0; index < target_count; ++index)
    {
        gchar *name = gdk_atom_name(targets[index]);
        if (name != nullptr)
        {
            names.emplace_back(name);
            g_free(name);
        }
    }
    g_free(targets);
    return !metasequoia::linux_ime::ClipboardHistory::marked_sensitive(names);
}

gboolean poll_clipboard(gpointer user_data)
{
    auto &state = *static_cast<ToolState *>(user_data);
    if (!state.history.enabled())
    {
        return G_SOURCE_CONTINUE;
    }
    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    const bool recordable = clipboard_content_is_recordable(clipboard);
    // Every gtk_clipboard_wait_* call spins a nested main loop that dispatches pending events, including the toplevel's
    // delete event, so the window and all of its children can be finalized while this callback is parked inside one.
    // g_source_remove cannot cancel a callback that is already dispatching, which leaves this the only place that can
    // stop it from touching freed widgets.
    if (state.shutting_down)
    {
        return G_SOURCE_REMOVE;
    }
    if (!recordable)
    {
        return G_SOURCE_CONTINUE;
    }
    gchar *text = gtk_clipboard_wait_for_text(clipboard);
    if (state.shutting_down)
    {
        g_free(text);
        return G_SOURCE_REMOVE;
    }
    if (text != nullptr)
    {
        // The targets query and the text query are separate round trips to whoever owns the selection right now, so
        // re-check the owner: a password manager that took ownership in between would otherwise have its secret stored
        // under the previous owner's clearance.
        const bool still_recordable = clipboard_content_is_recordable(clipboard);
        if (state.shutting_down)
        {
            g_free(text);
            return G_SOURCE_REMOVE;
        }
        std::string error;
        if (still_recordable && state.history.add(text, &error))
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
        show_error(state.window, error.empty() ? "Unable to update clipboard history." : error);
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
        show_error(state.window, error);
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
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD),
                           gtk_entry_get_text(GTK_ENTRY(state.keyboard_entry)), -1);
}

gboolean handwriting_draw(GtkWidget *widget, cairo_t *context, gpointer user_data)
{
    auto &state = *static_cast<ToolState *>(user_data);
    const double width = static_cast<double>(gtk_widget_get_allocated_width(widget));
    const double height = static_cast<double>(gtk_widget_get_allocated_height(widget));
    cairo_set_source_rgb(context, 0.95, 0.95, 0.95);
    cairo_paint(context);
    cairo_set_source_rgb(context, 0.2, 0.2, 0.2);
    cairo_set_line_width(context, 2.0);
    cairo_rectangle(context, 1, 1, std::max(0.0, width - 2.0), std::max(0.0, height - 2.0));
    cairo_stroke(context);
    cairo_set_line_width(context, 4.0);
    cairo_set_line_cap(context, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(context, CAIRO_LINE_JOIN_ROUND);
    for (const auto &stroke : state.handwriting_strokes)
    {
        if (stroke.empty())
        {
            continue;
        }
        cairo_move_to(context, stroke.front().x, stroke.front().y);
        for (std::size_t index = 1; index < stroke.size(); ++index)
        {
            cairo_line_to(context, stroke[index].x, stroke[index].y);
        }
        cairo_stroke(context);
    }
    return FALSE;
}

void refresh_handwriting_candidates(ToolState &state, const std::vector<std::string> &candidates)
{
    if (state.handwriting_candidates == nullptr)
    {
        return;
    }
    GList *children = gtk_container_get_children(GTK_CONTAINER(state.handwriting_candidates));
    for (GList *node = children; node != nullptr; node = node->next)
    {
        gtk_widget_destroy(GTK_WIDGET(node->data));
    }
    g_list_free(children);
    for (const auto &candidate : candidates)
    {
        GtkWidget *button = gtk_button_new_with_label(candidate.c_str());
        g_object_set_data_full(G_OBJECT(button), "handwriting-text", g_strdup(candidate.c_str()), g_free);
        g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton *button, gpointer) {
                             const auto *text =
                                 static_cast<const char *>(g_object_get_data(G_OBJECT(button), "handwriting-text"));
                             if (text != nullptr)
                             {
                                 gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), text, -1);
                             }
                         }),
                         nullptr);
        gtk_box_pack_start(GTK_BOX(state.handwriting_candidates), button, FALSE, FALSE, 2);
    }
    gtk_widget_show_all(state.handwriting_candidates);
}

void set_handwriting_status(ToolState &state, const std::string &message)
{
    if (state.handwriting_status == nullptr)
    {
        return;
    }
    gtk_label_set_text(GTK_LABEL(state.handwriting_status), message.c_str());
}

void cancel_handwriting_debounce(ToolState &state)
{
    if (state.handwriting_debounce_id != 0)
    {
        g_source_remove(state.handwriting_debounce_id);
        state.handwriting_debounce_id = 0;
    }
}

void start_handwriting_recognition(ToolState &state);

gboolean deliver_handwriting_result(gpointer user_data)
{
    auto &state = *static_cast<ToolState *>(user_data);
    HandwritingOutcome outcome;
    {
        const std::lock_guard<std::mutex> guard(state.handwriting_mutex);
        outcome = std::move(state.handwriting_outcome);
        state.handwriting_outcome = HandwritingOutcome{};
    }
    state.handwriting_running = false;
    if (state.handwriting_worker.joinable())
    {
        state.handwriting_worker.join();
    }
    const bool stale = state.handwriting_stale;
    state.handwriting_stale = false;
    if (state.shutting_down)
    {
        return G_SOURCE_REMOVE;
    }
    if (!stale)
    {
        refresh_handwriting_candidates(state, outcome.candidates);
        // The recognizer reports a missing tesseract install through the same channel as a failed run, so the status
        // label is where the user learns which one happened.
        set_handwriting_status(state, outcome.error.empty() ? "点击候选字即可复制到剪贴板。" : outcome.error);
    }
    if (state.handwriting_restart)
    {
        state.handwriting_restart = false;
        start_handwriting_recognition(state);
    }
    return G_SOURCE_REMOVE;
}

void start_handwriting_recognition(ToolState &state)
{
    if (state.handwriting_running)
    {
        // The strokes drawn since the running pass started are not in its bitmap, so recognize once more when it lands.
        state.handwriting_restart = true;
        return;
    }
    if (state.handwriting_canvas == nullptr || state.handwriting_strokes.empty())
    {
        set_handwriting_status(state, "请先在画板上书写。");
        return;
    }
    const int width = gtk_widget_get_allocated_width(state.handwriting_canvas);
    const int height = gtk_widget_get_allocated_height(state.handwriting_canvas);
    if (state.handwriting_worker.joinable())
    {
        state.handwriting_worker.join();
    }
    state.handwriting_running = true;
    set_handwriting_status(state, "正在识别…");
    // Recognition fork/execs tesseract and waits for it, which is hundreds of milliseconds to several seconds; on the
    // GTK main thread that freezes every tab, the clipboard poll and the window's own close button for the whole run.
    std::vector<HandwritingStroke> strokes = state.handwriting_strokes;
    state.handwriting_worker = std::thread([&state, strokes = std::move(strokes), width, height]() {
        HandwritingRecognizer recognizer;
        HandwritingOutcome outcome;
        outcome.candidates = recognizer.recognize(strokes, width, height, &outcome.error);
        {
            const std::lock_guard<std::mutex> guard(state.handwriting_mutex);
            state.handwriting_outcome = std::move(outcome);
        }
        g_idle_add(deliver_handwriting_result, &state);
    });
}

gboolean handwriting_debounce_elapsed(gpointer user_data)
{
    auto &state = *static_cast<ToolState *>(user_data);
    state.handwriting_debounce_id = 0;
    start_handwriting_recognition(state);
    return G_SOURCE_REMOVE;
}

void schedule_handwriting_recognition(ToolState &state)
{
    // Recognizing on every stroke release costs one tesseract run per stroke, so wait for the pen to settle and spend a
    // single run on the finished character.
    cancel_handwriting_debounce(state);
    state.handwriting_debounce_id = g_timeout_add(400, handwriting_debounce_elapsed, &state);
}

gboolean handwriting_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
    if (event->button != 1)
    {
        return FALSE;
    }
    auto &state = *static_cast<ToolState *>(user_data);
    state.handwriting_drawing = true;
    state.handwriting_strokes.push_back({{event->x, event->y}});
    gtk_widget_queue_draw(widget);
    return TRUE;
}

gboolean handwriting_motion(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
    auto &state = *static_cast<ToolState *>(user_data);
    if (!state.handwriting_drawing || state.handwriting_strokes.empty())
    {
        return FALSE;
    }
    auto &stroke = state.handwriting_strokes.back();
    const HandwritingPoint point{event->x, event->y};
    if (!stroke.empty() && std::abs(point.x - stroke.back().x) + std::abs(point.y - stroke.back().y) < 0.5)
    {
        return TRUE;
    }
    stroke.push_back(point);
    gtk_widget_queue_draw(widget);
    return TRUE;
}

gboolean handwriting_button_release(GtkWidget *, GdkEventButton *event, gpointer user_data)
{
    if (event->button != 1)
    {
        return FALSE;
    }
    auto &state = *static_cast<ToolState *>(user_data);
    state.handwriting_drawing = false;
    schedule_handwriting_recognition(state);
    return TRUE;
}

void handwriting_clear(GtkButton *, gpointer user_data)
{
    auto &state = *static_cast<ToolState *>(user_data);
    cancel_handwriting_debounce(state);
    state.handwriting_strokes.clear();
    state.handwriting_drawing = false;
    // A pass that started before the clear describes strokes that no longer exist, so drop its result instead of
    // listing candidates for a blank canvas.
    state.handwriting_restart = false;
    state.handwriting_stale = state.handwriting_running;
    refresh_handwriting_candidates(state, {});
    if (state.handwriting_canvas != nullptr)
    {
        gtk_widget_queue_draw(state.handwriting_canvas);
    }
    set_handwriting_status(state, "已清空，请重新书写。");
}

void handwriting_recognize_clicked(GtkButton *, gpointer user_data)
{
    auto &state = *static_cast<ToolState *>(user_data);
    cancel_handwriting_debounce(state);
    start_handwriting_recognition(state);
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
    state.handwriting_canvas = gtk_drawing_area_new();
    gtk_widget_set_size_request(state.handwriting_canvas, 400, 240);
    // A drawing area in a vertical box is allocated the full width of the page, and the recognizer refuses a canvas
    // wider than its bitmap limit, so a maximized window on a wide display would make every recognition fail. Centring
    // pins the canvas at its requested size instead of stretching it.
    gtk_widget_set_halign(state.handwriting_canvas, GTK_ALIGN_CENTER);
    gtk_widget_add_events(state.handwriting_canvas,
                          GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect(state.handwriting_canvas, "draw", G_CALLBACK(handwriting_draw), &state);
    g_signal_connect(state.handwriting_canvas, "button-press-event", G_CALLBACK(handwriting_button_press), &state);
    g_signal_connect(state.handwriting_canvas, "motion-notify-event", G_CALLBACK(handwriting_motion), &state);
    g_signal_connect(state.handwriting_canvas, "button-release-event", G_CALLBACK(handwriting_button_release), &state);
    gtk_box_pack_start(GTK_BOX(box), state.handwriting_canvas, FALSE, FALSE, 0);
    state.handwriting_status = gtk_label_new("请在画板上书写，松开鼠标后自动识别。");
    gtk_label_set_xalign(GTK_LABEL(state.handwriting_status), 0.0F);
    // Backend failures arrive here as full sentences ("install tesseract-ocr..."), which would otherwise widen the
    // window instead of wrapping.
    gtk_label_set_line_wrap(GTK_LABEL(state.handwriting_status), TRUE);
    gtk_box_pack_start(GTK_BOX(box), state.handwriting_status, FALSE, FALSE, 0);
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *recognize = gtk_button_new_with_label("Recognize");
    g_signal_connect(recognize, "clicked", G_CALLBACK(handwriting_recognize_clicked), &state);
    gtk_box_pack_start(GTK_BOX(actions), recognize, FALSE, FALSE, 0);
    GtkWidget *clear = gtk_button_new_with_label("Clear strokes");
    g_signal_connect(clear, "clicked", G_CALLBACK(handwriting_clear), &state);
    gtk_box_pack_start(GTK_BOX(actions), clear, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), actions, FALSE, FALSE, 0);
    GtkWidget *candidate_scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(candidate_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    state.handwriting_candidates = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_add(GTK_CONTAINER(candidate_scroll), state.handwriting_candidates);
    gtk_box_pack_start(GTK_BOX(box), candidate_scroll, FALSE, FALSE, 0);
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
                         // This can run from inside a blocking clipboard wait, which finalizes every child widget while
                         // the poll callback is still parked in that nested loop. Forgetting the pointers here is what
                         // turns the resumed callback into a no-op rather than a use-after-free.
                         state.shutting_down = true;
                         if (state.clipboard_poll_id != 0)
                         {
                             g_source_remove(state.clipboard_poll_id);
                             state.clipboard_poll_id = 0;
                         }
                         cancel_handwriting_debounce(state);
                         state.window = nullptr;
                         state.clipboard_list = nullptr;
                         state.clipboard_enabled = nullptr;
                         state.keyboard_entry = nullptr;
                         state.handwriting_canvas = nullptr;
                         state.handwriting_candidates = nullptr;
                         state.handwriting_status = nullptr;
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
    // The recognition worker holds a reference to state, so it has to be done before this frame goes away.
    if (state.handwriting_worker.joinable())
    {
        state.handwriting_worker.join();
    }
    return 0;
}
