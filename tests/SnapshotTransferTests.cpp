#include "account/SnapshotTransfer.h"
#include <glib.h>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace metasequoia::linux_ime;
namespace
{
void require(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}
template <class F> void fails(F action, long status)
{
    try
    {
        action();
    }
    catch (const account::Failure &error)
    {
        require(error.status() == status, "wrong error");
        return;
    }
    throw std::runtime_error("unexpected success");
}
std::string fixture(int revision)
{
    const auto body = "{\"type\":\"header\",\"format\":\"msime-dictionary-snapshot\",\"version\":1,\"revision\":" +
                      std::to_string(revision) + "}\n";
    gchar *hash =
        g_compute_checksum_for_data(G_CHECKSUM_SHA256, reinterpret_cast<const guchar *>(body.data()), body.size());
    auto result = body + "{\"type\":\"footer\",\"records\":1,\"sha256\":\"" + hash + "\"}\n";
    g_free(hash);
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
struct Transport final : online::HttpTransport
{
    int calls = 0, uploads = 0, revision = 3;
    bool broken = false;
    std::string uploaded;
    online::HttpResponse perform(const online::HttpRequest &request, const online::CancellationCheck &) override
    {
        ++calls;
        if (request.url.find("/logout") != std::string::npos)
            return {200, "{}", {}};
        if (request.method == online::HttpMethod::Get)
        {
            auto body = broken ? std::string("truncated") : fixture(revision);
            request.response_sink(body.data(), body.size());
            return {200, {}, {}};
        }
        ++uploads;
        if (request.url.find("?revision=" + std::to_string(revision)) == std::string::npos)
            return {409, {}, {}};
        uploaded.clear();
        char chunk[19];
        while (uploaded.size() < request.body_size)
        {
            auto count = request.body_source(uploaded.size(), chunk, sizeof(chunk));
            require(count > 0, "upload EOF");
            uploaded.append(chunk, count);
        }
        return {200, "{\"revision\":" + std::to_string(++revision) + ",\"reset\":true}", {}};
    }
};
} // namespace
int main()
{
    gchar *root = g_dir_make_tmp("msime-review-test-XXXXXX", nullptr);
    require(root, "temporary directory failed");
    std::filesystem::path directory(root);
    g_free(root);
    struct Cleanup
    {
        std::filesystem::path path;
        ~Cleanup()
        {
            std::filesystem::remove_all(path);
        }
    } cleanup{directory};
    const auto file = directory / "snapshot.ndjson";
    const auto source = fixture(1);
    {
        std::ofstream output(file);
        output << source;
    }
    Store secrets;
    account::AccountCredentialStore credentials(secrets);
    credentials.save({{std::string(64, 'a'), std::string(64, 'b'), 900, {"synthetic", "测试", "now"}}, 1900});
    Transport transport;
    account::BackendAccountClient client(transport);
    auto session = std::make_shared<account::AccountSession>(client, credentials, [] { return std::int64_t(1000); });
    const auto state = session->restore();
    auto review = account::SnapshotRestoreReview::prepare(session, state.generation, file.string());
    require(transport.uploads == 0 && review->source().revision == 1 && review->target().revision == 3,
            "prepare changed cloud or metadata");
    require(review->user().id == "synthetic", "review lost account binding");
    {
        std::ofstream output(file);
        output << "changed after preview";
    }
    require(review->restore() == 4 && transport.uploaded == source, "restore reread modified source");
    fails([&] { review->restore(); }, 409);
    const auto before = transport.calls;
    fails([&] { account::SnapshotRestoreReview::prepare(session, state.generation, file.string()); }, 400);
    require(transport.calls == before, "invalid source contacted cloud");
    transport.broken = true;
    fails([&] { account::download_validated_snapshot(*session, state.generation); }, 400);
    transport.broken = false;
    auto valid = account::download_validated_snapshot(*session, state.generation);
    require(valid->envelope().revision == 4, "download did not use current cloud state");
    session->logout(state.generation);
    const auto after = transport.calls;
    fails([&] { review->restore(); }, 0);
    require(transport.calls == after, "review uploaded after logout");
    std::cout << "Snapshot download, preview immutability, CAS, invalid data and logout binding passed\n";
}
