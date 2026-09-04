#include "../src/ClipboardHistory.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace metasequoia::linux_ime;

namespace
{
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
    try
    {
        const auto root = std::filesystem::temp_directory_path() / "metasequoia-clipboard-history-tests";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
        setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);

        ClipboardHistory history;
        std::string error;
        require(history.set_enabled(true, &error), "Clipboard history could not be enabled.");
        require(history.add("first\r", &error), "The first clipboard item was not stored.");
        require(history.add("second", &error), "The second clipboard item was not stored.");
        require(!history.add("second", &error), "A duplicate clipboard item was stored twice.");
        auto items = history.load(&error);
        require(items.size() == 2 && items[0] == "second" && items[1] == "first",
                "Clipboard history ordering or normalization was incorrect.");

        const std::string long_text(5000, 'x');
        require(history.add(long_text, &error), "A long clipboard item was not accepted.");
        items = history.load(&error);
        require(items.front().size() == ClipboardHistory::kMaxChars,
                "Clipboard history did not apply its maximum size.");
        require(history.remove("second", &error), "A clipboard item could not be removed.");
        require(history.clear(&error), "Clipboard history could not be cleared.");
        require(history.load(&error).empty(), "Clipboard history remained after clearing.");
        require(history.set_enabled(false, &error), "Clipboard history could not be disabled.");
        require(!history.add("disabled", &error), "Disabled clipboard history accepted a new item.");

        std::filesystem::remove_all(root);
        std::cout << "Clipboard history tests passed.\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
