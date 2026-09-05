#include "DictionaryBootstrap.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace
{
namespace fs = std::filesystem;
using metasequoia::linux_ime::seed_user_data;

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void write_file(const fs::path &path, const std::string &contents)
{
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << contents;
}

std::string read_file(const fs::path &path)
{
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

fs::path make_root()
{
    const fs::path root = fs::temp_directory_path() / fs::path("metasequoia-bootstrap-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

void seeds_every_missing_database()
{
    const fs::path root = make_root();
    const fs::path system_dir = root / "system";
    const fs::path user_dir = root / "user";
    write_file(system_dir / "msime.db", "main");
    write_file(system_dir / "others.db", "others");
    write_file(system_dir / "english.db", "english");
    write_file(system_dir / "helpcodes" / "helpcode.txt", "codes");
    write_file(system_dir / "helpcodes" / "ignored.bin", "binary");

    std::string error;
    const std::size_t copied = seed_user_data(user_dir, {system_dir}, &error);

    require(error.empty(), "seeding reported an error");
    require(copied == 4, "expected three databases and one helpcode file");
    require(read_file(user_dir / "msime.db") == "main", "msime.db was not seeded");
    require(read_file(user_dir / "others.db") == "others", "others.db was not seeded");
    require(read_file(user_dir / "english.db") == "english", "english.db was not seeded");
    require(read_file(user_dir / "helpcodes" / "helpcode.txt") == "codes", "helpcode was not seeded");
    require(!fs::exists(user_dir / "helpcodes" / "ignored.bin"), "a non-helpcode file was copied");
    fs::remove_all(root);
}

void never_overwrites_existing_user_data()
{
    const fs::path root = make_root();
    const fs::path system_dir = root / "system";
    const fs::path user_dir = root / "user";
    write_file(system_dir / "msime.db", "packaged");
    write_file(user_dir / "msime.db", "learned");

    std::string error;
    const std::size_t copied = seed_user_data(user_dir, {system_dir}, &error);

    require(error.empty(), "seeding reported an error");
    require(copied == 0, "existing user data was replaced");
    require(read_file(user_dir / "msime.db") == "learned", "learned data was overwritten");
    fs::remove_all(root);
}

// The engine creates an empty database when it opens one that is missing, which
// is exactly the broken state a packaged install ends up in.
void replaces_an_empty_database()
{
    const fs::path root = make_root();
    const fs::path system_dir = root / "system";
    const fs::path user_dir = root / "user";
    write_file(system_dir / "msime.db", "packaged");
    write_file(user_dir / "msime.db", "");

    std::string error;
    const std::size_t copied = seed_user_data(user_dir, {system_dir}, &error);

    require(error.empty(), "seeding reported an error");
    require(copied == 1, "an empty database was left in place");
    require(read_file(user_dir / "msime.db") == "packaged", "the empty database was not replaced");
    fs::remove_all(root);
}

void prefers_the_first_system_directory()
{
    const fs::path root = make_root();
    const fs::path first = root / "first";
    const fs::path second = root / "second";
    const fs::path user_dir = root / "user";
    write_file(first / "msime.db", "first");
    write_file(second / "msime.db", "second");
    write_file(second / "others.db", "only-in-second");

    std::string error;
    const std::size_t copied = seed_user_data(user_dir, {first, second}, &error);

    require(error.empty(), "seeding reported an error");
    require(copied == 2, "expected one database from each directory");
    require(read_file(user_dir / "msime.db") == "first", "a later directory won over an earlier one");
    require(read_file(user_dir / "others.db") == "only-in-second", "a later directory was not consulted");
    fs::remove_all(root);
}

void tolerates_absent_system_data()
{
    const fs::path root = make_root();
    const fs::path user_dir = root / "user";

    std::string error;
    const std::size_t copied = seed_user_data(user_dir, {root / "missing"}, &error);

    require(error.empty(), "a missing system directory was treated as a failure");
    require(copied == 0, "something was copied from a missing directory");
    fs::remove_all(root);
}

void leaves_no_staging_files_behind()
{
    const fs::path root = make_root();
    const fs::path system_dir = root / "system";
    const fs::path user_dir = root / "user";
    write_file(system_dir / "msime.db", "packaged");

    std::string error;
    seed_user_data(user_dir, {system_dir}, &error);

    for (const fs::directory_entry &entry : fs::directory_iterator(user_dir))
    {
        require(entry.path().extension() != ".seeding", "a staging file was left behind");
    }
    fs::remove_all(root);
}
void reports_a_missing_dictionary()
{
    const fs::path root = make_root();
    const fs::path user_dir = root / "user";
    fs::create_directories(user_dir);

    const std::string reason = metasequoia::linux_ime::describe_unusable_dictionary(user_dir);

    require(reason.find("missing") != std::string::npos, "a missing dictionary was not reported");
    require(reason.find("msime.db") != std::string::npos, "the reported reason does not name the file");
    fs::remove_all(root);
}

void reports_an_empty_dictionary()
{
    const fs::path root = make_root();
    const fs::path user_dir = root / "user";
    write_file(user_dir / "msime.db", "");

    const std::string reason = metasequoia::linux_ime::describe_unusable_dictionary(user_dir);

    require(reason.find("empty") != std::string::npos, "an empty dictionary was not reported");
    fs::remove_all(root);
}

void stays_quiet_for_a_usable_dictionary()
{
    const fs::path root = make_root();
    const fs::path user_dir = root / "user";
    write_file(user_dir / "msime.db", "content");

    require(metasequoia::linux_ime::describe_unusable_dictionary(user_dir).empty(),
            "a usable dictionary produced a warning");
    fs::remove_all(root);
}
} // namespace

int main()
{
    seeds_every_missing_database();
    never_overwrites_existing_user_data();
    replaces_an_empty_database();
    prefers_the_first_system_directory();
    tolerates_absent_system_data();
    leaves_no_staging_files_behind();
    reports_a_missing_dictionary();
    reports_an_empty_dictionary();
    stays_quiet_for_a_usable_dictionary();
    return 0;
}
