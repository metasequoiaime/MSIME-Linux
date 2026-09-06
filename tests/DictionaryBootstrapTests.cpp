#include "DictionaryBootstrap.h"

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <stdlib.h>
#include <string>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>

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

void set_environment(const char *name, const std::string &value)
{
    require(::setenv(name, value.c_str(), 1) == 0, "unable to set an environment variable");
}

void clear_environment(const char *name)
{
    ::unsetenv(name);
}

// The data directory the engine resolves is process-wide state, so every case that touches it states all three inputs
// rather than inheriting whatever the previous one left behind.
void clear_data_directory_environment()
{
    clear_environment("METASEQUOIA_IME_DATA_DIR");
    clear_environment("XDG_DATA_HOME");
    clear_environment("XDG_DATA_DIRS");
}

bool has_staging_file(const fs::path &directory)
{
    std::error_code code;
    if (!fs::is_directory(directory, code))
    {
        return false;
    }
    for (const fs::directory_entry &entry : fs::recursive_directory_iterator(directory))
    {
        if (entry.path().extension() == ".seeding")
        {
            return true;
        }
    }
    return false;
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

    require(!has_staging_file(user_dir), "a staging file was left behind");

    // The failing copy is the case the staging file exists for. A lowered RLIMIT_FSIZE fails the write only after the
    // destination has been created and part of the payload written, which is the shape of the ENOSPC/EIO this guards
    // against, and unlike an unreadable source it is enforced against root as well.
    const fs::path failing_system = root / "failing-system";
    const fs::path failing_user = root / "failing-user";
    write_file(failing_system / "msime.db", std::string(256 * 1024, 'x'));

    rlimit original{};
    require(::getrlimit(RLIMIT_FSIZE, &original) == 0, "unable to read the file size limit");
    rlimit limited = original;
    limited.rlim_cur = 4096;
    require(::setrlimit(RLIMIT_FSIZE, &limited) == 0, "unable to lower the file size limit");
    // Exceeding the limit also raises SIGXFSZ, whose default action would take the test process down before the copy
    // reports anything.
    void (*previous_handler)(int) = std::signal(SIGXFSZ, SIG_IGN);
    std::string failure;
    const std::size_t copied = seed_user_data(failing_user, {failing_system}, &failure);
    if (previous_handler != SIG_ERR)
    {
        std::signal(SIGXFSZ, previous_handler);
    }
    require(::setrlimit(RLIMIT_FSIZE, &original) == 0, "unable to restore the file size limit");

    require(copied == 0, "a copy that could not be written was counted as seeded");
    require(!failure.empty(), "a failed copy was not reported");
    require(!fs::exists(failing_user / "msime.db"), "a truncated database was published");
    require(!has_staging_file(failing_user), "a partially written staging file was left behind");
    fs::remove_all(root);
}

// The Japanese scheme this frontend advertises needs the sentence model that sits beside the databases, and the Mozc
// notice has to travel with it. Neither is a database, so a validity check shaped like SQLite would refuse both.
void seeds_the_japanese_sentence_model_and_its_notice()
{
    const fs::path root = make_root();
    const fs::path system_dir = root / "system";
    const fs::path user_dir = root / "user";
    std::string model;
    model.push_back('\0');
    model.push_back('\x01');
    model += "viterbi-model";
    write_file(system_dir / "msime.db", "main");
    write_file(system_dir / "dict_japanese.dat", model);
    write_file(system_dir / "mozc_dictionary_oss_README.txt", "Mozc dictionary notice");

    std::string error;
    const std::size_t copied = seed_user_data(user_dir, {system_dir}, &error);

    require(error.empty(), "seeding reported an error");
    require(copied == 3, "the sentence model and its notice were not seeded alongside the database");
    require(read_file(user_dir / "dict_japanese.dat") == model, "the sentence model was not copied byte for byte");
    require(read_file(user_dir / "mozc_dictionary_oss_README.txt") == "Mozc dictionary notice",
            "the Mozc notice was not seeded with the model");
    fs::remove_all(root);
}

// A directory sitting where a database belongs cannot be renamed over, so exactly that one file fails while every other
// payload remains perfectly placeable.
void attempts_every_payload_after_a_failure()
{
    const fs::path root = make_root();
    const fs::path system_dir = root / "system";
    const fs::path user_dir = root / "user";
    write_file(system_dir / "msime.db", "main");
    write_file(system_dir / "others.db", "others");
    write_file(system_dir / "english.db", "english");
    write_file(system_dir / "dict_japanese.dat", "model");
    write_file(system_dir / "helpcodes" / "helpcode.txt", "codes");
    write_file(user_dir / "msime.db" / "occupied", "in the way");
    write_file(user_dir / "english.db" / "occupied", "in the way");

    std::vector<std::string> errors;
    const std::size_t copied = seed_user_data(user_dir, {system_dir}, &errors);

    require(copied == 3, "seeding abandoned the payloads that followed a failure");
    require(errors.size() == 2, "each failure was not reported on its own");
    require(errors.front().find("msime.db") != std::string::npos, "the first failure does not name its file");
    require(errors.back().find("english.db") != std::string::npos, "the second failure does not name its file");
    require(read_file(user_dir / "others.db") == "others", "a database after the failed one was not seeded");
    require(read_file(user_dir / "dict_japanese.dat") == "model", "the sentence model was skipped after a failure");
    require(read_file(user_dir / "helpcodes" / "helpcode.txt") == "codes",
            "the helpcodes were skipped after a failure");
    require(!has_staging_file(user_dir), "a staging file was left behind by a failed placement");
    fs::remove_all(root);
}

void consults_later_system_directories_after_a_failure()
{
    const fs::path root = make_root();
    const fs::path first = root / "first";
    const fs::path second = root / "second";
    const fs::path user_dir = root / "user";
    write_file(first / "msime.db", "first");
    write_file(second / "others.db", "only-in-second");
    write_file(user_dir / "msime.db" / "occupied", "in the way");

    std::vector<std::string> errors;
    const std::size_t copied = seed_user_data(user_dir, {first, second}, &errors);

    require(copied == 1, "a later system directory was abandoned after an earlier failure");
    require(errors.size() == 1, "expected exactly one failure");
    require(read_file(user_dir / "others.db") == "only-in-second", "the second system directory was never consulted");
    fs::remove_all(root);
}

void joins_every_failure_for_single_line_callers()
{
    const fs::path root = make_root();
    const fs::path system_dir = root / "system";
    const fs::path user_dir = root / "user";
    write_file(system_dir / "msime.db", "main");
    write_file(system_dir / "english.db", "english");
    write_file(user_dir / "msime.db" / "occupied", "in the way");
    write_file(user_dir / "english.db" / "occupied", "in the way");

    std::string error;
    seed_user_data(user_dir, {system_dir}, &error);

    require(error.find("msime.db") != std::string::npos, "the first failure was not reported");
    require(error.find("english.db") != std::string::npos, "a later failure was dropped from the message");
    require(error.find("; ") != std::string::npos, "the failures were not joined into one line");
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

// The engine opens its databases through metasequoia::data_directory(), which honours METASEQUOIA_IME_DATA_DIR ahead of
// the XDG locations. Deriving a second answer here made the seeder fill one directory while the engine read another.
void resolves_the_directory_the_engine_reads()
{
    const fs::path root = make_root();
    const fs::path override_dir = root / "override";
    const fs::path xdg_dir = root / "xdg";
    fs::create_directories(override_dir);
    set_environment("XDG_DATA_HOME", xdg_dir.string());
    set_environment("METASEQUOIA_IME_DATA_DIR", override_dir.string());

    require(metasequoia::linux_ime::user_data_directory() == override_dir, "the engine's data directory was ignored");

    clear_data_directory_environment();
    fs::remove_all(root);
}

void seeds_the_directory_the_engine_reads()
{
    const fs::path root = make_root();
    const fs::path share = root / "share";
    const fs::path override_dir = root / "override";
    const fs::path xdg_dir = root / "xdg";
    write_file(share / "metasequoiaime" / "msime.db", "packaged");
    set_environment("XDG_DATA_DIRS", share.string());
    set_environment("XDG_DATA_HOME", xdg_dir.string());
    set_environment("METASEQUOIA_IME_DATA_DIR", override_dir.string());

    std::string error;
    const std::size_t copied = seed_user_data(&error);

    require(error.empty(), "seeding reported an error");
    require(copied == 1, "the packaged database was not seeded");
    require(read_file(override_dir / "msime.db") == "packaged", "the engine's data directory was not seeded");
    require(!fs::exists(xdg_dir / "metasequoiaime" / "msime.db"),
            "the seeder filled the XDG directory the engine does not read");

    clear_data_directory_environment();
    fs::remove_all(root);
}

void diagnoses_the_directory_the_engine_reads()
{
    const fs::path root = make_root();
    const fs::path healthy_override = root / "override";
    const fs::path empty_override = root / "empty-override";
    const fs::path xdg_dir = root / "xdg";
    write_file(healthy_override / "msime.db", "content");
    fs::create_directories(empty_override);
    fs::create_directories(xdg_dir / "metasequoiaime");
    set_environment("XDG_DATA_HOME", xdg_dir.string());
    set_environment("METASEQUOIA_IME_DATA_DIR", healthy_override.string());

    require(metasequoia::linux_ime::describe_unusable_dictionary().empty(),
            "a healthy override directory was reported as broken");

    // The inverse is the worse half: a healthy XDG directory must not hide the empty directory the engine really opens.
    write_file(xdg_dir / "metasequoiaime" / "msime.db", "content");
    set_environment("METASEQUOIA_IME_DATA_DIR", empty_override.string());
    const std::string reason = metasequoia::linux_ime::describe_unusable_dictionary();

    require(reason.find("missing") != std::string::npos, "the empty override directory was not reported");
    require(reason.find(empty_override.string()) != std::string::npos, "the reported reason names the wrong directory");

    clear_data_directory_environment();
    fs::remove_all(root);
}

void names_every_variable_that_locates_the_data_directory()
{
    const char *home = std::getenv("HOME");
    const bool had_home = home != nullptr;
    const std::string previous_home = had_home ? std::string(home) : std::string();
    clear_data_directory_environment();
    clear_environment("HOME");

    const std::string reason = metasequoia::linux_ime::describe_unusable_dictionary();

    if (had_home)
    {
        set_environment("HOME", previous_home);
    }
    require(reason.find("METASEQUOIA_IME_DATA_DIR") != std::string::npos,
            "the advice omits the variable the engine consults first");
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
    seeds_the_japanese_sentence_model_and_its_notice();
    attempts_every_payload_after_a_failure();
    consults_later_system_directories_after_a_failure();
    joins_every_failure_for_single_line_callers();
    reports_a_missing_dictionary();
    reports_an_empty_dictionary();
    stays_quiet_for_a_usable_dictionary();
    resolves_the_directory_the_engine_reads();
    seeds_the_directory_the_engine_reads();
    diagnoses_the_directory_the_engine_reads();
    names_every_variable_that_locates_the_data_directory();
    return 0;
}
