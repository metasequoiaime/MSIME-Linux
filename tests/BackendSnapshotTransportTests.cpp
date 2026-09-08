#include "account/AccountSession.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>
using namespace metasequoia::linux_ime;
namespace
{
void require(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}
template <class Action> void fails(Action action, long status)
{
    try
    {
        action();
    }
    catch (const account::Failure &error)
    {
        require(error.status() == status, "wrong failure status");
        return;
    }
    throw std::runtime_error("request unexpectedly succeeded");
}
struct Transport final : online::HttpTransport
{
    int calls = 0;
    online::HttpRequest last;
    std::function<online::HttpResponse(const online::HttpRequest &)> handler;
    online::HttpResponse perform(const online::HttpRequest &request, const online::CancellationCheck &) override
    {
        ++calls;
        last = request;
        return handler(request);
    }
};
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
} // namespace
int main()
{
    Transport transport;
    account::BackendAccountClient client(transport);
    const std::string token(64, 'a');
    std::string received;
    online::HttpResponseSink sink = [&](const char *data, std::size_t size) {
        received.append(data, size);
        return true;
    };
    transport.handler = [&](const online::HttpRequest &request) {
        require(request.method == online::HttpMethod::Get &&
                    request.url == "https://api.msime.app/v1/users/me/dictionary/snapshot",
                "wrong download endpoint");
        require(request.max_response_bytes == 512U * 1024U * 1024U && request.total_timeout == std::chrono::minutes(2),
                "wrong snapshot bounds");
        require(std::find(request.headers.begin(), request.headers.end(), "Authorization: Bearer " + token) !=
                    request.headers.end(),
                "authorization missing");
        require(request.response_sink("part1", 5) && request.response_sink("part2", 5), "download sink failed");
        return online::HttpResponse{200, {}, {}};
    };
    client.download_snapshot(sink, token);
    require(received == "part1part2", "download data changed");
    const int before = transport.calls;
    fails([&] { client.download_snapshot({}, token); }, 400);
    fails([&] { client.download_snapshot(sink, "bad"); }, 400);
    require(transport.calls == before, "invalid download made network request");
    transport.handler = [](const online::HttpRequest &request) {
        request.response_sink("x", 512U * 1024U * 1024U + 1);
        return online::HttpResponse{200, {}, {}};
    };
    fails([&] { client.download_snapshot(sink, token); }, 0);
    transport.handler = [](const online::HttpRequest &) { return online::HttpResponse{200, {}, {}}; };
    fails([&] { client.download_snapshot(sink, token); }, 0);
    transport.handler = [](const online::HttpRequest &) {
        return online::HttpResponse{200, "buffered unexpectedly", {}};
    };
    fails([&] { client.download_snapshot(sink, token); }, 0);
    const std::string upload = "frozen snapshot fixture";
    online::HttpBodySource source = [&](std::size_t offset, char *data, std::size_t capacity) {
        auto size = std::min(capacity, upload.size() - offset);
        std::memcpy(data, upload.data() + offset, size);
        return size;
    };
    transport.handler = [&](const online::HttpRequest &request) {
        require(request.method == online::HttpMethod::Put &&
                    request.url == "https://api.msime.app/v1/users/me/dictionary/snapshot?revision=7",
                "wrong restore CAS");
        require(request.body.empty() && request.body_size == upload.size() && request.max_response_bytes == 65536,
                "restore buffered source");
        require(std::find(request.headers.begin(), request.headers.end(), "Content-Type: application/x-ndjson") !=
                    request.headers.end(),
                "wrong restore media type");
        char bytes[64];
        auto count = request.body_source(0, bytes, sizeof(bytes));
        require(std::string(bytes, count) == upload, "wrong upload bytes");
        return online::HttpResponse{200, R"({"revision":8,"reset":true})", {}};
    };
    require(client.restore_snapshot(upload.size(), source, 7, token) == 8, "restore revision missing");
    fails([&] { client.restore_snapshot(0, source, 7, token); }, 400);
    fails([&] { client.restore_snapshot(512U * 1024U * 1024U + 1, source, 7, token); }, 400);
    fails([&] { client.restore_snapshot(1, source, -1, token); }, 400);
    fails([&] { client.restore_snapshot(1, source, std::numeric_limits<std::int64_t>::max(), token); }, 400);
    for (const auto *response : {R"({"revision":7,"reset":true})", R"({"revision":8,"reset":false})",
                                 R"({"revision":8,"reset":"true"})", R"({"revision":true,"reset":true})"})
    {
        transport.handler = [&](const online::HttpRequest &) { return online::HttpResponse{200, response, {}}; };
        fails([&] { client.restore_snapshot(upload.size(), source, 7, token); }, 0);
    }
    for (long status : {401, 409, 413, 429, 503})
    {
        transport.handler = [&](const online::HttpRequest &) {
            return online::HttpResponse{status, "upstream detail", {}};
        };
        const auto calls = transport.calls;
        fails([&] { client.restore_snapshot(upload.size(), source, 7, token); }, status);
        require(transport.calls == calls + 1, "restore retried on failure");
    }
    Store secrets;
    account::AccountCredentialStore storage(secrets);
    storage.save({{token, std::string(64, 'b'), 900, {"synthetic", "测试", "now"}}, 1900});
    account::AccountSession session(client, storage, [] { return std::int64_t(1000); });
    const auto current = session.restore();
    transport.handler = [](const online::HttpRequest &) { return online::HttpResponse{401, "", {}}; };
    fails([&] { session.download_snapshot(current.generation, sink); }, 401);
    require(!session.snapshot().user && !storage.load(), "download 401 retained account credentials");
    const auto calls = transport.calls;
    fails([&] { session.restore_snapshot(current.generation, upload.size(), source, 7); }, 0);
    require(transport.calls == calls, "stale snapshot generation made request");
    std::cout << "Snapshot transport bounds, CAS, failures and account binding passed\n";
}
