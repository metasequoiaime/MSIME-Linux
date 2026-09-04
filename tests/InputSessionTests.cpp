#include "../vendor/MetasequoiaImeEngine/core/input_session.h"
#include "../vendor/MetasequoiaImeEngine/core/data_path.h"

#include <sqlite3.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace
{
class Database
{
  public:
    explicit Database(const std::filesystem::path &path)
    {
        if (sqlite3_open(metasequoia::path_to_utf8(path).c_str(), &database_) != SQLITE_OK)
        {
            throw std::runtime_error("Failed to create the input-session test dictionary.");
        }
    }

    ~Database() { sqlite3_close(database_); }

    void execute(const char *sql)
    {
        char *error = nullptr;
        if (sqlite3_exec(database_, sql, nullptr, nullptr, &error) != SQLITE_OK)
        {
            const std::string message = error == nullptr ? "SQLite operation failed." : error;
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
    }

  private:
    sqlite3 *database_ = nullptr;
};

void type(metasequoia::InputSession &session, const std::string &text)
{
    for (char character : text)
    {
        if (!session.handle_character(character).handled)
        {
            throw std::runtime_error("A pinyin character was not handled.");
        }
    }
}

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}
} // namespace

int main()
{
    const auto suffix = std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const auto data_directory = std::filesystem::temp_directory_path() / ("metasequoia-ime-linux-" + suffix);
    std::filesystem::create_directories(data_directory);
    if (setenv("METASEQUOIA_IME_DATA_DIR", metasequoia::path_to_utf8(data_directory).c_str(), 1) != 0)
    {
        throw std::runtime_error("Failed to set the test data directory.");
    }

    {
        Database database(data_directory / "msime.db");
        database.execute("CREATE TABLE tbl_2_n(key TEXT, jp TEXT, value TEXT, weight INTEGER)");
        database.execute("INSERT INTO tbl_2_n VALUES('ni''hao', 'nh', '你好', 200)");
        database.execute("INSERT INTO tbl_2_n VALUES('ni''hao', 'nh', '拟好', 100)");

        metasequoia::InputSession session(SchemeType::Quanpin, true, true, true, false);
        type(session, "nihao");
        require(session.preedit() == "nihao", "The preedit did not mirror raw pinyin.");
        require(session.candidates().size() >= 2, "The engine did not return both candidates.");

        const auto selected = session.select_candidate(1);
        require(selected.handled && selected.commit == "拟好", "The second candidate was not committed.");

        type(session, "nihao");
        const auto space = session.handle_command(metasequoia::Command::CommitCandidate);
        require(space.handled && space.commit == "你好", "Space did not commit the leading candidate.");

        type(session, "nihao");
        session.handle_command(metasequoia::Command::Backspace);
        require(session.preedit() == "niha", "Backspace did not remove the last character.");
        const auto raw = session.handle_command(metasequoia::Command::CommitRaw);
        require(raw.handled && raw.commit == "niha", "Return did not commit raw input.");

        type(session, "nihao");
        const auto cancel = session.handle_command(metasequoia::Command::Cancel);
        require(cancel.handled && !cancel.commit.has_value() && session.preedit().empty(), "Escape did not cancel.");
    }

    std::filesystem::remove_all(data_directory);
    return 0;
}
