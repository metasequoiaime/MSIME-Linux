#include "NativeSnapshot.h"
#include "BackendAccountClient.h"
#include "SnapshotContents.h"
#include <boost/json.hpp>
#include <array>
namespace metasequoia::linux_ime::account
{
namespace
{
// The source is immutable and fully validated before Engine creates a directory.
// Keep only one transport line in memory while adapting its pull interface.
class Records
{
  public:
    Records(const PreparedSnapshot &snapshot, const online::CancellationCheck &cancelled)
        : snapshot_(snapshot), cancelled_(cancelled)
    {
    }
    bool next(DictionaryStateRecord &record)
    {
        if (cancelled_ && cancelled_())
            throw Failure(0, true);
        if (finished_)
            return false;
        std::string line;
        while (read_line(line))
        {
            const auto value = boost::json::parse(line);
            const auto &object = value.as_object();
            const auto type = text(object, "type");
            if (type == "header" || type == "entry")
                continue;
            if (type == "footer")
            {
                if (read_line(line))
                    throw Failure(400);
                finished_ = true;
                return false;
            }
            const auto &data = object.at("data").as_object();
            const auto code = text(data, "code"), word = text(data, "word");
            if (type == "overlay")
            {
                DictionaryStateEntry entry;
                const auto kind = text(data, "kind");
                if (kind == "pinyin")
                    entry.kind = PersonalDictionaryKind::Pinyin;
                else if (kind == "wubi")
                    entry.kind = PersonalDictionaryKind::Wubi;
                else if (kind == "quick")
                    entry.kind = PersonalDictionaryKind::QuickPhrase;
                else if (kind == "english")
                    entry.kind = PersonalDictionaryKind::English;
                else
                    throw Failure(400);
                entry.key = code;
                entry.value = word;
                entry.weight = data.at("weight").as_int64();
                entry.deleted = object.at("deleted").as_bool();
                const auto *owned = data.if_contains("user_inserted");
                entry.user_inserted = owned ? owned->as_bool() : true;
                entry.display = kind == "english" && !entry.deleted ? word : "";
                record = std::move(entry);
            }
            else if (type == "position")
                record = DictionaryStatePosition{text(data, "context"), code, word,
                                                 static_cast<int>(data.at("position").as_int64())};
            else if (type == "selection")
                record = DictionaryStateSelection{text(data, "context"), code, word,
                                                  static_cast<int>(data.at("count").as_int64())};
            else
                throw Failure(400);
            return true;
        }
        throw Failure(400); // EOF without a verified footer must never commit.
    }

  private:
    static std::string text(const boost::json::object &object, const char *key)
    {
        const auto &value = object.at(key).as_string();
        return {value.data(), value.size()};
    }
    bool read_line(std::string &line)
    {
        line.clear();
        if (cancelled_ && cancelled_())
            throw Failure(0, true);
        for (;;)
        {
            if (cursor_ == available_)
            {
                available_ = snapshot_.read(offset_, buffer_.data(), buffer_.size(), cancelled_);
                offset_ += available_;
                cursor_ = 0;
                if (!available_)
                    return !line.empty();
            }
            const char byte = buffer_[cursor_++];
            if (byte == '\n')
                return true;
            line += byte;
            if (line.size() >= 65536)
                throw Failure(400);
        }
    }
    const PreparedSnapshot &snapshot_;
    const online::CancellationCheck &cancelled_;
    std::array<char, 16384> buffer_{};
    std::size_t offset_ = 0, cursor_ = 0, available_ = 0;
    bool finished_ = false;
};
} // namespace
RuntimePaths stage_native_snapshot(const PreparedSnapshot &snapshot, const std::filesystem::path &resources,
                                   const std::filesystem::path &generation, const std::string &content_id,
                                   const online::CancellationCheck &cancelled)
{
    validate_snapshot_contents(snapshot, cancelled);
    Records records(snapshot, cancelled);
    return stage_dictionary_state(resources, generation, content_id,
                                  [&](DictionaryStateRecord &record) { return records.next(record); });
}
} // namespace metasequoia::linux_ime::account
