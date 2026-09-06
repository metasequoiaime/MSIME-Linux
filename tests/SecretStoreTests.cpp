#include "../src/SecretStore.h"

#include <chrono>
#include <stdexcept>
#include <string>

namespace
{
using metasequoia::linux_ime::LibsecretSecretStore;
using metasequoia::linux_ime::SecretKind;
using metasequoia::linux_ime::SecretStatus;

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}
} // namespace

int main(int argc, char **argv)
{
    require(argc == 2 && std::string(argv[1]) == "--integration",
            "SecretStoreTests must run inside the temporary Secret Service fixture.");

    LibsecretSecretStore store("session");
    const std::string suffix = std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const std::string ai_provider = "test-ai-" + suffix;
    const std::string translation_provider = "test-translation-" + suffix;
    const std::string ai_secret = "sk-libsecret-ai-" + suffix;
    const std::string translation_secret = "sk-libsecret-translation-" + suffix;
    std::string diagnostic;

    require(store.store(SecretKind::AiApiToken, ai_provider, ai_secret, &diagnostic) && diagnostic.empty(),
            "The AI credential could not be stored in Secret Service.");
    require(store.store(SecretKind::TranslationApiToken, translation_provider, translation_secret, &diagnostic) &&
                diagnostic.empty(),
            "The translation credential could not be stored in Secret Service.");

    const auto ai = store.lookup(SecretKind::AiApiToken, ai_provider);
    const auto translation = store.lookup(SecretKind::TranslationApiToken, translation_provider);
    const auto isolated = store.lookup(SecretKind::TranslationApiToken, ai_provider);
    require(ai.status == SecretStatus::Found && ai.value == ai_secret && ai.diagnostic.empty() &&
                translation.status == SecretStatus::Found && translation.value == translation_secret &&
                translation.diagnostic.empty() && isolated.status == SecretStatus::NotFound && isolated.value.empty(),
            "Secret Service did not preserve purpose/provider isolation.");
    require(ai.diagnostic.find(ai_secret) == std::string::npos &&
                translation.diagnostic.find(translation_secret) == std::string::npos,
            "A Secret Service diagnostic exposed a credential.");

    require(store.erase(SecretKind::AiApiToken, ai_provider, &diagnostic) && diagnostic.empty() &&
                store.erase(SecretKind::TranslationApiToken, translation_provider, &diagnostic) && diagnostic.empty(),
            "Temporary Secret Service credentials could not be removed.");
    require(store.lookup(SecretKind::AiApiToken, ai_provider).status == SecretStatus::NotFound &&
                store.lookup(SecretKind::TranslationApiToken, translation_provider).status == SecretStatus::NotFound,
            "Removed Secret Service credentials remained accessible.");

    // Erase is the undo half of a settings save, which rolls back secrets whose previous state was NotFound, so
    // "nothing matched" is a success and must not be dressed up as a Secret Service failure.
    const std::string absent_provider = "test-absent-" + suffix;
    require(store.lookup(SecretKind::AiApiToken, absent_provider).status == SecretStatus::NotFound,
            "The never-stored provider was unexpectedly already present in Secret Service.");
    require(store.erase(SecretKind::AiApiToken, absent_provider, &diagnostic),
            "Erasing a provider that was never stored must succeed.");
    require(diagnostic.empty(), "Erasing a provider that was never stored reported a diagnostic.");
    require(store.lookup(SecretKind::AiApiToken, absent_provider).status == SecretStatus::NotFound,
            "A provider that was never stored became visible after being erased.");

    require(store.erase(SecretKind::AiApiToken, ai_provider, &diagnostic),
            "Erasing an already-erased AI credential must be idempotent.");
    require(diagnostic.empty(), "A repeated erase of the AI credential reported a diagnostic.");
    require(store.erase(SecretKind::TranslationApiToken, translation_provider, &diagnostic),
            "Erasing an already-erased translation credential must be idempotent.");
    require(diagnostic.empty(), "A repeated erase of the translation credential reported a diagnostic.");
    require(store.lookup(SecretKind::AiApiToken, ai_provider).status == SecretStatus::NotFound &&
                store.lookup(SecretKind::TranslationApiToken, translation_provider).status == SecretStatus::NotFound,
            "A repeated erase resurrected a Secret Service credential.");

    // Idempotence must not degrade into "erase always succeeds": malformed input is still a caller error.
    require(!store.erase(SecretKind::AiApiToken, "invalid provider!", &diagnostic),
            "Erase accepted a malformed provider name.");
    require(!diagnostic.empty(), "Erase rejected a malformed provider without explaining why.");
    return 0;
}
