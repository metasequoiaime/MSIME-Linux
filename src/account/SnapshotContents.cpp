#include "SnapshotContents.h"
#include "BackendAccountClient.h"
#include <boost/json.hpp>
#include <glib.h>
#include <sqlite3.h>
#include <algorithm>
#include <memory>
#include <regex>
namespace metasequoia::linux_ime::account
{
namespace
{
using Object = boost::json::object;
std::string text(const Object &object, const char *key)
{
    const auto *value = object.if_contains(key);
    if (!value || !value->is_string())
        throw Failure(400);
    return std::string(value->as_string());
}
std::string bounded(const Object &object, const char *key, std::size_t maximum)
{
    auto value = text(object, key);
    if (value.empty() || value.size() > maximum || value.find_first_of(std::string("\0\t\r\n", 4)) != std::string::npos)
        throw Failure(400);
    return value;
}
std::int64_t integer(const Object &object, const char *key, std::int64_t minimum, std::int64_t maximum)
{
    const auto *value = object.if_contains(key);
    if (!value || !value->is_int64() || value->as_int64() < minimum || value->as_int64() > maximum)
        throw Failure(400);
    return value->as_int64();
}
void timestamp(std::string value)
{
    static const std::regex syntax(
        R"(^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}([.,][0-9]+)?(Z|[+-][0-9]{2}:[0-9]{2})$)");
    if (value.size() > 128 || !std::regex_match(value, syntax))
        throw Failure(400);
    const auto number = [&](int start, int count) { return std::stoi(value.substr(start, count)); };
    if (!g_date_valid_dmy(number(8, 2), static_cast<GDateMonth>(number(5, 2)), number(0, 4)) || number(11, 2) > 23 ||
        number(14, 2) > 59 || number(17, 2) > 59)
        throw Failure(400);
    if (value.back() != 'Z' && (number(value.size() - 5, 2) > 23 || number(value.size() - 2, 2) > 59))
        throw Failure(400);
    std::replace(value.begin(), value.end(), ',', '.');
    std::unique_ptr<GDateTime, decltype(&g_date_time_unref)> parsed(
        g_date_time_new_from_iso8601(value.c_str(), nullptr), g_date_time_unref);
    if (!parsed ||
        (g_date_time_to_unix(parsed.get()) == -62135596800LL && g_date_time_get_microsecond(parsed.get()) == 0))
        throw Failure(400);
}
struct Database
{
    sqlite3 *database = nullptr;
    sqlite3_stmt *insert = nullptr;
    ~Database()
    {
        sqlite3_finalize(insert);
        sqlite3_close(database);
    }
    void execute(const char *sql)
    {
        if (sqlite3_exec(database, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
            throw Failure(400);
    }
};
} // namespace
void validate_snapshot_contents(const PreparedSnapshot &snapshot, const online::CancellationCheck &cancelled)
{
    if (cancelled && cancelled())
        throw Failure(0, true);
    Database index;
    // An empty SQLite filename creates a private temporary database, deleted on close.
    if (sqlite3_open_v2("", &index.database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                        nullptr) != SQLITE_OK)
        throw Failure(0);
    sqlite3_progress_handler(
        index.database, 1000,
        [](void *context) -> int {
            const auto &check = *static_cast<const online::CancellationCheck *>(context);
            return check && check() ? 1 : 0;
        },
        const_cast<online::CancellationCheck *>(&cancelled));
    index.execute(R"(
      PRAGMA cache_size=-2048; PRAGMA temp_store=FILE;
      CREATE TABLE records(type TEXT, scope TEXT, code TEXT, word TEXT, id TEXT, weight INTEGER, owned INTEGER, deleted INTEGER, slot INTEGER,
        PRIMARY KEY(type,scope,code,word)) WITHOUT ROWID;
      CREATE UNIQUE INDEX entry_ids ON records(id) WHERE type='entry';
      CREATE UNIQUE INDEX position_slots ON records(scope,slot) WHERE type='position';
      BEGIN;
    )");
    if (sqlite3_prepare_v2(index.database, "INSERT INTO records VALUES(?,?,?,?,?,?,?,?,?)", -1, &index.insert,
                           nullptr) != SQLITE_OK)
        throw Failure(0);
    std::size_t entries = 0, records = 0;
    try
    {
        snapshot.inspect_records(
            [&](const std::string &line, std::int64_t revision) {
                if (++records > 500000)
                    throw Failure(400);
                const auto object = boost::json::parse(line).as_object();
                const auto type = text(object, "type");
                const auto &data = object.at("data").as_object();
                const auto code = bounded(data, "code", 512), word = bounded(data, "word", 2048);
                std::string scope, id;
                std::int64_t weight = 0, slot = 0;
                bool owned = true, deleted = false;
                if (type == "entry" || type == "overlay")
                {
                    scope = text(data, "kind");
                    if (scope != "pinyin" && scope != "wubi" && scope != "english" && scope != "quick")
                        throw Failure(400);
                    id = text(data, "id");
                    const auto *inserted = data.if_contains("user_inserted");
                    if (data.size() != (inserted ? 8 : 7))
                        throw Failure(400);
                    if (inserted)
                    {
                        if (!inserted->is_bool())
                            throw Failure(400);
                        owned = inserted->as_bool();
                    }
                    if (type == "overlay")
                        deleted = object.at("deleted").as_bool();
                    if (type == "entry")
                    {
                        id = bounded(data, "id", 128);
                        if (!owned || ++entries > 100000)
                            throw Failure(400);
                    }
                    weight = integer(data, "weight", deleted ? 0 : 1, 100000000);
                    integer(data, "revision", 1, revision);
                    timestamp(text(data, "updated_at"));
                }
                else
                {
                    scope = bounded(data, "context", 512);
                    if (data.size() != 4 || scope.size() + code.size() + word.size() > 2048)
                        throw Failure(400);
                    slot = type == "position" ? integer(data, "position", 1, 5) : integer(data, "count", 0, 10);
                }
                sqlite3_reset(index.insert);
                sqlite3_clear_bindings(index.insert);
                const std::string strings[] = {type, scope, code, word, id};
                for (int i = 0; i < 5; ++i)
                    if (sqlite3_bind_text(index.insert, i + 1, strings[i].data(), strings[i].size(),
                                          SQLITE_TRANSIENT) != SQLITE_OK)
                        throw Failure(0);
                const std::int64_t numbers[] = {weight, owned, deleted, slot};
                for (int i = 0; i < 4; ++i)
                    if (sqlite3_bind_int64(index.insert, i + 6, numbers[i]) != SQLITE_OK)
                        throw Failure(0);
                if (sqlite3_step(index.insert) != SQLITE_DONE)
                    throw Failure(400);
            },
            cancelled);
        sqlite3_stmt *statement = nullptr;
        const char *sql = R"(
          SELECT EXISTS(
            SELECT 1 FROM records e LEFT JOIN records o
              ON o.type='overlay' AND e.scope=o.scope AND e.code=o.code AND e.word=o.word
            WHERE e.type='entry' AND (o.type IS NULL OR o.deleted=1 OR o.owned=0 OR e.weight<>o.weight)
            UNION ALL
            SELECT 1 FROM records o LEFT JOIN records e
              ON e.type='entry' AND e.scope=o.scope AND e.code=o.code AND e.word=o.word
            WHERE o.type='overlay' AND o.deleted=0 AND o.owned=1 AND e.type IS NULL
          )
        )";
        if (sqlite3_prepare_v2(index.database, sql, -1, &statement, nullptr) != SQLITE_OK)
            throw Failure(0);
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> guard(statement, sqlite3_finalize);
        if (sqlite3_step(statement) != SQLITE_ROW || sqlite3_column_int(statement, 0) != 0)
            throw Failure(400);
        if (cancelled && cancelled())
            throw Failure(0, true);
    }
    catch (...)
    {
        if (cancelled && cancelled())
            throw Failure(0, true);
        throw;
    }
}
} // namespace metasequoia::linux_ime::account
