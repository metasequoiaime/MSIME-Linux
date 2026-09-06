#pragma once

#include "HttpTransport.h"
#include <metasequoia/session.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace metasequoia::linux_ime::online
{
class GoogleCloudProvider
{
  public:
    explicit GoogleCloudProvider(std::shared_ptr<HttpTransport> transport);

    std::optional<std::string> fetch(const OnlineQuery &query, const CancellationCheck &cancelled) const;
    static std::string build_url(const OnlineQuery &query);
    static std::optional<std::string> parse_candidate(std::string_view response);

  private:
    std::shared_ptr<HttpTransport> transport_;
};
} // namespace metasequoia::linux_ime::online
