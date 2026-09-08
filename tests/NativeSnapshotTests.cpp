#include "account/NativeSnapshot.h"
#include "account/NativeDictionaryRevision.h"
#include "account/NativeInstallation.h"
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
    const auto revision = native_dictionary_revision(paths);
    require(revision.size() == 64 && native_dictionary_revision(paths) == revision, "unstable native revision");
    const auto same_paths = stage_native_snapshot(*snapshot, resources, root / "same", "synthetic-fixture");
    require(native_dictionary_revision(same_paths) == revision, "same state has different revision in new directory");
    auto changed_records = records;
    changed_records.back().at("data").as_object()["count"] = 8;
    auto changed_snapshot = fixture(changed_records);
    const auto changed_paths =
        stage_native_snapshot(*changed_snapshot, resources, root / "changed", "synthetic-fixture");
    require(native_dictionary_revision(changed_paths) != revision, "native frequency mutation was invisible");
    auto require_changed = [&](const char *name, const std::vector<json::object> &rows) {
        auto modified = fixture(rows);
        const auto modified_paths = stage_native_snapshot(*modified, resources, root / name, "synthetic-fixture");
        require(native_dictionary_revision(modified_paths) != revision, "native mutation was invisible");
    };
    changed_records = records;
    changed_records[0].at("data").as_object()["weight"] = 151;
    changed_records[4].at("data").as_object()["weight"] = 151;
    require_changed("weight", changed_records);
    changed_records = records;
    changed_records[changed_records.size() - 2].at("data").as_object()["position"] = 2;
    require_changed("position", changed_records);
    changed_records = records;
    changed_records[9]["deleted"] = true; // Base override becomes a tombstone without changing its weight.
    require_changed("deletion", changed_records);
    changed_records = records;
    changed_records[8].at("data").as_object()["user_inserted"] = false;
    require_changed("ownership", changed_records);
    changed_records = records;
    changed_records.pop_back();
    require_changed("removed-frequency", changed_records);
    fails([&] { native_dictionary_revision(paths, [] { return true; }); });
    int emitted = 0;
    fails([&] { native_dictionary_revision(paths, [&] { return ++emitted == 3; }); });
    require(native_dictionary_revision(paths) == revision, "cancelled inspection changed native state");
    NativeInstallation installation(root / "installation", paths);
    require(installation.active().token.empty() && installation.active().paths.user_data == paths.user_data,
            "missing marker did not retain captured legacy paths");
    std::filesystem::copy(resources, installation.resources("synthetic-fixture"),
                          std::filesystem::copy_options::recursive);
    const auto installed_first = stage_native_snapshot(*snapshot, installation.resources("synthetic-fixture"),
                                                       installation.generation("first"), "synthetic-fixture");
    const auto installed_second = stage_native_snapshot(*changed_snapshot, installation.resources("synthetic-fixture"),
                                                        installation.generation("second"), "synthetic-fixture");
    const auto publication = installation.publish("first", "synthetic-fixture", "");
    require(publication.published && publication.durable, "first publication failed");
    auto active = installation.active();
    require(active.generation == "first" && active.paths.user_data == installed_first.user_data &&
                native_dictionary_revision(active.paths) == revision,
            "active marker resolved wrong generation");
    require(!installation.publish("second", "synthetic-fixture", "").published &&
                installation.active().token == active.token,
            "stale preview changed active generation");
    require(installation.publish("second", "synthetic-fixture", active.token).published, "second publication failed");
    require(installation.active().paths.user_data == installed_second.user_data &&
                std::filesystem::exists(installed_first.user(assets::user_journal)),
            "publication removed previous generation");
    const auto marker = root / "installation" / "active-dictionary";
    const auto published = installation.active().token;
    {
        std::ofstream out(marker);
        out << "corrupt";
    }
    fails([&] { installation.active(); });
    fails([&] { installation.publish("first", "synthetic-fixture", "corrupt"); });
    {
        std::ofstream out(marker);
        out << published;
    }
    std::filesystem::rename(marker, marker.string() + ".saved");
    std::filesystem::create_symlink(marker.string() + ".saved", marker);
    fails([&] { installation.active(); });
    fails([&] { installation.publish("first", "synthetic-fixture", published); });
    std::filesystem::remove(marker);
    std::filesystem::rename(marker.string() + ".saved", marker);
    const auto ready_file = installed_second.dictionaries / ".ready";
    {
        std::ofstream out(ready_file);
        out << "wrong-bundle\n";
    }
    fails([&] { installation.active(); });
    {
        std::ofstream out(ready_file);
        out << "synthetic-fixture\n";
    }
    require(installation.active().token == published, "failed inspection changed marker");
    fails([&] { installation.generation("../escape"); });
    fails([&] { installation.resources("bad/bundle"); });
    fails([&] { installation.publish("missing", "synthetic-fixture", published); });
    std::filesystem::remove(installed_first.dictionary(assets::main_dictionary));
    fails([&] { installation.publish("first", "synthetic-fixture", published); });
    require(installation.active().token == published, "incomplete generation replaced active state");
    for (const auto &file : std::filesystem::directory_iterator(root / "installation"))
        require(file.path().filename().string().find(".active-") != 0, "publication leaked temporary marker");
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
    require(native_dictionary_revision(empty_paths) != revision, "empty state reused populated revision");
    stream_dictionary_state(empty_paths, [](const auto &) {
        throw std::runtime_error("empty restore retained records");
        return true;
    });
    std::cout << "Native snapshot four dictionaries, overlays, rankings, empty state and failure cleanup passed\n";
}
