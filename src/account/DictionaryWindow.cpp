#include "DictionaryWindow.h"
#include <atomic>
#include <charconv>
namespace metasequoia::linux_ime::account
{
namespace
{
struct Window
{
    std::shared_ptr<AccountSession> session;
    std::uint64_t generation;
    std::atomic<bool> closed{false};
    GtkWidget *window, *body, *status, *kind, *query, *list, *code, *word, *weight, *previous, *next;
    GtkListStore *rows;
    DictionaryPage page;
    std::string loaded_kind, loaded_query;
    bool loaded = false;
};
using Handle = std::shared_ptr<Window>;
enum class Action
{
    Search,
    Previous,
    Next,
    Add,
    Update,
    Delete
};
struct Work
{
    Handle state;
    Action action;
    std::string kind, query, message;
    int offset = 0;
    std::optional<DictionaryEntry> selected;
    DictionaryEntry replacement;
    DictionaryPage result;
    bool success = false, changed = false;
};
void refresh(Window &state)
{
    gtk_widget_set_sensitive(state.previous, state.loaded && state.page.offset > 0);
    gtk_widget_set_sensitive(state.next, state.loaded && state.page.has_more);
}
void worker(GTask *task, gpointer, gpointer data, GCancellable *)
{
    auto &work = *static_cast<Work *>(data);
    auto &state = *work.state;
    const auto cancelled = [&state] { return state.closed.load(); };
    try
    {
        if (work.action == Action::Add || work.action == Action::Update || work.action == Action::Delete)
        {
            state.session->edit_dictionary(
                state.generation, work.kind, work.selected ? work.selected->id : "",
                work.selected ? work.selected->revision : 0,
                work.action == Action::Delete ? std::nullopt : std::optional<DictionaryEntry>(work.replacement),
                cancelled);
            work.changed = true;
        }
        work.result = state.session->dictionary(state.generation, work.kind, work.query, work.offset, 50, cancelled);
        work.success = true;
    }
    catch (const Failure &error)
    {
        work.message = work.changed ? "词条已保存，但列表刷新失败。请重新搜索，勿重复提交。"
                       : error.status() == 409 ? "词条已变化或与已有词条重复，请刷新后核对。"
                       : error.status() == 401 ? "登录已失效，请关闭窗口后重新登录。"
                       : error.status() == 400 ? "词条格式不正确，请检查编码、文字和权重。"
                                               : "云词库操作未完成，请稍后重试。";
    }
    catch (const std::exception &)
    {
        work.message = "云词库暂时不可用。";
    }
    g_task_return_boolean(task, TRUE);
}
void finished(GObject *, GAsyncResult *result, gpointer)
{
    auto &work = *static_cast<Work *>(g_task_get_task_data(G_TASK(result)));
    auto &state = *work.state;
    if (state.closed.load())
        return;
    gtk_widget_set_sensitive(state.body, TRUE);
    if (work.success)
    {
        state.page = std::move(work.result);
        state.loaded_kind = work.kind;
        state.loaded_query = work.query;
        state.loaded = true;
        gtk_list_store_clear(state.rows);
        for (std::size_t index = 0; index < state.page.entries.size(); ++index)
        {
            const auto &entry = state.page.entries[index];
            GtkTreeIter row;
            gtk_list_store_append(state.rows, &row);
            gtk_list_store_set(state.rows, &row, 0, static_cast<int>(index), 1, entry.code.c_str(), 2,
                               entry.word.c_str(), 3, std::to_string(entry.weight).c_str(), -1);
        }
        work.message = "云端个人词条：第 " + std::to_string(state.page.offset / 50 + 1) + " 页，本页 " +
                       std::to_string(state.page.entries.size()) + " 条。";
    }
    else
    {
        // Stale selections cannot be reused after a failed mutation or refresh.
        state.loaded = false;
        gtk_list_store_clear(state.rows);
    }
    gtk_label_set_text(GTK_LABEL(state.status), work.message.c_str());
    refresh(state);
}
void clicked(GtkButton *button, gpointer data)
{
    const auto state = *static_cast<Handle *>(data);
    if (state->closed.load() || !gtk_widget_get_sensitive(state->body))
        return;
    Work work{};
    work.state = state;
    work.action = static_cast<Action>(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "dictionary-action")));
    work.kind = gtk_combo_box_get_active_id(GTK_COMBO_BOX(state->kind));
    work.query = gtk_entry_get_text(GTK_ENTRY(state->query));
    const bool same = state->loaded && work.kind == state->loaded_kind && work.query == state->loaded_query;
    if (work.action == Action::Previous || work.action == Action::Next)
    {
        if (!same)
        {
            gtk_label_set_text(GTK_LABEL(state->status), "搜索条件已变化，请先重新搜索。");
            return;
        }
        work.offset = state->page.offset + (work.action == Action::Next ? 50 : -50);
    }
    if (work.action == Action::Update || work.action == Action::Delete)
    {
        GtkTreeIter row;
        GtkTreeModel *model = nullptr;
        if (!same ||
            !gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(state->list)), &model, &row))
        {
            gtk_label_set_text(GTK_LABEL(state->status), "请先搜索并选择一条云词条。");
            return;
        }
        int index = -1;
        gtk_tree_model_get(model, &row, 0, &index, -1);
        if (index < 0 || static_cast<std::size_t>(index) >= state->page.entries.size())
            return;
        work.selected = state->page.entries[index];
    }
    if (work.action == Action::Add || work.action == Action::Update)
    {
        work.replacement.code = gtk_entry_get_text(GTK_ENTRY(state->code));
        work.replacement.word = gtk_entry_get_text(GTK_ENTRY(state->word));
        const std::string weight = gtk_entry_get_text(GTK_ENTRY(state->weight));
        const auto parsed = std::from_chars(weight.data(), weight.data() + weight.size(), work.replacement.weight);
        if (parsed.ec != std::errc{} || parsed.ptr != weight.data() + weight.size() || work.replacement.weight < 0)
        {
            gtk_label_set_text(GTK_LABEL(state->status), "权重须为非负整数。");
            return;
        }
    }
    if (work.action == Action::Update || work.action == Action::Delete)
    {
        const auto message = work.action == Action::Delete
                                 ? "删除云词条：“" + work.selected->word + "”？"
                                 : "将云词条“" + work.selected->word + "”修改为“" + work.replacement.word + "”，编码“" +
                                       work.replacement.code + "”，权重 " + std::to_string(work.replacement.weight) +
                                       "？";
        auto *dialog = gtk_message_dialog_new(GTK_WINDOW(state->window),
                                              GtkDialogFlags(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
                                              GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL, "%s", message.c_str());
        g_object_ref_sink(dialog);
        const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        g_object_unref(dialog);
        if (state->closed.load() || response != GTK_RESPONSE_OK)
            return;
    }
    gtk_widget_set_sensitive(state->body, FALSE);
    gtk_label_set_text(GTK_LABEL(state->status), "正在处理云词库…");
    auto *task = g_task_new(nullptr, nullptr, finished, nullptr);
    g_task_set_task_data(task, new Work(std::move(work)), [](gpointer data) { delete static_cast<Work *>(data); });
    g_task_run_in_thread(task, worker);
    g_object_unref(task);
}
void add_button(const Handle &state, GtkWidget *box, const char *label, Action action, GtkWidget **out = nullptr)
{
    auto *button = gtk_button_new_with_label(label);
    g_object_set_data(G_OBJECT(button), "dictionary-action", GINT_TO_POINTER(static_cast<int>(action)));
    g_signal_connect_data(
        button, "clicked", G_CALLBACK(clicked), new Handle(state),
        [](gpointer data, GClosure *) { delete static_cast<Handle *>(data); }, G_CONNECT_DEFAULT);
    gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
    if (out)
        *out = button;
}
} // namespace
GtkWidget *create_dictionary_window(GtkWindow *parent, std::shared_ptr<AccountSession> session,
                                    std::uint64_t generation)
{
    auto state = std::make_shared<Window>();
    state->session = std::move(session);
    state->generation = generation;
    state->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(state->window), "云端个人词库");
    gtk_window_set_default_size(GTK_WINDOW(state->window), 740, 560);
    gtk_window_set_transient_for(GTK_WINDOW(state->window), parent);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(state->window), TRUE);
    gtk_window_set_modal(GTK_WINDOW(state->window), TRUE);
    g_signal_connect_data(
        state->window, "destroy",
        G_CALLBACK(+[](GtkWidget *, gpointer data) { (*static_cast<Handle *>(data))->closed.store(true); }),
        new Handle(state), [](gpointer data, GClosure *) { delete static_cast<Handle *>(data); }, G_CONNECT_DEFAULT);
    state->body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(state->body), 16);
    gtk_container_add(GTK_CONTAINER(state->window), state->body);
    state->status = gtk_label_new("管理云端个人词条；本机词库尚未同步。请选择种类并搜索。");
    gtk_label_set_line_wrap(GTK_LABEL(state->status), TRUE);
    gtk_box_pack_start(GTK_BOX(state->body), state->status, FALSE, FALSE, 0);
    state->kind = gtk_combo_box_text_new();
    for (const auto &pair : {std::pair{"pinyin", "拼音"}, {"wubi", "五笔"}, {"english", "英文"}, {"quick", "快捷短语"}})
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->kind), pair.first, pair.second);
    gtk_combo_box_set_active(GTK_COMBO_BOX(state->kind), 0);
    gtk_box_pack_start(GTK_BOX(state->body), state->kind, FALSE, FALSE, 0);
    auto entry = [&](const char *hint) {
        auto *widget = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(widget), hint);
        gtk_box_pack_start(GTK_BOX(state->body), widget, FALSE, FALSE, 0);
        return widget;
    };
    state->query = entry("搜索编码或词条");
    auto *navigation = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(state->body), navigation, FALSE, FALSE, 0);
    add_button(state, navigation, "搜索云词条", Action::Search);
    add_button(state, navigation, "上一页", Action::Previous, &state->previous);
    add_button(state, navigation, "下一页", Action::Next, &state->next);
    state->rows = gtk_list_store_new(4, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    state->list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(state->rows));
    g_object_unref(state->rows);
    int column = 1;
    for (const char *title : {"编码", "词条", "权重"})
        gtk_tree_view_append_column(
            GTK_TREE_VIEW(state->list),
            gtk_tree_view_column_new_with_attributes(title, gtk_cell_renderer_text_new(), "text", column++, nullptr));
    auto *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_container_add(GTK_CONTAINER(scroll), state->list);
    gtk_box_pack_start(GTK_BOX(state->body), scroll, TRUE, TRUE, 0);
    state->code = entry("词条编码");
    state->word = entry("词条文字");
    state->weight = entry("词条权重");
    gtk_entry_set_text(GTK_ENTRY(state->weight), "10");
    g_signal_connect_data(
        gtk_tree_view_get_selection(GTK_TREE_VIEW(state->list)), "changed",
        G_CALLBACK(+[](GtkTreeSelection *selection, gpointer data) {
            const auto state = *static_cast<Handle *>(data);
            GtkTreeIter row;
            GtkTreeModel *model = nullptr;
            if (!gtk_tree_selection_get_selected(selection, &model, &row))
                return;
            int index = -1;
            gtk_tree_model_get(model, &row, 0, &index, -1);
            if (index < 0 || static_cast<std::size_t>(index) >= state->page.entries.size())
                return;
            const auto &entry = state->page.entries[index];
            gtk_entry_set_text(GTK_ENTRY(state->code), entry.code.c_str());
            gtk_entry_set_text(GTK_ENTRY(state->word), entry.word.c_str());
            gtk_entry_set_text(GTK_ENTRY(state->weight), std::to_string(entry.weight).c_str());
        }),
        new Handle(state), [](gpointer data, GClosure *) { delete static_cast<Handle *>(data); }, G_CONNECT_DEFAULT);
    auto *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(state->body), actions, FALSE, FALSE, 0);
    add_button(state, actions, "新增云词条", Action::Add);
    add_button(state, actions, "修改选中词条", Action::Update);
    add_button(state, actions, "删除选中词条", Action::Delete);
    refresh(*state);
    gtk_widget_show_all(state->window);
    return state->window;
}
} // namespace metasequoia::linux_ime::account
