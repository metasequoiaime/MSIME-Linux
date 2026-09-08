#include "AccountPanel.h"
#include "AccountSession.h"
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
              *retry = nullptr;
    SessionSnapshot snapshot;
    std::map<std::string, bool> providers;
    Challenge challenge;
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
    Delete
};
struct Work
{
    Handle panel;
    Action action;
    std::string provider, target, code;
    std::string message;
    bool success = false;
};
void render(Panel &p)
{
    const bool signed_in = p.snapshot.user.has_value();
    gtk_widget_set_visible(p.form, !signed_in);
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
            p.providers = p.client.providers(cancelled);
            p.ready = true;
            break;
        case Action::Challenge:
            p.challenge = p.client.challenge(w.provider, w.target, {}, cancelled);
            p.challenge_deadline =
                g_get_monotonic_time() + static_cast<gint64>(p.challenge.expires_in) * G_USEC_PER_SEC;
            w.message = "验证码已发送，请在有效期内输入。";
            break;
        case Action::Login:
            if (p.challenge.id.empty() || g_get_monotonic_time() >= p.challenge_deadline)
                throw Failure(400);
            p.snapshot = p.session.login(p.snapshot.generation, p.challenge.id, w.code, cancelled);
            p.challenge = {};
            break;
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
        w.message = error.status() == 429   ? "请求过于频繁，请稍后重试。"
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
    if (w.success && w.action == Action::Login)
    {
        gtk_entry_set_text(GTK_ENTRY(p.code), "");
        gtk_entry_set_text(GTK_ENTRY(p.target), "");
    }
    if (w.message.empty())
        w.message = p.snapshot.user ? "已登录：" + p.snapshot.user->display_name : "尚未登录";
    gtk_label_set_text(GTK_LABEL(p.status), w.message.c_str());
    render(p);
    gtk_widget_set_sensitive(p.send, gtk_combo_box_get_active_id(GTK_COMBO_BOX(p.provider)) != nullptr);
}
void begin(const Handle &p, Action action)
{
    if (!gtk_widget_get_sensitive(p->root))
        return;
    const char *provider = gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->provider));
    auto *work = new Work{p,
                          action,
                          provider ? provider : "",
                          gtk_entry_get_text(GTK_ENTRY(p->target)),
                          gtk_entry_get_text(GTK_ENTRY(p->code)),
                          {},
                          false};
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
    if (action == Action::Delete)
    {
        GtkWidget *dialog =
            gtk_message_dialog_new(GTK_WINDOW(gtk_widget_get_toplevel(p->root)), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
                                   GTK_BUTTONS_OK_CANCEL, "%s", "注销账号将删除云端个人数据，且无法恢复。确定注销？");
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
GtkWidget *create_account_panel(std::shared_ptr<SecretStore> secrets, std::shared_ptr<online::HttpTransport> transport)
{
    auto p = std::make_shared<Panel>(std::move(secrets), std::move(transport));
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
    for (auto *widget : {p->form, p->logout, p->remove, p->retry})
        gtk_widget_set_no_show_all(widget, TRUE);
    render(*p);
    begin(p, Action::Restore);
    return p->root;
}
} // namespace metasequoia::linux_ime::account
