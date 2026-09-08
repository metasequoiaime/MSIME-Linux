#include "account/NativeInstallation.h"
#include "DictionaryLease.h"
#include "contracts/assets/assets.h"
#include <fstream>
#include <iostream>
#include <array>
int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3)
        return 2;
    using namespace metasequoia;
    using namespace metasequoia::linux_ime::account;
    const std::filesystem::path root(argv[1]);
    auto lease =
        metasequoia::linux_ime::DictionaryLease::acquire(root, metasequoia::linux_ime::DictionaryLease::Mode::Shared);
    if (!lease)
        return 1;
    NativeInstallation installation(root / "runtime", {root, root, root / "cache", root});
    const auto resources = installation.resources("smoke");
    std::filesystem::create_directory(resources);
    for (const auto *name : {assets::main_dictionary, assets::english_dictionary, assets::other_dictionary})
        std::filesystem::copy_file(root / name, resources / name);
    std::filesystem::copy(root / "helpcodes", resources / "helpcodes", std::filesystem::copy_options::recursive);
    stage_dictionary_state(resources, installation.generation("smoke"), "smoke",
                           [](DictionaryStateRecord &) { return false; });
    if (argc == 3)
    {
        const std::array<DictionaryStateEntry, 2> entries{
            {{PersonalDictionaryKind::Wubi, "qq", "代际测试词", 1000000, "", false, true},
             {PersonalDictionaryKind::Pinyin, "dai'ji'ce'shi'ci", "代际测试词", 1000000, "", false, true}}};
        std::size_t offset = 0;
        stage_dictionary_state(resources, installation.generation("replacement"), "smoke",
                               [&](DictionaryStateRecord &record) {
                                   if (offset == entries.size())
                                       return false;
                                   record = entries[offset++];
                                   return true;
                               });
    }
    NativePublication result;
    if (!lease->exclusively([&] { result = installation.publish("smoke", "smoke", ""); }))
        return 1;
    if (!result.published || !result.durable)
        return 1;
    // Prove both executables resolve the active layout, not this old location.
    std::filesystem::remove(root / assets::main_dictionary);
    std::filesystem::remove(root / assets::english_dictionary);
    std::ofstream old_journal(root / assets::user_journal);
    old_journal << "synthetic old journal must stay untouched";
    return old_journal ? 0 : 1;
}
