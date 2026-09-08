#include "DictionaryWindow.h"
#include "DictionaryExport.h"
#include "SnapshotTransfer.h"
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
    GCancellable *cancelled = g_cancellable_new();
    NativeRestoreLocations native_locations;
    ~Window()
    {
        g_object_unref(cancelled);
    }
    GtkWidget *window, *body, *status, *kind, *query, *list, *code, *word, *weight, *previous, *next;
    GtkListStore *rows;
    DictionaryPage page;
    std::string loaded_kind, loaded_query;
    bool loaded = false;
    std::string import_draft, import_format = "standard";
};
using Handle = std::shared_ptr<Window>;
enum class Action
{
    Search,
    Previous,
    Next,
    Add,
    Update,
    Delete,
    Import,
    Export,
    SnapshotPrepare,
    SnapshotRestore,
    SnapshotExport,
    NativePrepare,
    NativePublish
};
struct Work
{
    Handle state;
    Action action;
    std::string kind, query, message, import_text, import_format, export_path;
    int imported = 0;
    int offset = 0;
    std::optional<DictionaryEntry> selected;
    DictionaryEntry replacement;
    DictionaryPage result;
    bool success = false, changed = false;
    std::unique_ptr<SnapshotRestoreReview> snapshot_review;
    std::unique_ptr<NativeRestoreReview> native_review;
};
void start_work(Work work);
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
        if (work.action == Action::NativePrepare)
        {
            work.native_review =
                NativeRestoreReview::prepare(state.session, state.generation, state.native_locations, state.cancelled);
            work.success = true;
            g_task_return_boolean(task, TRUE);
            return;
        }
        if (work.action == Action::NativePublish)
        {
            const auto result = work.native_review->publish(state.cancelled);
            work.success = true;
            work.message =
                result.durable ? "完整云词库已应用到本机。" : "本机词库已切换，但持久化尚未确认，请勿重复提交。";
            g_task_return_boolean(task, TRUE);
            return;
        }
        if (work.action == Action::SnapshotPrepare)
        {
            work.snapshot_review =
                SnapshotRestoreReview::prepare(state.session, state.generation, work.export_path, cancelled);
            work.success = true;
            g_task_return_boolean(task, TRUE);
            return;
        }
        if (work.action == Action::SnapshotRestore)
        {
            work.snapshot_review->restore(cancelled);
            work.changed = true;
            work.query.clear();
        }
        if (work.action == Action::Add || work.action == Action::Update || work.action == Action::Delete)
        {
            state.session->edit_dictionary(
                state.generation, work.kind, work.selected ? work.selected->id : "",
                work.selected ? work.selected->revision : 0,
                work.action == Action::Delete ? std::nullopt : std::optional<DictionaryEntry>(work.replacement),
                cancelled);
            work.changed = true;
        }
        if (work.action == Action::Import)
        {
            const auto result = work.import_format == "hans"
                                    ? state.session->import_han_dictionary(state.generation, work.import_text,
                                                                           work.replacement.weight, cancelled)
                                    : state.session->import_dictionary(state.generation, work.kind, work.import_text,
                                                                       work.import_format, cancelled);
            work.imported = result.imported;
            work.changed = true;
            work.query.clear();
        }
        if (work.action == Action::SnapshotExport)
        {
            auto snapshot = download_validated_snapshot(*state.session, state.generation, cancelled);
            save_dictionary_export(
                work.export_path, snapshot->size(),
                [&](std::size_t offset, char *data, std::size_t size) {
                    return snapshot->read(offset, data, size, cancelled);
                },
                cancelled);
            work.message = "完整云词库快照已导出。";
            work.success = true;
            g_task_return_boolean(task, TRUE);
            return;
        }
        if (work.action == Action::Export)
        {
            const auto text =
                state.session->export_dictionary(state.generation, work.kind, work.import_format, cancelled);
            save_dictionary_export(work.export_path, text, cancelled);
            work.message = "词库已导出。";
            work.success = true;
            g_task_return_boolean(task, TRUE);
            return;
        }
        work.result = state.session->dictionary(state.generation, work.kind, work.query, work.offset, 50, cancelled);
        work.success = true;
    }
    catch (const Failure &error)
    {
        if (work.action == Action::NativePrepare || work.action == Action::NativePublish)
        {
            work.message = error.cancelled() ? "操作已取消或账号已变化，请重新预览。"
                           : error.status() == 423 ? "仍有输入或词库操作未结束，请结束后重新预览。"
                           : error.status() == 409 ? "本机词库已变化，请重新预览后确认。"
                           : error.status() == 401 ? "登录已失效，请重新登录。"
                           : work.action == Action::NativePublish
                               ? "切换结果未确认，请重新连接输入法并核对，勿直接重复提交。"
                           : error.status() == 503 ? "请先启动同一用户下的最新版水杉输入法，再准备本机恢复。"
                                                   : "未能准备完整词库，请检查快照及安装的词库资源。";
        }
        else if (work.action == Action::SnapshotPrepare || work.action == Action::SnapshotRestore)
        {
            work.message = work.changed ? "云词库已恢复，但列表刷新失败。请重新搜索，勿重复提交。"
                           : error.cancelled()     ? "操作已取消。"
                           : error.status() == 409 ? "云词库已变化，请重新选择文件并核对预览。"
                           : error.status() == 401 ? "登录已失效，请关闭窗口后重新登录。"
                           : error.status() == 400 ? "快照不完整或内容不合法，请重新选择文件。"
                                                   : "快照操作未完成，请稍后重试；恢复结果不确定时请先重新下载核对。";
        }
        else
            work.message =
                (work.action == Action::Export || work.action == Action::SnapshotExport) && error.status() != 401
                    ? (error.status() == 409 ? "目标文件已存在，请选择新的文件名。"
                                             : "导出未完成，请检查保存位置或稍后重试。")
                : work.changed ? "词条已保存，但列表刷新失败。请重新搜索，勿重复提交。"
                : error.status() == 409 ? "词条已变化或与已有词条重复，请刷新后核对。"
                : error.status() == 401 ? "登录已失效，请关闭窗口后重新登录。"
                : error.status() == 400 ? "词条格式不正确，请检查编码、文字和权重。"
                                        : "云词库操作未完成，请稍后重试。";
    }
    catch (const std::exception &)
    {
        work.message = work.action == Action::NativePublish ? "切换结果未确认，请重新连接输入法并核对，勿直接重复提交。"
                       : work.action == Action::NativePrepare ? "未能准备完整词库，请检查快照及安装的词库资源。"
                                                              : "云词库暂时不可用。";
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
    if (work.action == Action::NativePublish)
    {
        gtk_label_set_text(GTK_LABEL(state.status), work.message.c_str());
        refresh(state);
        return;
    }
    if (work.action == Action::NativePrepare)
    {
        if (!work.success)
        {
            gtk_label_set_text(GTK_LABEL(state.status), work.message.c_str());
            return;
        }
        const auto &source = work.native_review->source();
        const auto message = "账号：" + work.native_review->user().display_name + "\n\n云端完整快照：个人词条 " +
                             std::to_string(source.counts[0]) + " 条，词库调整 " + std::to_string(source.counts[1]) +
                             " 条，固定候选 " + std::to_string(source.counts[2]) + " 条，使用频次记录 " +
                             std::to_string(source.counts[3]) +
                             " 条。\n\n确认后将替换本机全部个人词条、词库调整、固定候选和使用频次记录。"
                             "本机在预览期间有变化时需要重新预览。";
        auto *dialog = gtk_message_dialog_new(GTK_WINDOW(state.window), GTK_DIALOG_DESTROY_WITH_PARENT,
                                              GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "%s", message.c_str());
        g_object_ref_sink(dialog);
        gtk_window_set_title(GTK_WINDOW(dialog), "确认应用到本机");
        gtk_dialog_add_buttons(GTK_DIALOG(dialog), "取消", GTK_RESPONSE_CANCEL, "确认替换本机词库", GTK_RESPONSE_ACCEPT,
                               nullptr);
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
        const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        g_object_unref(dialog);
        if (state.closed.load())
            return;
        if (response != GTK_RESPONSE_ACCEPT)
        {
            gtk_label_set_text(GTK_LABEL(state.status), "已取消，本机词库未修改。");
            return;
        }
        work.action = Action::NativePublish;
        work.success = false;
        start_work(std::move(work));
        return;
    }
    if (work.action == Action::SnapshotPrepare)
    {
        if (!work.success)
        {
            gtk_label_set_text(GTK_LABEL(state.status), work.message.c_str());
            return;
        }
        const auto describe = [](const SnapshotEnvelope &value) {
            return "个人词条 " + std::to_string(value.counts[0]) + " 条，词库调整 " + std::to_string(value.counts[1]) +
                   " 条，固定候选 " + std::to_string(value.counts[2]) + " 条，使用频次记录 " +
                   std::to_string(value.counts[3]) + " 条";
        };
        const auto message = "账号：" + work.snapshot_review->user().display_name + "\n\n当前云端：" +
                             describe(work.snapshot_review->target()) + "\n快照内容：" +
                             describe(work.snapshot_review->source()) +
                             "\n\n确认后将替换该账号的全部云词库、固定候选和使用频次记录。";
        auto *dialog = gtk_message_dialog_new(GTK_WINDOW(state.window), GTK_DIALOG_DESTROY_WITH_PARENT,
                                              GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "%s", message.c_str());
        g_object_ref_sink(dialog);
        gtk_window_set_title(GTK_WINDOW(dialog), "确认恢复完整云词库");
        gtk_dialog_add_buttons(GTK_DIALOG(dialog), "取消", GTK_RESPONSE_CANCEL, "确认替换云词库", GTK_RESPONSE_ACCEPT,
                               nullptr);
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
        const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        g_object_unref(dialog);
        if (state.closed.load())
            return;
        if (response != GTK_RESPONSE_ACCEPT)
        {
            gtk_label_set_text(GTK_LABEL(state.status), "已取消，云词库未修改。");
            return;
        }
        work.action = Action::SnapshotRestore;
        work.success = false;
        start_work(std::move(work));
        return;
    }
    if (work.action == Action::Export || work.action == Action::SnapshotExport)
    {
        gtk_label_set_text(GTK_LABEL(state.status), work.message.c_str());
        refresh(state);
        return;
    }
    if (work.changed && (work.action == Action::Import || work.action == Action::SnapshotRestore))
    {
        state.import_draft.clear();
        gtk_entry_set_text(GTK_ENTRY(state.query), "");
    }
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
        work.message = (work.action == Action::SnapshotRestore ? "完整云词库已恢复。"
                        : work.action == Action::Import ? "已导入 " + std::to_string(work.imported) + " 条。"
                                                        : std::string{}) +
                       "云端个人词条：第 " + std::to_string(state.page.offset / 50 + 1) + " 页，本页 " +
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
    if (work.action == Action::Add || work.action == Action::Update || work.action == Action::Import)
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
    if (work.action == Action::SnapshotExport)
    {
        auto *dialog =
            gtk_file_chooser_dialog_new("导出完整快照为新文件", GTK_WINDOW(state->window), GTK_FILE_CHOOSER_ACTION_SAVE,
                                        "取消", GTK_RESPONSE_CANCEL, "导出", GTK_RESPONSE_ACCEPT, nullptr);
        g_object_ref_sink(dialog);
        gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
        gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(dialog), TRUE);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "msime-dictionary-snapshot.ndjson");
        const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
        if (!state->closed.load() && response == GTK_RESPONSE_ACCEPT)
        {
            gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
            if (path)
            {
                work.export_path = path;
                g_free(path);
            }
        }
        gtk_widget_destroy(dialog);
        g_object_unref(dialog);
        if (state->closed.load() || response != GTK_RESPONSE_ACCEPT || work.export_path.empty())
            return;
    }
    if (work.action == Action::SnapshotPrepare)
    {
        auto *dialog =
            gtk_file_chooser_dialog_new("选择完整词库快照", GTK_WINDOW(state->window), GTK_FILE_CHOOSER_ACTION_OPEN,
                                        "取消", GTK_RESPONSE_CANCEL, "校验并预览", GTK_RESPONSE_ACCEPT, nullptr);
        g_object_ref_sink(dialog);
        gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
        gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(dialog), TRUE);
        const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
        if (!state->closed.load() && response == GTK_RESPONSE_ACCEPT)
        {
            gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
            if (path)
            {
                work.export_path = path;
                g_free(path);
            }
        }
        gtk_widget_destroy(dialog);
        g_object_unref(dialog);
        if (state->closed.load() || response != GTK_RESPONSE_ACCEPT || work.export_path.empty())
            return;
    }
    if (work.action == Action::Export)
    {
        auto *dialog =
            gtk_file_chooser_dialog_new("导出词库为新文件", GTK_WINDOW(state->window), GTK_FILE_CHOOSER_ACTION_SAVE,
                                        "取消", GTK_RESPONSE_CANCEL, "导出", GTK_RESPONSE_ACCEPT, nullptr);
        gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
        gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
        g_object_ref_sink(dialog);
        gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(dialog), TRUE);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), ("dictionary-" + work.kind + ".tsv").c_str());
        auto *format = gtk_combo_box_text_new();
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(format), "standard", "标准 TSV");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(format), "windows", "Windows TSV");
        gtk_combo_box_set_active(GTK_COMBO_BOX(format), 0);
        gtk_widget_show(format);
        gtk_file_chooser_set_extra_widget(GTK_FILE_CHOOSER(dialog), format);
        const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
        if (!state->closed.load() && response == GTK_RESPONSE_ACCEPT)
        {
            auto *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
            if (path)
                work.export_path = path;
            g_free(path);
            work.import_format = gtk_combo_box_get_active_id(GTK_COMBO_BOX(format));
        }
        gtk_widget_destroy(dialog);
        g_object_unref(dialog);
        if (state->closed.load() || response != GTK_RESPONSE_ACCEPT || work.export_path.empty())
            return;
    }
    if (work.action == Action::Import)
    {
        auto *dialog = gtk_dialog_new_with_buttons("导入云端个人词库", GTK_WINDOW(state->window),
                                                   GtkDialogFlags(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
                                                   "取消", GTK_RESPONSE_CANCEL, "确认导入", GTK_RESPONSE_OK, nullptr);
        g_object_ref_sink(dialog);
        gtk_window_set_default_size(GTK_WINDOW(dialog), 620, 420);
        auto *box = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
        auto *hint = gtk_label_new("粘贴最多 500 条。标准 TSV：词条、编码、权重，以制表符分隔。\nWindows "
                                   "格式的英文/快捷短语前两列为编码、词条。中文短语每行一条，使用窗口中的权重。");
        gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
        gtk_box_pack_start(GTK_BOX(box), hint, FALSE, FALSE, 8);
        auto *format = gtk_combo_box_text_new();
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(format), "standard", "标准 TSV");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(format), "windows", "Windows TSV");
        if (work.kind == "pinyin")
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(format), "hans", "中文短语自动注音");
        if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(format), state->import_format.c_str()))
            gtk_combo_box_set_active(GTK_COMBO_BOX(format), 0);
        gtk_box_pack_start(GTK_BOX(box), format, FALSE, FALSE, 8);
        auto *text = gtk_text_view_new();
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(text)), state->import_draft.c_str(), -1);
        auto *scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_container_add(GTK_CONTAINER(scroll), text);
        gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 8);
        gtk_widget_show_all(dialog);
        const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
        if (!state->closed.load() && response == GTK_RESPONSE_OK)
        {
            GtkTextIter first, last;
            auto *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text));
            gtk_text_buffer_get_bounds(buffer, &first, &last);
            auto *value = gtk_text_buffer_get_text(buffer, &first, &last, FALSE);
            work.import_text = value;
            g_free(value);
            work.import_format = gtk_combo_box_get_active_id(GTK_COMBO_BOX(format));
            state->import_draft = work.import_text;
            state->import_format = work.import_format;
        }
        gtk_widget_destroy(dialog);
        g_object_unref(dialog);
        if (state->closed.load() || response != GTK_RESPONSE_OK)
            return;
        if (work.import_text.empty() || work.import_text.size() > 65536)
        {
            gtk_label_set_text(GTK_LABEL(state->status), "请粘贴词条内容，最多 64 KiB（含请求编码开销）。");
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
    start_work(std::move(work));
}
void start_work(Work work)
{
    const auto state = work.state;
    if (state->closed.load())
        return;
    gtk_widget_set_sensitive(state->body, FALSE);
    gtk_label_set_text(GTK_LABEL(state->status),
                       work.action == Action::NativePrepare     ? "正在下载、校验并准备本机恢复预览…"
                       : work.action == Action::NativePublish   ? "正在切换本机词库…"
                       : work.action == Action::SnapshotPrepare ? "正在下载并校验快照，准备恢复预览…"
                                                                : "正在处理云词库…");
    auto *task = g_task_new(nullptr, state->cancelled, finished, nullptr);
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
                                    std::uint64_t generation, std::optional<NativeRestoreLocations> native_locations)
{
    auto state = std::make_shared<Window>();
    state->native_locations = native_locations ? std::move(*native_locations) : installed_native_restore_locations();
    state->session = std::move(session);
    state->generation = generation;
    state->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(state->window), "云端个人词库");
    gtk_window_set_default_size(GTK_WINDOW(state->window), 740, 560);
    gtk_window_set_transient_for(GTK_WINDOW(state->window), parent);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(state->window), TRUE);
    gtk_window_set_modal(GTK_WINDOW(state->window), TRUE);
    g_signal_connect_data(
        state->window, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer data) {
            const auto &state = *static_cast<Handle *>(data);
            state->closed.store(true);
            g_cancellable_cancel(state->cancelled);
        }),
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
    add_button(state, actions, "导出词库文件", Action::Export);
    add_button(state, actions, "批量导入词条", Action::Import);
    add_button(state, actions, "新增云词条", Action::Add);
    add_button(state, actions, "修改选中词条", Action::Update);
    add_button(state, actions, "删除选中词条", Action::Delete);
    auto *snapshots = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(state->body), snapshots, FALSE, FALSE, 0);
    add_button(state, snapshots, "导出完整云词库快照", Action::SnapshotExport);
    add_button(state, snapshots, "从完整快照恢复云词库", Action::SnapshotPrepare);
    add_button(state, snapshots, "将完整云词库应用到本机", Action::NativePrepare);
    refresh(*state);
    gtk_widget_show_all(state->window);
    return state->window;
}
} // namespace metasequoia::linux_ime::account
