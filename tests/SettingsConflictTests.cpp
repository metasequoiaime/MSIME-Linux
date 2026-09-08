#include "SettingsStore.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>

using namespace metasequoia::linux_ime;
namespace
{
void require(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}
struct Secrets final : SecretStore
{
    mutable std::atomic<int> calls{0};
    SecretLookupResult lookup(SecretKind, std::string_view) const override
    {
        ++calls;
        return {};
    }
    bool store(SecretKind, std::string_view, std::string_view, std::string *) override
    {
        ++calls;
        return true;
    }
    bool erase(SecretKind, std::string_view, std::string *) override
    {
        ++calls;
        return true;
    }
};
struct Fixture
{
    std::filesystem::path root = std::filesystem::temp_directory_path() /
                                 ("msime-settings-conflict-" +
                                  std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    ~Fixture()
    {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};
} // namespace
int main()
{
    Fixture fixture;
    SettingsStore store(fixture.root);
    Secrets secrets;
    InputSettings settings;
    settings.online.ai.enabled = false;
    settings.voice.enabled = false;
    settings.online.candidate_translations_enabled = false;
    settings.page_size = 5;
    const auto missing = store.version();
    require(missing && missing->missing, "new configuration not represented as missing");
    std::string error;
    require(store.save_if_unchanged(settings, secrets, *missing, &error), "first conditional save failed");
    const auto original = store.version();
    require(original && !original->missing && !original->digest.empty(), "saved version missing digest");
    const auto size = std::filesystem::file_size(store.config_path());
    const auto time = std::filesystem::last_write_time(store.config_path());
    settings.page_size = 7;
    require(store.save(settings, &error), "external settings update failed");
    require(std::filesystem::file_size(store.config_path()) == size, "fixture changed byte count");
    std::filesystem::last_write_time(store.config_path(), time);
    settings.page_size = 9;
    settings.online.ai.enabled = true;
    settings.online.ai.token = "synthetic-new-credential";
    const int calls = secrets.calls.load();
    require(!store.save_if_unchanged(settings, secrets, *original, &error), "stale preview overwrote same-size edit");
    require(secrets.calls.load() == calls && store.load().page_size == 7,
            "conflict changed credentials or configuration");
    require(!store.save_if_unchanged(settings, secrets, *missing, &error), "stale missing baseline replaced new file");
    settings.online.ai.enabled = false;
    settings.online.ai.token.clear();
    const auto baseline = *store.version();
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    auto writer = [&](std::size_t page_size) {
        SettingsStore independent(fixture.root);
        auto next = settings;
        next.page_size = page_size;
        ++ready;
        while (!start.load())
            std::this_thread::yield();
        return independent.save_if_unchanged(next, secrets, baseline);
    };
    auto first = std::async(std::launch::async, writer, 5);
    auto second = std::async(std::launch::async, writer, 9);
    while (ready.load() != 2)
        std::this_thread::yield();
    start.store(true);
    const bool first_saved = first.get(), second_saved = second.get();
    require(first_saved != second_saved, "two writers committed the same version");
    require(store.load().page_size == (first_saved ? 5 : 9), "winning write not preserved");
    const auto process_baseline = *store.version();
    int gate[2];
    require(pipe(gate) == 0, "process gate could not be created");
    const auto child = fork();
    require(child >= 0, "writer process could not start");
    if (child == 0)
    {
        close(gate[1]);
        char signal;
        if (read(gate[0], &signal, 1) != 1)
            _exit(2);
        close(gate[0]);
        SettingsStore independent(fixture.root);
        auto next = settings;
        next.page_size = 6;
        _exit(independent.save_if_unchanged(next, secrets, process_baseline) ? 0 : 1);
    }
    close(gate[0]);
    require(write(gate[1], "x", 1) == 1, "writer process not released");
    close(gate[1]);
    settings.page_size = 8;
    const bool parent_saved = store.save_if_unchanged(settings, secrets, process_baseline);
    int status = 0;
    require(waitpid(child, &status, 0) == child && WIFEXITED(status), "writer process failed");
    require(WEXITSTATUS(status) <= 1 && parent_saved != (WEXITSTATUS(status) == 0),
            "processes committed the same version");
    require(store.load().page_size == (parent_saved ? 8 : 6), "process winner not preserved");
    SettingsStore unreadable(fixture.root / "unreadable");
    std::filesystem::create_directories(unreadable.config_path());
    require(!unreadable.version(), "unreadable file treated as absent");
    require(!unreadable.save_if_unchanged(settings, secrets, *missing), "unreadable baseline overwritten");
    std::cout << "conditional settings save and concurrent writer tests passed\n";
}
