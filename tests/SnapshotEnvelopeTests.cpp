#include "account/SnapshotEnvelope.h"
#include "account/BackendAccountClient.h"
#include <glib.h>
#include <sstream>
#include <iostream>
using namespace metasequoia::linux_ime::account;
namespace
{
const std::string header = R"({"type":"header","format":"msime-dictionary-snapshot","version":1,"revision":9})";
const std::string entry =
    R"({"type":"entry","data":{"id":"a","kind":"pinyin","code":"ni'hao","word":"你好","weight":10,"revision":9,"updated_at":"now"}})";
std::string envelope(const std::string &body, int records)
{
    auto *hash =
        g_compute_checksum_for_data(G_CHECKSUM_SHA256, reinterpret_cast<const guchar *>(body.data()), body.size());
    const auto result =
        body + "{\"type\":\"footer\",\"records\":" + std::to_string(records) + ",\"sha256\":\"" + hash + "\"}\n";
    g_free(hash);
    return result;
}
SnapshotEnvelope inspect(const std::string &text)
{
    std::istringstream input(text);
    return inspect_snapshot_envelope(input);
}
void require(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}
void bad(const std::string &text)
{
    try
    {
        inspect(text);
    }
    catch (const Failure &)
    {
        return;
    }
    throw std::runtime_error("malformed snapshot accepted");
}
} // namespace
int main()
{
    const auto valid = envelope(header + "\n" + entry + "\n", 2);
    auto metadata = inspect(valid);
    require(metadata.revision == 9 && metadata.records == 2 && metadata.counts[0] == 1 && metadata.sha256.size() == 64,
            "wrong snapshot metadata");
    require(inspect(valid.substr(0, valid.size() - 1)).records == 2, "footer EOF rejected");
    require(inspect(envelope(header + "\n", 1)).counts[0] == 0, "empty snapshot rejected");
    bad("");
    bad(header);
    bad(header + "\n");
    bad(valid + "\n");
    bad(valid + entry + "\n");
    bad(envelope(header + "\n" + entry + "\n", 1));
    auto changed = valid;
    changed[changed.find("你好")] = 'x';
    bad(changed);
    bad(envelope(header + "\n" + header + "\n", 2));
    bad(envelope(entry + "\n" + header + "\n", 2));
    bad(envelope(header + "\n\n", 2));
    bad(envelope(R"({"type":"header","type":"header","format":"msime-dictionary-snapshot","version":1,"revision":9})"
                 "\n",
                 1));
    bad(envelope(R"({"type":"header","format":"msime-dictionary-snapshot","version":1,"revision":true})"
                 "\n",
                 1));
    bad(envelope(R"({"type":"header","format":"msime-dictionary-snapshot","version":1,"revision":9.0})"
                 "\n",
                 1));
    bad(envelope(R"({"type":"header","format":"msime-dictionary-snapshot","version":2,"revision":9})"
                 "\n",
                 1));
    bad(envelope(header + "\n" + R"({"type":"entry","data":{"word":"one","\u0077ord":"two"}})" + "\n", 2));
    bad(envelope(header + "\n" + R"({"type":"position","data":{}})" + "\n" + entry + "\n", 3));
    bad(envelope(header + "\n" + R"({"type":"overlay","data":{},"deleted":0})" + "\n", 2));
    bad(envelope(header + "\n" + R"({"type":"unknown","data":{}})" + "\n", 2));
    bad(envelope(header + "\n" + R"({"type":"entry","data":{},"extra":1})" + "\n", 2));
    bad(envelope(header + "\n" + R"({"type":"entry","data":{"word":[]}})" + "\n", 2));
    bad(std::string(65536, ' ') + "\n");
    std::istringstream cancelled(valid);
    bool caught = false;
    try
    {
        inspect_snapshot_envelope(cancelled, [] { return true; });
    }
    catch (const Failure &e)
    {
        caught = e.cancelled();
    }
    require(caught, "snapshot cancellation lost");
    std::istringstream failed(valid);
    failed.setstate(std::ios::badbit);
    caught = false;
    try
    {
        inspect_snapshot_envelope(failed);
    }
    catch (const Failure &)
    {
        caught = true;
    }
    require(caught, "I/O failure accepted");
    std::cout << "Snapshot envelope framing, duplicate keys, digest and cancellation passed\n";
}
