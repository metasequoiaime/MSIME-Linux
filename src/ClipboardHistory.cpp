#include "ClipboardHistory.h"

#include "SettingsStore.h"
#include "core/data_path.h"

#include <boost/json.hpp>
#include <glib.h>

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <optional>
#include <system_error>
#include <unistd.h>

namespace metasequoia::linux_ime
{
namespace
{
constexpr const char *kStoreName = "clipboard_history.json";
std::mutex g_history_mutex;

void set_error(std::string *error, const std::string &message)
{
    if (error != nullptr)
    {
        *error = message;
    }
}

std::vector<std::string> parse_items(const std::string &payload)
{
    boost::system::error_code parse_error;
    const boost::json::value value = boost::json::parse(payload, parse_error);
    if (parse_error || !value.is_array())
    {
        return {};
    }
    std::vector<std::string> items;
    for (const auto &entry : value.as_array())
    {
        if (!entry.is_string())
        {
            continue;
        }
        const std::string text = std::string(entry.as_string().c_str(), entry.as_string().size());
        if (g_utf8_validate(text.c_str(), static_cast<gssize>(text.size()), nullptr))
        {
            items.push_back(text);
        }
        if (items.size() == ClipboardHistory::kMaxItems)
        {
            break;
        }
    }
    return items;
}

std::string serialize_items(const std::vector<std::string> &items)
{
    boost::json::array array;
    for (const auto &item : items)
    {
        array.emplace_back(item);
    }
    return boost::json::serialize(array) + "\n";
}

std::optional<std::string> read_file(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool write_file_atomically(const std::filesystem::path &path, const std::string &payload, std::string *error)
{
    std::error_code filesystem_error;
    std::filesystem::create_directories(path.parent_path(), filesystem_error);
    if (filesystem_error)
    {
        set_error(error, "Unable to create the clipboard history directory.");
        return false;
    }
    std::string template_path = metasequoia::path_to_utf8(path) + ".tmp.XXXXXX";
    std::vector<char> writable(template_path.begin(), template_path.end());
    writable.push_back('\0');
    const int descriptor = mkstemp(writable.data());
    if (descriptor < 0)
    {
        set_error(error, "Unable to create a clipboard history temporary file.");
        return false;
    }
    const auto close_descriptor = [&]() {
        if (close(descriptor) != 0)
        {
            return false;
        }
        return true;
    };
    const ssize_t expected = static_cast<ssize_t>(payload.size());
    ssize_t written = 0;
    while (written < expected)
    {
        const ssize_t result =
            write(descriptor, payload.data() + written, static_cast<std::size_t>(expected - written));
        if (result <= 0)
        {
            const int truncate_result = ftruncate(descriptor, 0);
            (void)truncate_result;
            close_descriptor();
            unlink(writable.data());
            set_error(error, "Unable to write clipboard history.");
            return false;
        }
        written += result;
    }
    if (fsync(descriptor) != 0 || !close_descriptor() ||
        rename(writable.data(), metasequoia::path_to_utf8(path).c_str()) != 0)
    {
        unlink(writable.data());
        set_error(error, "Unable to atomically replace clipboard history.");
        return false;
    }
    return true;
}

std::filesystem::path default_data_directory()
{
    return metasequoia::data_directory();
}
} // namespace

ClipboardHistory::ClipboardHistory() : data_directory_(default_data_directory())
{
}

ClipboardHistory::ClipboardHistory(std::filesystem::path data_directory) : data_directory_(std::move(data_directory))
{
}

std::filesystem::path ClipboardHistory::store_path() const
{
    return data_directory_.empty() ? std::filesystem::path{} : data_directory_ / kStoreName;
}

bool ClipboardHistory::enabled() const
{
    SettingsStore settings;
    return settings.load().clipboard_history_enabled;
}

bool ClipboardHistory::set_enabled(bool value, std::string *error)
{
    SettingsStore settings;
    InputSettings current = settings.load();
    current.clipboard_history_enabled = value;
    if (!settings.save(current, error))
    {
        return false;
    }
    if (!value)
    {
        return clear(error);
    }
    return true;
}

std::vector<std::string> ClipboardHistory::load(std::string *error) const
{
    std::lock_guard<std::mutex> lock(g_history_mutex);
    if (error != nullptr)
    {
        error->clear();
    }
    const auto payload = read_file(store_path());
    if (!payload)
    {
        return {};
    }
    return parse_items(*payload);
}

bool ClipboardHistory::add(std::string text, std::string *error)
{
    text = normalize(std::move(text));
    if (text.empty() || !enabled())
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_history_mutex);
    std::vector<std::string> items;
    if (const auto payload = read_file(store_path()))
    {
        items = parse_items(*payload);
    }
    if (!items.empty() && items.front() == text)
    {
        return false;
    }
    items.erase(std::remove(items.begin(), items.end(), text), items.end());
    items.insert(items.begin(), std::move(text));
    if (items.size() > kMaxItems)
    {
        items.resize(kMaxItems);
    }
    return write_file_atomically(store_path(), serialize_items(items), error);
}

bool ClipboardHistory::remove(const std::string &text, std::string *error)
{
    if (text.empty())
    {
        return false;
    }
    auto items = load(error);
    const auto old_size = items.size();
    items.erase(std::remove(items.begin(), items.end(), text), items.end());
    if (items.size() == old_size)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_history_mutex);
    return write_file_atomically(store_path(), serialize_items(items), error);
}

bool ClipboardHistory::clear(std::string *error)
{
    std::lock_guard<std::mutex> lock(g_history_mutex);
    if (store_path().empty())
    {
        set_error(error, "Clipboard history has no data directory.");
        return false;
    }
    std::error_code filesystem_error;
    if (!std::filesystem::exists(store_path(), filesystem_error) || filesystem_error)
    {
        return true;
    }
    return write_file_atomically(store_path(), "[]\n", error);
}

std::string ClipboardHistory::normalize(std::string text)
{
    while (!text.empty() && (text.back() == '\0' || text.back() == '\r'))
    {
        text.pop_back();
    }
    if (text.empty() || !g_utf8_validate(text.c_str(), static_cast<gssize>(text.size()), nullptr))
    {
        return {};
    }
    const glong characters = g_utf8_strlen(text.c_str(), static_cast<gssize>(text.size()));
    if (characters > static_cast<glong>(kMaxChars))
    {
        const gchar *end = g_utf8_offset_to_pointer(text.c_str(), static_cast<glong>(kMaxChars));
        text.resize(static_cast<std::size_t>(end - text.c_str()));
    }
    return text;
}
} // namespace metasequoia::linux_ime
