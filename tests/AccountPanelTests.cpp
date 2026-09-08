#include "account/AccountPanel.h"
#include "account/CloudSettingsMapper.h"
#include <boost/json.hpp>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace metasequoia::linux_ime;
namespace
{
void require(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}
struct Store final : SecretStore
{
    SecretLookupResult record;
    SecretLookupResult lookup(SecretKind kind, std::string_view) const override
    {
        return kind == SecretKind::AccountSession ? record : SecretLookupResult{};
    }
    bool store(SecretKind kind, std::string_view, std::string_view value, std::string *) override
    {
        if (kind == SecretKind::AccountSession)
            record = {SecretStatus::Found, std::string(value), {}};
        return true;
    }
    bool erase(SecretKind kind, std::string_view, std::string *) override
    {
        if (kind == SecretKind::AccountSession)
            record = {};
        return true;
    }
};
struct Transport final : online::HttpTransport
{
    int preference_requests = 0, preference_writes = 0;
    std::int64_t revision = 0;
    boost::json::object preferences{{"appearance.page_size", 5}};
    bool linking = false, linked = false, cloud_enabled = false;
    int cloud_requests = 0;
    boost::json::array cloud_items;
    std::string nickname = "测试用户";
    std::atomic<bool> block{false}, entered{false}, cancelled{false};
    online::HttpResponse perform(const online::HttpRequest &request, const online::CancellationCheck &check) override
    {
        if (block.load())
        {
            entered.store(true);
            while (!check())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            cancelled.store(true);
            return {0, {}, "cancelled"};
        }
        if (request.url.find("/providers") != std::string::npos)
            return {
                200, R"({"providers":{"apple":false,"google":false,"wechat":false,"phone":true,"email":true}})", {}};
        if (request.url.find("/challenges") != std::string::npos)
        {
            linking = boost::json::parse(request.body).at("purpose").as_string() == "link";
            const auto provider = boost::json::parse(request.body).at("provider").as_string();
            require(provider == (linking ? "phone" : "email"), "wrong selected login provider");
            if (linking)
                require(request.headers.back().find("Authorization: Bearer ") == 0,
                        "link challenge lacks authorization");
            return {201, R"({"challenge_id":"synthetic-challenge","expires_in":300})", {}};
        }
        if (request.url.find("/login") != std::string::npos)
        {
            if (linking)
            {
                require(request.headers.back().find("Authorization: Bearer ") == 0,
                        "link exchange lacks authorization");
                linked = true;
            }
            return {
                200,
                boost::json::serialize(boost::json::object{
                    {"access_token", std::string(64, 'a')},
                    {"refresh_token", std::string(64, 'b')},
                    {"expires_in", 900},
                    {"token_type", "Bearer"},
                    {"user",
                     boost::json::object{{"id", "synthetic"}, {"display_name", "测试用户"}, {"created_at", "now"}}}}),
                {}};
        }
        if (request.url.find("/clipboard") != std::string::npos)
        {
            ++cloud_requests;
            if (request.url.find("/settings") != std::string::npos)
            {
                cloud_enabled = boost::json::parse(request.body).at("enabled").as_bool();
                if (!cloud_enabled)
                    cloud_items.clear();
                return {200, boost::json::serialize(boost::json::object{{"enabled", cloud_enabled}}), {}};
            }
            if (request.method == online::HttpMethod::Post)
            {
                if (!cloud_enabled)
                    return {403, "{}", {}};
                auto item = boost::json::object{{"id", std::string(64, 'f')},
                                                {"text", boost::json::parse(request.body).at("text")},
                                                {"updated_at", "now"}};
                cloud_items.push_back(item);
                return {200, boost::json::serialize(item), {}};
            }
            if (request.method == online::HttpMethod::Delete)
            {
                cloud_items.clear();
                return {204, {}, {}};
            }
            return {
                200,
                boost::json::serialize(boost::json::object{
                    {"enabled", cloud_enabled},
                    {"items", request.url.find("missing") != std::string::npos ? boost::json::array{} : cloud_items}}),
                {}};
        }
        if (request.url.find("/preferences") != std::string::npos)
        {
            ++preference_requests;
            if (request.url.find("/schema") != std::string::npos)
            {
                boost::json::object fields;
                for (const auto &[key, value] : account::CloudSettingsMapper::export_settings(InputSettings{}))
                    fields[key] =
                        boost::json::object{{"type", std::holds_alternative<bool>(value)           ? "boolean"
                                                     : std::holds_alternative<std::int64_t>(value) ? "integer"
                                                                                                   : "string"}};
                return {200,
                        boost::json::serialize(boost::json::object{{"fields", fields},
                                                                   {"maximum_bytes", 1048576},
                                                                   {"update_mode", "replace"},
                                                                   {"revision_required", true}}),
                        {}};
            }
            if (request.method == online::HttpMethod::Put)
            {
                const auto body = boost::json::parse(request.body);
                if (body.at("revision").as_int64() != revision)
                    return {409, "{}", {}};
                preferences = body.at("settings").as_object();
                ++revision;
                ++preference_writes;
            }
            return {200,
                    boost::json::serialize(boost::json::object{{"revision", revision}, {"settings", preferences}}),
                    {}};
        }
        if (request.url.find("/users/me") != std::string::npos)
        {
            if (request.method == online::HttpMethod::Patch)
                nickname = std::string(boost::json::parse(request.body).at("display_name").as_string());
            boost::json::array identities{
                boost::json::object{{"provider", "email"}, {"subject", "synthetic@example.invalid"}}};
            if (linked)
                identities.push_back(boost::json::object{{"provider", "phone"}, {"subject", "+819012345678"}});
            return {200,
                    boost::json::serialize(boost::json::object{
                        {"user",
                         boost::json::object{{"id", "synthetic"}, {"display_name", nickname}, {"created_at", "now"}}},
                        {"identities", identities}}),
                    {}};
        }
        return {204, {}, {}};
    }
};
template <typename Predicate> void wait(Predicate done)
{
    const auto limit = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    while (!done() && g_get_monotonic_time() < limit)
    {
        while (g_main_context_iteration(nullptr, FALSE))
        {
        }
        g_usleep(1000);
    }
    require(done(), "GTK work timed out");
}
GtkWidget *find(GtkWidget *root, const char *text)
{
    if (GTK_IS_TEXT_VIEW(root) && g_strcmp0(text, "云端输入") == 0)
        return root;
    if (GTK_IS_TREE_VIEW(root) && g_strcmp0(text, "云端列表") == 0)
        return root;
    if (GTK_IS_COMBO_BOX(root) && g_strcmp0(text, "登录渠道") == 0)
        return root;
    if (GTK_IS_LABEL(root) && g_strcmp0(gtk_label_get_text(GTK_LABEL(root)), text) == 0)
        return root;
    if (GTK_IS_BUTTON(root) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(root)), text) == 0)
        return root;
    if (GTK_IS_ENTRY(root) && g_strcmp0(gtk_entry_get_placeholder_text(GTK_ENTRY(root)), text) == 0)
        return root;
    if (GTK_IS_CONTAINER(root))
    {
        GList *children = gtk_container_get_children(GTK_CONTAINER(root));
        GtkWidget *result = nullptr;
        for (GList *child = children; child && !result; child = child->next)
            result = find(GTK_WIDGET(child->data), text);
        g_list_free(children);
        return result;
    }
    return nullptr;
}
void click(GtkWidget *panel, const char *text)
{
    auto *widget = find(panel, text);
    require(widget && gtk_widget_is_sensitive(widget) && gtk_widget_get_visible(widget), "button unavailable");
    gtk_button_clicked(GTK_BUTTON(widget));
}
} // namespace
int main(int argc, char **argv)
{
    const bool integration = argc == 2 && std::string(argv[1]) == "--integration";
    argc = 1;
    gtk_init(&argc, &argv);
    std::shared_ptr<SecretStore> secrets;
    if (integration)
        secrets = std::make_shared<LibsecretSecretStore>("session");
    else
        secrets = std::make_shared<Store>();
    auto http = std::make_shared<Transport>();
    auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gchar *directory = g_dir_make_tmp("msime-settings-ui-XXXXXX", nullptr);
    require(directory != nullptr, "temporary settings directory failed");
    auto settings_store = std::make_shared<SettingsStore>(directory);
    g_free(directory);
    InputSettings local;
    local.page_size = 9;
    require(settings_store->save(local), "fixture settings save failed");
    bool dirty = false, busy = false;
    int applied = 0;
    account::SettingsSyncHooks hooks;
    hooks.store = settings_store;
    hooks.allow = [&] { return !dirty; };
    hooks.busy = [&](bool value) {
        busy = value;
        gtk_widget_set_sensitive(window, !value);
    };
    hooks.applied = [&](const InputSettings &settings) {
        ++applied;
        local = settings;
    };
    auto *panel = account::create_account_panel(secrets, http, hooks);
    gtk_container_add(GTK_CONTAINER(window), panel);
    gtk_widget_show_all(window);
    wait([&] { return gtk_widget_get_sensitive(panel); });
    require(!gtk_widget_get_visible(find(panel, "退出登录")), "signed-out account shows logout");
    auto *target = find(panel, "邮箱或含国家区号的手机号码");
    auto *code = find(panel, "验证码");
    require(gtk_widget_get_visible(target), "login input hidden");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(panel, "登录渠道")), "email");
    gtk_entry_set_text(GTK_ENTRY(target), "synthetic@example.invalid");
    click(panel, "发送验证码");
    wait([&] { return gtk_widget_get_sensitive(panel); });
    require(!gtk_widget_is_sensitive(target), "challenge target not bound");
    g_object_ref(panel);
    gtk_container_remove(GTK_CONTAINER(window), panel);
    gtk_container_add(GTK_CONTAINER(window), panel);
    g_object_unref(panel);
    require(!gtk_widget_is_sensitive(target), "page reattachment discarded challenge");
    gtk_entry_set_text(GTK_ENTRY(code), "123456");
    click(panel, "登录");
    wait([&] { return gtk_widget_get_sensitive(panel); });
    require(secrets->lookup(SecretKind::AccountSession, "msime").status == SecretStatus::Found,
            "login credentials not saved");
    require(std::string(gtk_entry_get_text(GTK_ENTRY(code))).empty(), "verification code not cleared");
    auto *nickname = find(panel, "昵称");
    require(std::string(gtk_entry_get_text(GTK_ENTRY(nickname))) == "测试用户", "profile not loaded");
    gtk_entry_set_text(GTK_ENTRY(nickname), "新昵称");
    click(panel, "保存昵称");
    wait([&] { return gtk_widget_get_sensitive(panel); });
    require(std::string(gtk_entry_get_text(GTK_ENTRY(nickname))) == "新昵称" &&
                boost::json::parse(secrets->lookup(SecretKind::AccountSession, "msime").value)
                        .at("user")
                        .at("display_name")
                        .as_string() == "新昵称",
            "nickname did not reach service and credential store");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(panel, "登录渠道")), "phone");
    gtk_entry_set_text(GTK_ENTRY(target), "+819012345678");
    click(panel, "发送验证码");
    wait([&] { return gtk_widget_get_sensitive(panel); });
    gtk_entry_set_text(GTK_ENTRY(code), "123456");
    click(panel, "绑定登录方式");
    wait([&] { return gtk_widget_get_sensitive(panel); });
    require(http->linked, "signed-in flow did not bind identity");
    require(find(panel, "已绑定登录方式：邮箱、手机号码") != nullptr, "linked identity not shown");
    require(boost::json::parse(secrets->lookup(SecretKind::AccountSession, "msime").value)
                    .at("user")
                    .at("id")
                    .as_string() == "synthetic",
            "binding replaced user");
    click(panel, "刷新资料");
    wait([&] { return gtk_widget_get_sensitive(panel); });
    require(http->cloud_requests == 0, "cloud data accessed without explicit action");
    click(panel, "加载或搜索云记录");
    wait([&] { return gtk_widget_get_sensitive(panel); });
    require(!gtk_widget_is_sensitive(find(panel, "上传文字")), "upload allowed while disabled");
    click(panel, "开启云剪贴板");
    wait([&] { return gtk_widget_get_sensitive(panel); });
    auto *cloud_text = gtk_text_view_get_buffer(GTK_TEXT_VIEW(find(panel, "云端输入")));
    auto *cloud_model = gtk_tree_view_get_model(GTK_TREE_VIEW(find(panel, "云端列表")));
    auto *cloud_query = find(panel, "搜索云剪贴板");
    gtk_text_buffer_set_text(cloud_text, "合成云端文字", -1);
    click(panel, "上传文字");
    wait([&] { return gtk_widget_get_sensitive(panel); });
    require(http->cloud_items.size() == 1 && gtk_tree_model_iter_n_children(cloud_model, nullptr) == 1,
            "cloud upload not shown");
    gtk_entry_set_text(GTK_ENTRY(cloud_query), "missing");
    click(panel, "加载或搜索云记录");
    wait([&] { return gtk_widget_get_sensitive(panel); });
    require(gtk_tree_model_iter_n_children(cloud_model, nullptr) == 0, "cloud search not applied");
    gtk_entry_set_text(GTK_ENTRY(cloud_query), "");
    click(panel, "加载或搜索云记录");
    wait([&] { return gtk_widget_get_sensitive(panel); });
    auto *path = gtk_tree_path_new_from_indices(0, -1);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(find(panel, "云端列表"))), path);
    gtk_tree_path_free(path);
    click(panel, "复制选中记录");
    gchar *copied = gtk_clipboard_wait_for_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
    require(copied && std::string(copied) == "合成云端文字", "selected cloud record not copied");
    g_free(copied);
    click(panel, "删除选中记录");
    wait([&] { return gtk_widget_get_sensitive(panel); });
    require(http->cloud_items.empty(), "selected record not deleted");
    gtk_text_buffer_set_text(cloud_text, "关闭清理合成记录", -1);
    click(panel, "上传文字");
    wait([&] { return gtk_widget_get_sensitive(panel); });
    auto respond = +[](gpointer response) -> gboolean {
        GList *windows = gtk_window_list_toplevels();
        for (auto *item = windows; item; item = item->next)
            if (GTK_IS_DIALOG(item->data))
                gtk_dialog_response(GTK_DIALOG(item->data), GPOINTER_TO_INT(response));
        g_list_free(windows);
        return G_SOURCE_REMOVE;
    };
    g_idle_add(respond, GINT_TO_POINTER(GTK_RESPONSE_CANCEL));
    click(panel, "清空云记录");
    require(http->cloud_items.size() == 1, "cancelled clear mutated data");
    g_idle_add(respond, GINT_TO_POINTER(GTK_RESPONSE_OK));
    click(panel, "关闭云剪贴板");
    wait([&] { return gtk_widget_get_sensitive(panel); });
    require(!http->cloud_enabled && http->cloud_items.empty(), "disable did not clear cloud records");
    require(http->preference_requests == 0, "preferences accessed without user request");
    dirty = true;
    click(panel, "预览下载云端设置");
    require(http->preference_requests == 0 && !busy, "dirty settings were not blocked");
    dirty = false;
    struct ReviewResponse
    {
        int response;
        std::function<void()> before;
        bool seen = false;
    };
    auto review = [&](const char *label, int response, std::function<void()> before = {}) {
        ReviewResponse answer{response, std::move(before), false};
        const auto timer = g_timeout_add(
            5,
            +[](gpointer data) -> gboolean {
                auto &answer = *static_cast<ReviewResponse *>(data);
                GList *windows = gtk_window_list_toplevels();
                GtkDialog *dialog = nullptr;
                for (auto *item = windows; item; item = item->next)
                    if (GTK_IS_DIALOG(item->data))
                        dialog = GTK_DIALOG(item->data);
                g_list_free(windows);
                if (!dialog)
                    return G_SOURCE_CONTINUE;
                answer.seen = true;
                auto *text = find(GTK_WIDGET(dialog), "云端输入");
                require(text && gtk_text_buffer_get_char_count(gtk_text_view_get_buffer(GTK_TEXT_VIEW(text))) > 0,
                        "empty settings preview");
                if (answer.before)
                    answer.before();
                gtk_dialog_response(dialog, answer.response);
                return G_SOURCE_REMOVE;
            },
            &answer);
        click(panel, label);
        wait([&] { return !busy; });
        if (!answer.seen)
            g_source_remove(timer);
        require(answer.seen, "settings confirmation not shown");
    };
    review("预览下载云端设置", GTK_RESPONSE_CANCEL);
    require(settings_store->load().page_size == 9 && applied == 0, "cancelled download changed settings");
    review("预览下载云端设置", GTK_RESPONSE_OK);
    require(settings_store->load().page_size == 5 && applied == 1 && local.page_size == 5,
            "confirmed download did not persist and update model");
    review("预览下载云端设置", GTK_RESPONSE_OK);
    require(applied == 1, "unchanged download unnecessarily saved credentials and file");
    http->preferences["appearance.page_size"] = 7;
    review("预览下载云端设置", GTK_RESPONSE_OK, [&] { ++http->revision; });
    require(settings_store->load().page_size == 5 && applied == 1, "stale cloud review overwrote local settings");
    review("预览下载云端设置", GTK_RESPONSE_OK, [&] {
        auto external = settings_store->load();
        external.page_size = 8;
        require(settings_store->save(external), "external write failed");
    });
    require(settings_store->load().page_size == 8 && applied == 1, "stale local review overwrote external edit");
    review("预览上传本机设置", GTK_RESPONSE_CANCEL);
    require(http->preference_writes == 0, "cancelled upload wrote cloud settings");
    review("预览上传本机设置", GTK_RESPONSE_OK, [&] { dirty = true; });
    require(http->preference_writes == 0, "draft changed during review was not blocked");
    dirty = false;
    review("预览上传本机设置", GTK_RESPONSE_OK);
    require(http->preference_writes == 1 && http->preferences.at("appearance.page_size").as_int64() == 8,
            "confirmed upload did not use saved settings");
    review("预览上传本机设置", GTK_RESPONSE_OK);
    require(http->preference_writes == 1, "unchanged upload unnecessarily incremented revision");
    gtk_text_buffer_set_text(cloud_text, "退出时应丢弃的草稿", -1);
    const auto saved_account = secrets->lookup(SecretKind::AccountSession, "msime").value;
    click(panel, "退出登录");
    wait([&] { return gtk_widget_get_sensitive(panel); });
    require(secrets->lookup(SecretKind::AccountSession, "msime").status == SecretStatus::NotFound,
            "logout credentials remained");
    require(gtk_text_buffer_get_char_count(cloud_text) == 0 &&
                gtk_tree_model_iter_n_children(cloud_model, nullptr) == 0,
            "logout retained cloud data");
    gtk_widget_destroy(window);
    require(secrets->store(SecretKind::AccountSession, "msime", saved_account), "restore fixture failed");
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    panel = account::create_account_panel(secrets, http, hooks);
    gtk_container_add(GTK_CONTAINER(window), panel);
    gtk_widget_show_all(window);
    wait([&] { return gtk_widget_get_sensitive(panel); });
    http->preferences["appearance.page_size"] = 6;
    bool closed_review = false;
    struct CloseReview
    {
        GtkWidget *window;
        bool *closed;
    } close_review{window, &closed_review};
    g_timeout_add(
        5,
        +[](gpointer data) -> gboolean {
            auto &close = *static_cast<CloseReview *>(data);
            GList *windows = gtk_window_list_toplevels();
            bool found = false;
            for (auto *item = windows; item; item = item->next)
                found = found || GTK_IS_DIALOG(item->data);
            g_list_free(windows);
            if (!found)
                return G_SOURCE_CONTINUE;
            *close.closed = true;
            gtk_widget_destroy(close.window);
            return G_SOURCE_REMOVE;
        },
        &close_review);
    const auto previous_applied = applied;
    click(panel, "预览下载云端设置");
    wait([&] { return closed_review; });
    require(applied == previous_applied && settings_store->load().page_size == 8,
            "closing preview applied settings or invoked model callback");
    require(secrets->erase(SecretKind::AccountSession, "msime"), "fixture credential cleanup failed");
    http->block.store(true);
    panel = account::create_account_panel(secrets, http);
    g_object_ref_sink(panel);
    wait([&] { return http->entered.load(); });
    gtk_widget_destroy(panel);
    g_object_unref(panel);
    wait([&] { return http->cancelled.load(); });
    for (int i = 0; i < 100; ++i)
    {
        while (g_main_context_iteration(nullptr, FALSE))
        {
        }
        g_usleep(1000);
    }
    std::filesystem::remove_all(settings_store->config_path().parent_path().parent_path());
    std::cout << "GTK account login, logout and close-during-request tests passed\n";
}
