#pragma once
#include <metasequoia/dictionary_state.h>
namespace metasequoia::linux_ime::account
{
struct ActiveDictionary
{
    RuntimePaths paths;
    std::string generation;
    std::string content_id;
    std::string token;
};
struct NativePublication
{
    bool published = false;
    bool durable = false;
};
// The caller holds the shared coordination lease for reads and exclusive lease
// for publication, with native sessions quiesced. Resources are verified by the
// installer before staging; this store only validates the ready layout/marker.
class NativeInstallation
{
  public:
    NativeInstallation(std::filesystem::path root, RuntimePaths legacy);
    std::filesystem::path resources(const std::string &content_id) const;
    std::filesystem::path generation(const std::string &identifier) const;
    ActiveDictionary active() const;
    // Validate a staged layout before dropping the running session references.
    RuntimePaths prepared_paths(const std::string &identifier, const std::string &content_id) const;
    // Compare the preview's active token AND native journal revision under the
    // exclusive lease before calling. False means token conflict. A successful
    // rename with failed directory fsync reports published=true,durable=false.
    NativePublication publish(const std::string &identifier, const std::string &content_id,
                              const std::string &expected_token) const;

  private:
    std::filesystem::path root_;
    RuntimePaths legacy_;
};
} // namespace metasequoia::linux_ime::account
