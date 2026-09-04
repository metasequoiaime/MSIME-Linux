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
    HttpResponse response{200, R"({"text":"你好世界"})", {}};

    HttpResponse perform(const HttpRequest &value, const CancellationCheck &) override
    {
        request = value;
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
        const auto result = provider.transcribe("RIFF\0audio", config, [] { return false; });
        require(result && *result == "你好世界", "Voice transcription response was not parsed.");
        require(transport->request.method == HttpMethod::Post, "Voice transcription did not use POST.");
        require(transport->request.headers.size() == 2, "Voice transcription headers were incomplete.");
        require(transport->request.headers[0] == "Authorization: Bearer voice-secret",
                "Voice transcription authorization leaked or was malformed.");
        require(transport->request.body.find("name=\"file\"") != std::string::npos,
                "Voice transcription did not send a multipart file field.");
        require(transport->request.body.find("RIFF") != std::string::npos,
                "Voice transcription did not include the audio payload.");
        require(!provider.transcribe("audio", VoiceInputConfig{}, [] { return false; }),
                "An insecure or incomplete voice configuration was accepted.");
        transport->response = {200, R"({"result":{"text":"nested"}})", {}};
        require(provider.transcribe("audio", config, [] { return false; }).value_or("") == "nested",
                "Nested voice transcription response was not parsed.");
        transport->response = {500, {}, "server"};
        require(!provider.transcribe("audio", config, [] { return false; }),
                "A failed voice transcription response was accepted.");
        require(VoiceInputRecorder::valid_duration(5) && !VoiceInputRecorder::valid_duration(0) &&
                    !VoiceInputRecorder::valid_duration(121),
                "Voice recorder duration validation was incorrect.");
        VoiceInputRecorder recorder;
        std::string recording_error;
        require(!recorder.record({}, 5, &recording_error) && !recording_error.empty(),
                "Voice recorder accepted an empty output path.");
        require(!recorder.record("/tmp/metasequoia-voice-test.wav", 0, &recording_error) &&
                    !recording_error.empty(),
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
