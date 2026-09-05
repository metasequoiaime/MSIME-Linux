#pragma once

#include <string_view>

namespace metasequoia::linux_ime::online
{
// Every provider that sends a credential somewhere has to decide whether the
// configured endpoint may receive it. That decision used to be written three
// times, once per provider, and the three copies had drifted: only the
// translation provider rejected carriage returns and newlines, and only the
// voice provider had no length limit at all. One implementation removes the
// question of which provider is the strict one.
//
// allow_loopback_for_tests exists so the test suites can point a provider at a
// local HTTP server. Nothing in the shipped configuration path sets it.
bool endpoint_allowed(std::string_view endpoint, bool allow_loopback_for_tests = false);
} // namespace metasequoia::linux_ime::online
