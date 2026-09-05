#include "../src/VoiceInput.h"

#include <iostream>
#include <memory>
#include <stdexcept>

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

        std::cout << "Voice input tests passed.\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
