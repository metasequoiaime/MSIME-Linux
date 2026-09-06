#include "../src/VoiceInput.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

using namespace metasequoia::linux_ime;
using namespace metasequoia::linux_ime::online;

namespace
{
class FakeTransport final : public HttpTransport
{
  public:
    HttpRequest request;
    int calls = 0;
    bool *cancel_after_response = nullptr;
    HttpResponse response{200, R"({"text":"你好世界"})", {}};

    HttpResponse perform(const HttpRequest &value, const CancellationCheck &) override
    {
        ++calls;
        request = value;
        if (cancel_after_response)
            *cancel_after_response = true;
        return response;
    }
};

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}
} // namespace

int main()
{
    try
    {
        auto transport = std::make_shared<FakeTransport>();
        VoiceInputProvider provider(transport);
        VoiceInputConfig config;
        config.enabled = true;
        config.endpoint = "https://voice.example/v1/audio/transcriptions";
        config.token = "voice-secret";
        config.model = "whisper-1";
        const std::string audio("RIFF\0audio", 10);
        const auto result = provider.transcribe(audio, config, [] { return false; });
        require(result && *result == "你好世界", "Voice transcription response was not parsed.");
        require(transport->request.method == HttpMethod::Post, "Voice transcription did not use POST.");
        require(transport->request.headers.size() == 2, "Voice transcription headers were incomplete.");
        require(transport->request.headers[0] == "Authorization: Bearer voice-secret",
                "Voice transcription authorization leaked or was malformed.");
        require(transport->request.body.find("name=\"file\"") != std::string::npos,
                "Voice transcription did not send a multipart file field.");
        require(transport->request.body.find("RIFF") != std::string::npos,
                "Voice transcription did not include the audio payload.");
        require(transport->request.body.find(audio) != std::string::npos,
                "Shared voice protocol lost binary NUL bytes.");
        require(transport->request.body.find("name=\"language\"\r\n\r\nzh") != std::string::npos,
                "Shared voice protocol dropped the selected language.");
        const auto calls = transport->calls;
        require(!provider.transcribe(audio, config, [] { return true; }) && transport->calls == calls,
                "Cancelled transcription reached the transport.");
        bool cancelled = false;
        transport->cancel_after_response = &cancelled;
        require(!provider.transcribe(audio, config, [&] { return cancelled; }),
                "Transcription returned a response after cancellation.");
        transport->cancel_after_response = nullptr;
        require(!provider.transcribe("audio", VoiceInputConfig{}, [] { return false; }),
                "An insecure or incomplete voice configuration was accepted.");
        transport->response = {200, R"({"result":{"text":"nested"}})", {}};
        require(provider.transcribe("audio", config, [] { return false; }).value_or("") == "nested",
                "Nested voice transcription response was not parsed.");
        transport->response = {500, {}, "server"};
        require(!provider.transcribe("audio", config, [] { return false; }),
                "A failed voice transcription response was accepted.");
        config.polish_enabled = true;
        config.polish_endpoint = "https://voice.example/v1/chat/completions";
        config.polish_model = "polish-model";
        config.polish_prompt = "请整理：";
        transport->response = {200, R"({"choices":[{"message":{"content":"整理后的文本"}}]})", {}};
        const auto polished = provider.polish("原始文本", config, [] { return false; });
        require(polished && *polished == "整理后的文本", "Voice polish response was not parsed.");
        require(transport->request.url == config.polish_endpoint &&
                    transport->request.body.find("chat/completions") == std::string::npos &&
                    transport->request.body.find("polish-model") != std::string::npos &&
                    transport->request.body.find("原始文本") != std::string::npos &&
                    transport->request.body.find("voice-secret") == std::string::npos,
                "Voice polish request did not contain the configured model and text.");
        require(!provider.polish("原始文本", config, [] { return true; }), "Cancelled polish returned a result.");
        cancelled = false;
        transport->cancel_after_response = &cancelled;
        require(!provider.polish("原始文本", config, [&] { return cancelled; }),
                "Polish returned a response after cancellation.");
        transport->cancel_after_response = nullptr;
        require(VoiceInputRecorder::valid_duration(5) && !VoiceInputRecorder::valid_duration(0) &&
                    !VoiceInputRecorder::valid_duration(121),
                "Voice recorder duration validation was incorrect.");
        VoiceInputRecorder recorder;
        std::string recording_error;
        require(!recorder.record({}, 5, &recording_error) && !recording_error.empty(),
                "Voice recorder accepted an empty output path.");
        require(!recorder.record("/tmp/metasequoia-voice-test.wav", 0, &recording_error) && !recording_error.empty(),
                "Voice recorder accepted an invalid duration.");

        // The total timeout bounds the whole transfer, so it has to grow with the audio being uploaded: 120 s of
        // 16 kHz mono S16_LE is roughly 3.8 MB, which cannot leave the machine inside a flat budget. These checks pin
        // the growth, the configured base, the fallback and the ceiling.
        transport->response = {200, R"({"text":"ok"})", {}};
        VoiceInputConfig timeout_config = config;
        const std::string audio_one_mebibyte(1024 * 1024, '\x01');
        const std::string audio_two_mebibytes(2 * 1024 * 1024, '\x01');
        require(provider.transcribe(audio_one_mebibyte, timeout_config, [] { return false; }).has_value(),
                "Transcribing a mebibyte of audio failed.");
        const auto total_for_one_mebibyte = transport->request.total_timeout;
        require(provider.transcribe(audio_two_mebibytes, timeout_config, [] { return false; }).has_value(),
                "Transcribing two mebibytes of audio failed.");
        const auto total_for_two_mebibytes = transport->request.total_timeout;
        require(total_for_one_mebibyte >= std::chrono::milliseconds(16000),
                "The transcribe total timeout did not budget any upload time for a mebibyte of audio.");
        require(total_for_two_mebibytes - total_for_one_mebibyte >= std::chrono::milliseconds(7900),
                "The transcribe total timeout did not scale with the size of the uploaded audio.");
        require(total_for_two_mebibytes <= std::chrono::milliseconds(300000),
                "The transcribe total timeout lost its upper bound.");

        timeout_config.connect_timeout = std::chrono::milliseconds(4500);
        timeout_config.total_timeout = std::chrono::milliseconds(45000);
        const std::string short_audio("RIFF\0short", 10);
        require(provider.transcribe(short_audio, timeout_config, [] { return false; }).has_value(),
                "Transcribing a short recording failed.");
        require(transport->request.total_timeout >= std::chrono::milliseconds(45000) &&
                    transport->request.total_timeout < std::chrono::milliseconds(46000),
                "The configured total timeout was not used as the transcribe budget base.");
        require(transport->request.connect_timeout == std::chrono::milliseconds(4500),
                "The configured connect timeout never reached the transcribe request.");
        require(provider.transcribe(audio_one_mebibyte, timeout_config, [] { return false; }).has_value(),
                "Transcribing a mebibyte of audio with a configured budget failed.");
        require(transport->request.total_timeout >= std::chrono::milliseconds(53000),
                "The configured total timeout replaced the upload allowance instead of being its base.");

        timeout_config.connect_timeout = std::chrono::milliseconds(0);
        timeout_config.total_timeout = std::chrono::milliseconds(0);
        require(provider.transcribe(audio_one_mebibyte, timeout_config, [] { return false; }).has_value(),
                "Transcribing with unset timeouts failed.");
        require(transport->request.connect_timeout == std::chrono::milliseconds(2500) &&
                    transport->request.total_timeout >= std::chrono::milliseconds(16000),
                "A non-positive configured timeout did not fall back to a bounded, size-aware budget.");

        timeout_config.connect_timeout = std::chrono::milliseconds(2500);
        timeout_config.total_timeout = std::chrono::milliseconds(299000);
        require(provider.transcribe(audio_one_mebibyte, timeout_config, [] { return false; }).has_value(),
                "Transcribing with a near-ceiling budget failed.");
        require(transport->request.total_timeout == std::chrono::milliseconds(300000),
                "The transcribe total timeout was not clamped to its ceiling.");

        transport->response = {200, R"({"choices":[{"message":{"content":"整理后的文本"}}]})", {}};
        VoiceInputConfig polish_timeout_config = config;
        polish_timeout_config.connect_timeout = std::chrono::milliseconds(4500);
        polish_timeout_config.total_timeout = std::chrono::milliseconds(45000);
        require(provider.polish("原始文本", polish_timeout_config, [] { return false; }).has_value(),
                "Polishing with configured timeouts failed.");
        require(transport->request.connect_timeout == std::chrono::milliseconds(4500) &&
                    transport->request.total_timeout >= std::chrono::milliseconds(45000),
                "The configured timeouts never reached the polish request.");

        std::cout << "Voice input tests passed.\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
