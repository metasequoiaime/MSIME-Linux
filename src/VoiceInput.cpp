#include "VoiceInput.h"

#include "online/EndpointPolicy.h"

#include <msime/voice/provider_protocol.h>
#include <msime/voice/stt_service.h>

#include <algorithm>
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
