#include "account/NativeSnapshot.h"
#include "../vendor/MetasequoiaImeEngine/contracts/assets/assets.h"
#include "../vendor/MetasequoiaImeEngine/english/english_dictionary.h"
#include <boost/json.hpp>
#include <glib.h>
#include <sqlite3.h>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace metasequoia;
using namespace metasequoia::linux_ime::account;
namespace json = boost::json;
namespace
{
void require(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}
template <class F> void fails(F action)
{
    try
    {
        action();
    }
    catch (const std::exception &)
    {
        return;
    }
    throw std::runtime_error("unexpected success");
}
json::object entry(const char *type, const char *kind, const std::string &code, const std::string &word,
                   bool owned = true, bool deleted = false)
{
    json::object result{{"type", type},
                        {"data", json::object{{"id", code + word},
                                              {"kind", kind},
                                              {"code", code},
                                              {"word", word},
                                              {"weight", deleted ? 0 : 150},
                                              {"revision", 1},
                                              {"updated_at", "2026-09-09T00:00:00Z"},
                                              {"user_inserted", owned}}}};
    if (std::string(type) == "overlay")
        result["deleted"] = deleted;
    return result;
}
std::unique_ptr<PreparedSnapshot> fixture(const std::vector<json::object> &records)
{
    std::string body = "{\"type\":\"header\",\"format\":\"msime-dictionary-snapshot\",\"version\":1,\"revision\":1}\n";
    for (const auto &record : records)
        body += json::serialize(record) + "\n";
    gchar *hash =
        g_compute_checksum_for_data(G_CHECKSUM_SHA256, reinterpret_cast<const guchar *>(body.data()), body.size());
    body += json::serialize(json::object{{"type", "footer"}, {"records", records.size() + 1}, {"sha256", hash}});
    g_free(hash); // Deliberately exercise a footer without a final newline.
    return PreparedSnapshot::receive(
        [&](const auto &sink) { require(sink(body.data(), body.size()), "freeze failed"); });
}
} // namespace
int main()
{
    gchar *name = g_dir_make_tmp("msime-native-snapshot-XXXXXX", nullptr);
    require(name, "temporary directory failed");
    const std::filesystem::path root(name);
    g_free(name);
    struct Cleanup
    {
        std::filesystem::path root;
        ~Cleanup()
        {
            std::filesystem::remove_all(root);
        }
    } cleanup{root};
    const auto resources = root / "resources";
    std::filesystem::create_directory(resources);
    sqlite3 *raw = nullptr;
    require(sqlite3_open((resources / assets::main_dictionary).c_str(), &raw) == SQLITE_OK, "fixture open failed");
    const auto status = sqlite3_exec(raw,
                                     "CREATE TABLE tbl_2_n(key TEXT,jp TEXT,value TEXT,weight INTEGER);"
                                     "INSERT INTO tbl_2_n VALUES('ni''hao','nh','你好',100),('ni''hao','nh','拟好',80);"
                                     "CREATE TABLE wubi86(key TEXT,value TEXT,weight INTEGER);"
                                     "CREATE TABLE quick_parases(key TEXT,value TEXT,weight INTEGER);"
                                     "CREATE INDEX idx_quick_parases_key_weight ON quick_parases(key,weight DESC);",
                                     nullptr, nullptr, nullptr);
    sqlite3_close(raw);
    require(status == SQLITE_OK, "fixture schema failed");
    require(EnglishDictionary::ensure_schema((resources / assets::english_dictionary).string()),
            "English schema failed");
    std::vector<json::object> records;
    const std::array<std::array<const char *, 3>, 4> words{{{"pinyin", "ni'hao", "拟蒿"},
                                                            {"wubi", "wq", "你"},
                                                            {"english", "cloudfixture", "Cloudfixture"},
                                                            {"quick", "fixture", "合成短语"}}};
    for (const auto &word : words)
        records.push_back(entry("entry", word[0], word[1], word[2]));
    for (const auto &word : words)
        records.push_back(entry("overlay", word[0], word[1], word[2]));
    records.push_back(entry("overlay", "pinyin", "ni'hao", "你好", true, true));
    records.push_back(entry("overlay", "pinyin", "ni'hao", "拟好", false));
    records.push_back(
        {{"type", "position"},
         {"data", json::object{{"context", "ni'hao"}, {"code", "ni'hao"}, {"word", "拟蒿"}, {"position", 1}}}});
    records.push_back(
        {{"type", "selection"},
         {"data", json::object{{"context", "ni'hao"}, {"code", "ni'hao"}, {"word", "拟好"}, {"count", 7}}}});
    auto snapshot = fixture(records);
    const auto generation = root / "generation";
    const auto paths = stage_native_snapshot(*snapshot, resources, generation, "synthetic-fixture");
    int entries = 0, positions = 0, selections = 0;
    stream_dictionary_state(paths, [&](const auto &record) {
        if (const auto *value = std::get_if<DictionaryStateEntry>(&record))
        {
            ++entries;
            if (value->kind == PersonalDictionaryKind::English)
                require(value->display == "Cloudfixture" && value->value == "Cloudfixture", "English display lost");
            if (value->value == "你好")
                require(value->deleted && value->weight == 0, "tombstone lost");
            else
                require(!value->deleted && value->weight == 150, "entry weight lost");
            require(value->user_inserted == (value->value != "拟好"), "base ownership lost");
        }
        else if (const auto *position = std::get_if<DictionaryStatePosition>(&record))
        {
            ++positions;
            require(position->context == "ni'hao" && position->value == "拟蒿" && position->position == 1,
                    "position lost");
        }
        else if (const auto *selection = std::get_if<DictionaryStateSelection>(&record))
        {
            ++selections;
            require(selection->context == "ni'hao" && selection->value == "拟好" && selection->count == 7,
                    "count lost");
        }
        return true;
    });
    require(entries == 6 && positions == 1 && selections == 1, "duplicated or omitted records");
    fails([&] { stage_native_snapshot(*snapshot, resources, generation, "synthetic-fixture"); });
    require(std::filesystem::exists(paths.user(assets::user_journal)), "existing generation damaged");
    const auto cancelled = root / "cancelled";
    fails([&] {
        stage_native_snapshot(*snapshot, resources, cancelled, "synthetic-fixture",
                              [&] { return std::filesystem::exists(cancelled); });
    });
    require(!std::filesystem::exists(cancelled), "cancelled generation leaked");
    auto invalid = fixture({entry("entry", "pinyin", "ni'hao", "孤立词条")});
    fails([&] { stage_native_snapshot(*invalid, resources, root / "invalid", "synthetic-fixture"); });
    require(!std::filesystem::exists(root / "invalid"), "invalid transport staged");
    fails([&] { stage_native_snapshot(*snapshot, root / "missing", root / "failed", "synthetic-fixture"); });
    require(!std::filesystem::exists(root / "failed"), "Engine failure leaked generation");
    // Both lines and record boundaries cross the adapter's 16 KiB read buffer.
    std::vector<json::object> large;
    const std::string long_word(1800, 'x');
    for (const auto *type : {"entry", "overlay"})
        for (int i = 0; i < 20; ++i)
        {
            const auto code = "boundary" + std::to_string(i);
            auto row = entry(type, "quick", code, long_word);
            row.at("data").as_object()["id"] = code;
            large.push_back(std::move(row));
        }
    auto boundary = fixture(large);
    const auto boundary_paths = stage_native_snapshot(*boundary, resources, root / "boundary", "synthetic-fixture");
    int restored = 0;
    stream_dictionary_state(boundary_paths, [&](const auto &record) {
        const auto *value = std::get_if<DictionaryStateEntry>(&record);
        require(value && value->kind == PersonalDictionaryKind::QuickPhrase && value->value == long_word,
                "chunk boundary damaged record");
        ++restored;
        return true;
    });
    require(restored == 20, "chunk boundary truncated snapshot");
    auto empty = fixture({});
    const auto empty_paths = stage_native_snapshot(*empty, resources, root / "empty", "synthetic-fixture");
    stream_dictionary_state(empty_paths, [](const auto &) {
        throw std::runtime_error("empty restore retained records");
        return true;
    });
    std::cout << "Native snapshot four dictionaries, overlays, rankings, empty state and failure cleanup passed\n";
}
