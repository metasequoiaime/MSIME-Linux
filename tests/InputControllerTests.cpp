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
using metasequoia::linux_ime::CharacterWidth;
using metasequoia::linux_ime::InputController;
using metasequoia::linux_ime::InputMode;
using metasequoia::linux_ime::InputOptions;
using metasequoia::linux_ime::PunctuationMode;

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

FrontendKeyEvent punctuation(char value)
{
    return {FrontendKey::Punctuation, value};
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
        database.execute("INSERT INTO tbl_2_n VALUES('ni''men', 'nm', '你们', 200)");
        database.execute("INSERT INTO tbl_2_n VALUES('ni''men', 'nm', '君好', 100)");

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
        const auto page_click = controller.select_page_candidate(1);
        require(page_click.handled && page_click.commit == "candidate-4",
                "A page-relative candidate click committed the wrong absolute candidate.");

        type(controller, "nihao");
        controller.handle_key(key(FrontendKey::PageDown));
        controller.handle_key(key(FrontendKey::PageDown));
        controller.handle_key(key(FrontendKey::PageDown));
        const auto missing_page_digit = controller.handle_key(digit(9));
        require(missing_page_digit.handled && !missing_page_digit.commit.has_value() && controller.has_composition(),
                "An out-of-range page digit escaped into the client application.");

        const auto raw = controller.handle_key(key(FrontendKey::Enter));
        require(raw.handled && raw.commit == "nihao", "Enter did not commit raw input.");
        require(controller.highlighted_candidate() == 0, "Enter left a stale candidate highlight.");
        type(controller, "nihao");
        controller.handle_key(key(FrontendKey::Down));
        const auto cancel = controller.handle_key(key(FrontendKey::Escape));
        require(cancel.handled && !cancel.commit.has_value() && !controller.has_composition() &&
                    controller.highlighted_candidate() == 0,
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

        controller.switch_scheme(SchemeType::Quanpin);
        type(controller, "nihao");
        controller.handle_key(key(FrontendKey::Down));
        const auto direct_mode = controller.set_mode(InputMode::Direct);
        require(direct_mode.handled && direct_mode.commit == "candidate-1",
                "Direct mode did not commit the highlighted candidate before activation.");
        require(!controller.handle_key({FrontendKey::Character, 'a'}).handled,
                "Direct mode swallowed a client character.");
        require(controller.set_mode(InputMode::Ime).handled, "IME mode was not restored.");

        type(controller, "nihao");
        controller.handle_key(key(FrontendKey::Down));
        const auto toggled_direct = controller.toggle_mode();
        require(toggled_direct.handled && toggled_direct.commit == "candidate-1" &&
                    controller.mode() == InputMode::Direct && !controller.has_composition(),
                "Mode toggle did not commit the highlighted candidate exactly once.");
        const auto toggled_ime = controller.toggle_mode();
        require(toggled_ime.handled && !toggled_ime.commit.has_value() && controller.mode() == InputMode::Ime,
                "A second mode toggle repeated the previous composition commit.");

        type(controller, "nihao");
        const auto clicked = controller.select_candidate(8);
        require(clicked.handled && clicked.commit == "candidate-8", "Absolute candidate selection committed wrong text.");

        InputController punctuation_controller(SchemeType::Quanpin, 3);
        const auto plain_comma = punctuation_controller.handle_key(punctuation(','));
        require(plain_comma.handled && plain_comma.commit == "，",
                "Chinese punctuation was not committed without a composition.");

        type(punctuation_controller, "nihao");
        punctuation_controller.handle_key(key(FrontendKey::Down));
        const auto candidate_punctuation = punctuation_controller.handle_key(punctuation('!'));
        require(candidate_punctuation.handled && candidate_punctuation.commit == "candidate-1！" &&
                    !punctuation_controller.has_composition(),
                "Punctuation did not commit the highlighted candidate exactly once.");

        require(punctuation_controller.set_punctuation_mode(PunctuationMode::English).handled,
                "English punctuation mode was not activated.");
        require(!punctuation_controller.handle_key(punctuation(',')).handled,
                "Plain English punctuation was swallowed without a composition.");
        type(punctuation_controller, "nihao");
        const auto english_candidate_punctuation = punctuation_controller.handle_key(punctuation(','));
        require(english_candidate_punctuation.handled && english_candidate_punctuation.commit == "candidate-0,",
                "English punctuation did not follow the committed candidate.");

        require(punctuation_controller.set_punctuation_mode(PunctuationMode::Chinese).handled,
                "Chinese punctuation mode was not restored.");
        const auto opening_quote = punctuation_controller.handle_key(punctuation('\''));
        require(opening_quote.handled && opening_quote.commit == "‘",
                "A standalone apostrophe did not become Chinese punctuation.");
        type(punctuation_controller, "ni");
        const auto separator = punctuation_controller.handle_key(punctuation('\''));
        require(separator.handled && !separator.commit.has_value() && punctuation_controller.preedit() == "ni'",
                "An apostrophe inside a composition stopped acting as a pinyin separator.");
        punctuation_controller.reset();

        type(punctuation_controller, "nihao");
        punctuation_controller.handle_key(punctuation('='));
        require(punctuation_controller.highlighted_candidate() == 3,
                "Equals did not retain PageDown semantics during composition.");
        punctuation_controller.handle_key(punctuation('-'));
        require(punctuation_controller.highlighted_candidate() == 0,
                "Minus did not retain PageUp semantics during composition.");
        const auto default_comma = punctuation_controller.handle_key(punctuation(','));
        require(default_comma.handled && default_comma.commit == "candidate-0，",
                "Comma unexpectedly paged when comma/period paging was disabled.");

        InputOptions paging_options;
        paging_options.page_size = 3;
        paging_options.comma_period_paging = true;
        InputController paging_controller(SchemeType::Quanpin, paging_options);
        type(paging_controller, "nihao");
        paging_controller.handle_key(punctuation('='));
        paging_controller.handle_key(punctuation(','));
        require(paging_controller.has_composition() && paging_controller.highlighted_candidate() == 0,
                "Comma did not page when comma/period paging was enabled.");

        InputController disabled_edge_controller(SchemeType::Quanpin, 3);
        type(disabled_edge_controller, "nimen");
        const auto disabled_edge = disabled_edge_controller.handle_key(punctuation('['));
        require(disabled_edge.handled && disabled_edge.commit == "你们【",
                "A bracket selected a candidate edge while word-to-character was disabled.");

        InputOptions edge_options;
        edge_options.page_size = 3;
        edge_options.word_to_character = true;
        InputController edge_controller(SchemeType::Quanpin, edge_options);
        require(edge_controller.word_to_character() && !edge_controller.bracket_paging(),
                "Word-to-character options were not retained by the controller.");
        type(edge_controller, "nimen");
        edge_controller.handle_key(key(FrontendKey::Down));
        const auto first_edge = edge_controller.handle_key(punctuation('['));
        require(first_edge.handled && first_edge.commit == "君" && !edge_controller.has_composition(),
                "Left bracket did not commit the highlighted candidate's first Han character.");

        type(edge_controller, "nimen");
        edge_controller.handle_key(key(FrontendKey::Down));
        const auto last_edge = edge_controller.handle_key(punctuation(']'));
        require(last_edge.handled && last_edge.commit == "好" && !edge_controller.has_composition(),
                "Right bracket did not commit the highlighted candidate's last Han character.");

        InputOptions bracket_options;
        bracket_options.page_size = 3;
        bracket_options.word_to_character = true;
        bracket_options.bracket_paging = true;
        InputController bracket_controller(SchemeType::Quanpin, bracket_options);
        type(bracket_controller, "nihao");
        const auto bracket_page_down = bracket_controller.handle_key(punctuation(']'));
        require(bracket_page_down.handled && !bracket_page_down.commit.has_value() &&
                    bracket_controller.has_composition() && bracket_controller.highlighted_candidate() == 3,
                "Bracket paging did not take precedence over word-to-character selection.");
        bracket_controller.handle_key(punctuation('['));
        require(bracket_controller.highlighted_candidate() == 0,
                "Left bracket did not page backward when bracket paging was enabled.");

        InputOptions full_width_options;
        full_width_options.character_width = CharacterWidth::Full;
        full_width_options.punctuation_mode = PunctuationMode::English;
        InputController full_width_controller(SchemeType::Quanpin, full_width_options);
        full_width_controller.set_mode(InputMode::Direct);
        require(full_width_controller.handle_key({FrontendKey::Character, 'A'}).commit == "Ａ",
                "Direct uppercase input was not converted to full width.");
        require(full_width_controller.handle_key(digit(2)).commit == "２",
                "Direct digit input was not converted to full width.");
        require(full_width_controller.handle_key(key(FrontendKey::Space)).commit == "　",
                "Direct space input was not converted to full width.");
        require(full_width_controller.handle_key(punctuation('@')).commit == "＠",
                "Direct punctuation input was not converted to full width.");

        full_width_controller.set_punctuation_mode(PunctuationMode::Chinese);
        require(full_width_controller.handle_key(punctuation('$')).commit == "￥",
                "Chinese punctuation did not take precedence over full-width conversion.");

        InputController toggle_controller(SchemeType::Quanpin, 3);
        type(toggle_controller, "nihao");
        const auto punctuation_toggle = toggle_controller.handle_key(key(FrontendKey::TogglePunctuation));
        require(punctuation_toggle.handled && !punctuation_toggle.commit.has_value() &&
                    toggle_controller.punctuation_mode() == PunctuationMode::English &&
                    toggle_controller.has_composition(),
                "Toggling punctuation changed the active composition.");
        const auto width_toggle = toggle_controller.handle_key(key(FrontendKey::ToggleWidth));
        require(width_toggle.handled && !width_toggle.commit.has_value() &&
                    toggle_controller.character_width() == CharacterWidth::Full && toggle_controller.has_composition(),
                "Toggling character width changed the active composition.");
    }

    std::filesystem::remove_all(data_directory);
    return 0;
}
