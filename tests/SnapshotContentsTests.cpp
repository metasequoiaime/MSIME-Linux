#include "account/SnapshotContents.h"
#include "account/BackendAccountClient.h"
#include <boost/json.hpp>
#include <glib.h>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace metasequoia::linux_ime::account;
namespace json = boost::json;
namespace
{
json::object entry(const std::string &type, const std::string &word = "你好", bool owned = true, bool deleted = false)
{
    json::object result{{"type", type},
                        {"data", json::object{{"id", word},
                                              {"kind", "pinyin"},
                                              {"code", "ni'hao"},
                                              {"word", word},
                                              {"weight", 10},
                                              {"revision", 2},
                                              {"updated_at", "2026-09-09T01:02:03.123Z"},
                                              {"user_inserted", owned}}}};
    if (type == "overlay")
        result["deleted"] = deleted;
    return result;
}
json::object rank(const std::string &type, const std::string &word = "你好", int value = 1)
{
    return {{"type", type},
            {"data", json::object{{"context", "nihao"},
                                  {"code", "ni'hao"},
                                  {"word", word},
                                  {type == "position" ? "position" : "count", value}}}};
}
void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
std::string envelope(const std::vector<json::object> &records)
{
    std::string body = "{\"type\":\"header\",\"format\":\"msime-dictionary-snapshot\",\"version\":1,\"revision\":3}\n";
    for (const auto &record : records)
        body += json::serialize(record) + "\n";
    gchar *hash =
        g_compute_checksum_for_data(G_CHECKSUM_SHA256, reinterpret_cast<const guchar *>(body.data()), body.size());
    body += json::serialize(json::object{{"type", "footer"}, {"records", records.size() + 1}, {"sha256", hash}}) + "\n";
    g_free(hash);
    return body;
}
void check(const std::filesystem::path &file, const std::vector<json::object> &records, bool valid)
{
    {
        std::ofstream output(file);
        output << envelope(records);
        require(bool(output), "fixture write failed");
    }
    // All negative fixtures have correct framing and checksum.
    auto snapshot = PreparedSnapshot::open(file.string());
    bool passed = true;
    try
    {
        validate_snapshot_contents(*snapshot);
    }
    catch (const Failure &error)
    {
        require(!error.cancelled(), "unexpected cancellation");
        passed = false;
    }
    require(passed == valid, "wrong content validation result");
}
} // namespace
int main()
{
    gchar *directory = g_dir_make_tmp("msime-contents-test-XXXXXX", nullptr);
    require(directory, "temporary directory failed");
    const std::filesystem::path root(directory);
    g_free(directory);
    struct Cleanup
    {
        std::filesystem::path root;
        ~Cleanup()
        {
            std::filesystem::remove_all(root);
        }
    } cleanup{root};
    const auto file = root / "snapshot.ndjson";
    const auto personal = entry("entry"), overlay = entry("overlay");
    check(file, {}, true);
    check(file, {personal, overlay, rank("position"), rank("selection")}, true);
    check(file, {entry("overlay", "基础词", false)}, true);
    check(file, {entry("overlay", "删除词", true, true)}, true);
    check(file, {personal}, false);
    check(file, {overlay}, false);
    check(file, {personal, personal, overlay}, false);
    check(file, {personal, overlay, overlay}, false);
    check(file, {personal, entry("overlay", "你好", false)}, false);
    check(file, {personal, entry("overlay", "你好", true, true)}, false);
    auto changed = overlay;
    changed["data"].as_object()["weight"] = 11;
    check(file, {personal, changed}, false);
    check(file, {rank("position"), rank("position", "另一个")}, false);
    check(file, {rank("selection"), rank("selection")}, false);
    check(file, {rank("position", "你好", 0)}, false);
    check(file, {rank("position", "你好", 6)}, false);
    check(file, {rank("selection", "你好", 11)}, false);
    check(file, {rank("selection", "你好", 0)}, true);
    for (const auto &field : {"kind", "id", "code", "word", "updated_at"})
    {
        changed = personal;
        changed["data"].as_object()[field] = "";
        check(file, {changed, overlay}, false);
    }
    for (const auto &field : {"weight", "revision"})
    {
        changed = personal;
        changed["data"].as_object()[field] = -1;
        check(file, {changed, overlay}, false);
        changed["data"].as_object()[field] = 100000001;
        check(file, {changed, overlay}, false);
    }
    for (const auto *date : {"2026-02-30T00:00:00Z", "2026-09-09T25:00:00Z", "2026-09-09T00:00:00+25:00",
                             "0001-01-01T00:00:00Z", "2026-09-09T00:00:00Zsuffix"})
    {
        changed = personal;
        changed["data"].as_object()["updated_at"] = date;
        check(file, {changed, overlay}, false);
    }
    changed = personal;
    changed["data"].as_object()["user_inserted"] = false;
    check(file, {changed, overlay}, false);
    changed = personal;
    changed["data"].as_object()["unexpected"] = true;
    check(file, {changed, overlay}, false);
    changed = personal;
    changed["data"].as_object()["word"] = "bad\tword";
    check(file, {changed, overlay}, false);
    auto other = entry("entry", "重复ID");
    other["data"].as_object()["id"] = "你好";
    check(file, {personal, other, overlay, entry("overlay", "重复ID")}, false);
    std::vector<json::object> many;
    for (int i = 0; i < 100000; ++i)
        many.push_back(entry("entry", "词条" + std::to_string(i)));
    for (int i = 0; i < 100000; ++i)
        many.push_back(entry("overlay", "词条" + std::to_string(i)));
    check(file, many, true);
    auto snapshot = PreparedSnapshot::open(file.string());
    bool cancelled = false;
    int calls = 0;
    try
    {
        validate_snapshot_contents(*snapshot, [&] { return ++calls > 30; });
    }
    catch (const Failure &error)
    {
        cancelled = error.cancelled();
    }
    require(cancelled, "content validation did not preserve cancellation");
    std::cout << "Snapshot fields, duplicate records, relationships and cancellation passed\n";
}
