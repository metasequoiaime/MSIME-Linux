#include "account/AccountPanel.h"
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
    SecretLookupResult lookup(SecretKind, std::string_view) const override
    {
        return record;
    }
    bool store(SecretKind, std::string_view, std::string_view value, std::string *) override
    {
        record = {SecretStatus::Found, std::string(value), {}};
        return true;
    }
    bool erase(SecretKind, std::string_view, std::string *) override
    {
        record = {};
        return true;
    }
};
struct Transport final : online::HttpTransport
{
    bool linking = false, linked = false;
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
    gtk_init(&argc, &argv);
    auto secrets = std::make_shared<Store>();
    auto http = std::make_shared<Transport>();
    auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    auto *panel = account::create_account_panel(secrets, http);
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
    require(secrets->record.status == SecretStatus::Found, "login credentials not saved");
    require(std::string(gtk_entry_get_text(GTK_ENTRY(code))).empty(), "verification code not cleared");
    auto *nickname = find(panel, "昵称");
    require(std::string(gtk_entry_get_text(GTK_ENTRY(nickname))) == "测试用户", "profile not loaded");
    gtk_entry_set_text(GTK_ENTRY(nickname), "新昵称");
    click(panel, "保存昵称");
    wait([&] { return gtk_widget_get_sensitive(panel); });
    require(std::string(gtk_entry_get_text(GTK_ENTRY(nickname))) == "新昵称" &&
                boost::json::parse(secrets->record.value).at("user").at("display_name").as_string() == "新昵称",
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
    require(boost::json::parse(secrets->record.value).at("user").at("id").as_string() == "synthetic",
            "binding replaced user");
    click(panel, "刷新资料");
    wait([&] { return gtk_widget_get_sensitive(panel); });
    click(panel, "退出登录");
    wait([&] { return gtk_widget_get_sensitive(panel); });
    require(secrets->record.status == SecretStatus::NotFound, "logout credentials remained");
    gtk_widget_destroy(window);
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
    std::cout << "GTK account login, logout and close-during-request tests passed\n";
}
