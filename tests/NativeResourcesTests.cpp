#include "account/NativeResources.h"
#include "account/BackendAccountClient.h"
#include <gio/gio.h>
#include <signal.h>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
using namespace metasequoia::linux_ime::account;
namespace
{
void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
template <class F> void fails(F action)
{
    try
    {
        action();
    }
    catch (const Failure &)
    {
        return;
    }
    throw std::runtime_error("unexpected resource preparation success");
}
} // namespace
int main(int argc, char **argv)
{
    if (argc == 4)
    {
        std::cout << prepare_native_resources(argv[1], argv[2], argv[3]).directory.string() << '\n';
        return 0;
    }
    gchar *temporary = g_dir_make_tmp("msime-resource-process-XXXXXX", nullptr);
    require(temporary, "temporary directory failed");
    const std::filesystem::path root(temporary);
    g_free(temporary);
    struct Cleanup
    {
        std::filesystem::path root;
        ~Cleanup()
        {
            std::filesystem::remove_all(root);
        }
    } cleanup{root};
    const auto tools = root / "tools with spaces", source = root / "source;echo shell", resources = root / "resources";
    std::filesystem::create_directory(tools);
    std::filesystem::create_directory(source);
    std::ofstream(tools / "native-resource-lock.json") << "{}";
    std::ofstream(tools / "native_resources.py") << R"PY(import os, sys, time
from pathlib import Path
assert sys.argv[1] == 'prepare' and len(sys.argv) == 5
source, root, lock = map(Path, sys.argv[2:])
assert source.name == 'source;echo shell' and lock.name == 'native-resource-lock.json'
assert 'tools with spaces' == lock.parent.name
mode = (source / 'mode').read_text()
if mode == 'cancel':
    (source / 'pid').write_text(str(os.getpid()))
    time.sleep(30)
if mode == 'overflow':
    sys.stdout.write('x' * 9000); sys.stdout.flush(); time.sleep(30)
if mode == 'escape':
    print(root / '..' / ('a' * 64))
elif mode == 'invalid':
    print(root / 'not-a-content-id')
elif mode == 'extra':
    print(root / ('a' * 64)); print('unexpected output')
else:
    target = root / ('a' * 64)
    target.mkdir(parents=True, exist_ok=True)
    print(target)
if mode == 'failure':
    sys.exit(1)
)PY";
    auto run = [&](const char *mode) {
        std::ofstream(source / "mode") << mode;
        return prepare_native_resources(tools, source, resources);
    };
    const auto bundle = run("success");
    require(bundle.content_id == std::string(64, 'a') && bundle.directory == resources / bundle.content_id,
            "valid helper result rejected");
    for (const char *mode : {"escape", "invalid", "extra", "failure", "overflow"})
        fails([&] { run(mode); });
    fails([&] { prepare_native_resources("relative", source, resources); });
    fails([&] { prepare_native_resources(root / "missing", source, resources); });
    GCancellable *cancelled = g_cancellable_new();
    g_cancellable_cancel(cancelled);
    fails([&] { prepare_native_resources(tools, source, resources, cancelled); });
    g_cancellable_reset(cancelled);
    std::ofstream(source / "mode") << "cancel";
    std::thread cancel_thread([&] {
        for (int i = 0; i < 500 && !std::filesystem::exists(source / "pid"); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        g_cancellable_cancel(cancelled);
    });
    bool rejected = false;
    try
    {
        prepare_native_resources(tools, source, resources, cancelled);
    }
    catch (const Failure &)
    {
        rejected = true;
    }
    cancel_thread.join();
    g_object_unref(cancelled);
    require(rejected, "cancelled helper accepted");
    pid_t pid = 0;
    std::ifstream(source / "pid") >> pid;
    require(pid > 0 && kill(pid, 0) == -1 && errno == ESRCH, "cancelled helper was not reaped");
    std::cout << "Resource helper arguments, output validation and process cancellation passed\n";
}
