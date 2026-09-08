#include "NativeDictionaryRevision.h"
#include "BackendAccountClient.h"
#include <glib.h>
#include <array>
#include <memory>
#include <type_traits>
namespace metasequoia::linux_ime::account
{
std::string native_dictionary_revision(const RuntimePaths &paths, const online::CancellationCheck &cancelled)
{
    auto check_cancelled = [&] {
        if (cancelled && cancelled())
            throw Failure(0, true);
    };
    check_cancelled();
    std::unique_ptr<GChecksum, decltype(&g_checksum_free)> checksum(g_checksum_new(G_CHECKSUM_SHA256), g_checksum_free);
    if (!checksum)
        throw std::bad_alloc();
    auto number = [&](std::uint64_t value) {
        std::array<guchar, 8> bytes{};
        for (auto &byte : bytes)
        {
            byte = static_cast<guchar>(value & 255);
            value >>= 8;
        }
        g_checksum_update(checksum.get(), bytes.data(), bytes.size());
    };
    auto text = [&](const std::string &value) {
        number(value.size());
        g_checksum_update(checksum.get(), reinterpret_cast<const guchar *>(value.data()), value.size());
    };
    text("msime-native-dictionary-state-v1");
    stream_dictionary_state(paths, [&](const DictionaryStateRecord &record) {
        check_cancelled();
        std::visit(
            [&](const auto &value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, DictionaryStateEntry>)
                {
                    text("overlay");
                    switch (value.kind)
                    {
                    case PersonalDictionaryKind::Pinyin:
                        text("pinyin");
                        break;
                    case PersonalDictionaryKind::Wubi:
                        text("wubi");
                        break;
                    case PersonalDictionaryKind::English:
                        text("english");
                        break;
                    case PersonalDictionaryKind::QuickPhrase:
                        text("quick");
                        break;
                    }
                    text(value.key);
                    text(value.value);
                    number(static_cast<std::uint64_t>(value.weight));
                    text(value.display);
                    number(value.deleted);
                    number(value.user_inserted);
                }
                else
                {
                    if constexpr (std::is_same_v<T, DictionaryStatePosition>)
                        text("position");
                    else
                        text("selection");
                    text(value.context);
                    text(value.key);
                    text(value.value);
                    if constexpr (std::is_same_v<T, DictionaryStatePosition>)
                        number(static_cast<std::uint64_t>(value.position));
                    else
                        number(static_cast<std::uint64_t>(value.count));
                }
            },
            record);
        return true;
    });
    check_cancelled();
    return g_checksum_get_string(checksum.get());
}
} // namespace metasequoia::linux_ime::account
