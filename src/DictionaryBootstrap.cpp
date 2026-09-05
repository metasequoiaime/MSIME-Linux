#include "DictionaryBootstrap.h"

#include <array>
#include <cstdlib>
#include <system_error>

namespace metasequoia::linux_ime
{
namespace
{
constexpr std::array<const char *, 3> kDatabases = {"msime.db", "others.db", "english.db"};

std::filesystem::path directory_from_environment(const char *variable, const char *suffix)
{
    const char *value = std::getenv(variable);
    if (value == nullptr || *value == '\0')
    {
        return {};
    }
    std::filesystem::path path(value);
    if (!path.is_absolute())
    {
        return {};
    }
    if (suffix != nullptr)
    {
        path /= suffix;
    }
    return path;
}

// A zero-byte database is not a real one. The engine creates an empty file when
// it opens a database that does not exist, so treating "present" as "usable"
// would leave a packaged install permanently without candidates.
bool is_usable_file(const std::filesystem::path &path)
{
    std::error_code code;
    return std::filesystem::is_regular_file(path, code) && std::filesystem::file_size(path, code) > 0 && !code;
}

bool copy_if_missing(const std::filesystem::path &source, const std::filesystem::path &destination, std::size_t &copied,
                     std::string *error)
{
    if (!is_usable_file(source) || is_usable_file(destination))
    {
        return true;
    }
    std::error_code code;
    std::filesystem::create_directories(destination.parent_path(), code);
    if (code)
    {
        if (error != nullptr)
        {
            *error = "Unable to create " + destination.parent_path().string() + ": " + code.message();
        }
        return false;
    }
    // Copy to a sibling first so a failure part way through never leaves a
    // truncated database behind, which is exactly the state being repaired here.
    const std::filesystem::path staged = destination.parent_path() / (destination.filename().string() + ".seeding");
    std::filesystem::remove(staged, code);
    std::filesystem::copy_file(source, staged, std::filesystem::copy_options::overwrite_existing, code);
    if (code)
    {
        if (error != nullptr)
        {
            *error = "Unable to copy " + source.string() + ": " + code.message();
        }
        return false;
    }
    std::filesystem::rename(staged, destination, code);
    if (code)
    {
        std::error_code cleanup_code;
        std::filesystem::remove(staged, cleanup_code);
        if (error != nullptr)
        {
            *error = "Unable to place " + destination.string() + ": " + code.message();
        }
        return false;
    }
    ++copied;
    return true;
}
} // namespace

std::filesystem::path user_data_directory()
{
    std::filesystem::path directory = directory_from_environment("XDG_DATA_HOME", "metasequoiaime");
    if (!directory.empty())
    {
        return directory;
    }
    directory = directory_from_environment("HOME", nullptr);
    if (directory.empty())
    {
        return {};
    }
    return directory / ".local" / "share" / "metasequoiaime";
}

std::vector<std::filesystem::path> system_data_directories()
{
    std::vector<std::filesystem::path> directories;
    const char *value = std::getenv("XDG_DATA_DIRS");
    const std::string entries = (value != nullptr && *value != '\0') ? value : "/usr/local/share:/usr/share";
    std::size_t start = 0;
    while (start <= entries.size())
    {
        const std::size_t end = entries.find(':', start);
        const std::string entry = entries.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!entry.empty() && std::filesystem::path(entry).is_absolute())
        {
            directories.emplace_back(std::filesystem::path(entry) / "metasequoiaime");
        }
        if (end == std::string::npos)
        {
            break;
        }
        start = end + 1;
    }
    return directories;
}

std::size_t seed_user_data(const std::filesystem::path &user_directory,
                           const std::vector<std::filesystem::path> &system_directories, std::string *error)
{
    if (user_directory.empty())
    {
        return 0;
    }
    std::size_t copied = 0;
    for (const std::filesystem::path &system_directory : system_directories)
    {
        for (const char *database : kDatabases)
        {
            if (!copy_if_missing(system_directory / database, user_directory / database, copied, error))
            {
                return copied;
            }
        }
        std::error_code code;
        const std::filesystem::path helpcodes = system_directory / "helpcodes";
        if (!std::filesystem::is_directory(helpcodes, code))
        {
            continue;
        }
        for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(helpcodes, code))
        {
            if (entry.path().extension() != ".txt")
            {
                continue;
            }
            if (!copy_if_missing(entry.path(), user_directory / "helpcodes" / entry.path().filename(), copied, error))
            {
                return copied;
            }
        }
    }
    return copied;
}

std::size_t seed_user_data(std::string *error)
{
    return seed_user_data(user_data_directory(), system_data_directories(), error);
}

std::string describe_unusable_dictionary(const std::filesystem::path &user_directory)
{
    if (user_directory.empty())
    {
        return "No data directory: set HOME or XDG_DATA_HOME.";
    }
    const std::filesystem::path main_database = user_directory / kDatabases.front();
    std::error_code code;
    if (!std::filesystem::exists(main_database, code))
    {
        return "Dictionary missing: " + main_database.string();
    }
    if (!is_usable_file(main_database))
    {
        return "Dictionary is empty: " + main_database.string();
    }
    return {};
}

std::string describe_unusable_dictionary()
{
    return describe_unusable_dictionary(user_data_directory());
}
} // namespace metasequoia::linux_ime
