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
std::string snapshot_fixture(std::int64_t revision)
{
    const auto body =
        boost::json::serialize(boost::json::object{
            {"type", "header"}, {"format", "msime-dictionary-snapshot"}, {"version", 1}, {"revision", revision}}) +
        "\n";
    gchar *hash =
        g_compute_checksum_for_data(G_CHECKSUM_SHA256, reinterpret_cast<const guchar *>(body.data()), body.size());
    auto result =
        body + boost::json::serialize(boost::json::object{{"type", "footer"}, {"records", 1}, {"sha256", hash}}) + "\n";
    g_free(hash);
    return result;
}
struct Transport final : online::HttpTransport
{
    int dictionary_requests = 0;
    int snapshot_reads = 0, snapshot_writes = 0;
    bool snapshot_conflict = false;
    std::string snapshot_uploaded;
    std::int64_t dictionary_revision = 10;
    std::map<std::string, boost::json::array> dictionaries;
    bool dictionary_conflict = false;
    std::string imported_text, imported_format;
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
        if (request.url.find("/dictionary/snapshot") != std::string::npos)
        {
            if (request.method == online::HttpMethod::Get)
            {
                ++snapshot_reads;
                const auto body = snapshot_fixture(dictionary_revision);
                request.response_sink(body.data(), body.size());
                return {200, {}, {}};
            }
            ++snapshot_writes;
            if (snapshot_conflict)
                return {409, {}, {}};
            snapshot_uploaded.clear();
            char buffer[64];
            while (snapshot_uploaded.size() < request.body_size)
            {
                auto count = request.body_source(snapshot_uploaded.size(), buffer, sizeof(buffer));
                require(count > 0, "snapshot upload truncated");
                snapshot_uploaded.append(buffer, count);
            }
            for (auto &[kind, entries] : dictionaries)
            {
                (void)kind;
                entries.clear();
            }
            return {200,
                    boost::json::serialize(boost::json::object{{"revision", ++dictionary_revision}, {"reset", true}}),
                    {}};
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
        if (const auto prefix = request.url.find("/dictionaries/"); prefix != std::string::npos)
        {
            ++dictionary_requests;
            const auto start = prefix + std::string("/dictionaries/").size();
            const auto end = request.url.find_first_of("/?", start);
            const auto kind = request.url.substr(start, end - start);
            auto &entries = dictionaries[kind];
            if (request.url.find("/export") != std::string::npos)
                return {200, "测试\ttest\t10\n", {}};
            if (request.method == online::HttpMethod::Get)
            {
                const int offset = request.url.find("offset=50") != std::string::npos ? 50 : 0;
                boost::json::array page;
                for (std::size_t index = offset;
                     index < entries.size() && index < static_cast<std::size_t>(offset + 50); ++index)
                    page.push_back(entries[index]);
                return {200,
                        boost::json::serialize(
                            boost::json::object{{"entries", page},
                                                {"offset", offset},
                                                {"has_more", entries.size() > static_cast<std::size_t>(offset + 50)}}),
                        {}};
            }
            if (dictionary_conflict)
                return {409, "{}", {}};
            const auto body = boost::json::parse(request.body).as_object();
            if (request.url.find("/import") != std::string::npos)
            {
                imported_text = std::string(body.at("text").as_string());
                imported_format = request.url.find("import-hans") != std::string::npos
                                      ? "hans"
                                      : std::string(body.at("format").as_string());
                ++dictionary_revision;
                entries = {boost::json::object{{"id", std::string(64, 'e')},
                                               {"kind", kind},
                                               {"code", "test"},
                                               {"word", "测试"},
                                               {"weight", 10},
                                               {"revision", dictionary_revision},
                                               {"updated_at", "now"}}};
                return {200,
                        boost::json::serialize(boost::json::object{{"imported", 1}, {"revision", dictionary_revision}}),
                        {}};
            }

            boost::json::value previous = nullptr, replacement = nullptr;
            const std::string id(64, 'd');
            if (request.method != online::HttpMethod::Post)
            {
                require(entries.size() == 1, "wrong mutation fixture");
                previous = entries[0];
                require(body.at("revision") == previous.at("revision"), "UI lost selected revision");
                entries.clear();
            }
            ++dictionary_revision;
            if (request.method != online::HttpMethod::Delete)
            {
                replacement = boost::json::object{{"id", id},
                                                  {"kind", kind},
                                                  {"code", body.at("code")},
                                                  {"word", body.at("word")},
                                                  {"weight", body.at("weight")},
                                                  {"revision", dictionary_revision},
                                                  {"updated_at", "now"}};
                entries.push_back(replacement);
            }
            return {200,
                    boost::json::serialize(boost::json::object{
                        {"revision", dictionary_revision}, {"previous", previous}, {"replacement", replacement}}),
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
    require(http->dictionary_requests == 0, "dictionary loaded without action");
    click(panel, "管理云端个人词库");
    GtkWidget *dictionary_window = nullptr;
    GList *windows = gtk_window_list_toplevels();
    for (auto *item = windows; item; item = item->next)
        if (g_strcmp0(gtk_window_get_title(GTK_WINDOW(item->data)), "云端个人词库") == 0)
            dictionary_window = GTK_WIDGET(item->data);
    g_list_free(windows);
    require(dictionary_window != nullptr, "dictionary window missing");
    auto *dictionary_body = gtk_bin_get_child(GTK_BIN(dictionary_window));
    auto ready = [&] { wait([&] { return gtk_widget_get_sensitive(dictionary_body); }); };
    auto *dictionary_list = find(dictionary_window, "云端列表");
    auto *dictionary_model = gtk_tree_view_get_model(GTK_TREE_VIEW(dictionary_list));
    for (const char *kind : {"pinyin", "wubi", "english", "quick"})
    {
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(dictionary_window, "登录渠道")), kind);
        click(dictionary_window, "搜索云词条");
        ready();
        require(gtk_tree_model_iter_n_children(dictionary_model, nullptr) == 0, "empty dictionary not shown");
        gtk_entry_set_text(GTK_ENTRY(find(dictionary_window, "词条编码")), "test");
        gtk_entry_set_text(GTK_ENTRY(find(dictionary_window, "词条文字")), "测试词条");
        click(dictionary_window, "新增云词条");
        ready();
        require(gtk_tree_model_iter_n_children(dictionary_model, nullptr) == 1, "new dictionary entry not shown");
        auto *selected = gtk_tree_path_new_from_indices(0, -1);
        gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(dictionary_list)), selected);
        gtk_tree_path_free(selected);
        gtk_entry_set_text(GTK_ENTRY(find(dictionary_window, "词条权重")), "25");
        g_idle_add(respond, GINT_TO_POINTER(GTK_RESPONSE_CANCEL));
        click(dictionary_window, "修改选中词条");
        require(http->dictionaries[kind][0].at("weight").as_int64() == 10, "cancelled edit wrote data");
        g_idle_add(respond, GINT_TO_POINTER(GTK_RESPONSE_OK));
        click(dictionary_window, "修改选中词条");
        ready();
        require(http->dictionaries[kind][0].at("weight").as_int64() == 25, "weight update missing");
        selected = gtk_tree_path_new_from_indices(0, -1);
        gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(dictionary_list)), selected);
        gtk_tree_path_free(selected);
        g_idle_add(respond, GINT_TO_POINTER(GTK_RESPONSE_OK));
        click(dictionary_window, "删除选中词条");
        ready();
        require(http->dictionaries[kind].empty(), "dictionary deletion failed");
        gtk_entry_set_text(GTK_ENTRY(find(dictionary_window, "词条权重")), "10");
    }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(dictionary_window, "登录渠道")), "pinyin");
    struct ImportAnswer
    {
        const char *format;
        const char *text;
        int response;
        bool draft;
    };
    auto import = [&](const char *format, const char *text, int response, bool draft = false) {
        ImportAnswer answer{format, text, response, draft};
        g_idle_add(
            +[](gpointer data) -> gboolean {
                auto &answer = *static_cast<ImportAnswer *>(data);
                GList *windows = gtk_window_list_toplevels();
                for (auto *item = windows; item; item = item->next)
                {
                    auto *dialog = GTK_WIDGET(item->data);
                    if (!GTK_IS_DIALOG(dialog))
                        continue;
                    auto *text = find(dialog, "云端输入");
                    require(text != nullptr, "import editor missing");
                    auto *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text));
                    if (answer.draft)
                        require(gtk_text_buffer_get_char_count(buffer) > 0, "failed import lost draft");
                    gtk_text_buffer_set_text(buffer, answer.text, -1);
                    require(gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(dialog, "登录渠道")), answer.format),
                            "import format missing");
                    gtk_dialog_response(GTK_DIALOG(dialog), answer.response);
                }
                g_list_free(windows);
                return G_SOURCE_REMOVE;
            },
            &answer);
        click(dictionary_window, "批量导入词条");
        ready();
    };
    const auto before_import = http->dictionary_requests;
    import("standard", "测试\ttest\t10", GTK_RESPONSE_CANCEL);
    require(http->dictionary_requests == before_import, "cancelled import reached backend");
    for (const char *format : {"standard", "windows", "hans"})
    {
        const auto *text = std::string(format) == "hans" ? "测试" : "测试\ttest\t10";
        import(format, text, GTK_RESPONSE_OK);
        require(http->imported_format == format && http->imported_text == text &&
                    gtk_tree_model_iter_n_children(dictionary_model, nullptr) == 1,
                "import not reflected in list");
    }
    http->dictionary_conflict = true;
    import("standard", "测试\ttest\t10", GTK_RESPONSE_OK);
    http->dictionary_conflict = false;
    import("standard", "测试\ttest\t10", GTK_RESPONSE_CANCEL, true);
    http->dictionaries["pinyin"].clear();
    const auto export_file = (settings_store->config_path().parent_path() / "ui-export.tsv").string();
    struct ExportAnswer
    {
        std::string directory;
        const char *name;
        bool initialized = false;
    };
    ExportAnswer export_answer{settings_store->config_path().parent_path().string(), "ui-export.tsv", false};
    const auto choose_export = +[](gpointer data) -> gboolean {
        auto &answer = *static_cast<ExportAnswer *>(data);
        GList *windows = gtk_window_list_toplevels();
        GtkWidget *dialog = nullptr;
        for (auto *item = windows; item; item = item->next)
            if (GTK_IS_FILE_CHOOSER(item->data))
                dialog = GTK_WIDGET(item->data);
        g_list_free(windows);
        if (!dialog)
            return G_SOURCE_CONTINUE;
        if (!answer.initialized)
        {
            gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), answer.directory.c_str());
            gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), answer.name);
            answer.initialized = true;
            return G_SOURCE_CONTINUE;
        }
        auto *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        const bool ready = path && std::string(path) == answer.directory + "/" + answer.name;
        g_free(path);
        if (!ready)
            return G_SOURCE_CONTINUE;
        gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
        return G_SOURCE_REMOVE;
    };
    g_timeout_add(30, choose_export, &export_answer);
    click(dictionary_window, "导出词库文件");
    ready();
    gchar *exported = nullptr;
    gsize exported_size = 0;
    require(g_file_get_contents(export_file.c_str(), &exported, &exported_size, nullptr) &&
                std::string(exported, exported_size) == "测试\ttest\t10\n",
            "UI export did not save validated data");
    g_free(exported);
    const auto requests_before_export_cancel = http->dictionary_requests;
    g_idle_add(respond, GINT_TO_POINTER(GTK_RESPONSE_CANCEL));
    click(dictionary_window, "导出词库文件");
    require(http->dictionary_requests == requests_before_export_cancel, "cancelled export sent request");

    const auto snapshot_file = (settings_store->config_path().parent_path() / "ui-snapshot.ndjson").string();
    const auto source_snapshot = snapshot_fixture(0);
    const auto restore_snapshot = [&](int response) {
        require(g_file_set_contents(snapshot_file.c_str(), source_snapshot.data(), source_snapshot.size(), nullptr),
                "snapshot fixture write failed");
        struct Answer
        {
            std::string path;
            int response;
            bool initialized = false, preview = false;
        } answer{snapshot_file, response};
        g_timeout_add(
            30,
            +[](gpointer data) -> gboolean {
                auto &answer = *static_cast<Answer *>(data);
                GList *windows = gtk_window_list_toplevels();
                GtkWidget *chooser = nullptr, *preview = nullptr;
                for (auto *item = windows; item; item = item->next)
                {
                    if (GTK_IS_FILE_CHOOSER(item->data))
                        chooser = GTK_WIDGET(item->data);
                    if (GTK_IS_MESSAGE_DIALOG(item->data) &&
                        std::string(gtk_window_get_title(GTK_WINDOW(item->data))) == "确认恢复完整云词库")
                        preview = GTK_WIDGET(item->data);
                }
                g_list_free(windows);
                if (chooser)
                {
                    if (!answer.initialized)
                    {
                        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(chooser), answer.path.c_str());
                        answer.initialized = true;
                        return G_SOURCE_CONTINUE;
                    }
                    gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
                    const bool ready = path && std::string(path) == answer.path;
                    g_free(path);
                    if (ready)
                        gtk_dialog_response(GTK_DIALOG(chooser), GTK_RESPONSE_ACCEPT);
                }
                if (!preview)
                    return G_SOURCE_CONTINUE;
                gchar *text = nullptr;
                g_object_get(preview, "text", &text, nullptr);
                require(text && std::string(text).find("当前云端：") != std::string::npos &&
                            std::string(text).find("快照内容：") != std::string::npos,
                        "restore preview missing counts");
                g_free(text);
                require(g_file_set_contents(answer.path.c_str(), "changed after preview", -1, nullptr),
                        "source replacement failed");
                answer.preview = true;
                gtk_dialog_response(GTK_DIALOG(preview), answer.response);
                return G_SOURCE_REMOVE;
            },
            &answer);
        click(dictionary_window, "从完整快照恢复云词库");
        wait([&] { return answer.preview; });
        ready();
    };
    const auto writes_before_snapshot = http->snapshot_writes;
    restore_snapshot(GTK_RESPONSE_CANCEL);
    require(http->snapshot_writes == writes_before_snapshot, "cancelled preview modified cloud");
    restore_snapshot(GTK_RESPONSE_ACCEPT);
    require(http->snapshot_writes == writes_before_snapshot + 1 && http->snapshot_uploaded == source_snapshot,
            "confirmed restore did not use frozen file");
    http->snapshot_conflict = true;
    restore_snapshot(GTK_RESPONSE_ACCEPT);
    require(http->snapshot_writes == writes_before_snapshot + 2 &&
                find(dictionary_window, "云词库已变化，请重新选择文件并核对预览。"),
            "snapshot conflict retried or lost");
    http->snapshot_conflict = false;
    const auto reads_before_cancel = http->snapshot_reads;
    g_idle_add(respond, GINT_TO_POINTER(GTK_RESPONSE_CANCEL));
    click(dictionary_window, "从完整快照恢复云词库");
    require(http->snapshot_reads == reads_before_cancel, "cancelled chooser downloaded snapshot");

    ExportAnswer snapshot_export{settings_store->config_path().parent_path().string(), "ui-snapshot-export.ndjson",
                                 false};
    const auto snapshot_export_file = snapshot_export.directory + "/" + snapshot_export.name;
    g_timeout_add(30, choose_export, &snapshot_export);
    click(dictionary_window, "导出完整云词库快照");
    ready();
    gchar *snapshot_bytes = nullptr;
    gsize snapshot_size = 0;
    require(g_file_get_contents(snapshot_export_file.c_str(), &snapshot_bytes, &snapshot_size, nullptr) &&
                std::string(snapshot_bytes, snapshot_size) == snapshot_fixture(http->dictionary_revision),
            "full snapshot export missing or changed");
    g_free(snapshot_bytes);
    require(g_file_set_contents(snapshot_export_file.c_str(), "keep existing file", -1, nullptr),
            "existing file fixture failed");
    snapshot_export.initialized = false;
    g_timeout_add(30, choose_export, &snapshot_export);
    click(dictionary_window, "导出完整云词库快照");
    ready();
    require(find(dictionary_window, "目标文件已存在，请选择新的文件名。") &&
                g_file_get_contents(snapshot_export_file.c_str(), &snapshot_bytes, &snapshot_size, nullptr) &&
                std::string(snapshot_bytes, snapshot_size) == "keep existing file",
            "snapshot export replaced existing file");
    g_free(snapshot_bytes);
    const auto reads_before_export_cancel = http->snapshot_reads;
    g_idle_add(respond, GINT_TO_POINTER(GTK_RESPONSE_CANCEL));
    click(dictionary_window, "导出完整云词库快照");
    require(http->snapshot_reads == reads_before_export_cancel, "cancelled snapshot export sent request");

    for (int index = 0; index < 51; ++index)
    {
        const auto number = std::to_string(index);
        http->dictionaries["pinyin"].push_back(
            boost::json::object{{"id", std::string(64 - number.size(), '0') + number},
                                {"kind", "pinyin"},
                                {"code", "test"},
                                {"word", "测试"},
                                {"weight", 10},
                                {"revision", 1},
                                {"updated_at", "now"}});
    }
    click(dictionary_window, "搜索云词条");
    ready();
    require(gtk_tree_model_iter_n_children(dictionary_model, nullptr) == 50, "first page incorrect");
    click(dictionary_window, "下一页");
    ready();
    require(gtk_tree_model_iter_n_children(dictionary_model, nullptr) == 1, "second page incorrect");
    click(dictionary_window, "上一页");
    ready();
    require(gtk_tree_model_iter_n_children(dictionary_model, nullptr) == 50, "previous page incorrect");
    http->dictionary_conflict = true;
    click(dictionary_window, "新增云词条");
    ready();
    require(find(dictionary_window, "词条已变化或与已有词条重复，请刷新后核对。") != nullptr &&
                gtk_tree_model_iter_n_children(dictionary_model, nullptr) == 0,
            "conflict retained stale rows");
    http->dictionary_conflict = false;
    http->block.store(true);
    click(dictionary_window, "搜索云词条");
    wait([&] { return http->entered.load(); });
    gtk_widget_destroy(dictionary_window);
    wait([&] { return http->cancelled.load(); });
    http->block.store(false);
    http->entered.store(false);
    http->cancelled.store(false);
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
