#include "DictionaryLease.h"
#include "account/NativeInstallation.h"
#include "core/data_path.h"
#include "user_dictionary/user_dictionary_journal.h"

#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char **argv)
{
    std::filesystem::path data_directory = metasequoia::data_directory();
    std::filesystem::path main_database;
    std::filesystem::path english_database;
    for (int index = 1; index < argc; ++index)
    {
        if (std::string(argv[index]) == "--data-dir" && index + 1 < argc)
        {
            data_directory = metasequoia::path_from_utf8(argv[++index]);
        }
        else if (std::string(argv[index]) == "--main-db" && index + 1 < argc)
        {
            main_database = metasequoia::path_from_utf8(argv[++index]);
        }
        else if (std::string(argv[index]) == "--english-db" && index + 1 < argc)
        {
            english_database = metasequoia::path_from_utf8(argv[++index]);
        }
        else
        {
            std::cerr << "Usage: metasequoia-ime-dictionary-replay [--data-dir <directory>] "
                         "[--main-db <database>] [--english-db <database>]\n";
            return 2;
        }
    }

    const bool explicit_databases = !main_database.empty() || !english_database.empty();
    if (main_database.empty())
    {
        main_database = data_directory / "msime.db";
    }
    if (english_database.empty())
    {
        english_database = data_directory / "english.db";
    }
    if (data_directory.empty() || !data_directory.is_absolute() || !main_database.is_absolute() ||
        !english_database.is_absolute())
    {
        std::cerr << "Valid absolute data and main-database paths are required.\n";
        return 2;
    }

    std::unique_ptr<metasequoia::linux_ime::DictionaryLease> lease;
    try
    {
        lease = metasequoia::linux_ime::DictionaryLease::acquire(data_directory,
                                                                 metasequoia::linux_ime::DictionaryLease::Mode::Shared);
        if (!lease)
        {
            std::cerr << "Dictionary publication is in progress; retry replay after it finishes.\n";
            return 1;
        }
    }
    catch (const std::exception &)
    {
        std::cerr << "Unable to acquire dictionary session lease.\n";
        return 1;
    }

    auto journal_directory = data_directory;
    if (!explicit_databases)
    {
        try
        {
            const metasequoia::RuntimePaths legacy{data_directory, data_directory, data_directory / "cache",
                                                   data_directory};
            metasequoia::linux_ime::account::NativeInstallation installation(data_directory / "runtime", legacy);
            const auto active = installation.active();
            journal_directory = active.paths.user_data;
            main_database = active.paths.dictionary("msime.db");
            english_database = active.paths.dictionary("english.db");
        }
        catch (const std::exception &)
        {
            std::cerr << "Unable to resolve the active dictionary installation.\n";
            return 1;
        }
    }

    const auto result =
        user_dictionary::replay(metasequoia::path_to_utf8(journal_directory / "msime_user.db"),
                                metasequoia::path_to_utf8(main_database), metasequoia::path_to_utf8(english_database));
    if (!result.error.empty())
    {
        std::cerr << "Unable to replay user dictionary operations: " << result.error << '\n';
        return 1;
    }

    std::cout << "Applied " << result.applied << " user dictionary operations.\n";
    return 0;
}
