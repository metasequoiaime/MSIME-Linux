#include "../src/InputController.h"
#include "../vendor/MetasequoiaImeEngine/core/data_path.h"

#include <sqlite3.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace
{
using metasequoia::linux_ime::FrontendKey;
using metasequoia::linux_ime::FrontendKeyEvent;
using metasequoia::linux_ime::InputController;
using metasequoia::linux_ime::InputMode;

class Database
{
  public:
    explicit Database(const std::filesystem::path &path)
    {
        if (sqlite3_open(metasequoia::path_to_utf8(path).c_str(), &database_) != SQLITE_OK)
        {
            throw std::runtime_error("Failed to create the input-controller test dictionary.");
        }
    }

    ~Database() { sqlite3_close(database_); }

    void execute(const std::string &sql)
    {
        char *error = nullptr;
        if (sqlite3_exec(database_, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK)
        {
            const std::string message = error == nullptr ? "SQLite operation failed." : error;
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
    }

  private:
    sqlite3 *database_ = nullptr;
};

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void type(InputController &controller, const std::string &text)
{
    for (const char character : text)
    {
        const auto result = controller.handle_key({FrontendKey::Character, character});
        require(result.handled && !result.commit.has_value(), "A composition character was not handled.");
    }
}

FrontendKeyEvent key(FrontendKey value)
{
    return {value};
}

FrontendKeyEvent digit(unsigned value)
{
    FrontendKeyEvent event{FrontendKey::Digit};
    event.digit = value;
    return event;
}
} // namespace

int main()
{
    const auto suffix = std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const auto data_directory = std::filesystem::temp_directory_path() / ("metasequoia-controller-" + suffix);
    std::filesystem::create_directories(data_directory);
    if (setenv("METASEQUOIA_IME_DATA_DIR", metasequoia::path_to_utf8(data_directory).c_str(), 1) != 0)
    {
        throw std::runtime_error("Failed to set the test data directory.");
    }

    {
        Database database(data_directory / "msime.db");
        database.execute("CREATE TABLE tbl_2_n(key TEXT, jp TEXT, value TEXT, weight INTEGER)");
        for (int index = 0; index < 12; ++index)
        {
            database.execute("INSERT INTO tbl_2_n VALUES('ni''hao', 'nh', 'candidate-" +
                             std::to_string(index) + "', " + std::to_string(120 - index) + ")");
        }

        InputController controller(SchemeType::Quanpin, 3);
        require(controller.mode() == InputMode::Ime, "The controller did not start in IME mode.");
        require(controller.scheme() == SchemeType::Quanpin, "The controller did not keep the requested scheme.");

        type(controller, "nihao");
        require(controller.preedit() == "nihao", "The controller did not expose the engine preedit.");
        require(controller.candidates().size() == 12, "The controller did not expose all dictionary candidates.");
        require(controller.highlighted_candidate() == 0, "A new composition did not highlight its first candidate.");

        require(controller.handle_key(key(FrontendKey::Down)).handled, "Down was not handled in a candidate list.");
        require(controller.handle_key(key(FrontendKey::Down)).handled, "A second Down was not handled.");
        require(controller.highlighted_candidate() == 2, "Down did not move the candidate highlight.");
        controller.handle_key(key(FrontendKey::Down));
        require(controller.highlighted_candidate() == 3, "Down did not cross the candidate page boundary.");
        controller.handle_key(key(FrontendKey::PageDown));
        require(controller.highlighted_candidate() == 6, "PageDown did not preserve the cursor within the page.");
        controller.handle_key(key(FrontendKey::PageUp));
        require(controller.highlighted_candidate() == 3, "PageUp did not preserve the cursor within the page.");

        const auto space = controller.handle_key(key(FrontendKey::Space));
        require(space.handled && space.commit == "candidate-3", "Space did not commit the highlighted candidate.");
        require(!controller.has_composition(), "Space did not finish the composition.");

        type(controller, "nihao");
        controller.handle_key(key(FrontendKey::PageDown));
        const auto page_digit = controller.handle_key(digit(2));
        require(page_digit.handled && page_digit.commit == "candidate-4",
                "A digit did not select relative to the visible page.");

        type(controller, "nihao");
        controller.handle_key(key(FrontendKey::PageDown));
        controller.handle_key(key(FrontendKey::PageDown));
        controller.handle_key(key(FrontendKey::PageDown));
        const auto missing_page_digit = controller.handle_key(digit(9));
        require(missing_page_digit.handled && !missing_page_digit.commit.has_value() && controller.has_composition(),
                "An out-of-range page digit escaped into the client application.");

        const auto raw = controller.handle_key(key(FrontendKey::Enter));
        require(raw.handled && raw.commit == "nihao", "Enter did not commit raw input.");
        type(controller, "nihao");
        const auto cancel = controller.handle_key(key(FrontendKey::Escape));
        require(cancel.handled && !cancel.commit.has_value() && !controller.has_composition(),
                "Escape did not cancel the composition.");

        type(controller, "nihao");
        controller.handle_key(key(FrontendKey::Down));
        FrontendKeyEvent shortcut{FrontendKey::Character, 'x'};
        shortcut.host_shortcut = true;
        const auto passthrough = controller.handle_key(shortcut);
        require(!passthrough.handled && passthrough.commit == "candidate-1",
                "A host shortcut did not commit the highlighted candidate before passthrough.");

        type(controller, "nihao");
        controller.handle_key(key(FrontendKey::Down));
        const auto scheme_change = controller.switch_scheme(SchemeType::Wubi);
        require(scheme_change.handled && scheme_change.commit == "candidate-1",
                "Scheme switching did not commit the highlighted candidate first.");
        require(controller.scheme() == SchemeType::Wubi && !controller.has_composition(),
                "Scheme switching did not reset into the requested scheme.");

        const auto direct_mode = controller.set_mode(InputMode::Direct);
        require(direct_mode.handled && !direct_mode.commit.has_value(), "Direct mode was not activated.");
        require(!controller.handle_key({FrontendKey::Character, 'a'}).handled,
                "Direct mode swallowed a client character.");
        require(controller.set_mode(InputMode::Ime).handled, "IME mode was not restored.");

        controller.switch_scheme(SchemeType::Quanpin);
        type(controller, "nihao");
        const auto clicked = controller.select_candidate(8);
        require(clicked.handled && clicked.commit == "candidate-8", "Absolute candidate selection committed wrong text.");
    }

    std::filesystem::remove_all(data_directory);
    return 0;
}
