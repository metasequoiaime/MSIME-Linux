#include "NativeRestoreReview.h"
#include "DictionaryBootstrap.h"
#include "DictionaryLease.h"
#include "ToolLauncher.h"
#include "NativeInstallation.h"
#include "NativeResources.h"
#include "NativeSnapshot.h"
#include "SnapshotTransfer.h"
#include <gio/gio.h>
#include <algorithm>

namespace metasequoia::linux_ime::account
{
namespace
{
using Variant = std::unique_ptr<GVariant, decltype(&g_variant_unref)>;
void check_cancelled(GCancellable *cancelled)
{
    if (cancelled && g_cancellable_is_cancelled(cancelled))
        throw Failure(0, true);
}
User checked_user(AccountSession &session, std::uint64_t generation)
{
    const auto current = session.snapshot();
    if (current.generation != generation || !current.user)
        throw Failure(0, true);
    return *current.user;
}
Variant call(GDBusConnection *connection, const std::string &owner, const char *method, GVariant *parameters,
             const char *result, GCancellable *cancelled)
{
    Variant response(g_dbus_connection_call_sync(connection, owner.c_str(), "/app/msime/Dictionary",
                                                 "app.msime.Dictionary", method, parameters, G_VARIANT_TYPE(result),
                                                 G_DBUS_CALL_FLAGS_NO_AUTO_START, 120000, cancelled, nullptr),
                     g_variant_unref);
    if (!response)
    {
        check_cancelled(cancelled);
        throw Failure(503);
    }
    return response;
}
NativeInstallation installation(const std::filesystem::path &root)
{
    return {root / "runtime", {root, root, root / "cache", root}};
}
struct Discard
{
    std::filesystem::path root;
    std::string generation;
};
void discard_worker(GTask *task, gpointer, gpointer data, GCancellable *)
{
    const auto &discard = *static_cast<Discard *>(data);
    try
    {
        const auto lease = DictionaryLease::acquire(discard.root, DictionaryLease::Mode::Shared);
        if (lease)
        {
            const auto store = installation(discard.root);
            if (store.active().generation != discard.generation)
            {
                std::error_code error;
                std::filesystem::remove_all(store.generation(discard.generation), error);
            }
        }
    }
    catch (...)
    {
    } // Retain an unused generation when safe cleanup cannot be proven.
    g_task_return_boolean(task, TRUE);
}
} // namespace
NativeRestoreLocations installed_native_restore_locations()
{
    const auto legacy = metasequoia::RuntimePaths::legacy();
    const auto here = running_program_directory();
    NativeRestoreLocations result{legacy.user_data, here / "metasequoia-native-resources", {legacy.resources}};
    for (const auto &directory :
         {result.tools, here.parent_path() / METASEQUOIA_NATIVE_RESOURCE_LIBEXECDIR / "metasequoia-native-resources"})
    {
        std::error_code error;
        if (std::filesystem::is_regular_file(directory / "native-resource-lock.json", error))
        {
            result.tools = directory;
            break;
        }
    }
    result.sources.push_back(here.parent_path() / "share" / "metasequoiaime");
    for (const auto &directory : system_data_directories())
        result.sources.push_back(directory);
    return result;
}
std::unique_ptr<NativeRestoreReview> NativeRestoreReview::prepare(std::shared_ptr<AccountSession> session,
                                                                  std::uint64_t generation,
                                                                  NativeRestoreLocations locations,
                                                                  GCancellable *cancelled)
{
    check_cancelled(cancelled);
    if (!session || !locations.root.is_absolute() || !locations.tools.is_absolute() || locations.sources.empty())
        throw Failure(400);
    std::unique_ptr<NativeRestoreReview> review(new NativeRestoreReview);
    review->session_ = std::move(session);
    review->user_ = checked_user(*review->session_, generation);
    review->account_generation_ = generation;
    review->locations_ = std::move(locations);
    auto *connection = g_bus_get_sync(G_BUS_TYPE_SESSION, cancelled, nullptr);
    if (!connection)
    {
        check_cancelled(cancelled);
        throw Failure(503);
    }
    review->connection_ = {connection, [](GDBusConnection *value) { g_object_unref(value); }};
    Variant owner(g_dbus_connection_call_sync(connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                              "org.freedesktop.DBus", "GetNameOwner",
                                              g_variant_new("(s)", "app.msime.Dictionary"), G_VARIANT_TYPE("(s)"),
                                              G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, cancelled, nullptr),
                  g_variant_unref);
    if (!owner)
    {
        check_cancelled(cancelled);
        throw Failure(503);
    }
    const gchar *name;
    g_variant_get(owner.get(), "(&s)", &name);
    review->owner_ = name; // Pin the process, not just a replaceable well-known name.
    const auto current = call(connection, review->owner_, "Inspect", nullptr, "(sss)", cancelled);
    const gchar *root, *token, *revision;
    g_variant_get(current.get(), "(&s&s&s)", &root, &token, &revision);
    if (std::filesystem::path(root).lexically_normal() != review->locations_.root.lexically_normal())
        throw Failure(503);
    review->token_ = token;
    review->revision_ = revision;
    if (review->token_.size() > 512 || review->revision_.size() != 64 ||
        !std::all_of(review->revision_.begin(), review->revision_.end(),
                     [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        throw Failure(503);
    auto snapshot = download_validated_snapshot(
        *review->session_, generation, [cancelled] { return cancelled && g_cancellable_is_cancelled(cancelled); });
    const auto lease = DictionaryLease::acquire(review->locations_.root, DictionaryLease::Mode::Shared);
    if (!lease)
        throw Failure(423);
    auto store = installation(review->locations_.root);
    std::optional<NativeResources> resources;
    for (const auto &source : review->locations_.sources)
    {
        try
        {
            resources = prepare_native_resources(review->locations_.tools, source,
                                                 review->locations_.root / "runtime" / "resources", cancelled);
            break;
        }
        catch (const Failure &error)
        {
            if (error.cancelled())
                throw;
        }
    }
    if (!resources)
        throw Failure(500);
    gchar *identifier = g_uuid_string_random();
    review->generation_ = identifier;
    g_free(identifier);
    review->content_ = resources->content_id;
    stage_native_snapshot(*snapshot, resources->directory, store.generation(review->generation_), review->content_,
                          [cancelled] { return cancelled && g_cancellable_is_cancelled(cancelled); });
    review->staged_ = true;
    check_cancelled(cancelled);
    checked_user(*review->session_, generation);
    review->source_ = snapshot->envelope();
    return review;
}
NativeRestoreReview::~NativeRestoreReview()
{
    if (staged_ && !attempted_)
    {
        auto *task = g_task_new(nullptr, nullptr, nullptr, nullptr);
        g_task_set_task_data(task, new Discard{locations_.root, generation_},
                             [](gpointer data) { delete static_cast<Discard *>(data); });
        g_task_run_in_thread(task, discard_worker);
        g_object_unref(task);
    }
}
NativeRestoreResult NativeRestoreReview::publish(GCancellable *cancelled)
{
    check_cancelled(cancelled);
    if (!staged_ || attempted_)
        throw Failure(409);
    bool durable = false;
    session_->with_current_user(account_generation_, user_.id, [&] {
        check_cancelled(cancelled);
        attempted_ = true;
        // Once sent, closing the window cannot retract a publication. Await its
        // reply without cancellation; timeout/disconnect retains the generation.
        const auto result =
            call(connection_.get(), owner_, "Publish",
                 g_variant_new("(ssss)", generation_.c_str(), content_.c_str(), token_.c_str(), revision_.c_str()),
                 "(sb)", nullptr);
        const gchar *status;
        gboolean synced;
        g_variant_get(result.get(), "(&sb)", &status, &synced);
        if (std::string(status) == "busy" || std::string(status) == "conflict")
        {
            attempted_ = false;
            throw Failure(std::string(status) == "busy" ? 423 : 409);
        }
        if (std::string(status) != "published")
            throw Failure(503);
        durable = synced;
    });
    return {durable};
}
} // namespace metasequoia::linux_ime::account
