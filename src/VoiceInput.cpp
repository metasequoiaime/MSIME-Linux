#include "VoiceInput.h"

#include <boost/json.hpp>

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
constexpr std::size_t kMaximumAudioBytes = 20 * 1024 * 1024;

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

bool endpoint_allowed(std::string_view endpoint, bool allow_loopback)
{
    if (endpoint.rfind("https://", 0) == 0)
    {
        return true;
    }
    if (!allow_loopback || endpoint.rfind("http://127.0.0.1:", 0) != 0)
    {
        return false;
    }
    return endpoint.find('/', 7) != std::string_view::npos;
}

std::string make_multipart(std::string_view audio, std::string_view boundary, std::string_view model,
                           std::string_view language)
{
    std::string body;
    body.reserve(audio.size() + 512);
    const auto append = [&body](std::string_view value) { body.append(value.data(), value.size()); };
    append("--");
    append(boundary);
    append("\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n");
    append(model);
    append("\r\n--");
    append(boundary);
    append("\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\n");
    append(language);
    append("\r\n--");
    append(boundary);
    append("\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\nContent-Type: audio/wav\r\n\r\n");
    append(audio);
    append("\r\n--");
    append(boundary);
    append("--\r\n");
    return body;
}

std::optional<std::string> string_member(const boost::json::object &object, std::string_view key)
{
    const auto found = object.find(key);
    if (found == object.end() || !found->value().is_string())
    {
        return std::nullopt;
    }
    return std::string(found->value().as_string().c_str(), found->value().as_string().size());
}
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
    if (!transport_ || !config.enabled || audio.empty() || audio.size() > kMaximumAudioBytes || config.token.empty() || config.model.empty() ||
        !endpoint_allowed(config.endpoint, allow_insecure_loopback_for_tests_))
    {
        set_error(error, "Voice input configuration or audio was invalid.");
        return std::nullopt;
    }
    const std::string boundary = "----MetasequoiaImeVoiceBoundary7d2c9a";
    online::HttpRequest request;
    request.method = online::HttpMethod::Post;
    request.url = config.endpoint;
    request.headers = {"Authorization: Bearer " + config.token,
                       "Content-Type: multipart/form-data; boundary=" + boundary};
    request.body = make_multipart(audio, boundary, config.model, config.language);
    request.max_response_bytes = 256 * 1024;
    const auto response = transport_->perform(request, cancelled);
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
    if (!transport_ || !config.polish_enabled || text.empty() || config.token.empty() || config.polish_model.empty() ||
        config.polish_prompt.empty() || !endpoint_allowed(config.polish_endpoint, allow_insecure_loopback_for_tests_))
    {
        set_error(error, "Voice polish configuration or text was invalid.");
        return std::nullopt;
    }
    boost::json::object system_message;
    system_message["role"] = "system";
    system_message["content"] = config.polish_prompt;
    boost::json::object user_message;
    user_message["role"] = "user";
    user_message["content"] = std::string(text);
    boost::json::array messages;
    messages.push_back(std::move(system_message));
    messages.push_back(std::move(user_message));
    boost::json::object request_body;
    request_body["model"] = config.polish_model;
    request_body["stream"] = false;
    request_body["messages"] = std::move(messages);

    online::HttpRequest request;
    request.method = online::HttpMethod::Post;
    request.url = config.polish_endpoint;
    request.headers = {"Authorization: Bearer " + config.token, "Content-Type: application/json"};
    request.body = boost::json::serialize(request_body);
    request.max_response_bytes = 256 * 1024;
    const auto response = transport_->perform(request, cancelled);
    if (response.status_code < 200 || response.status_code >= 300)
    {
        set_error(error, response.error.empty() ? "Voice polish request failed." : response.error.c_str());
        return std::nullopt;
    }
    boost::system::error_code parse_error;
    const boost::json::value parsed = boost::json::parse(response.body, parse_error);
    if (parse_error || !parsed.is_object())
    {
        set_error(error, "Voice polish response was invalid.");
        return std::nullopt;
    }
    const auto choices = parsed.as_object().find("choices");
    if (choices == parsed.as_object().end() || !choices->value().is_array() || choices->value().as_array().empty() ||
        !choices->value().as_array().front().is_object())
    {
        set_error(error, "Voice polish response did not contain content.");
        return std::nullopt;
    }
    const auto message = choices->value().as_array().front().as_object().find("message");
    if (message == choices->value().as_array().front().as_object().end() || !message->value().is_object())
    {
        set_error(error, "Voice polish response did not contain content.");
        return std::nullopt;
    }
    const auto content = message->value().as_object().find("content");
    if (content == message->value().as_object().end() || !content->value().is_string() ||
        content->value().as_string().empty())
    {
        set_error(error, "Voice polish response did not contain content.");
        return std::nullopt;
    }
    return std::string(content->value().as_string().c_str(), content->value().as_string().size());
}

std::optional<std::string> VoiceInputProvider::parse_transcription(std::string_view response)
{
    boost::system::error_code parse_error;
    const boost::json::value parsed = boost::json::parse(response, parse_error);
    if (parse_error || !parsed.is_object())
    {
        return std::nullopt;
    }
    const auto &object = parsed.as_object();
    if (const auto text = string_member(object, "text"); text && !text->empty())
    {
        return text;
    }
    if (const auto text = string_member(object, "transcription"); text && !text->empty())
    {
        return text;
    }
    const auto nested = object.find("result");
    if (nested != object.end() && nested->value().is_object())
    {
        if (const auto text = string_member(nested->value().as_object(), "text"); text && !text->empty())
        {
            return text;
        }
    }
    return std::nullopt;
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
    const char *arecord_arguments[] = {"arecord", "-q", "-f", "S16_LE", "-r", "16000", "-c", "1", "-d",
                                       duration.c_str(), output_path.c_str(), nullptr};
    int status = 0;
    if (run_recorder("arecord", arecord_arguments, &status) && WIFEXITED(status) && WEXITSTATUS(status) == 0)
    {
        return true;
    }

    const char *pw_record_arguments[] = {"pw-record", "--format", "s16", "--rate", "16000", "--channels", "1",
                                         "--limit", duration.c_str(), output_path.c_str(), nullptr};
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
