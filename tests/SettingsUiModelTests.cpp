#include "../src/SettingsUiModel.h"

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
        InputSettings settings;
        settings.online.ai.token = "must-not-be-exposed";
        SettingsUiModel model(settings);

        const auto &rows = model.rows();
        require(!rows.empty(), "The settings model did not expose editable rows.");
        for (const auto &row : rows)
        {
            require(row.id.find("token") == std::string::npos, "Secret credentials leaked into the settings model.");
        }

        std::string error;
        require(model.set("scheme", "shuangpin", &error), "A valid choice could not be applied.");
        require(model.settings().scheme == SchemeType::Shuangpin, "The scheme choice was not applied.");
        require(model.set("page-size", "7", &error), "A valid integer could not be applied.");
        require(model.settings().page_size == 7, "The page size was not applied.");
        require(model.set("cloud-enabled", "false", &error), "A valid online toggle could not be applied.");
        require(!model.settings().online.cloud_candidates_enabled, "The online toggle was not applied.");
        require(!model.set("page-size", "99", &error) && !error.empty(),
                "An out-of-range page size was accepted.");
        require(!model.set("unknown-setting", "x", &error) && !error.empty(),
                "An unknown setting was accepted.");
        require(!model.set("ai-endpoint", "http://insecure.example", &error) && !error.empty(),
                "An insecure AI endpoint was accepted by the settings UI model.");
        require(model.set("ai-endpoint", "https://secure.example", &error),
                "A valid HTTPS AI endpoint was rejected.");
        require(model.settings().online.ai.token == "must-not-be-exposed", "Editing discarded the in-memory token.");

        std::cout << "Settings UI model tests passed.\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
