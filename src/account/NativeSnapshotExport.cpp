#include "NativeSnapshot.h"
#include "SnapshotContents.h"
#include "BackendAccountClient.h"
#include <boost/json.hpp>
#include <glib.h>
#include <sqlite3.h>
#include <memory>
#include <type_traits>

namespace metasequoia::linux_ime::account
{
namespace
{
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
Statement statement(sqlite3 *database, const char *sql)
{
    sqlite3_stmt *raw = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK)
        throw Failure(0);
    return {raw, sqlite3_finalize};
}
std::string kind_name(PersonalDictionaryKind kind)
{
    switch (kind)
    {
    case PersonalDictionaryKind::Pinyin:
        return "pinyin";
    case PersonalDictionaryKind::Wubi:
        return "wubi";
    case PersonalDictionaryKind::QuickPhrase:
        return "quick";
    case PersonalDictionaryKind::English:
        return "english";
    }
    throw Failure(400);
}
std::string identity(const std::string &kind, const std::string &key, const std::string &value)
{
    const auto encoded = boost::json::serialize(boost::json::array{kind, key, value});
    std::unique_ptr<gchar, decltype(&g_free)> digest(
        g_compute_checksum_for_data(G_CHECKSUM_SHA256, reinterpret_cast<const guchar *>(encoded.data()),
                                    encoded.size()),
        g_free);
    if (!digest)
        throw std::bad_alloc();
    return digest.get();
}
} // namespace
std::unique_ptr<PreparedSnapshot> export_native_snapshot(const RuntimePaths &paths,
                                                         const online::CancellationCheck &cancelled)
{
    const auto check = [&] {
        if (cancelled && cancelled())
            throw Failure(0, true);
    };
    check();
    sqlite3 *raw = nullptr;
    const auto opened =
        sqlite3_open_v2("", &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw, sqlite3_close);
    if (opened != SQLITE_OK)
        throw Failure(0);
    if (sqlite3_exec(raw,
                     "PRAGMA cache_size=-2048; PRAGMA temp_store=FILE; CREATE TABLE records(category INTEGER, sequence "
                     "INTEGER PRIMARY KEY, line TEXT); BEGIN;",
                     nullptr, nullptr, nullptr) != SQLITE_OK)
        throw Failure(0);
    sqlite3_progress_handler(
        raw, 1000,
        [](void *data) -> int {
            const auto &cancel = *static_cast<const online::CancellationCheck *>(data);
            return cancel && cancel() ? 1 : 0;
        },
        const_cast<online::CancellationCheck *>(&cancelled));
    auto insert = statement(raw, "INSERT INTO records(category,line) VALUES(?,?)");
    std::size_t count = 0, bytes = 0;
    const auto spool = [&](int category, boost::json::object record) {
        check();
        const auto line = boost::json::serialize(record) + "\n";
        if (++count > 500000 || line.size() >= 65536 || line.size() > 512 * 1024 * 1024 - bytes)
            throw Failure(400);
        bytes += line.size();
        sqlite3_reset(insert.get());
        if (sqlite3_bind_int(insert.get(), 1, category) != SQLITE_OK ||
            sqlite3_bind_text(insert.get(), 2, line.data(), static_cast<int>(line.size()), SQLITE_TRANSIENT) !=
                SQLITE_OK ||
            sqlite3_step(insert.get()) != SQLITE_DONE)
            throw Failure(0);
    };
    std::unique_ptr<GDateTime, decltype(&g_date_time_unref)> now(g_date_time_new_now_utc(), g_date_time_unref);
    std::unique_ptr<gchar, decltype(&g_free)> formatted(g_date_time_format(now.get(), "%Y-%m-%dT%H:%M:%SZ"), g_free);
    const std::string timestamp(formatted.get());
    stream_dictionary_state(paths, [&](const DictionaryStateRecord &record) {
        check();
        std::visit(
            [&](const auto &value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, DictionaryStateEntry>)
                {
                    const auto kind = kind_name(value.kind);
                    // Version 1 has no separate display field. Never silently discard
                    // state that cannot round-trip through its existing wire format.
                    if ((!value.deleted && kind == "english" && value.display != value.value) ||
                        ((value.deleted || kind != "english") && !value.display.empty()))
                        throw Failure(400);
                    boost::json::object data{{"id", value.user_inserted ? identity(kind, value.key, value.value) : ""},
                                             {"kind", kind},
                                             {"code", value.key},
                                             {"word", value.value},
                                             {"weight", value.weight},
                                             {"revision", 1},
                                             {"updated_at", timestamp},
                                             {"user_inserted", value.user_inserted}};
                    if (value.user_inserted && !value.deleted)
                        spool(1, {{"type", "entry"}, {"data", data}});
                    spool(2, {{"type", "overlay"}, {"deleted", value.deleted}, {"data", std::move(data)}});
                }
                else
                {
                    boost::json::object data{{"context", value.context}, {"code", value.key}, {"word", value.value}};
                    if constexpr (std::is_same_v<T, DictionaryStatePosition>)
                    {
                        data["position"] = value.position;
                        spool(3, {{"type", "position"}, {"data", std::move(data)}});
                    }
                    else
                    {
                        data["count"] = value.count;
                        spool(4, {{"type", "selection"}, {"data", std::move(data)}});
                    }
                }
            },
            record);
        return true;
    });
    check();
    if (sqlite3_exec(raw, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK)
        throw Failure(0);
    auto snapshot = PreparedSnapshot::receive(
        [&](const online::HttpResponseSink &sink) {
            std::unique_ptr<GChecksum, decltype(&g_checksum_free)> hash(g_checksum_new(G_CHECKSUM_SHA256),
                                                                        g_checksum_free);
            const auto emit = [&](const std::string &line) {
                check();
                if (!sink(line.data(), line.size()))
                    throw Failure(0);
                g_checksum_update(hash.get(), reinterpret_cast<const guchar *>(line.data()), line.size());
            };
            emit("{\"type\":\"header\",\"format\":\"msime-dictionary-snapshot\",\"version\":1,\"revision\":1}\n");
            auto rows = statement(raw, "SELECT line FROM records ORDER BY category,sequence");
            int result;
            while ((result = sqlite3_step(rows.get())) == SQLITE_ROW)
                emit({reinterpret_cast<const char *>(sqlite3_column_text(rows.get(), 0)),
                      static_cast<std::size_t>(sqlite3_column_bytes(rows.get(), 0))});
            if (result != SQLITE_DONE)
                throw Failure(0);
            const auto footer =
                boost::json::serialize(boost::json::object{
                    {"type", "footer"}, {"records", count + 1}, {"sha256", g_checksum_get_string(hash.get())}}) +
                "\n";
            check();
            if (!sink(footer.data(), footer.size()))
                throw Failure(0);
        },
        cancelled);
    validate_snapshot_contents(*snapshot, cancelled);
    return snapshot;
}
} // namespace metasequoia::linux_ime::account
