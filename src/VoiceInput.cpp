#include "VoiceInput.h"

#include "online/EndpointPolicy.h"

#include <msime/voice/provider_protocol.h>
#include <msime/voice/stt_service.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

#if defined(__linux__)
#include <cerrno>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace metasequoia::linux_ime
{
namespace
{
constexpr std::size_t kMaximumAudioBytes = voice::maximum_encoded_audio_bytes;
constexpr std::chrono::milliseconds kFallbackConnectTimeout{2500};
constexpr std::chrono::milliseconds kFallbackTotalTimeout{8000};
// Hard ceiling so a stalled upload still fails instead of pinning the caller indefinitely; the largest payload the
// shared protocol accepts (20 MiB) stays well below it at the assumed rate.
constexpr std::chrono::milliseconds kMaximumTotalTimeout{300000};
// Deliberately pessimistic uplink floor (1 Mbit/s) used to turn the request body into an upload allowance.
constexpr std::uint64_t kAssumedUploadBytesPerSecond = 128 * 1024;

std::chrono::milliseconds connect_timeout_for(const VoiceInputConfig &config)
{
    // A non-positive value means "no limit" to libcurl, which is never what a misconfigured setting should buy.
    return config.connect_timeout.count() > 0 ? config.connect_timeout : kFallbackConnectTimeout;
}

// CURLOPT_TIMEOUT_MS bounds the whole transfer, so a flat budget rejects the recordings this repo itself advertises:
// 120 s of 16 kHz mono S16_LE is roughly 3.8 MB, which cannot even leave the machine within the 8 s default. Spend the
// configured total timeout on connection setup and server-side processing, then add time proportional to the bytes
// actually being uploaded.
std::chrono::milliseconds total_timeout_for(const VoiceInputConfig &config, std::size_t body_bytes)
{
    const std::chrono::milliseconds base =
        config.total_timeout.count() > 0 ? config.total_timeout : kFallbackTotalTimeout;
    const std::uint64_t upload_allowance =
        static_cast<std::uint64_t>(body_bytes) * 1000U / kAssumedUploadBytesPerSecond;
    const std::uint64_t budget = std::min(static_cast<std::uint64_t>(base.count()) + upload_allowance,
                                          static_cast<std::uint64_t>(kMaximumTotalTimeout.count()));
    return std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(budget));
}

void set_error(std::string *error, const char *message)
{
    if (error != nullptr)
    {
        *error = message;
    }
}

#if defined(__linux__)
bool run_recorder(const char *program, const char *const arguments[], int *status)
{
    const pid_t child = ::fork();
    if (child < 0)
    {
        return false;
    }
    if (child == 0)
    {
        ::execvp(program, const_cast<char *const *>(arguments));
        _exit(127);
    }
    int wait_status = 0;
    while (::waitpid(child, &wait_status, 0) < 0)
    {
        if (errno != EINTR)
        {
            return false;
        }
    }
    if (status != nullptr)
    {
        *status = wait_status;
    }
    return true;
}
#endif

} // namespace

VoiceInputProvider::VoiceInputProvider(std::shared_ptr<online::HttpTransport> transport,
                                       bool allow_insecure_loopback_for_tests)
    : transport_(std::move(transport)), allow_insecure_loopback_for_tests_(allow_insecure_loopback_for_tests)
{
}

std::optional<std::string> VoiceInputProvider::transcribe(std::string_view audio, const VoiceInputConfig &config,
                                                          const online::CancellationCheck &cancelled,
                                                          std::string *error) const
{
    if (error != nullptr)
    {
        error->clear();
    }
    if (!transport_ || !config.enabled || audio.empty() || audio.size() > kMaximumAudioBytes || config.token.empty() ||
        !online::token_allowed(config.token) || config.model.empty() ||
        !online::endpoint_allowed(config.endpoint, allow_insecure_loopback_for_tests_))
    {
        set_error(error, "Voice input configuration or audio was invalid.");
        return std::nullopt;
    }
    const auto payload = voice::make_transcription_request(audio, config.model, config.language);
    online::HttpRequest request;
    request.method = online::HttpMethod::Post;
    request.url = config.endpoint;
    request.headers = {"Authorization: Bearer " + config.token, "Content-Type: " + payload.content_type};
    request.body = payload.body;
    request.connect_timeout = connect_timeout_for(config);
    request.total_timeout = total_timeout_for(config, request.body.size());
    request.max_response_bytes = 256 * 1024;
    if (cancelled && cancelled())
    {
        set_error(error, "Voice request cancelled.");
        return std::nullopt;
    }
    const auto response = transport_->perform(request, cancelled);
    if (cancelled && cancelled())
    {
        set_error(error, "Voice request cancelled.");
        return std::nullopt;
    }
    if (response.status_code < 200 || response.status_code >= 300)
    {
        set_error(error, response.error.empty() ? "Voice transcription request failed." : response.error.c_str());
        return std::nullopt;
    }
    const auto result = parse_transcription(response.body);
    if (!result)
    {
        set_error(error, "Voice transcription response did not contain text.");
    }
    return result;
}

std::optional<std::string> VoiceInputProvider::polish(std::string_view text, const VoiceInputConfig &config,
                                                      const online::CancellationCheck &cancelled,
                                                      std::string *error) const
{
    if (error != nullptr)
    {
        error->clear();
    }
    if (!transport_ || !config.polish_enabled || text.empty() || config.token.empty() ||
        !online::token_allowed(config.token) || config.polish_model.empty() || config.polish_prompt.empty() ||
        !online::endpoint_allowed(config.polish_endpoint, allow_insecure_loopback_for_tests_))
    {
        set_error(error, "Voice polish configuration or text was invalid.");
        return std::nullopt;
    }
    online::HttpRequest request;
    request.method = online::HttpMethod::Post;
    request.url = config.polish_endpoint;
    request.headers = {"Authorization: Bearer " + config.token, "Content-Type: application/json"};
    try
    {
        request.body = voice::make_polish_request(config.polish_model, config.polish_prompt, text);
    }
    catch (const voice::VoiceError &)
    {
        set_error(error, "Voice polish configuration or text was invalid.");
        return std::nullopt;
    }
    request.connect_timeout = connect_timeout_for(config);
    request.total_timeout = total_timeout_for(config, request.body.size());
    request.max_response_bytes = 256 * 1024;
    if (cancelled && cancelled())
    {
        set_error(error, "Voice request cancelled.");
        return std::nullopt;
    }
    const auto response = transport_->perform(request, cancelled);
    if (cancelled && cancelled())
    {
        set_error(error, "Voice request cancelled.");
        return std::nullopt;
    }
    if (response.status_code < 200 || response.status_code >= 300)
    {
        set_error(error, response.error.empty() ? "Voice polish request failed." : response.error.c_str());
        return std::nullopt;
    }
    try
    {
        return voice::parse_polished_text(response.body);
    }
    catch (const voice::VoiceError &)
    {
        set_error(error, "Voice polish response did not contain content.");
        return std::nullopt;
    }
}

std::optional<std::string> VoiceInputProvider::parse_transcription(std::string_view response)
{
    try
    {
        return voice::parse_transcription(response);
    }
    catch (const voice::VoiceError &)
    {
        return std::nullopt;
    }
}

bool VoiceInputRecorder::valid_duration(int seconds)
{
    return seconds >= 1 && seconds <= 120;
}

bool VoiceInputRecorder::record(const std::filesystem::path &output, int seconds, std::string *error) const
{
    if (error != nullptr)
    {
        error->clear();
    }
    if (output.empty() || !valid_duration(seconds))
    {
        set_error(error, "Voice recording path or duration was invalid.");
        return false;
    }

#if defined(__linux__)
    const std::string duration = std::to_string(seconds);
    const std::string output_path = output.string();
    const char *arecord_arguments[] = {
        "arecord",           "-q",   "-f", "S16_LE", "-r", "16000", "-c", "1", "-d", duration.c_str(),
        output_path.c_str(), nullptr};
    int status = 0;
    if (run_recorder("arecord", arecord_arguments, &status) && WIFEXITED(status) && WEXITSTATUS(status) == 0)
    {
        return true;
    }

    const char *pw_record_arguments[] = {"pw-record",      "--format",          "s16",  "--rate",
                                         "16000",          "--channels",        "1",    "--limit",
                                         duration.c_str(), output_path.c_str(), nullptr};
    if (run_recorder("pw-record", pw_record_arguments, &status) && WIFEXITED(status) && WEXITSTATUS(status) == 0)
    {
        return true;
    }
    set_error(error, "No supported Linux recorder was available or recording failed.");
    return false;
#else
    set_error(error, "Voice recording is only supported on Linux.");
    return false;
#endif
}
} // namespace metasequoia::linux_ime
