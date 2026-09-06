#include "DictionaryBootstrap.h"

#include "core/data_path.h"

#include <array>
#include <cstdlib>
#include <system_error>

namespace metasequoia::linux_ime
{
namespace
{
constexpr const char *kMainDatabase = "msime.db";
constexpr std::array<const char *, 3> kDatabases = {kMainDatabase, "others.db", "english.db"};
// The Japanese sentence model the engine loads from the same directory as the databases, and the Mozc notice whose
// licences require it to be distributed with the model. Without the model JapaneseSentenceDecoder::Load fails, the
// provider stores a null decoder without reporting anything, and the Japanese scheme this frontend advertises degrades
// to single-word candidates. Neither file is a database, so nothing here may probe them as one.
constexpr std::array<const char *, 2> kDataFiles = {"dict_japanese.dat", "mozc_dictionary_oss_README.txt"};

// A zero-byte database is not a real one. The engine creates an empty file when
// it opens a database that does not exist, so treating "present" as "usable"
// would leave a packaged install permanently without candidates. This asks only
// whether a file exists and has content, which is as true of the Viterbi model
// and the plain-text notice above as it is of a SQLite database; a check that
// understood database structure would reject both.
bool is_usable_file(const std::filesystem::path &path)
{
    std::error_code code;
    return std::filesystem::is_regular_file(path, code) && std::filesystem::file_size(path, code) > 0 && !code;
}

bool copy_if_missing(const std::filesystem::path &source, const std::filesystem::path &destination, std::size_t &copied,
                     std::string &error)
{
    if (!is_usable_file(source) || is_usable_file(destination))
    {
        return true;
    }
    std::error_code code;
    std::filesystem::create_directories(destination.parent_path(), code);
    if (code)
    {
        error = "Unable to create " + destination.parent_path().string() + ": " + code.message();
        return false;
    }
    // Copy to a sibling first so a failure part way through never leaves a
    // truncated database behind, which is exactly the state being repaired here.
    const std::filesystem::path staged = destination.parent_path() / (destination.filename().string() + ".seeding");
    std::filesystem::remove(staged, code);
    std::filesystem::copy_file(source, staged, std::filesystem::copy_options::overwrite_existing, code);
    if (code)
    {
        // copy_file does not unlink what it already wrote, so an out-of-space or I/O failure part way through would
        // otherwise strand a partial database next to the one being repaired.
        std::error_code cleanup_code;
        std::filesystem::remove(staged, cleanup_code);
        error = "Unable to copy " + source.string() + ": " + code.message();
        return false;
    }
    std::filesystem::rename(staged, destination, code);
    if (code)
    {
        std::error_code cleanup_code;
        std::filesystem::remove(staged, cleanup_code);
        error = "Unable to place " + destination.string() + ": " + code.message();
        return false;
    }
    ++copied;
    return true;
}
} // namespace

std::filesystem::path user_data_directory()
{
    // The engine is the authority on where its databases live, and it honours METASEQUOIA_IME_DATA_DIR ahead of the
    // XDG locations. Re-deriving the path here made the seeder fill one directory while the engine opened another.
    return metasequoia::data_directory();
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
                           const std::vector<std::filesystem::path> &system_directories,
                           std::vector<std::string> *errors)
{
    if (errors != nullptr)
    {
        errors->clear();
    }
    if (user_directory.empty())
    {
        return 0;
    }
    std::size_t copied = 0;
    // Nothing below gives up early. Returning at the first per-file failure abandoned every remaining payload, the
    // helpcodes, and every later entry in XDG_DATA_DIRS, so one unreadable file or one stale system directory left the
    // install with most of its data missing and a single message that named none of it.
    std::vector<std::string> failures;
    const auto seed = [&](const std::filesystem::path &source, const std::filesystem::path &destination) {
        std::string failure;
        if (!copy_if_missing(source, destination, copied, failure))
        {
            failures.push_back(std::move(failure));
        }
    };
    for (const std::filesystem::path &system_directory : system_directories)
    {
        for (const char *database : kDatabases)
        {
            seed(system_directory / database, user_directory / database);
        }
        for (const char *data_file : kDataFiles)
        {
            seed(system_directory / data_file, user_directory / data_file);
        }
        std::error_code code;
        const std::filesystem::path helpcodes = system_directory / "helpcodes";
        if (!std::filesystem::is_directory(helpcodes, code))
        {
            continue;
        }
        // Iterated by hand rather than with a range-for, because operator++ on a directory_iterator throws on an
        // unreadable entry and this runs on the engine's startup path, where an exception would take out more than the
        // seeding it escaped from.
        std::error_code iteration_code;
        std::filesystem::directory_iterator iterator(helpcodes, iteration_code);
        const std::filesystem::directory_iterator end;
        while (!iteration_code && iterator != end)
        {
            if (iterator->path().extension() == ".txt")
            {
                seed(iterator->path(), user_directory / "helpcodes" / iterator->path().filename());
            }
            iterator.increment(iteration_code);
        }
        if (iteration_code)
        {
            failures.push_back("Unable to read " + helpcodes.string() + ": " + iteration_code.message());
        }
    }
    if (errors != nullptr)
    {
        *errors = std::move(failures);
    }
    return copied;
}

std::size_t seed_user_data(const std::filesystem::path &user_directory,
                           const std::vector<std::filesystem::path> &system_directories, std::string *error)
{
    std::vector<std::string> failures;
    const std::size_t copied = seed_user_data(user_directory, system_directories, &failures);
    if (error != nullptr)
    {
        error->clear();
        for (const std::string &failure : failures)
        {
            if (!error->empty())
            {
                *error += "; ";
            }
            *error += failure;
        }
    }
    return copied;
}

std::size_t seed_user_data(std::vector<std::string> *errors)
{
    return seed_user_data(user_data_directory(), system_data_directories(), errors);
}

std::size_t seed_user_data(std::string *error)
{
    return seed_user_data(user_data_directory(), system_data_directories(), error);
}

std::string describe_unusable_dictionary(const std::filesystem::path &user_directory)
{
    if (user_directory.empty())
    {
        return "No data directory: set METASEQUOIA_IME_DATA_DIR, XDG_DATA_HOME or HOME.";
    }
    const std::filesystem::path main_database = user_directory / kMainDatabase;
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
