#include "account/DictionaryWindow.h"
#include "account/NativeInstallation.h"
#include "DictionaryLease.h"
#include <boost/json.hpp>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
using namespace metasequoia::linux_ime;
namespace
{
void require(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}
template <class F> void wait(F done)
{
    const auto deadline = g_get_monotonic_time() + 30 * G_USEC_PER_SEC;
    while (!done() && g_get_monotonic_time() < deadline)
    {
        while (g_main_context_iteration(nullptr, FALSE))
        {
        }
        g_usleep(1000);
    }
    require(done(), "native restore UI timed out");
}
GtkWidget *find(GtkWidget *root, const char *text)
{
    if ((GTK_IS_BUTTON(root) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(root)), text) == 0) ||
        (GTK_IS_LABEL(root) && g_strcmp0(gtk_label_get_text(GTK_LABEL(root)), text) == 0))
        return root;
    if (!GTK_IS_CONTAINER(root))
        return nullptr;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root));
    GtkWidget *result = nullptr;
    for (auto *node = children; node && !result; node = node->next)
        result = find(GTK_WIDGET(node->data), text);
    g_list_free(children);
    return result;
}
struct Store final : SecretStore
{
    SecretLookupResult value;
    SecretLookupResult lookup(SecretKind, std::string_view) const override
    {
        return value;
    }
    bool store(SecretKind, std::string_view, std::string_view text, std::string *) override
    {
        value = {SecretStatus::Found, std::string(text), {}};
        return true;
    }
    bool erase(SecretKind, std::string_view, std::string *) override
    {
        value = {};
        return true;
    }
};
std::string fixture()
{
    std::string body = R"({"type":"header","format":"msime-dictionary-snapshot","version":1,"revision":7})"
                       "\n";
    auto data = boost::json::object{{"id", "fixture"},
                                    {"kind", "english"},
                                    {"code", "nativefixture"},
                                    {"word", "Nativefixture"},
                                    {"weight", 150},
                                    {"revision", 1},
                                    {"updated_at", "2026-09-09T00:00:00Z"},
                                    {"user_inserted", true}};
    body += boost::json::serialize(boost::json::object{{"type", "entry"}, {"data", data}}) + "\n";
    body += boost::json::serialize(boost::json::object{{"type", "overlay"}, {"deleted", false}, {"data", data}}) + "\n";
    gchar *hash =
        g_compute_checksum_for_data(G_CHECKSUM_SHA256, reinterpret_cast<const guchar *>(body.data()), body.size());
    body += boost::json::serialize(boost::json::object{{"type", "footer"}, {"records", 3}, {"sha256", hash}}) + "\n";
    g_free(hash);
    return body;
}
struct Transport final : online::HttpTransport
{
    std::string body = fixture();
    std::atomic<int> reads{0}, writes{0};
    std::atomic<bool> block{false}, entered{false}, cancelled{false};
    online::HttpResponse perform(const online::HttpRequest &request, const online::CancellationCheck &check) override
    {
        if (request.url.find("/logout") != std::string::npos)
            return {200, "{}", {}};
        require(request.method == online::HttpMethod::Get, "native restore attempted a cloud mutation");
        ++reads;
        if (block)
        {
            entered = true;
            while (!check())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            cancelled = true;
            return {0, {}, "cancelled"};
        }
        require(request.response_sink(body.data(), body.size()), "snapshot sink rejected fixture");
        return {200, {}, {}};
    }
};
struct Service
{
    std::filesystem::path root;
    bool wrong_root = false;
    std::string mode = "published", last_generation;
    int requests = 0;
    account::NativeInstallation installation() const
    {
        return {root / "runtime", {root, root, root / "cache", root}};
    }
    std::size_t generations() const
    {
        const auto folder = root / "runtime" / "generations";
        if (!std::filesystem::exists(folder))
            return 0;
        return std::distance(std::filesystem::directory_iterator(folder), std::filesystem::directory_iterator{});
    }
};
void method(GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *name, GVariant *parameters,
            GDBusMethodInvocation *invocation, gpointer data)
{
    auto &service = *static_cast<Service *>(data);
    auto installation = service.installation();
    const auto current = installation.active();
    const std::string revision(64, 'a');
    if (std::string(name) == "Inspect")
    {
        const auto root = service.wrong_root ? service.root / "wrong" : service.root;
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(sss)", root.c_str(), current.token.c_str(), revision.c_str()));
        return;
    }
    ++service.requests;
    const gchar *generation, *content, *token, *expected_revision;
    g_variant_get(parameters, "(&s&s&s&s)", &generation, &content, &token, &expected_revision);
    require(current.token == token && revision == expected_revision, "preview identity was not preserved");
    service.last_generation = generation;
    if (service.mode == "busy" || service.mode == "conflict")
    {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(sb)", service.mode.c_str(), FALSE));
        return;
    }
    const auto lease = DictionaryLease::acquire(service.root, DictionaryLease::Mode::Exclusive);
    require(bool(lease), "review retained a session lease during publication");
    const auto result = installation.publish(generation, content, token);
    require(result.published && result.durable, "prepared generation could not be published");
    if (service.mode == "uncertain")
        g_dbus_method_invocation_return_dbus_error(invocation, "app.msime.Dictionary.Error", "synthetic lost reply");
    else
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(sb)", "published", TRUE));
}
} // namespace
int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    require(argc == 4 || argc == 5, "expected tools, source, private root and optional live mode");
    const bool live = argc == 5 && std::string(argv[4]) == "live";
    Service service;
    service.root = argv[3];
    auto *connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    require(connection, "session bus unavailable");
    guint registration = 0;
    if (live)
    {
        wait([&] {
            auto *reply = g_dbus_connection_call_sync(
                connection, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
                g_variant_new("(s)", "app.msime.Dictionary"), G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 1000,
                nullptr, nullptr);
            gboolean owned = FALSE;
            if (reply)
            {
                g_variant_get(reply, "(b)", &owned);
                g_variant_unref(reply);
            }
            return bool(owned);
        });
    }
    else
    {
        auto *name = g_dbus_connection_call_sync(connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                 "org.freedesktop.DBus", "RequestName",
                                                 g_variant_new("(su)", "app.msime.Dictionary", 0u),
                                                 G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr);
        require(name, "test service name unavailable");
        g_variant_unref(name);
        auto *node = g_dbus_node_info_new_for_xml(R"XML(<node><interface name="app.msime.Dictionary">
      <method name="Inspect"><arg type="s" direction="out"/><arg type="s" direction="out"/><arg type="s" direction="out"/></method>
      <method name="Publish"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="s" direction="in"/>
      <arg type="s" direction="in"/><arg type="s" direction="out"/><arg type="b" direction="out"/></method>
      </interface></node>)XML",
                                                  nullptr);
        static const GDBusInterfaceVTable vtable{method, nullptr, nullptr, {}};
        registration = g_dbus_connection_register_object(connection, "/app/msime/Dictionary", node->interfaces[0],
                                                         &vtable, &service, nullptr, nullptr);
        require(registration, "test service registration failed");
        g_dbus_node_info_unref(node);
    }
    Store secrets;
    account::AccountCredentialStore credentials(secrets);
    credentials.save({{std::string(64, 'a'), std::string(64, 'b'), 900, {"synthetic", "测试账号", "now"}}, 1900});
    Transport transport;
    account::BackendAccountClient client(transport);
    auto session = std::make_shared<account::AccountSession>(client, credentials, [] { return std::int64_t(1000); });
    const auto generation = session->restore().generation;
    auto *parent = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    account::NativeRestoreLocations locations{service.root, argv[1], {std::filesystem::path(argv[2])}};
    auto *window = account::create_dictionary_window(GTK_WINDOW(parent), session, generation, locations);
    const auto click = [&] {
        auto *button = find(window, "将完整云词库应用到本机");
        require(button && gtk_widget_is_sensitive(button), "restore button disabled");
        gtk_button_clicked(GTK_BUTTON(button));
    };
    const auto ready = [&] { wait([&] { return gtk_widget_is_sensitive(find(window, "将完整云词库应用到本机")); }); };
    const auto answer = [&](int response, bool logout = false) {
        struct Answer
        {
            int response;
            bool answered = false;
            std::function<void()> before;
        };
        Answer answer{response, false, [&] {
                          transport.body = "changed after preview";
                          if (logout)
                              session->logout(generation);
                      }};
        g_timeout_add(
            10,
            +[](gpointer data) -> gboolean {
                auto &answer = *static_cast<Answer *>(data);
                auto *windows = gtk_window_list_toplevels();
                GtkWidget *dialog = nullptr;
                for (auto *item = windows; item; item = item->next)
                    if (GTK_IS_MESSAGE_DIALOG(item->data))
                        dialog = GTK_WIDGET(item->data);
                g_list_free(windows);
                if (!dialog)
                    return G_SOURCE_CONTINUE;
                gchar *text = nullptr;
                g_object_get(dialog, "text", &text, nullptr);
                require(text && std::string(text).find("测试账号") != std::string::npos &&
                            std::string(text).find("个人词条 1 条") != std::string::npos,
                        "native preview omitted identity or counts");
                g_free(text);
                auto *default_button = gtk_window_get_default_widget(GTK_WINDOW(dialog));
                require(default_button && g_strcmp0(gtk_button_get_label(GTK_BUTTON(default_button)), "取消") == 0,
                        "destructive preview default was not cancel");
                answer.before();
                answer.answered = true;
                gtk_dialog_response(GTK_DIALOG(dialog), answer.response);
                return G_SOURCE_REMOVE;
            },
            &answer);
        transport.body = fixture();
        click();
        wait([&] { return answer.answered; });
        ready();
        transport.body = fixture();
    };
    if (!live)
    {
        service.wrong_root = true;
        click();
        ready();
        require(transport.reads == 0, "wrong native process fetched cloud data");
        service.wrong_root = false;
        answer(GTK_RESPONSE_CANCEL);
        require(service.requests == 0, "cancelled preview published");
        wait([&] { return service.generations() == 0; });
        for (const auto *status : {"busy", "conflict"})
        {
            service.mode = status;
            answer(GTK_RESPONSE_ACCEPT);
            require(service.installation().active().generation.empty(), "rejected publication changed the marker");
            wait([&] { return service.generations() == 0; });
        }
    }
    service.mode = "published";
    answer(GTK_RESPONSE_ACCEPT);
    require(find(window, "完整云词库已应用到本机。"), "confirmed publication was not reported");
    bool found = false;
    metasequoia::stream_dictionary_state(service.installation().active().paths, [&](const auto &record) {
        if (const auto *entry = std::get_if<metasequoia::DictionaryStateEntry>(&record))
            found |= entry->value == "Nativefixture";
        return true;
    });
    require(found, "publication did not use the frozen cloud content");
    if (!live)
    {
        service.mode = "uncertain";
        answer(GTK_RESPONSE_ACCEPT);
        require(find(window, "切换结果未确认，请重新连接输入法并核对，勿直接重复提交。"),
                "lost reply claimed a definite outcome");
        require(service.generations() == 2, "uncertain publication deleted a generation");
        transport.block = true;
        click();
        wait([&] { return transport.entered.load(); });
        gtk_widget_destroy(window);
        wait([&] { return transport.cancelled.load() && session.use_count() == 1; });
        transport.block = false;
        window = account::create_dictionary_window(GTK_WINDOW(parent), session, generation, locations);
        const auto before = service.requests;
        answer(GTK_RESPONSE_ACCEPT, true);
        require(service.requests == before, "account replacement crossed the publication boundary");
        wait([&] { return service.generations() == 2; });
    }
    gtk_widget_destroy(window);
    gtk_widget_destroy(parent);
    wait([&] { return session.use_count() == 1; });
    if (registration)
        g_dbus_connection_unregister_object(connection, registration);
    g_object_unref(connection);
    std::cout << (live ? "Native restore GUI published frozen cloud content through the real IBus service\n"
                       : "Native restore GUI verified preview consent, conflicts, frozen content, cancellation and "
                         "account binding\n");
}
