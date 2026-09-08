#include "SnapshotEnvelope.h"
#include "BackendAccountClient.h"
#include <boost/json.hpp>
#include <boost/json/basic_parser_impl.hpp>
#include <glib.h>
#include <memory>
#include <set>
#include <vector>
#include <limits>
namespace metasequoia::linux_ime::account
{
namespace
{
using Error = boost::system::error_code;
using View = boost::json::string_view;
struct StrictKeys
{
    static constexpr std::size_t max_array_size = 0, max_object_size = 16, max_string_size = 65535, max_key_size = 128;
    std::vector<std::set<std::string>> objects;
    std::string key;
    bool on_document_begin(Error &)
    {
        return true;
    }
    bool on_document_end(Error &)
    {
        return true;
    }
    bool on_object_begin(Error &)
    {
        objects.emplace_back();
        return true;
    }
    bool on_object_end(std::size_t, Error &)
    {
        objects.pop_back();
        return true;
    }
    bool on_array_begin(Error &)
    {
        throw Failure(400);
    }
    bool on_array_end(std::size_t, Error &)
    {
        return true;
    }
    bool on_key_part(View value, std::size_t, Error &)
    {
        key.append(value.data(), value.size());
        return true;
    }
    bool on_key(View value, std::size_t, Error &)
    {
        key.append(value.data(), value.size());
        if (!objects.back().insert(key).second)
            throw Failure(400);
        key.clear();
        return true;
    }
    bool on_string_part(View, std::size_t, Error &)
    {
        return true;
    }
    bool on_string(View, std::size_t, Error &)
    {
        return true;
    }
    bool on_number_part(View, Error &)
    {
        return true;
    }
    bool on_int64(std::int64_t, View, Error &)
    {
        return true;
    }
    bool on_uint64(std::uint64_t value, View, Error &)
    {
        if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw Failure(400);
        return true;
    }
    bool on_double(double, View, Error &)
    {
        throw Failure(400);
    }
    bool on_bool(bool, Error &)
    {
        return true;
    }
    bool on_null(Error &)
    {
        return true;
    }
    bool on_comment_part(View, Error &)
    {
        throw Failure(400);
    }
    bool on_comment(View, Error &)
    {
        throw Failure(400);
    }
};
boost::json::object parse(const std::string &line)
{
    boost::json::parse_options options;
    options.max_depth = 8;
    boost::json::basic_parser<StrictKeys> parser(options);
    Error error;
    const auto consumed = parser.write_some(false, line.data(), line.size(), error);
    if (error || consumed != line.size() || !parser.done())
        throw Failure(400);
    auto value = boost::json::parse(line, error);
    if (error || !value.is_object())
        throw Failure(400);
    return std::move(value.as_object());
}
std::int64_t integer(const boost::json::object &object, const char *key)
{
    const auto *value = object.if_contains(key);
    if (!value || !value->is_int64() || value->as_int64() < 0)
        throw Failure(400);
    return value->as_int64();
}
std::string string(const boost::json::object &object, const char *key)
{
    const auto *value = object.if_contains(key);
    if (!value || !value->is_string())
        throw Failure(400);
    return std::string(value->as_string());
}
} // namespace
SnapshotEnvelope inspect_snapshot_envelope(std::istream &input, const online::CancellationCheck &cancelled)
{
    std::unique_ptr<GChecksum, decltype(&g_checksum_free)> hash(g_checksum_new(G_CHECKSUM_SHA256), g_checksum_free);
    if (!hash)
        throw Failure(0);
    SnapshotEnvelope result;
    std::size_t bytes = 0;
    int category = -1;
    bool finished = false;
    char buffer[65537];
    for (;;)
    {
        if (cancelled && cancelled())
            throw Failure(0, true);
        input.getline(buffer, sizeof(buffer));
        const auto read = input.gcount();
        if (input.bad() || (input.fail() && !input.eof()))
            throw Failure(400);
        if (read == 0 && input.eof())
            break;
        bytes += static_cast<std::size_t>(read);
        if (bytes > 512U * 1024U * 1024U || finished)
            throw Failure(400);
        const auto size = static_cast<std::size_t>(read) - (input.eof() ? 0 : 1);
        if (size == 0 || size >= 65536)
            throw Failure(400);
        const std::string line(buffer, size);
        const auto object = parse(line);
        const auto type = string(object, "type");
        if (type == "footer")
        {
            if (category < 0 || object.size() != 3 ||
                integer(object, "records") != static_cast<std::int64_t>(result.records))
                throw Failure(400);
            result.sha256 = string(object, "sha256");
            if (result.sha256 != g_checksum_get_string(hash.get()))
                throw Failure(400);
            finished = true;
            continue;
        }
        int next = -1;
        if (type == "header")
        {
            if (category != -1 || object.size() != 4 || string(object, "format") != "msime-dictionary-snapshot" ||
                integer(object, "version") != 1)
                throw Failure(400);
            result.revision = integer(object, "revision");
            next = 0;
        }
        else
        {
            if (category < 0)
                throw Failure(400);
            if (type == "entry")
                next = 1;
            else if (type == "overlay")
                next = 2;
            else if (type == "position")
                next = 3;
            else if (type == "selection")
                next = 4;
            const auto *data = object.if_contains("data");
            if (next < 0 || next < category || !data || !data->is_object() || object.size() != (next == 2 ? 3 : 2))
                throw Failure(400);
            if (next == 2)
            {
                const auto *deleted = object.if_contains("deleted");
                if (!deleted || !deleted->is_bool())
                    throw Failure(400);
            }
            ++result.counts[next - 1];
        }
        category = next;
        ++result.records;
        g_checksum_update(hash.get(), reinterpret_cast<const guchar *>(line.data()), line.size());
        g_checksum_update(hash.get(), reinterpret_cast<const guchar *>("\n"), 1);
    }
    if (!finished)
        throw Failure(400);
    return result;
}
} // namespace metasequoia::linux_ime::account
