#include "AccountPanel.h"
#include "AccountSession.h"
#include "DictionaryWindow.h"
#include "CloudSettingsMapper.h"
#include <sstream>
#include <atomic>
#include <memory>
#include <utility>

namespace metasequoia::linux_ime::account
{
namespace
{
struct Panel
{
    std::shared_ptr<SecretStore> secrets;
    std::shared_ptr<online::HttpTransport> transport;
    BackendAccountClient client{*transport};
    AccountCredentialStore credentials{*secrets};
    Panel(std::shared_ptr<SecretStore> store, std::shared_ptr<online::HttpTransport> http)
        : secrets(std::move(store)), transport(std::move(http))
    {
    }
    AccountSession session{client, credentials};
    std::atomic<bool> closed{false};
    GtkWidget *root = nullptr, *status = nullptr, *form = nullptr, *provider = nullptr, *target = nullptr,
              *code = nullptr, *send = nullptr, *login = nullptr, *logout = nullptr, *remove = nullptr,
              *retry = nullptr, *details = nullptr, *nickname = nullptr, *identities = nullptr, *cloud = nullptr,
              *cloud_status = nullptr, *cloud_toggle = nullptr, *cloud_query = nullptr, *cloud_text = nullptr,
              *cloud_upload = nullptr, *cloud_list = nullptr;
    GtkListStore *cloud_rows = nullptr;
    SettingsSyncHooks sync;
    GtkWidget *settings_box = nullptr;
    ClipboardSnapshot cloud_snapshot;
    std::uint64_t cloud_generation = 0;
    bool cloud_loaded = false;
    SessionSnapshot snapshot;
    std::map<std::string, bool> providers;
    Profile profile;
    Challenge challenge;
    std::uint64_t challenge_generation = 0;
    bool challenge_is_link = false;
    gint64 challenge_deadline = 0;
    bool ready = false;
};
using Handle = std::shared_ptr<Panel>;
enum class Action
{
    Restore,
    Challenge,
    Login,
    Logout,
    Delete,
    Profile,
    Rename,
    ClipboardLoad,
    ClipboardToggle,
    ClipboardAdd,
    ClipboardDelete,
    ClipboardClear,
    SettingsUploadReview,
    SettingsDownloadReview,
    SettingsUpload,
    SettingsDownload
};
struct Work
{
    Handle panel;
    Action action;
    std::string provider, target, code, nickname;
    std::string query, clip_text, clip_id;
    std::string message;
    bool success = false;
    PreferencesReview review;
    SettingsFileVersion version;
    InputSettings local;
    SettingsPreview preview;
    std::string diff;
    bool has_changes = false;
};
bool settings_action(Action action)
{
    return action == Action::SettingsUploadReview || action == Action::SettingsDownloadReview ||
           action == Action::SettingsUpload || action == Action::SettingsDownload;
}
std::string display_value(const PreferenceValue &value)
{
    if (const auto *enabled = std::get_if<bool>(&value))
        return *enabled ? "开启" : "关闭";
    return std::visit(
        [](const auto &item) {
            std::ostringstream stream;
            stream << std::boolalpha << item;
            return stream.str();
        },
        value);
}
void launch(Work *work);
void render(Panel &p)
{
    const bool signed_in = p.snapshot.user.has_value();
    gtk_widget_set_visible(p.form, TRUE);
    gtk_button_set_label(GTK_BUTTON(p.login), signed_in ? "绑定登录方式" : "登录");
    gtk_widget_set_visible(p.details, signed_in);
    gtk_widget_set_visible(p.cloud, signed_in);
    gtk_widget_set_visible(p.settings_box, signed_in && bool(p.sync.store));
    gtk_widget_set_sensitive(p.cloud_toggle, p.cloud_loaded);
    gtk_widget_set_sensitive(p.cloud_upload, p.cloud_loaded && p.cloud_snapshot.enabled);
    gtk_button_set_label(GTK_BUTTON(p.cloud_toggle), p.cloud_snapshot.enabled ? "关闭云剪贴板" : "开启云剪贴板");
    const auto cloud_status = !p.cloud_loaded ? std::string("尚未加载云记录")
                              : p.cloud_snapshot.enabled
                                  ? "已开启，当前显示 " + std::to_string(p.cloud_snapshot.items.size()) + " 条"
                                  : "已关闭，云记录为空";
    gtk_label_set_text(GTK_LABEL(p.cloud_status), cloud_status.c_str());
    gtk_widget_set_visible(p.logout, signed_in);
    gtk_widget_set_visible(p.remove, signed_in);
    gtk_widget_set_sensitive(p.form, p.ready);
    gtk_widget_set_sensitive(p.login, !p.challenge.id.empty());
    gtk_widget_set_sensitive(p.provider, p.challenge.id.empty());
    gtk_widget_set_sensitive(p.target, p.challenge.id.empty());
    gtk_widget_set_visible(p.retry, !p.ready);
}
void worker(GTask *task, gpointer, gpointer data, GCancellable *)
{
    auto &w = *static_cast<Work *>(data);
    auto &p = *w.panel;
    const auto cancelled = [&p] { return p.closed.load(); };
    try
    {
        switch (w.action)
        {
        case Action::Restore:
            p.snapshot = p.session.restore();
            if (p.snapshot.user)
                p.profile = p.session.profile(p.snapshot.generation, cancelled);
            p.providers = p.client.providers(cancelled);
            p.ready = true;
            break;
        case Action::Challenge:
            p.challenge_is_link = p.snapshot.user.has_value();
            p.challenge_generation = p.snapshot.generation;
            p.challenge = p.challenge_is_link
                              ? p.session.begin_link(p.snapshot.generation, w.provider, w.target, cancelled)
                              : p.client.challenge(w.provider, w.target, {}, cancelled);
            p.challenge_deadline =
                g_get_monotonic_time() + static_cast<gint64>(p.challenge.expires_in) * G_USEC_PER_SEC;
            w.message = "验证码已发送，请在有效期内输入。";
            break;
        case Action::Login:
            if (p.challenge.id.empty() || g_get_monotonic_time() >= p.challenge_deadline ||
                p.challenge_generation != p.snapshot.generation || p.challenge_is_link != p.snapshot.user.has_value())
                throw Failure(400);
            p.snapshot = p.challenge_is_link
                             ? p.session.link(p.snapshot.generation, p.challenge.id, w.code, cancelled)
                             : p.session.login(p.snapshot.generation, p.challenge.id, w.code, cancelled);
            p.challenge = {};
            p.profile = {};
            p.profile = p.session.profile(p.snapshot.generation, cancelled);
            break;
        case Action::Profile:
            p.profile = p.session.profile(p.snapshot.generation, cancelled);
            break;
        case Action::Rename:
            p.profile = p.session.rename(p.snapshot.generation, w.nickname, cancelled);
            w.message = "昵称已保存。";
            break;
        case Action::ClipboardLoad:
            p.cloud_snapshot = p.session.clipboard(p.snapshot.generation, w.query, cancelled);
            p.cloud_generation = p.snapshot.generation;
            p.cloud_loaded = true;
            break;
        case Action::ClipboardToggle:
            p.cloud_snapshot =
                p.session.set_clipboard_enabled(p.snapshot.generation, !p.cloud_snapshot.enabled, cancelled);
            p.cloud_generation = p.snapshot.generation;
            break;
        case Action::ClipboardAdd:
            p.cloud_snapshot = p.session.add_clipboard(p.snapshot.generation, w.clip_text, cancelled);
            p.cloud_generation = p.snapshot.generation;
            break;
        case Action::ClipboardDelete:
        case Action::ClipboardClear:
            p.cloud_snapshot = p.session.delete_clipboard(p.snapshot.generation, w.clip_id, cancelled);
            p.cloud_generation = p.snapshot.generation;
            break;
        case Action::SettingsUploadReview:
        case Action::SettingsDownloadReview: {
            const auto before = p.sync.store->version();
            if (!before)
                throw Failure(409);
            std::string warning;
            w.local = p.sync.store->load(*p.secrets, &warning);
            if (!warning.empty())
                throw Failure(400);
            const auto after = p.sync.store->version();
            if (!after || !(*before == *after))
                throw Failure(409);
            w.version = *after;
            w.review = p.session.review_preferences(p.snapshot.generation, cancelled);
            if (w.action == Action::SettingsDownloadReview)
            {
                w.preview = CloudSettingsMapper::prepare_download(w.local, w.review.remote);
                for (const auto &change : w.preview.changes)
                    w.diff += CloudSettingsMapper::field_label(change.field) + "：" + display_value(change.before) +
                              " → " + display_value(change.after) + "\n";
                w.has_changes = !w.preview.changes.empty();
                if (!w.preview.unsupported_fields.empty())
                {
                    w.diff += "\n另有 " + std::to_string(w.preview.unsupported_fields.size()) +
                              " 项设置不适用于本机，将保持不变。\n";
                }
            }
            else
            {
                for (const auto &[field, value] : CloudSettingsMapper::export_settings(w.local))
                {
                    const auto old = w.review.remote.settings.find(field);
                    if (old == w.review.remote.settings.end() || old->second != value)
                    {
                        w.has_changes = true;
                        w.diff += CloudSettingsMapper::field_label(field) + "：" +
                                  (old == w.review.remote.settings.end() ? "（未设置）" : display_value(old->second)) +
                                  " → " + display_value(value) + "\n";
                    }
                }
            }
            break;
        }
        case Action::SettingsUpload:
        case Action::SettingsDownload: {
            const auto current = p.sync.store->version();
            if (!current || !(*current == w.version))
                throw Failure(409);
            if (w.action == Action::SettingsUpload)
                p.session.upload_preferences(w.review, CloudSettingsMapper::export_settings(w.local), cancelled);
            else
                p.session.apply_preferences(
                    w.review,
                    [&] {
                        if (!p.sync.store->save_if_unchanged(w.preview.settings, *p.secrets, w.version))
                            throw Failure(409);
                    },
                    cancelled);
            w.message = w.action == Action::SettingsUpload ? "本机设置已上传。" : "云端设置已保存到本机。";
            break;
        }
        case Action::Logout:
            p.session.logout(p.snapshot.generation, false, cancelled);
            break;
        case Action::Delete:
            p.session.delete_account(p.snapshot.generation, cancelled);
            break;
        }
        w.success = true;
    }
    catch (const Failure &error)
    {
        w.message = error.status() == 409 ? "本机或云端设置已变化，请重新预览。"
                    : error.status() == 400 && settings_action(w.action)
                        ? "设置不兼容或本机配置无法完整读取，请检查后重试。"
                    : error.status() == 429 ? "请求过于频繁，请稍后重试。"
                    : error.status() == 401 ? "登录凭据已失效，请重新登录。"
                                            : "操作未完成，请稍后重试。";
    }
    catch (const std::exception &)
    {
        w.message = "账号服务暂时不可用，请稍后重试。";
    }
    p.snapshot = p.session.snapshot();
    g_task_return_boolean(task, TRUE);
}
void finished(GObject *, GAsyncResult *result, gpointer)
{
    auto &w = *static_cast<Work *>(g_task_get_task_data(G_TASK(result)));
    auto &p = *w.panel;
    if (p.closed.load())
        return;
    gtk_widget_set_sensitive(p.root, TRUE);
    if (settings_action(w.action) && p.sync.busy)
        p.sync.busy(false);
    if (w.success && w.action == Action::SettingsDownload && p.sync.applied)
        p.sync.applied(w.preview.settings);
    if (w.success && (w.action == Action::SettingsUploadReview || w.action == Action::SettingsDownloadReview))
    {
        const bool upload = w.action == Action::SettingsUploadReview;
        GtkWidget *dialog = gtk_dialog_new_with_buttons(
            upload ? "上传设置差异" : "下载设置差异", GTK_WINDOW(gtk_widget_get_toplevel(p.root)),
            GtkDialogFlags(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT), "取消", GTK_RESPONSE_CANCEL,
            upload ? "确认上传" : "确认保存到本机", GTK_RESPONSE_OK, nullptr);
        gtk_window_set_default_size(GTK_WINDOW(dialog), 640, 440);
        auto *scroll = gtk_scrolled_window_new(nullptr, nullptr);
        auto *text = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(text), FALSE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text), GTK_WRAP_WORD_CHAR);
        const std::string details = (w.has_changes ? std::string{} : "设置相同，无需修改。\n") + w.diff;
        gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_OK, w.has_changes);
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(text)), details.c_str(), -1);
        gtk_container_add(GTK_CONTAINER(scroll), text);
        gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), scroll, TRUE, TRUE, 0);
        gtk_widget_show_all(dialog);
        g_object_ref_sink(dialog);
        const int response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        g_object_unref(dialog);
        if (p.closed.load())
            return;
        if (w.has_changes && response == GTK_RESPONSE_OK && (!p.sync.allow || p.sync.allow()))
        {
            auto *next = new Work(w);
            next->action = upload ? Action::SettingsUpload : Action::SettingsDownload;
            next->success = false;
            launch(next);
            return;
        }
        w.message = "已取消设置同步。";
    }
    if (w.action == Action::Restore && w.success)
    {
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(p.provider));
        if (p.providers["phone"])
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p.provider), "phone", "手机号码");
        if (p.providers["email"])
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p.provider), "email", "邮箱");
        gtk_combo_box_set_active(GTK_COMBO_BOX(p.provider), 0);
        if (!p.providers["phone"] && !p.providers["email"] && !p.snapshot.user)
            w.message = "手机和邮箱登录暂未开放。此版本暂不支持 Apple、Google 或微信登录。";
    }
    if (w.action == Action::Login && p.challenge.id.empty())
    {
        gtk_entry_set_text(GTK_ENTRY(p.code), "");
        gtk_entry_set_text(GTK_ENTRY(p.target), "");
    }
    if (p.snapshot.user)
    {
        if (w.action != Action::Rename || w.success)
            gtk_entry_set_text(GTK_ENTRY(p.nickname), p.snapshot.user->display_name.c_str());
        std::string text = "已绑定登录方式：";
        for (const auto &identity : p.profile.identities)
        {
            if (text != "已绑定登录方式：")
                text += "、";
            const auto &provider = identity.provider;
            text += provider == "apple"    ? "Apple"
                    : provider == "google" ? "Google"
                    : provider == "wechat" ? "微信"
                    : provider == "phone"  ? "手机号码"
                    : provider == "email"  ? "邮箱"
                                           : "其他方式";
        }
        if (p.profile.identities.empty())
            text += "暂无资料，请刷新。";
        gtk_label_set_text(GTK_LABEL(p.identities), text.c_str());
    }
    if (!p.snapshot.user)
        p.profile = {};
    if (p.cloud_generation != p.snapshot.generation || !p.snapshot.user)
    {
        p.cloud_snapshot = {};
        p.cloud_loaded = false;
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(p.cloud_text)), "", 0);
        gtk_entry_set_text(GTK_ENTRY(p.cloud_query), "");
    }
    if (w.success && w.action == Action::ClipboardAdd)
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(p.cloud_text)), "", 0);
    if (w.success && (w.action == Action::ClipboardAdd || w.action == Action::ClipboardDelete ||
                      w.action == Action::ClipboardClear || w.action == Action::ClipboardToggle))
        gtk_entry_set_text(GTK_ENTRY(p.cloud_query), "");
    gtk_list_store_clear(p.cloud_rows);
    for (const auto &item : p.cloud_snapshot.items)
    {
        GtkTreeIter row;
        gtk_list_store_append(p.cloud_rows, &row);
        gtk_list_store_set(p.cloud_rows, &row, 0, item.id.c_str(), 1, item.text.c_str(), -1);
    }
    if (w.message.empty())
        w.message = p.snapshot.user ? "已登录：" + p.snapshot.user->display_name : "尚未登录";
    gtk_label_set_text(GTK_LABEL(p.status), w.message.c_str());
    if (p.challenge_generation != p.snapshot.generation)
        p.challenge = {};
    render(p);
    gtk_widget_set_sensitive(p.send, gtk_combo_box_get_active_id(GTK_COMBO_BOX(p.provider)) != nullptr);
}
void begin(const Handle &p, Action action)
{
    if (!gtk_widget_get_sensitive(p->root))
        return;
    if (settings_action(action) && (!p->sync.store || (p->sync.allow && !p->sync.allow())))
    {
        gtk_label_set_text(GTK_LABEL(p->status), "请先保存或重新加载本机设置，再进行同步。");
        return;
    }
    const char *provider = gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->provider));
    auto *work = new Work;
    work->panel = p;
    work->action = action;
    work->provider = provider ? provider : "";
    work->target = gtk_entry_get_text(GTK_ENTRY(p->target));
    work->code = gtk_entry_get_text(GTK_ENTRY(p->code));
    work->nickname = gtk_entry_get_text(GTK_ENTRY(p->nickname));
    work->query = gtk_entry_get_text(GTK_ENTRY(p->cloud_query));
    if (action == Action::ClipboardAdd)
    {
        GtkTextIter start, end;
        auto *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(p->cloud_text));
        gtk_text_buffer_get_bounds(buffer, &start, &end);
        gchar *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
        work->clip_text = text;
        g_free(text);
    }
    if (action == Action::ClipboardDelete)
    {
        GtkTreeIter row;
        GtkTreeModel *model = nullptr;
        if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->cloud_list)), &model, &row))
        {
            delete work;
            gtk_label_set_text(GTK_LABEL(p->status), "请先选择一条云记录。");
            return;
        }
        gchar *id = nullptr;
        gtk_tree_model_get(model, &row, 0, &id, -1);
        work->clip_id = id;
        g_free(id);
    }
    launch(work);
}
void launch(Work *work)
{
    const auto &p = work->panel;
    if (settings_action(work->action) && p->sync.busy)
        p->sync.busy(true);
    gtk_widget_set_sensitive(p->root, FALSE);
    gtk_label_set_text(GTK_LABEL(p->status), "正在处理…");
    GTask *task = g_task_new(nullptr, nullptr, finished, nullptr);
    g_task_set_task_data(task, work, [](gpointer value) { delete static_cast<Work *>(value); });
    g_task_run_in_thread(task, worker);
    g_object_unref(task);
}
void clicked(GtkButton *button, gpointer data)
{
    const auto p = *static_cast<Handle *>(data);
    auto action = static_cast<Action>(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "account-action")));
    if (action == Action::Delete || action == Action::ClipboardClear ||
        (action == Action::ClipboardToggle && p->cloud_snapshot.enabled))
    {
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(gtk_widget_get_toplevel(p->root)), GTK_DIALOG_MODAL,
                                                   GTK_MESSAGE_WARNING, GTK_BUTTONS_OK_CANCEL, "%s",
                                                   action == Action::Delete
                                                       ? "注销账号将删除云端个人数据，且无法恢复。确定注销？"
                                                       : "此操作会清空该账号的云剪贴板记录，且无法恢复。确定继续？");
        const int response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (p->closed.load() || response != GTK_RESPONSE_OK)
            return;
    }
    begin(p, action);
}
GtkWidget *button(const Handle &p, GtkWidget *box, const char *title, Action action)
{
    GtkWidget *widget = gtk_button_new_with_label(title);
    g_object_set_data(G_OBJECT(widget), "account-action", GINT_TO_POINTER(static_cast<int>(action)));
    g_signal_connect_data(
        widget, "clicked", G_CALLBACK(clicked), new Handle(p),
        [](gpointer data, GClosure *) { delete static_cast<Handle *>(data); }, G_CONNECT_DEFAULT);
    gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0);
    return widget;
}
} // namespace
GtkWidget *create_account_panel()
{
    return create_account_panel(std::make_shared<LibsecretSecretStore>(),
                                std::make_shared<online::CurlHttpTransport>());
}
GtkWidget *create_account_panel(std::shared_ptr<SecretStore> secrets, std::shared_ptr<online::HttpTransport> transport,
                                SettingsSyncHooks sync)
{
    auto p = std::make_shared<Panel>(std::move(secrets), std::move(transport));
    p->sync = std::move(sync);
    p->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(p->root, 20);
    gtk_widget_set_margin_end(p->root, 20);
    gtk_widget_set_margin_top(p->root, 18);
    gtk_widget_set_margin_bottom(p->root, 18);
    g_signal_connect_data(
        p->root, "destroy",
        G_CALLBACK(+[](GtkWidget *, gpointer data) { (*static_cast<Handle *>(data))->closed.store(true); }),
        new Handle(p), [](gpointer data, GClosure *) { delete static_cast<Handle *>(data); }, G_CONNECT_DEFAULT);
    p->status = gtk_label_new("正在恢复账号…");
    gtk_label_set_line_wrap(GTK_LABEL(p->status), TRUE);
    gtk_label_set_xalign(GTK_LABEL(p->status), 0);
    gtk_box_pack_start(GTK_BOX(p->root), p->status, FALSE, FALSE, 0);
    p->form = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(p->root), p->form, FALSE, FALSE, 0);
    p->provider = gtk_combo_box_text_new();
    p->target = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(p->target), "邮箱或含国家区号的手机号码");
    p->code = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(p->code), "验证码");
    gtk_entry_set_visibility(GTK_ENTRY(p->code), FALSE);
    gtk_entry_set_max_length(GTK_ENTRY(p->code), 6);
    for (auto *widget : {p->provider, p->target, p->code})
        gtk_box_pack_start(GTK_BOX(p->form), widget, FALSE, FALSE, 0);
    p->send = button(p, p->form, "发送验证码", Action::Challenge);
    p->login = button(p, p->form, "登录", Action::Login);
    p->details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(p->root), p->details, FALSE, FALSE, 0);
    p->nickname = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(p->nickname), "昵称");
    gtk_entry_set_max_length(GTK_ENTRY(p->nickname), 128);
    gtk_box_pack_start(GTK_BOX(p->details), p->nickname, FALSE, FALSE, 0);
    button(p, p->details, "保存昵称", Action::Rename);
    p->identities = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(p->identities), TRUE);
    gtk_box_pack_start(GTK_BOX(p->details), p->identities, FALSE, FALSE, 0);
    button(p, p->details, "刷新资料", Action::Profile);
    p->cloud = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(p->root), p->cloud, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p->cloud), gtk_label_new("云剪贴板 · 手动上传"), FALSE, FALSE, 0);
    p->cloud_status = gtk_label_new("尚未加载云记录");
    gtk_box_pack_start(GTK_BOX(p->cloud), p->cloud_status, FALSE, FALSE, 0);
    p->cloud_query = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(p->cloud_query), "搜索云剪贴板");
    gtk_box_pack_start(GTK_BOX(p->cloud), p->cloud_query, FALSE, FALSE, 0);
    button(p, p->cloud, "加载或搜索云记录", Action::ClipboardLoad);
    p->cloud_toggle = button(p, p->cloud, "开启云剪贴板", Action::ClipboardToggle);
    p->cloud_text = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(p->cloud_text), GTK_WRAP_WORD_CHAR);
    GtkWidget *editor_scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_widget_set_size_request(editor_scroll, -1, 80);
    gtk_container_add(GTK_CONTAINER(editor_scroll), p->cloud_text);
    gtk_box_pack_start(GTK_BOX(p->cloud), gtk_label_new("输入或粘贴要上传的文字"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p->cloud), editor_scroll, FALSE, FALSE, 0);
    p->cloud_upload = button(p, p->cloud, "上传文字", Action::ClipboardAdd);
    p->cloud_rows = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    p->cloud_list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(p->cloud_rows));
    g_object_unref(p->cloud_rows);
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, "width-chars", 40, nullptr);
    gtk_tree_view_append_column(GTK_TREE_VIEW(p->cloud_list),
                                gtk_tree_view_column_new_with_attributes("云端文字", renderer, "text", 1, nullptr));
    GtkWidget *list_scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_widget_set_size_request(list_scroll, -1, 150);
    gtk_container_add(GTK_CONTAINER(list_scroll), p->cloud_list);
    gtk_box_pack_start(GTK_BOX(p->cloud), list_scroll, FALSE, FALSE, 0);
    GtkWidget *copy = gtk_button_new_with_label("复制选中记录");
    g_signal_connect_data(
        copy, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
            const auto panel = *static_cast<Handle *>(data);
            GtkTreeModel *model = nullptr;
            GtkTreeIter row;
            if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(panel->cloud_list)), &model,
                                                &row))
            {
                gchar *text = nullptr;
                gtk_tree_model_get(model, &row, 1, &text, -1);
                gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), text, -1);
                g_free(text);
            }
        }),
        new Handle(p), [](gpointer data, GClosure *) { delete static_cast<Handle *>(data); }, G_CONNECT_DEFAULT);
    gtk_box_pack_start(GTK_BOX(p->cloud), copy, FALSE, FALSE, 0);
    button(p, p->cloud, "删除选中记录", Action::ClipboardDelete);
    button(p, p->cloud, "清空云记录", Action::ClipboardClear);
    p->settings_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(p->root), p->settings_box, FALSE, FALSE, 0);
    button(p, p->settings_box, "预览上传本机设置", Action::SettingsUploadReview);
    button(p, p->settings_box, "预览下载云端设置", Action::SettingsDownloadReview);
    auto *dictionaries = gtk_button_new_with_label("管理云端个人词库");
    g_signal_connect_data(
        dictionaries, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
            const auto panel = *static_cast<Handle *>(data);
            create_dictionary_window(GTK_WINDOW(gtk_widget_get_toplevel(panel->root)),
                                     std::shared_ptr<AccountSession>(panel, &panel->session),
                                     panel->snapshot.generation);
        }),
        new Handle(p), [](gpointer data, GClosure *) { delete static_cast<Handle *>(data); }, G_CONNECT_DEFAULT);
    gtk_box_pack_start(GTK_BOX(p->details), dictionaries, FALSE, FALSE, 0);
    p->logout = button(p, p->root, "退出登录", Action::Logout);
    p->remove = button(p, p->root, "注销账号", Action::Delete);
    p->retry = button(p, p->root, "重新加载账号", Action::Restore);
    GtkWidget *change = gtk_button_new_with_label("更换登录账号");
    g_signal_connect_data(
        change, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
            auto &panel = **static_cast<Handle *>(data);
            panel.challenge = {};
            gtk_entry_set_text(GTK_ENTRY(panel.code), "");
            render(panel);
        }),
        new Handle(p), [](gpointer data, GClosure *) { delete static_cast<Handle *>(data); }, G_CONNECT_DEFAULT);
    gtk_box_pack_start(GTK_BOX(p->form), change, FALSE, FALSE, 0);
    gtk_widget_show_all(p->root);
    for (auto *widget : {p->form, p->details, p->cloud, p->settings_box, p->logout, p->remove, p->retry})
        gtk_widget_set_no_show_all(widget, TRUE);
    render(*p);
    begin(p, Action::Restore);
    return p->root;
}
} // namespace metasequoia::linux_ime::account
