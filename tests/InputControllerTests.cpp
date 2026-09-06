#include "../src/InputController.h"
#include "../vendor/MetasequoiaImeEngine/core/data_path.h"

#include "../vendor/MetasequoiaImeEngine/english/english_dictionary.h"
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>

namespace
{
using metasequoia::FrequencyAdjustmentMode;
using metasequoia::LocalInputMode;
using metasequoia::linux_ime::CharacterWidth;
using metasequoia::linux_ime::FrontendKey;
using metasequoia::linux_ime::FrontendKeyEvent;
using metasequoia::linux_ime::InputController;
using metasequoia::linux_ime::InputMode;
using metasequoia::linux_ime::InputOptions;
using metasequoia::linux_ime::PreeditStyle;
using metasequoia::linux_ime::PunctuationLock;
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

    ~Database()
    {
        sqlite3_close(database_);
    }

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

    std::int64_t query_integer(const std::string &sql)
    {
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(database_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK ||
            sqlite3_step(statement) != SQLITE_ROW)
        {
            sqlite3_finalize(statement);
            throw std::runtime_error("Failed to query the input-controller test dictionary.");
        }
        const std::int64_t value = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        return value;
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

void write_file(const std::filesystem::path &path, const std::string &contents)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path);
    stream << contents;
    if (!stream)
    {
        throw std::runtime_error("Failed to prepare an input-controller helpcode fixture.");
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

FrontendKeyEvent digit(unsigned value, bool shift_only = false)
{
    FrontendKeyEvent event{FrontendKey::Digit};
    event.digit = value;
    event.shift_only = shift_only;
    return event;
}

FrontendKeyEvent punctuation(char value, std::optional<char32_t> preceding_character = std::nullopt)
{
    FrontendKeyEvent event{FrontendKey::Punctuation, value};
    event.preceding_character = preceding_character;
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
        const std::filesystem::path helpcode_directory = data_directory / "helpcodes";
        write_file(helpcode_directory / "helpcode.txt", "你=ab\n君=cd\n们=ef\n好=gh\n");
        write_file(helpcode_directory / "zrm_helpcode_big_unique.txt", "你=cb\n君=ad\n们=ef\n好=gh\n");
        write_file(helpcode_directory / "shouyou2_0_helpcode.txt", "你=ab\n君=cd\n们=ef\n好=gh\n");
        write_file(helpcode_directory / "shouyouplus_helpcode.txt", "你=ab\n君=cd\n们=ef\n好=gh\n");
        write_file(helpcode_directory / "xiaohe_helpcode.txt", "你=ab\n君=cd\n们=ef\n好=gh\n");

        Database database(data_directory / "msime.db");
        database.execute("CREATE TABLE tbl_2_n(key TEXT, jp TEXT, value TEXT, weight INTEGER)");
        for (int index = 0; index < 12; ++index)
        {
            database.execute("INSERT INTO tbl_2_n VALUES('ni''hao', 'nh', 'candidate-" + std::to_string(index) + "', " +
                             std::to_string(120 - index) + ")");
        }
        database.execute("INSERT INTO tbl_2_n VALUES('ni''men', 'nm', '你们', 200)");
        database.execute("INSERT INTO tbl_2_n VALUES('ni''men', 'nm', '君好', 100)");
        database.execute("INSERT INTO tbl_2_n VALUES('na''han', 'nh', '呐喊', 99)");
        database.execute("INSERT INTO tbl_2_n VALUES('ni''shuo', 'ns', '你说', 98)");
        database.execute("INSERT INTO tbl_2_n VALUES('ni''si', 'ns', '你思', 97)");
        database.execute("CREATE TABLE tbl_1_x(key TEXT, jp TEXT, value TEXT, weight INTEGER)");
        database.execute("INSERT INTO tbl_1_x VALUES('xi', 'x', '甲频', 100)");
        database.execute("INSERT INTO tbl_1_x VALUES('xi', 'x', '乙频', 90)");
        database.execute("INSERT INTO tbl_1_x VALUES('xi', 'x', '丙频', 80)");
        database.execute("INSERT INTO tbl_1_x VALUES('xi', 'x', '丁频', 70)");
        database.execute("INSERT INTO tbl_1_x VALUES('xi', 'x', '戊频', 60)");
        database.execute("INSERT INTO tbl_1_x VALUES('xi', 'x', '己频', 50)");
        database.execute("CREATE TABLE tbl_1_s(key TEXT,jp TEXT,value TEXT,weight INTEGER)");
        database.execute("INSERT INTO tbl_1_s VALUES('shui','s','水',100)");
        database.execute("CREATE TABLE tbl_1_l(key TEXT,jp TEXT,value TEXT,weight INTEGER)");
        database.execute("INSERT INTO tbl_1_l VALUES('lin','l','林',100)");
        database.execute("CREATE TABLE tbl_2_s(key TEXT,jp TEXT,value TEXT,weight INTEGER)");
        database.execute("CREATE TABLE tbl_3_s(key TEXT,jp TEXT,value TEXT,weight INTEGER)");
        database.execute("CREATE TABLE tbl_4_s(key TEXT,jp TEXT,value TEXT,weight INTEGER)");
        database.execute("CREATE TABLE tbl_5_s(key TEXT,jp TEXT,value TEXT,weight INTEGER)");
        {
            InputOptions partial_options;
            InputController partial(SchemeType::Quanpin, partial_options);
            type(partial, "shui'lin");
            const auto first = partial.handle_key(key(FrontendKey::Space));
            require(first.commit == "水" && partial.preedit() == "lin" && partial.has_composition(),
                    "Space discarded the unconsumed pinyin suffix.");
            const auto last = partial.handle_key(key(FrontendKey::Space));
            require(last.commit == "林" && !partial.has_composition(),
                    "The second selection did not finish the remaining input.");
            type(partial, "shui'lin'lin");
            const auto punctuated = partial.handle_key(punctuation(','));
            require(punctuated.commit == "水林林，" && !partial.has_composition(),
                    "Punctuation lost the unselected suffix.");
            type(partial, "shui'lin'lin'lin");
            FrontendKeyEvent shortcut{FrontendKey::Character, 'c'};
            shortcut.host_shortcut = true;
            const auto passed = partial.handle_key(shortcut);
            require(!passed.handled && passed.commit == "水林林林" && !partial.has_composition(),
                    "Host passthrough left a hidden partial composition.");
            type(partial, "shui'lin'lin'lin'lin");
            require(partial.switch_scheme(SchemeType::Wubi).commit == "水林林林林" && !partial.has_composition(),
                    "Scheme switching discarded the pending suffix.");
        }
        database.execute("CREATE TABLE quick_parases(key TEXT,value TEXT,weight INTEGER)");
        database.execute("INSERT INTO quick_parases VALUES('ab','控制器短语一',20)");
        database.execute("INSERT INTO quick_parases VALUES('aa','控制器短语二',10)");
        Database others_database(data_directory / "others.db");
        others_database.execute("CREATE TABLE emoji_pinyin(key TEXT,emoji TEXT,sort_order INTEGER)");
        others_database.execute("INSERT INTO emoji_pinyin VALUES('xi','✨',0)");
        others_database.execute("INSERT INTO emoji_pinyin VALUES('xiaolian','😀',10)");
        others_database.execute("CREATE TABLE kaomoji(pinyin TEXT,jianpin TEXT,kaomoji TEXT,sort_order INTEGER)");
        others_database.execute("INSERT INTO kaomoji VALUES('xi','x','(^_^)',0)");
        others_database.execute("INSERT INTO kaomoji VALUES('haixiu','hx','(*/ω＼*)',10)");
        Database english_database(data_directory / "english.db");
        english_database.execute("CREATE TABLE english_words(word TEXT COLLATE BINARY NOT NULL,display TEXT NOT NULL,"
                                 "weight INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(word,display)) WITHOUT ROWID");
        english_database.execute("CREATE TABLE en_zh_glosses(english TEXT PRIMARY KEY,chinese_gloss TEXT NOT NULL)");
        english_database.execute("CREATE TABLE zh_en_glosses(chinese TEXT PRIMARY KEY,english_gloss TEXT NOT NULL)");
        english_database.execute("INSERT INTO english_words VALUES('xi','Xi',100)");
        english_database.execute("INSERT INTO english_words VALUES('xigua','Xigua',90)");
        english_database.execute("INSERT INTO english_words VALUES('hello','Hello',100)");
        english_database.execute("INSERT INTO english_words VALUES('help','Help',90)");

        InputController online_controller(SchemeType::Quanpin, 3);
        const std::uint64_t initial_generation = online_controller.online_generation();
        for (const char character : std::string("nihao"))
        {
            const std::uint64_t before_character = online_controller.online_generation();
            require(online_controller.handle_key({FrontendKey::Character, character}).handled &&
                        online_controller.online_generation() > before_character,
                    "An accepted composition character did not advance the online generation.");
        }
        const auto online_request = online_controller.online_request();
        require(online_request.has_value() && online_request->generation == online_controller.online_generation() &&
                    online_request->generation > initial_generation && online_request->query.cloud_eligible &&
                    online_request->query.ai_eligible,
                "The controller did not expose a current online request generation.");

        const std::uint64_t before_navigation = online_controller.online_generation();
        online_controller.handle_key(key(FrontendKey::Down));
        online_controller.handle_key(key(FrontendKey::Down));
        require(online_controller.online_generation() == before_navigation,
                "Candidate navigation invalidated an otherwise current online request.");
        const std::size_t highlighted_before_online = online_controller.highlighted_candidate();
        const std::size_t candidate_count_before_online = online_controller.candidates().size();
        require(online_controller.apply_online_candidate(online_request->generation, online_request->query, "云候选",
                                                         CandidateSource::CloudSuggestion),
                "The controller rejected a current cloud result.");
        require(online_controller.candidates().size() == candidate_count_before_online + 1 &&
                    online_controller.candidates()[1].word == "云候选" &&
                    online_controller.highlighted_candidate() == highlighted_before_online &&
                    online_controller.online_generation() == online_request->generation,
                "Applying a new cloud row reset the cursor or advanced the composition generation.");

        const std::size_t candidate_count_before_replacement = online_controller.candidates().size();
        require(online_controller.apply_online_candidate(online_request->generation, online_request->query, "新云候选",
                                                         CandidateSource::CloudSuggestion),
                "The controller rejected a replacement cloud result.");
        require(online_controller.candidates().size() == candidate_count_before_replacement &&
                    online_controller.highlighted_candidate() == highlighted_before_online,
                "Replacing an online row reset the highlighted candidate.");

        const auto stale_request = online_controller.online_request();
        require(stale_request.has_value(), "The stale-result fixture did not expose an online request.");
        const std::uint64_t before_backspace = online_controller.online_generation();
        require(online_controller.handle_key(key(FrontendKey::Backspace)).handled &&
                    online_controller.online_generation() > before_backspace,
                "Backspace did not advance the online generation.");
        require(!online_controller.apply_online_candidate(stale_request->generation, stale_request->query, "过期候选",
                                                          CandidateSource::CloudSuggestion),
                "A cloud result survived a composition edit.");

        const std::uint64_t before_cancel = online_controller.online_generation();
        require(online_controller.handle_key(key(FrontendKey::Escape)).handled &&
                    online_controller.online_generation() > before_cancel,
                "Cancel did not advance the online generation.");
        const std::uint64_t before_reset = online_controller.online_generation();
        online_controller.reset();
        require(online_controller.online_generation() > before_reset, "Reset did not advance the online generation.");

        const std::uint64_t before_direct = online_controller.online_generation();
        require(online_controller.set_mode(InputMode::Direct).handled &&
                    online_controller.online_generation() > before_direct,
                "Entering direct mode did not advance the online generation.");
        const std::uint64_t before_ime = online_controller.online_generation();
        require(online_controller.set_mode(InputMode::Ime).handled &&
                    online_controller.online_generation() > before_ime,
                "Returning to IME mode did not advance the online generation.");

        const std::uint64_t before_scheme = online_controller.online_generation();
        require(online_controller.switch_scheme(SchemeType::Wubi).handled &&
                    online_controller.online_generation() > before_scheme,
                "Scheme switching did not advance the online generation.");
        online_controller.switch_scheme(SchemeType::Quanpin);

        const std::uint64_t before_punctuation_mode = online_controller.online_generation();
        require(online_controller.toggle_punctuation_mode().handled &&
                    online_controller.online_generation() > before_punctuation_mode,
                "Punctuation mode switching did not advance the online generation.");
        const std::uint64_t before_width_mode = online_controller.online_generation();
        require(online_controller.toggle_character_width().handled &&
                    online_controller.online_generation() > before_width_mode,
                "Character-width switching did not advance the online generation.");
        const std::uint64_t before_english_mode = online_controller.online_generation();
        require(online_controller.toggle_dedicated_english_mode().handled &&
                    online_controller.online_generation() > before_english_mode,
                "Dedicated English mode switching did not advance the online generation.");
        online_controller.toggle_dedicated_english_mode();

        type(online_controller, "nihao");
        const auto focus_request = online_controller.online_request();
        require(focus_request.has_value(), "The context-invalidation fixture did not expose an online request.");
        const std::uint64_t before_invalidation = online_controller.online_generation();
        online_controller.invalidate_context();
        require(online_controller.online_generation() > before_invalidation &&
                    !online_controller.apply_online_candidate(focus_request->generation, focus_request->query,
                                                              "失焦候选", CandidateSource::CloudSuggestion),
                "Context invalidation did not reject an in-flight online result.");

        const std::uint64_t before_commit = online_controller.online_generation();
        require(online_controller.handle_key(key(FrontendKey::Enter)).handled &&
                    online_controller.online_generation() > before_commit,
                "Commit did not advance the online generation.");
        type(online_controller, "nihao");
        const std::uint64_t before_candidate_commit = online_controller.online_generation();
        require(online_controller.handle_key(key(FrontendKey::Space)).handled &&
                    online_controller.online_generation() > before_candidate_commit,
                "Candidate commit did not advance the online generation.");

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
        require(clicked.handled && clicked.commit == "candidate-8",
                "Absolute candidate selection committed wrong text.");

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

        InputController lock_controller(SchemeType::Quanpin, 3);
        lock_controller.set_punctuation_lock(PunctuationLock::Follow);
        require(lock_controller.punctuation_mode() == PunctuationMode::Chinese,
                "Follow did not derive Chinese punctuation for Chinese input.");
        lock_controller.set_mode(InputMode::Direct);
        require(lock_controller.punctuation_mode() == PunctuationMode::English,
                "Follow did not switch punctuation with the language.");
        lock_controller.set_mode(InputMode::Ime);
        require(lock_controller.punctuation_mode() == PunctuationMode::Chinese,
                "Follow did not switch punctuation back with the language.");

        // A manual toggle stands until the next language switch recomputes it, which is what the
        // Windows implementation does on its mode-switch message.
        lock_controller.set_punctuation_mode(PunctuationMode::English);
        require(lock_controller.punctuation_mode() == PunctuationMode::English,
                "Follow refused a manual punctuation toggle.");
        lock_controller.set_mode(InputMode::Direct);
        require(lock_controller.punctuation_mode() == PunctuationMode::English,
                "A manual toggle survived the language switch that should have recomputed it.");

        lock_controller.set_punctuation_lock(PunctuationLock::Chinese);
        require(lock_controller.punctuation_mode() == PunctuationMode::Chinese,
                "A Chinese lock did not take effect immediately.");
        lock_controller.set_mode(InputMode::Ime);
        lock_controller.set_mode(InputMode::Direct);
        require(lock_controller.punctuation_mode() == PunctuationMode::Chinese,
                "A Chinese lock did not hold across language switches.");
        lock_controller.set_punctuation_mode(PunctuationMode::English);
        lock_controller.set_mode(InputMode::Ime);
        require(lock_controller.punctuation_mode() == PunctuationMode::Chinese,
                "A Chinese lock did not reassert itself after a manual toggle.");

        lock_controller.set_punctuation_lock(PunctuationLock::English);
        require(lock_controller.punctuation_mode() == PunctuationMode::English,
                "An English lock did not take effect immediately.");
        lock_controller.set_mode(InputMode::Direct);
        lock_controller.set_mode(InputMode::Ime);
        require(lock_controller.punctuation_mode() == PunctuationMode::English,
                "An English lock did not hold across language switches.");
        require(!punctuation_controller.handle_key(punctuation(',')).handled,
                "Plain English punctuation was swallowed without a composition.");
        type(punctuation_controller, "nihao");
        const auto english_candidate_punctuation = punctuation_controller.handle_key(punctuation(','));
        require(english_candidate_punctuation.handled && english_candidate_punctuation.commit == "candidate-0,",
                "English punctuation did not follow the committed candidate.");

        require(punctuation_controller.set_punctuation_mode(PunctuationMode::Chinese).handled,
                "Chinese punctuation mode was not restored.");
        const auto opening_quote = punctuation_controller.handle_key(punctuation('\''));
        require(opening_quote.handled && opening_quote.commit == "‘’" && opening_quote.cursor_left == 1,
                "A standalone apostrophe did not become a paired Chinese punctuation commit.");
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
        require(default_comma.handled && default_comma.commit == "candidate-0,",
                "A candidate's ASCII tail was ignored by smart punctuation.");

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
        require(disabled_edge.handled && disabled_edge.commit == "你们【】" && disabled_edge.cursor_left == 1,
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

        auto now = std::chrono::steady_clock::time_point{};
        InputOptions smart_options;
        smart_options.paired_punctuation = false;
        smart_options.now = [&now] { return now; };
        InputController smart_controller(SchemeType::Quanpin, smart_options);
        const auto smart_comma = smart_controller.handle_key(punctuation(',', U'A'));
        require(smart_comma.handled && smart_comma.commit == "," && smart_comma.delete_before == 0,
                "Smart punctuation did not keep comma ASCII after an ASCII letter.");

        now += std::chrono::seconds(1);
        const auto repeated_comma = smart_controller.handle_key(punctuation(',', U','));
        require(repeated_comma.handled && repeated_comma.commit == "，" && repeated_comma.delete_before == 1,
                "Repeated smart punctuation did not replace the recent ASCII comma.");

        const auto smart_period = smart_controller.handle_key(punctuation('.', U'7'));
        require(smart_period.handled && smart_period.commit == ".",
                "Smart punctuation did not keep period ASCII after an ASCII digit.");
        now += std::chrono::milliseconds(2001);
        const auto expired_period = smart_controller.handle_key(punctuation('.', U'.'));
        require(expired_period.handled && expired_period.commit == "。" && expired_period.delete_before == 0,
                "Expired smart punctuation history deleted application text.");

        const auto missing_context = smart_controller.handle_key(punctuation(':'));
        require(missing_context.handled && missing_context.commit == "：",
                "Missing surrounding text did not fall back to Chinese punctuation.");

        const auto smart_before_reset = smart_controller.handle_key(punctuation(',', U'B'));
        require(smart_before_reset.commit == ",", "The reset invalidation fixture did not commit ASCII punctuation.");
        smart_controller.reset();
        const auto after_reset = smart_controller.handle_key(punctuation(',', U','));
        require(after_reset.commit == "，" && after_reset.delete_before == 0,
                "Reset left stale smart punctuation replacement state.");

        const auto smart_before_navigation = smart_controller.handle_key(punctuation(':', U'C'));
        require(smart_before_navigation.commit == ":",
                "The navigation invalidation fixture did not commit ASCII punctuation.");
        smart_controller.handle_key(key(FrontendKey::PageDown));
        const auto after_navigation = smart_controller.handle_key(punctuation(':', U':'));
        require(after_navigation.commit == "：" && after_navigation.delete_before == 0,
                "Navigation left stale smart punctuation replacement state.");

        InputController candidate_smart_controller(SchemeType::Quanpin, smart_options);
        type(candidate_smart_controller, "nihao");
        const auto candidate_smart_comma = candidate_smart_controller.handle_key(punctuation(','));
        require(candidate_smart_comma.commit == "candidate-0,",
                "Candidate commit did not use its ASCII tail for smart punctuation.");
        now += std::chrono::seconds(1);
        const auto repeated_candidate_comma = candidate_smart_controller.handle_key(punctuation(',', U','));
        require(repeated_candidate_comma.commit == "，" && repeated_candidate_comma.delete_before == 1,
                "Repeated smart punctuation did not replace ASCII punctuation committed with a candidate.");

        InputOptions disabled_smart_options;
        disabled_smart_options.smart_punctuation = false;
        disabled_smart_options.paired_punctuation = false;
        InputController disabled_smart_controller(SchemeType::Quanpin, disabled_smart_options);
        require(disabled_smart_controller.handle_key(punctuation(',', U'A')).commit == "，",
                "Disabled smart punctuation still emitted ASCII punctuation.");

        InputOptions pair_options;
        pair_options.smart_punctuation = false;
        pair_options.paired_punctuation = true;
        InputController pair_controller(SchemeType::Quanpin, pair_options);
        const auto double_quote_pair = pair_controller.handle_key(punctuation('"'));
        require(double_quote_pair.handled && double_quote_pair.commit == "“”" && double_quote_pair.cursor_left == 1,
                "Double quote pair did not request one cursor-left event.");
        const auto repeated_double_quote_pair = pair_controller.handle_key(punctuation('"'));
        require(repeated_double_quote_pair.commit == "“”" && repeated_double_quote_pair.cursor_left == 1,
                "A repeated paired quote started with the closing quote.");
        const auto parenthesis_pair = pair_controller.handle_key(punctuation('('));
        require(parenthesis_pair.commit == "（）" && parenthesis_pair.cursor_left == 1,
                "Opening parenthesis did not add its closing pair.");
        const auto outer_book_title_pair = pair_controller.handle_key(punctuation('<'));
        const auto inner_book_title_pair = pair_controller.handle_key(punctuation('<'));
        require(outer_book_title_pair.commit == "《》" && outer_book_title_pair.cursor_left == 1 &&
                    inner_book_title_pair.commit == "〈〉" && inner_book_title_pair.cursor_left == 1,
                "Nested book-title marks did not produce outer and inner pairs.");
        pair_controller.invalidate_context();
        require(pair_controller.handle_key(punctuation('<')).commit == "《》",
                "Context invalidation left stale book-title nesting.");
        pair_controller.reset();
        require(pair_controller.handle_key(punctuation('<')).commit == "《》", "Reset left stale book-title nesting.");
        FrontendKeyEvent document_navigation;
        document_navigation.host_shortcut = true;
        pair_controller.handle_key(document_navigation);
        require(pair_controller.handle_key(punctuation('<')).commit == "《》",
                "Passthrough document navigation left stale book-title nesting.");

        InputController type_over_controller(SchemeType::Quanpin, pair_options);
        const auto type_over_parenthesis_pair = type_over_controller.handle_key(punctuation('('));
        require(type_over_parenthesis_pair.commit == "（）" && type_over_parenthesis_pair.cursor_left == 1 &&
                    type_over_parenthesis_pair.cursor_right == 0,
                "The type-over fixture did not insert a closing parenthesis to type over.");
        const auto typed_over_parenthesis = type_over_controller.handle_key(punctuation(')'));
        require(typed_over_parenthesis.handled && !typed_over_parenthesis.commit.has_value() &&
                    typed_over_parenthesis.cursor_right == 1 && typed_over_parenthesis.cursor_left == 0,
                "Typing the closing parenthesis emitted a second one instead of stepping over the inserted one.");
        const auto unpaired_parenthesis = type_over_controller.handle_key(punctuation(')'));
        require(unpaired_parenthesis.handled && unpaired_parenthesis.commit == "）" &&
                    unpaired_parenthesis.cursor_right == 0,
                "A closing parenthesis with nothing to type over stopped committing its own mark.");

        const auto type_over_bracket_pair = type_over_controller.handle_key(punctuation('['));
        require(type_over_bracket_pair.commit == "【】" && type_over_bracket_pair.cursor_left == 1,
                "The type-over fixture did not insert a closing bracket to type over.");
        const auto typed_over_bracket = type_over_controller.handle_key(punctuation(']'));
        require(typed_over_bracket.handled && !typed_over_bracket.commit.has_value() &&
                    typed_over_bracket.cursor_right == 1,
                "Typing the closing bracket emitted a second one instead of stepping over the inserted one.");

        const auto type_over_outer_pair = type_over_controller.handle_key(punctuation('<'));
        const auto type_over_inner_pair = type_over_controller.handle_key(punctuation('<'));
        require(type_over_outer_pair.commit == "《》" && type_over_inner_pair.commit == "〈〉",
                "The nested type-over fixture did not insert both book-title pairs.");
        const auto typed_over_inner = type_over_controller.handle_key(punctuation('>'));
        const auto typed_over_outer = type_over_controller.handle_key(punctuation('>'));
        require(typed_over_inner.handled && !typed_over_inner.commit.has_value() &&
                    typed_over_inner.cursor_right == 1 && typed_over_outer.handled &&
                    !typed_over_outer.commit.has_value() && typed_over_outer.cursor_right == 1,
                "Nested book-title marks did not type over the inner closing mark and then the outer one.");

        require(type_over_controller.handle_key(punctuation('(')).commit == "（）",
                "The mismatched type-over fixture did not insert a closing parenthesis.");
        const auto mismatched_closing = type_over_controller.handle_key(punctuation('>'));
        require(mismatched_closing.handled && mismatched_closing.commit == "》" && mismatched_closing.cursor_right == 0,
                "A different closing mark typed over the pending parenthesis instead of committing itself.");
        const auto surviving_type_over = type_over_controller.handle_key(punctuation(')'));
        require(surviving_type_over.handled && !surviving_type_over.commit.has_value() &&
                    surviving_type_over.cursor_right == 1,
                "A mismatched closing mark discarded the parenthesis that was still waiting to be typed over.");

        require(type_over_controller.handle_key(punctuation('(')).commit == "（）",
                "The caret-move type-over fixture did not insert a closing parenthesis.");
        type_over_controller.invalidate_context();
        const auto closing_after_caret_move = type_over_controller.handle_key(punctuation(')'));
        require(closing_after_caret_move.handled && closing_after_caret_move.commit == "）" &&
                    closing_after_caret_move.cursor_right == 0,
                "A caret move left a stale type-over that swallowed the closing parenthesis.");
        require(type_over_controller.handle_key(punctuation('(')).commit == "（）",
                "The reset type-over fixture did not insert a closing parenthesis.");
        type_over_controller.reset();
        const auto closing_after_reset = type_over_controller.handle_key(punctuation(')'));
        require(closing_after_reset.handled && closing_after_reset.commit == "）" &&
                    closing_after_reset.cursor_right == 0,
                "Reset left a stale type-over that swallowed the closing parenthesis.");

        InputController type_over_composition_controller(SchemeType::Quanpin, pair_options);
        require(type_over_composition_controller.handle_key(punctuation('<')).commit == "《》",
                "The quoted-text type-over fixture did not insert a closing book-title mark.");
        type(type_over_composition_controller, "nihao");
        require(type_over_composition_controller.handle_key(key(FrontendKey::Space)).commit == "candidate-0",
                "The quoted-text type-over fixture did not commit the text between the marks.");
        const auto typed_over_after_text = type_over_composition_controller.handle_key(punctuation('>'));
        require(typed_over_after_text.handled && !typed_over_after_text.commit.has_value() &&
                    typed_over_after_text.cursor_right == 1,
                "Typing the closing book-title mark after the quoted text emitted a second one.");

        InputOptions quanpin_pinyin_options;
        quanpin_pinyin_options.preedit_style = PreeditStyle::Pinyin;
        InputController quanpin_pinyin_controller(SchemeType::Quanpin, quanpin_pinyin_options);
        require(quanpin_pinyin_controller.quanpin_helpcode_enabled() &&
                    quanpin_pinyin_controller.shuangpin_helpcode_enabled() &&
                    quanpin_pinyin_controller.quanpin_helpcode_schema() == "lantian" &&
                    quanpin_pinyin_controller.shuangpin_helpcode_schema() == "lantian",
                "Windows-compatible helpcode defaults were not retained by the controller.");
        type(quanpin_pinyin_controller, "nimenC");
        require(quanpin_pinyin_controller.preedit() == "ni'men'C" && !quanpin_pinyin_controller.candidates().empty() &&
                    quanpin_pinyin_controller.candidates().front().word == "君好",
                "Quanpin pinyin preedit or Lantian helpcode selection was not applied.");

        InputOptions ziranma_options;
        ziranma_options.quanpin_helpcode_schema = "ziranma";
        InputController ziranma_controller(SchemeType::Quanpin, ziranma_options);
        type(ziranma_controller, "nimenA");
        require(!ziranma_controller.candidates().empty() && ziranma_controller.candidates().front().word == "君好",
                "The independent Quanpin helpcode schema was not selected before input.");
        ziranma_controller.reset();
        type(ziranma_controller, "nimenAG");
        InputController competing_lantian_controller(SchemeType::Quanpin, quanpin_pinyin_options);
        type(competing_lantian_controller, "nimen");
        const auto ziranma_backspace = ziranma_controller.handle_key(key(FrontendKey::Backspace));
        require(ziranma_backspace.handled && !ziranma_controller.candidates().empty() &&
                    ziranma_controller.candidates().front().word == "君好",
                "Backspace reused another input context's process-global helpcode schema.");

        InputOptions shuangpin_pinyin_options;
        shuangpin_pinyin_options.preedit_style = PreeditStyle::Pinyin;
        shuangpin_pinyin_options.shuangpin_helpcode_schema = "lantian";
        InputController shuangpin_pinyin_controller(SchemeType::Shuangpin, shuangpin_pinyin_options);
        type(shuangpin_pinyin_controller, "nihcc");
        require(shuangpin_pinyin_controller.preedit() == "ni'hao'c" && shuangpin_pinyin_controller.has_composition(),
                "Shuangpin pinyin preedit did not expose normalized segmentation.");

        InputOptions separate_helpcode_flags;
        separate_helpcode_flags.quanpin_helpcode_enabled = false;
        separate_helpcode_flags.shuangpin_helpcode_enabled = true;
        InputController separate_helpcode_controller(SchemeType::Quanpin, separate_helpcode_flags);
        type(separate_helpcode_controller, "ni");
        require(!separate_helpcode_controller.handle_key({FrontendKey::Character, 'C'}).handled,
                "Disabled Quanpin helpcode consumed uppercase input.");
        separate_helpcode_controller.switch_scheme(SchemeType::Shuangpin);
        type(separate_helpcode_controller, "nihc");
        require(separate_helpcode_controller.handle_key({FrontendKey::Character, 'C'}).handled,
                "Switching schemes lost Shuangpin's independent helpcode enable flag.");
        separate_helpcode_controller.switch_scheme(SchemeType::Quanpin);
        type(separate_helpcode_controller, "ni");
        require(!separate_helpcode_controller.handle_key({FrontendKey::Character, 'C'}).handled,
                "Returning to Quanpin retained Shuangpin's helpcode enable flag.");

        InputOptions hidden_options;
        hidden_options.preedit_style = PreeditStyle::Hidden;
        InputController hidden_controller(SchemeType::Quanpin, hidden_options);
        type(hidden_controller, "nihao");
        require(hidden_controller.preedit().empty() && hidden_controller.has_composition() &&
                    !hidden_controller.candidates().empty(),
                "Hidden inline preedit also hid the active composition or candidates.");

        bool rejected_invalid_schema = false;
        try
        {
            InputOptions invalid_helpcode_options;
            invalid_helpcode_options.quanpin_helpcode_schema = "unknown";
            InputController invalid_helpcode_controller(SchemeType::Quanpin, invalid_helpcode_options);
            (void)invalid_helpcode_controller;
        }
        catch (const std::invalid_argument &)
        {
            rejected_invalid_schema = true;
        }
        require(rejected_invalid_schema, "The controller accepted an unsupported helpcode schema.");

        InputOptions frequency_options;
        frequency_options.frequency_adjustment_mode = FrequencyAdjustmentMode::Pin;
        frequency_options.frequency_trigger_count = 1;
        frequency_options.frequency_linear_step = 2;
        InputController frequency_controller(SchemeType::Quanpin, frequency_options);
        require(frequency_controller.frequency_adjustment_mode() == FrequencyAdjustmentMode::Pin &&
                    frequency_controller.frequency_trigger_count() == 1 &&
                    frequency_controller.frequency_linear_step() == 2,
                "The controller did not retain frequency adjustment settings.");
        type(frequency_controller, "xi");
        const auto learned_frequency = frequency_controller.select_candidate(5);
        require(learned_frequency.handled && learned_frequency.commit == "己频" &&
                    !learned_frequency.diagnostic.has_value(),
                "Frequency learning changed a successful controller commit.");
        type(frequency_controller, "xi");
        require(!frequency_controller.candidates().empty() && frequency_controller.candidates().front().word == "己频",
                "The controller did not expose the shared Engine's learned candidate order.");

        InputController unicode_controller(SchemeType::Quanpin, 3);
        FrontendKeyEvent unicode_prefix{FrontendKey::Character, 'U'};
        unicode_prefix.shift_only = true;
        require(unicode_controller.handle_key(unicode_prefix).handled &&
                    unicode_controller.local_input_mode() == LocalInputMode::Unicode &&
                    unicode_controller.preedit() == "U",
                "The controller did not enter Unicode mode from Shift+U.");
        require(unicode_controller.handle_key(digit(4)).handled, "A bare digit did not extend Unicode composition.");
        require(unicode_controller.handle_key({FrontendKey::Character, 'e'}).handled,
                "A hexadecimal letter did not extend Unicode composition.");
        require(unicode_controller.handle_key(digit(0)).handled && unicode_controller.handle_key(digit(0)).handled &&
                    unicode_controller.candidates().size() == 1 && unicode_controller.candidates().front().word == "一",
                "The controller did not expose the generated Unicode candidate.");
        const auto unicode_space = unicode_controller.handle_key(key(FrontendKey::Space));
        require(unicode_space.handled && unicode_space.commit == "一" &&
                    unicode_controller.local_input_mode() == LocalInputMode::None,
                "Space did not commit and leave Unicode mode.");

        require(unicode_controller.handle_key(unicode_prefix).handled &&
                    unicode_controller.handle_key(punctuation('+')).handled,
                "The controller did not route the optional Unicode plus prefix.");
        for (const char character : std::string("1f600"))
        {
            const auto result = character >= '0' && character <= '9'
                                    ? unicode_controller.handle_key(digit(static_cast<unsigned>(character - '0')))
                                    : unicode_controller.handle_key({FrontendKey::Character, character});
            require(result.handled, "The controller rejected Unicode hexadecimal input.");
        }
        const auto unicode_shift_digit = unicode_controller.handle_key(digit(1, true));
        require(unicode_shift_digit.handled && unicode_shift_digit.commit == "😀" &&
                    unicode_controller.local_input_mode() == LocalInputMode::None,
                "Shift+1 did not select the Unicode candidate.");

        require(unicode_controller.handle_key(unicode_prefix).handled,
                "The controller could not re-enter Unicode mode for shifted punctuation selection.");
        for (const char character : std::string("4e00"))
        {
            const auto result = character >= '0' && character <= '9'
                                    ? unicode_controller.handle_key(digit(static_cast<unsigned>(character - '0')))
                                    : unicode_controller.handle_key({FrontendKey::Character, character});
            require(result.handled, "The controller rejected Unicode input before shifted punctuation selection.");
        }
        FrontendKeyEvent shifted_exclamation = punctuation('!');
        shifted_exclamation.shift_only = true;
        const auto unicode_shift_symbol = unicode_controller.handle_key(shifted_exclamation);
        require(unicode_shift_symbol.handled && unicode_shift_symbol.commit == "一" &&
                    unicode_controller.local_input_mode() == LocalInputMode::None,
                "A physical Shift+1 key symbol did not select the Unicode candidate.");

        require(unicode_controller.handle_key(unicode_prefix).handled,
                "The controller could not re-enter Unicode mode for invalid-input handling.");
        const std::string unicode_preedit = unicode_controller.preedit();
        require(unicode_controller.handle_key(punctuation('?')).handled &&
                    unicode_controller.preedit() == unicode_preedit,
                "Invalid Unicode punctuation leaked to the application or changed preedit.");
        unicode_controller.handle_key(key(FrontendKey::Escape));

        InputOptions disabled_unicode_options;
        disabled_unicode_options.local_modes.unicode = false;
        InputController disabled_unicode_controller(SchemeType::Quanpin, disabled_unicode_options);
        require(!disabled_unicode_controller.handle_key(unicode_prefix).handled &&
                    !disabled_unicode_controller.has_composition() &&
                    disabled_unicode_controller.local_input_mode() == LocalInputMode::None,
                "A disabled Unicode shortcut swallowed Shift+U.");

        InputController date_time_controller(SchemeType::Quanpin, 3);
        FrontendKeyEvent date_time_prefix{FrontendKey::Character, 'T'};
        date_time_prefix.shift_only = true;
        require(date_time_controller.handle_key(date_time_prefix).handled &&
                    date_time_controller.local_input_mode() == LocalInputMode::DateTime &&
                    date_time_controller.preedit() == "T",
                "The controller did not enter date/time mode from Shift+T.");
        require(date_time_controller.handle_key({FrontendKey::Character, 'r'}).handled &&
                    date_time_controller.handle_key({FrontendKey::Character, 'q'}).handled &&
                    !date_time_controller.candidates().empty(),
                "The controller did not expose current-date candidates for rq.");
        const std::string current_date = date_time_controller.candidates().front().word;
        const auto date_space = date_time_controller.handle_key(key(FrontendKey::Space));
        require(date_space.handled && date_space.commit == current_date &&
                    date_time_controller.local_input_mode() == LocalInputMode::None,
                "Space did not commit and leave date/time mode.");

        InputOptions disabled_date_time_options;
        disabled_date_time_options.local_modes.date_time = false;
        InputController disabled_date_time_controller(SchemeType::Quanpin, disabled_date_time_options);
        require(!disabled_date_time_controller.handle_key(date_time_prefix).handled &&
                    !disabled_date_time_controller.has_composition() &&
                    disabled_date_time_controller.local_input_mode() == LocalInputMode::None,
                "A disabled date/time shortcut swallowed Shift+T.");

        InputController quick_phrase_controller(SchemeType::Quanpin, 3);
        FrontendKeyEvent quick_phrase_prefix{FrontendKey::Character, 'K'};
        quick_phrase_prefix.shift_only = true;
        require(quick_phrase_controller.handle_key(quick_phrase_prefix).handled &&
                    quick_phrase_controller.local_input_mode() == LocalInputMode::QuickPhrase &&
                    quick_phrase_controller.preedit() == "K",
                "The controller did not enter quick-phrase mode from Shift+K.");
        const auto quick_phrase_query = quick_phrase_controller.handle_key({FrontendKey::Character, 'a'});
        require(quick_phrase_query.handled && !quick_phrase_query.diagnostic.has_value() &&
                    quick_phrase_controller.candidates().size() == 2 &&
                    quick_phrase_controller.candidates().front().word == "控制器短语一",
                "The controller did not expose ordered quick-phrase candidates.");
        const auto quick_phrase_space = quick_phrase_controller.handle_key(key(FrontendKey::Space));
        require(quick_phrase_space.handled && quick_phrase_space.commit == "控制器短语一" &&
                    quick_phrase_controller.local_input_mode() == LocalInputMode::None,
                "Space did not commit and leave quick-phrase mode.");

        InputOptions disabled_quick_phrase_options;
        disabled_quick_phrase_options.local_modes.quick_phrase = false;
        InputController disabled_quick_phrase_controller(SchemeType::Quanpin, disabled_quick_phrase_options);
        require(!disabled_quick_phrase_controller.handle_key(quick_phrase_prefix).handled &&
                    !disabled_quick_phrase_controller.has_composition() &&
                    disabled_quick_phrase_controller.local_input_mode() == LocalInputMode::None,
                "A disabled quick-phrase shortcut swallowed Shift+K.");

        InputController expressive_controller(SchemeType::Quanpin, 3);
        FrontendKeyEvent emoji_prefix{FrontendKey::Character, 'E'};
        emoji_prefix.shift_only = true;
        require(expressive_controller.handle_key(emoji_prefix).handled &&
                    expressive_controller.local_input_mode() == LocalInputMode::Emoji &&
                    expressive_controller.preedit() == "E",
                "The controller did not enter Emoji mode from Shift+E.");
        type(expressive_controller, "xiaolian");
        require(expressive_controller.candidates().size() == 1 &&
                    expressive_controller.candidates().front().word == "😀",
                "The controller did not expose the Emoji candidate.");
        const auto emoji_space = expressive_controller.handle_key(key(FrontendKey::Space));
        require(emoji_space.handled && emoji_space.commit == "😀" &&
                    expressive_controller.local_input_mode() == LocalInputMode::None,
                "Space did not commit and leave Emoji mode.");

        FrontendKeyEvent kaomoji_prefix{FrontendKey::Character, 'M'};
        kaomoji_prefix.shift_only = true;
        require(expressive_controller.handle_key(kaomoji_prefix).handled &&
                    expressive_controller.local_input_mode() == LocalInputMode::Kaomoji &&
                    expressive_controller.preedit() == "M",
                "The controller did not enter kaomoji mode from Shift+M.");
        type(expressive_controller, "haixiu");
        require(expressive_controller.candidates().size() == 1 &&
                    expressive_controller.candidates().front().word == "(*/ω＼*)",
                "The controller did not expose the kaomoji candidate.");
        const auto kaomoji_space = expressive_controller.handle_key(key(FrontendKey::Space));
        require(kaomoji_space.handled && kaomoji_space.commit == "(*/ω＼*)" &&
                    expressive_controller.local_input_mode() == LocalInputMode::None,
                "Space did not commit and leave kaomoji mode.");

        InputOptions disabled_expressive_options;
        disabled_expressive_options.local_modes.emoji = false;
        disabled_expressive_options.local_modes.kaomoji = false;
        InputController disabled_expressive_controller(SchemeType::Quanpin, disabled_expressive_options);
        require(!disabled_expressive_controller.handle_key(emoji_prefix).handled &&
                    !disabled_expressive_controller.has_composition() &&
                    disabled_expressive_controller.local_input_mode() == LocalInputMode::None,
                "A disabled Emoji shortcut swallowed Shift+E.");
        require(!disabled_expressive_controller.handle_key(kaomoji_prefix).handled &&
                    !disabled_expressive_controller.has_composition() &&
                    disabled_expressive_controller.local_input_mode() == LocalInputMode::None,
                "A disabled kaomoji shortcut swallowed Shift+M.");

        FrontendKeyEvent jianpin_prefix{FrontendKey::Character, 'J'};
        jianpin_prefix.shift_only = true;
        InputController jianpin_controller(SchemeType::Quanpin, 3);
        require(jianpin_controller.super_jianpin_mode_enabled() &&
                    jianpin_controller.handle_key(jianpin_prefix).handled &&
                    jianpin_controller.local_input_mode() == LocalInputMode::SuperJianpin &&
                    jianpin_controller.preedit() == "J",
                "The controller did not enter enabled super-jianpin mode from Shift+J.");
        type(jianpin_controller, "nh");
        require(jianpin_controller.preedit() == "Jnh" &&
                    std::any_of(jianpin_controller.candidates().begin(), jianpin_controller.candidates().end(),
                                [](const WordItem &candidate) { return candidate.word == "呐喊"; }),
                "Quanpin super-jianpin did not expose all matching initials.");
        require(jianpin_controller.handle_key(key(FrontendKey::PageDown)).handled &&
                    jianpin_controller.page_start() == 3,
                "Super-jianpin candidates did not page through the controller.");
        const auto jianpin_page_digit = jianpin_controller.handle_key(digit(2));
        if (!jianpin_page_digit.handled || jianpin_page_digit.commit != "candidate-4" ||
            jianpin_controller.local_input_mode() != LocalInputMode::None || jianpin_controller.has_composition())
        {
            throw std::runtime_error(
                "Page-relative super-jianpin selection did not commit and reset the mode; commit=" +
                jianpin_page_digit.commit.value_or("<none>"));
        }

        InputController shuangpin_jianpin_controller(SchemeType::Shuangpin, 3);
        require(shuangpin_jianpin_controller.handle_key(jianpin_prefix).handled,
                "Shift+J did not enter Shuangpin super-jianpin mode.");
        type(shuangpin_jianpin_controller, "nu");
        require(std::any_of(shuangpin_jianpin_controller.candidates().begin(),
                            shuangpin_jianpin_controller.candidates().end(),
                            [](const WordItem &candidate) { return candidate.word == "你说"; }) &&
                    std::none_of(shuangpin_jianpin_controller.candidates().begin(),
                                 shuangpin_jianpin_controller.candidates().end(),
                                 [](const WordItem &candidate) { return candidate.word == "你思"; }),
                "Xiaohe nu did not decode to n-sh initials in super-jianpin mode.");
        shuangpin_jianpin_controller.handle_key(key(FrontendKey::Escape));
        require(shuangpin_jianpin_controller.handle_key(jianpin_prefix).handled,
                "Shuangpin super-jianpin mode could not be re-entered after cancellation.");
        type(shuangpin_jianpin_controller, "ns");
        require(std::any_of(shuangpin_jianpin_controller.candidates().begin(),
                            shuangpin_jianpin_controller.candidates().end(),
                            [](const WordItem &candidate) { return candidate.word == "你思"; }) &&
                    std::none_of(shuangpin_jianpin_controller.candidates().begin(),
                                 shuangpin_jianpin_controller.candidates().end(),
                                 [](const WordItem &candidate) { return candidate.word == "你说"; }),
                "Xiaohe ns did not remain distinct from n-sh in super-jianpin mode.");

        InputOptions disabled_jianpin_options;
        disabled_jianpin_options.local_modes.super_jianpin = false;
        InputController disabled_jianpin_controller(SchemeType::Quanpin, disabled_jianpin_options);
        require(!disabled_jianpin_controller.super_jianpin_mode_enabled() &&
                    !disabled_jianpin_controller.handle_key(jianpin_prefix).handled &&
                    !disabled_jianpin_controller.has_composition() &&
                    disabled_jianpin_controller.local_input_mode() == LocalInputMode::None,
                "A disabled super-jianpin shortcut swallowed Shift+J.");

        FrontendKeyEvent temporary_english_prefix{FrontendKey::Character, 'Y'};
        temporary_english_prefix.shift_only = true;
        InputController temporary_english_controller(SchemeType::Quanpin, 3);
        require(temporary_english_controller.temporary_english_mode_enabled() &&
                    temporary_english_controller.handle_key(temporary_english_prefix).handled &&
                    temporary_english_controller.local_input_mode() == LocalInputMode::TemporaryEnglish &&
                    temporary_english_controller.preedit() == "Y",
                "The controller did not enter temporary English mode from Shift+Y.");
        type(temporary_english_controller, "he");
        require(temporary_english_controller.candidates().size() == 3 &&
                    temporary_english_controller.candidates()[0].word == "he" &&
                    temporary_english_controller.candidates()[1].word == "Hello" &&
                    temporary_english_controller.candidates()[2].word == "Help",
                "The controller did not expose raw temporary English before completions.");
        const auto temporary_english_commit = temporary_english_controller.handle_key(digit(2));
        require(temporary_english_commit.handled && temporary_english_commit.commit == "Hello" &&
                    temporary_english_controller.local_input_mode() == LocalInputMode::None &&
                    temporary_english_controller.scheme() == SchemeType::Quanpin,
                "Temporary English selection did not commit and return to Quanpin.");
        require(temporary_english_controller.handle_key(temporary_english_prefix).handled &&
                    temporary_english_controller.handle_key(key(FrontendKey::Backspace)).handled &&
                    temporary_english_controller.local_input_mode() == LocalInputMode::None,
                "Backspace on the bare Y prefix did not exit temporary English mode.");
        require(temporary_english_controller.handle_key(temporary_english_prefix).handled,
                "Temporary English could not start for a bare-prefix Space.");
        const auto bare_english_space = temporary_english_controller.handle_key(key(FrontendKey::Space));
        require(bare_english_space.handled && !bare_english_space.commit.has_value() &&
                    temporary_english_controller.local_input_mode() == LocalInputMode::None,
                "Space committed the bare temporary-English display prefix.");
        require(temporary_english_controller.handle_key(temporary_english_prefix).handled,
                "Temporary English could not start for bare-prefix punctuation.");
        const auto bare_english_comma = temporary_english_controller.handle_key(punctuation(','));
        require(bare_english_comma.handled && bare_english_comma.commit == "，" &&
                    temporary_english_controller.local_input_mode() == LocalInputMode::None,
                "Punctuation committed the bare temporary-English display prefix.");

        InputOptions temporary_english_paging_options;
        temporary_english_paging_options.page_size = 1;
        temporary_english_paging_options.comma_period_paging = true;
        InputController temporary_english_paging(SchemeType::Quanpin, temporary_english_paging_options);
        temporary_english_paging.handle_key(temporary_english_prefix);
        type(temporary_english_paging, "he");
        const auto temporary_english_equals = temporary_english_paging.handle_key(punctuation('='));
        require(temporary_english_equals.handled && !temporary_english_equals.commit.has_value() &&
                    temporary_english_paging.highlighted_candidate() == 1 &&
                    temporary_english_paging.local_input_mode() == LocalInputMode::TemporaryEnglish,
                "Equals committed temporary English instead of paging forward.");
        const auto temporary_english_period = temporary_english_paging.handle_key(punctuation('.'));
        require(temporary_english_period.handled && !temporary_english_period.commit.has_value() &&
                    temporary_english_paging.highlighted_candidate() == 2,
                "Period did not page forward in temporary English mode.");
        const auto temporary_english_paging_comma = temporary_english_paging.handle_key(punctuation(','));
        require(temporary_english_paging_comma.handled && !temporary_english_paging_comma.commit.has_value() &&
                    temporary_english_paging.highlighted_candidate() == 1,
                "Comma did not page backward in temporary English mode.");
        temporary_english_paging.handle_key(key(FrontendKey::Escape));
        InputOptions temporary_english_punctuation_options;
        temporary_english_punctuation_options.punctuation_mode = PunctuationMode::English;
        InputController temporary_english_punctuation(SchemeType::Quanpin, temporary_english_punctuation_options);
        temporary_english_punctuation.handle_key(temporary_english_prefix);
        type(temporary_english_punctuation, "he");
        temporary_english_punctuation.handle_key(key(FrontendKey::Down));
        const auto temporary_english_comma = temporary_english_punctuation.handle_key(punctuation(','));
        require(temporary_english_comma.handled && temporary_english_comma.commit == "Hello," &&
                    temporary_english_punctuation.local_input_mode() == LocalInputMode::None,
                "Temporary English swallowed punctuation instead of committing its highlighted candidate.");

        FrontendKeyEvent temporary_japanese_prefix{FrontendKey::Character, 'R'};
        temporary_japanese_prefix.shift_only = true;
        InputController temporary_japanese_controller(SchemeType::Quanpin, 3);
        require(temporary_japanese_controller.temporary_japanese_mode_enabled() &&
                    temporary_japanese_controller.handle_key(temporary_japanese_prefix).handled &&
                    temporary_japanese_controller.local_input_mode() == LocalInputMode::TemporaryJapanese &&
                    temporary_japanese_controller.preedit() == "R",
                "The controller did not enter temporary Japanese mode from Shift+R.");
        type(temporary_japanese_controller, "ka");
        require(std::any_of(temporary_japanese_controller.candidates().begin(),
                            temporary_japanese_controller.candidates().end(),
                            [](const WordItem &candidate) { return candidate.word == "か"; }),
                "The controller did not expose temporary Japanese conversion candidates.");
        const auto temporary_japanese_commit = temporary_japanese_controller.handle_key(key(FrontendKey::Space));
        require(temporary_japanese_commit.handled && temporary_japanese_commit.commit == "か" &&
                    temporary_japanese_controller.local_input_mode() == LocalInputMode::None &&
                    temporary_japanese_controller.scheme() == SchemeType::Quanpin,
                "Temporary Japanese commit did not restore Quanpin.");
        require(temporary_japanese_controller.handle_key(temporary_japanese_prefix).handled &&
                    temporary_japanese_controller.handle_key(key(FrontendKey::Backspace)).handled &&
                    temporary_japanese_controller.local_input_mode() == LocalInputMode::None &&
                    temporary_japanese_controller.scheme() == SchemeType::Quanpin,
                "Backspace on the bare R prefix did not restore Quanpin.");
        require(temporary_japanese_controller.handle_key(temporary_japanese_prefix).handled,
                "Temporary Japanese could not start before a bare-prefix scheme switch.");
        const auto bare_japanese_scheme_switch = temporary_japanese_controller.switch_scheme(SchemeType::Shuangpin);
        require(bare_japanese_scheme_switch.handled && !bare_japanese_scheme_switch.commit.has_value() &&
                    temporary_japanese_controller.local_input_mode() == LocalInputMode::None &&
                    temporary_japanese_controller.scheme() == SchemeType::Shuangpin,
                "A scheme switch committed the bare temporary-Japanese display prefix.");
        temporary_japanese_controller.switch_scheme(SchemeType::Quanpin);
        require(temporary_japanese_controller.handle_key(temporary_japanese_prefix).handled,
                "Temporary Japanese could not start before reselecting the original scheme.");
        type(temporary_japanese_controller, "ka");
        const auto reselect_quanpin = temporary_japanese_controller.switch_scheme(SchemeType::Quanpin);
        require(reselect_quanpin.handled && reselect_quanpin.commit == "か" &&
                    temporary_japanese_controller.local_input_mode() == LocalInputMode::None &&
                    temporary_japanese_controller.scheme() == SchemeType::Quanpin,
                "Reselecting the original scheme did not finish temporary Japanese mode.");
        InputOptions temporary_japanese_punctuation_options;
        temporary_japanese_punctuation_options.punctuation_mode = PunctuationMode::English;
        InputController temporary_japanese_punctuation(SchemeType::Quanpin, temporary_japanese_punctuation_options);
        temporary_japanese_punctuation.handle_key(temporary_japanese_prefix);
        type(temporary_japanese_punctuation, "ka");
        const auto temporary_japanese_comma = temporary_japanese_punctuation.handle_key(punctuation(','));
        require(temporary_japanese_comma.handled && temporary_japanese_comma.commit == "か," &&
                    temporary_japanese_punctuation.local_input_mode() == LocalInputMode::None &&
                    temporary_japanese_punctuation.scheme() == SchemeType::Quanpin,
                "Temporary Japanese swallowed punctuation or failed to restore Quanpin.");

        InputOptions disabled_temporary_options;
        disabled_temporary_options.local_modes.temporary_english = false;
        disabled_temporary_options.local_modes.temporary_japanese = false;
        InputController disabled_temporary_controller(SchemeType::Quanpin, disabled_temporary_options);
        require(!disabled_temporary_controller.temporary_english_mode_enabled() &&
                    !disabled_temporary_controller.temporary_japanese_mode_enabled() &&
                    !disabled_temporary_controller.handle_key(temporary_english_prefix).handled &&
                    !disabled_temporary_controller.has_composition() &&
                    disabled_temporary_controller.local_input_mode() == LocalInputMode::None,
                "Disabled temporary English swallowed Shift+Y.");
        require(!disabled_temporary_controller.handle_key(temporary_japanese_prefix).handled &&
                    !disabled_temporary_controller.has_composition() &&
                    disabled_temporary_controller.local_input_mode() == LocalInputMode::None,
                "Disabled temporary Japanese swallowed Shift+R.");

        InputOptions mixed_english_options;
        mixed_english_options.english_input.mixed_candidates = true;
        mixed_english_options.english_input.minimum_prefix = 2;
        mixed_english_options.mixed_expressive.emoji_candidates = true;
        mixed_english_options.mixed_expressive.kaomoji_candidates = true;
        InputController mixed_english_controller(SchemeType::Quanpin, mixed_english_options);
        type(mixed_english_controller, "xi");
        if (!mixed_english_controller.mixed_english_candidates_enabled() ||
            mixed_english_controller.mixed_english_minimum_prefix() != 2 ||
            !mixed_english_controller.mixed_emoji_candidates_enabled() ||
            !mixed_english_controller.mixed_kaomoji_candidates_enabled() ||
            mixed_english_controller.candidates().size() != 11 ||
            (mixed_english_controller.candidates()[0].source != CandidateSource::Database &&
             mixed_english_controller.candidates()[0].source != CandidateSource::UserDatabase) ||
            mixed_english_controller.candidates()[1].word != "Xi" ||
            mixed_english_controller.candidates()[1].source != CandidateSource::EnglishDictionary ||
            mixed_english_controller.candidates()[2].word != "✨" ||
            mixed_english_controller.candidates()[2].source != CandidateSource::Emoji ||
            mixed_english_controller.candidates()[3].word != "(^_^)" ||
            mixed_english_controller.candidates()[3].source != CandidateSource::Kaomoji ||
            mixed_english_controller.candidates()[9].word != "Xigua" ||
            mixed_english_controller.candidates()[10].word != "😀")
        {
            std::string actual = "The controller did not expose configured mixed candidates in stable order:";
            for (const auto &candidate : mixed_english_controller.candidates())
            {
                actual += " [" + candidate.word + "]";
            }
            throw std::runtime_error(actual);
        }

        InputController dedicated_english_controller(SchemeType::Quanpin, 3);
        const auto enter_english = dedicated_english_controller.handle_key(key(FrontendKey::ToggleEnglish));
        require(enter_english.handled && dedicated_english_controller.dedicated_english_mode() &&
                    !dedicated_english_controller.has_composition(),
                "The controller did not enter dedicated English mode.");
        type(dedicated_english_controller, "he");
        require(dedicated_english_controller.candidates().size() == 2 &&
                    dedicated_english_controller.candidates()[0].word == "Hello" &&
                    dedicated_english_controller.candidates()[1].word == "Help",
                "Dedicated English mode did not expose English-only candidates.");
        const auto select_english = dedicated_english_controller.handle_key(digit(2));
        require(select_english.handled && select_english.commit == "Help" &&
                    dedicated_english_controller.dedicated_english_mode(),
                "Dedicated English candidate selection committed the wrong candidate or left the mode.");
        type(dedicated_english_controller, "Linuxword");
        const auto enter_raw_english = dedicated_english_controller.handle_key(key(FrontendKey::Enter));
        require(enter_raw_english.handled && enter_raw_english.commit == "Linuxword" &&
                    dedicated_english_controller.dedicated_english_mode(),
                "Enter did not commit and learn raw dedicated English.");
        require(english_database.query_integer(
                    "SELECT COUNT(*) FROM english_words WHERE word='linuxword' AND display='Linuxword'") == 1,
                "The controller did not persist a learned dedicated English word.");
        const auto leave_english = dedicated_english_controller.handle_key(key(FrontendKey::ToggleEnglish));
        require(leave_english.handled && !dedicated_english_controller.dedicated_english_mode() &&
                    !dedicated_english_controller.has_composition(),
                "The controller did not exit dedicated English mode.");

        type(dedicated_english_controller, "nihao");
        const auto english_toggle_on_commit = dedicated_english_controller.handle_key(key(FrontendKey::ToggleEnglish));
        require(english_toggle_on_commit.handled && english_toggle_on_commit.commit == "candidate-0" &&
                    dedicated_english_controller.dedicated_english_mode() &&
                    !dedicated_english_controller.has_composition(),
                "Entering dedicated English mode destroyed the active composition instead of committing it.");
        type(dedicated_english_controller, "he");
        const auto english_toggle_off_commit = dedicated_english_controller.handle_key(key(FrontendKey::ToggleEnglish));
        require(english_toggle_off_commit.handled && english_toggle_off_commit.commit.has_value() &&
                    !english_toggle_off_commit.commit->empty() &&
                    !dedicated_english_controller.dedicated_english_mode() &&
                    !dedicated_english_controller.has_composition(),
                "Leaving dedicated English mode destroyed the active English composition instead of committing it.");

        InputController direct_english_controller(SchemeType::Quanpin, 3);
        require(direct_english_controller.set_mode(InputMode::Direct).handled,
                "The direct-mode dedicated English fixture did not reach direct mode.");
        const auto direct_english_toggle = direct_english_controller.handle_key(key(FrontendKey::ToggleEnglish));
        require(direct_english_toggle.handled && !direct_english_toggle.commit.has_value() &&
                    !direct_english_controller.dedicated_english_mode(),
                "The dedicated English hotkey flipped an invisible flag while direct mode was active.");
        require(direct_english_controller.set_mode(InputMode::Ime).handled &&
                    !direct_english_controller.dedicated_english_mode(),
                "Dedicated English leaked out of a direct-mode round trip.");
        type(direct_english_controller, "nihao");
        require(std::any_of(direct_english_controller.candidates().begin(),
                            direct_english_controller.candidates().end(),
                            [](const WordItem &candidate) { return candidate.word == "candidate-0"; }),
                "Leaked dedicated English routed a Chinese composition to the English dictionary.");
        direct_english_controller.handle_key(key(FrontendKey::Escape));

        InputController scheme_english_controller(SchemeType::Quanpin, 3);
        require(scheme_english_controller.handle_key(key(FrontendKey::ToggleEnglish)).handled &&
                    scheme_english_controller.dedicated_english_mode(),
                "The scheme-switch dedicated English fixture did not reach the mode.");
        const auto english_scheme_switch = scheme_english_controller.switch_scheme(SchemeType::Wubi);
        require(english_scheme_switch.handled && !scheme_english_controller.dedicated_english_mode() &&
                    scheme_english_controller.scheme() == SchemeType::Wubi,
                "Dedicated English survived a scheme switch while scheme() reported the new scheme.");
        scheme_english_controller.switch_scheme(SchemeType::Quanpin);
        type(scheme_english_controller, "nihao");
        require(std::any_of(scheme_english_controller.candidates().begin(),
                            scheme_english_controller.candidates().end(),
                            [](const WordItem &candidate) { return candidate.word == "candidate-0"; }),
                "A scheme switch left dedicated English routing a Chinese composition to the English dictionary.");
        scheme_english_controller.handle_key(key(FrontendKey::Escape));

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

        require(full_width_controller.handle_key(punctuation('@')).commit == "＠" &&
                    full_width_controller.handle_key(punctuation('#')).commit == "＃" &&
                    full_width_controller.handle_key(punctuation('%')).commit == "％" &&
                    full_width_controller.handle_key(punctuation('&')).commit == "＆" &&
                    full_width_controller.handle_key(punctuation('*')).commit == "＊" &&
                    full_width_controller.handle_key(punctuation('~')).commit == "～",
                "The punctuation characters the Chinese formatter passes through ignored the full-width setting.");
        full_width_controller.set_character_width(CharacterWidth::Half);
        require(full_width_controller.handle_key(punctuation('@')).commit == "@",
                "Half width stopped passing an ASCII-only Chinese punctuation character straight through.");
        require(full_width_controller.handle_key(punctuation('$')).commit == "￥",
                "Half width changed the Chinese mapping of a punctuation character that has one.");

        InputOptions full_width_ime_options;
        full_width_ime_options.character_width = CharacterWidth::Full;
        InputController full_width_ime_controller(SchemeType::Quanpin, full_width_ime_options);
        const auto ime_uppercase = full_width_ime_controller.handle_key({FrontendKey::Character, 'A'});
        require(ime_uppercase.handled && ime_uppercase.commit == "Ａ" && !full_width_ime_controller.has_composition(),
                "An uppercase letter the engine rejects in IME mode never reached the full-width conversion.");

        InputController half_width_ime_controller(SchemeType::Quanpin, 3);
        const auto half_width_uppercase = half_width_ime_controller.handle_key({FrontendKey::Character, 'A'});
        require(!half_width_uppercase.handled && !half_width_uppercase.commit.has_value(),
                "A rejected uppercase letter was swallowed instead of reaching the client at half width.");

        InputOptions full_width_composition_options;
        full_width_composition_options.character_width = CharacterWidth::Full;
        full_width_composition_options.quanpin_helpcode_enabled = false;
        InputController full_width_composition_controller(SchemeType::Quanpin, full_width_composition_options);
        type(full_width_composition_controller, "ni");
        const auto composition_uppercase = full_width_composition_controller.handle_key({FrontendKey::Character, 'A'});
        require(!composition_uppercase.handled && !composition_uppercase.commit.has_value() &&
                    full_width_composition_controller.preedit() == "ni" &&
                    full_width_composition_controller.has_composition(),
                "Full-width conversion consumed an uppercase letter that belonged to the active composition.");

        // Prepared paths must flow through the controller unchanged, even when the process
        // default points somewhere else. Both working DBs and resource helpcodes are independent.
        {
            const auto prepare = [&](const std::string &name, const std::string &word) {
                const auto root = data_directory / name;
                const auto resources = root / "resources";
                std::filesystem::create_directories(resources);
                {
                    Database fixture(resources / "msime.db");
                    fixture.execute("CREATE TABLE tbl_1_n(key TEXT,jp TEXT,value TEXT,weight INTEGER)");
                    fixture.execute("INSERT INTO tbl_1_n VALUES('ni','n','" + word + "',100),('ni','n','拟',90)");
                }
                require(EnglishDictionary::ensure_schema(metasequoia::path_to_utf8(resources / "english.db")),
                        "Cannot prepare explicit-path English schema.");
                write_file(resources / "helpcodes/helpcode.txt", word + "=ab\n拟=cc\n");
                return metasequoia::prepare_runtime_paths(resources, root / "user", root / "cache", "initial");
            };
            const auto paths_a = prepare("explicit-a", "你");
            const auto paths_b = prepare("explicit-b", "妮");
            InputController first(SchemeType::Quanpin, InputOptions{}, paths_a);
            const auto unrelated = data_directory / "unrelated-default";
            require(setenv("METASEQUOIA_IME_DATA_DIR", metasequoia::path_to_utf8(unrelated).c_str(), 1) == 0,
                    "Cannot change the process default for the isolation test.");
            InputController second(SchemeType::Quanpin, InputOptions{}, paths_b);
            type(first, "niA");
            type(second, "niA");
            require(!first.candidates().empty() && first.candidates().front().word == "你" &&
                        !second.candidates().empty() && second.candidates().front().word == "妮",
                    "Explicit controller paths were replaced by another context or process defaults.");
            first.handle_key(key(FrontendKey::Backspace));
            type(first, "A");
            require(first.candidates().front().word == "你", "Refreshing one context reused another resource map.");
            require(!std::filesystem::exists(unrelated), "Explicit path setup touched the process-default directory.");
            require(setenv("METASEQUOIA_IME_DATA_DIR", metasequoia::path_to_utf8(data_directory).c_str(), 1) == 0,
                    "Cannot restore the test data directory.");
        }

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
