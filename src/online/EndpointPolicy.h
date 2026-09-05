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

// Whether a credential is shaped like something that can go into a header. The
// providers concatenate it into "Authorization: Bearer ..." and hand that to
// curl_slist_append, and libcurl passes header values through verbatim, so a
// carriage return in a token adds headers to the request. Two of the three
// providers rejected that and one did not.
//
// Emptiness is deliberately not decided here: the translation provider sends no
// Authorization header at all when the token is empty, while the others require
// one, so each caller keeps that part.
bool token_allowed(std::string_view token);
} // namespace metasequoia::linux_ime::online
