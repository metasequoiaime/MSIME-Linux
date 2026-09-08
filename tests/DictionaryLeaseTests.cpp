#include "DictionaryLease.h"
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
using metasequoia::linux_ime::DictionaryLease;
using Mode = DictionaryLease::Mode;
namespace fs = std::filesystem;
namespace
{
void require(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}
template <class F> void fails(F action)
{
    try
    {
        action();
    }
    catch (const std::exception &)
    {
        return;
    }
    throw std::runtime_error("unexpected success");
}
void finish(pid_t child)
{
    int status = 0;
    require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "child failed");
}
} // namespace
int main(int argc, char **argv)
{
    alarm(12);
    if (argc == 3)
        return DictionaryLease::acquire(argv[2], Mode::Exclusive) ? 0 : 2;
    char pattern[] = "/tmp/msime-dictionary-lease-XXXXXX";
    require(mkdtemp(pattern), "temporary directory failed");
    const fs::path root(pattern);
    struct Cleanup
    {
        fs::path root;
        ~Cleanup()
        {
            fs::remove_all(root);
        }
    } cleanup{root};
    auto first = DictionaryLease::acquire(root, Mode::Shared);
    auto second = DictionaryLease::acquire(root, Mode::Shared);
    require(first && second, "shared sessions excluded one another");
    require(!DictionaryLease::acquire(root, Mode::Exclusive), "publication bypassed active sessions");
    first.reset();
    require(!DictionaryLease::acquire(root, Mode::Exclusive), "publication ignored remaining session");
    second.reset();
    auto writer = DictionaryLease::acquire(root, Mode::Exclusive);
    require(bool(writer), "exclusive lease unavailable after release");
    require(!DictionaryLease::acquire(root, Mode::Shared) && !DictionaryLease::acquire(root, Mode::Exclusive),
            "exclusive publication did not exclude sessions/writers");
    writer.reset();

    // Exercise independent processes, crash release, and the stable inode.
    int ready[2];
    require(pipe(ready) == 0, "pipe failed");
    const auto child = fork();
    require(child >= 0, "fork failed");
    if (child == 0)
    {
        close(ready[0]);
        auto lease = DictionaryLease::acquire(root, Mode::Shared);
        if (!lease || write(ready[1], "r", 1) != 1)
            _exit(2);
        for (;;)
            pause();
    }
    close(ready[1]);
    char byte = 0;
    require(read(ready[0], &byte, 1) == 1, "child not ready");
    close(ready[0]);
    require(!DictionaryLease::acquire(root, Mode::Exclusive), "cross-process lock ineffective");
    require(kill(child, SIGKILL) == 0, "kill failed");
    int status = 0;
    require(waitpid(child, &status, 0) == child && WIFSIGNALED(status), "crashed child not reaped");
    require(bool(DictionaryLease::acquire(root, Mode::Exclusive)), "crash stranded lock");
    require(fs::is_regular_file(root / "dictionary-sessions.lock"), "lease deleted stable inode");

    // A forked child inherits flock descriptions, but exec must close them.
    auto inherited = DictionaryLease::acquire(root, Mode::Shared);
    require(pipe(ready) == 0, "pipe failed");
    const auto exec_child = fork();
    require(exec_child >= 0, "fork failed");
    if (exec_child == 0)
    {
        close(ready[1]);
        if (read(ready[0], &byte, 1) != 1)
            _exit(2);
        close(ready[0]);
        execl(argv[0], argv[0], "--exclusive", root.c_str(), nullptr);
        _exit(3);
    }
    close(ready[0]);
    inherited.reset();
    require(write(ready[1], "r", 1) == 1, "child signal failed");
    close(ready[1]);
    finish(exec_child);

    fails([&] { DictionaryLease::acquire("relative", Mode::Shared); });
    const auto unsafe = root / "unsafe";
    fs::create_directory(unsafe);
    chmod(unsafe.c_str(), 0777);
    fails([&] { DictionaryLease::acquire(unsafe, Mode::Shared); });
    chmod(unsafe.c_str(), 0700);
    fs::create_symlink(root / "dictionary-sessions.lock", unsafe / "dictionary-sessions.lock");
    fails([&] { DictionaryLease::acquire(unsafe, Mode::Shared); });
    fs::remove(unsafe / "dictionary-sessions.lock");
    fs::create_hard_link(root / "dictionary-sessions.lock", unsafe / "dictionary-sessions.lock");
    fails([&] { DictionaryLease::acquire(unsafe, Mode::Shared); });
    fs::remove(unsafe / "dictionary-sessions.lock");
    require(mkfifo((unsafe / "dictionary-sessions.lock").c_str(), 0600) == 0, "FIFO failed");
    fails([&] { DictionaryLease::acquire(unsafe, Mode::Shared); });
    fs::create_directory_symlink(root, root / "alias");
    fails([&] { DictionaryLease::acquire(root / "alias", Mode::Shared); });
    require(bool(DictionaryLease::acquire(root, Mode::Exclusive)), "rejected inputs damaged lock");
    std::cout
        << "Dictionary cross-process sharing, publication exclusion, crash/exec release and unsafe paths passed\n";
}
