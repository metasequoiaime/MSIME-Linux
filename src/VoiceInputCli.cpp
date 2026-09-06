#include "SettingsStore.h"
#include "VoiceInput.h"
#include "online/HttpTransport.h"

#include <fstream>
#include <charconv>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#if defined(__linux__)
#include <unistd.h>
#endif

using namespace metasequoia::linux_ime;

namespace
{
void usage(const char *program)
{
    std::cerr << "Usage: " << program << " --file AUDIO.wav\n"
              << "       " << program << " --record SECONDS\n"
              << "       " << program << " --check\n";
}

bool parse_duration(const char *value, int *duration)
{
    if (value == nullptr || duration == nullptr)
    {
        return false;
    }
    const std::string text(value);
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), *duration);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

std::optional<std::filesystem::path> temporary_audio_path()
{
#if defined(__linux__)
    std::string pattern = (std::filesystem::temp_directory_path() / "metasequoia-ime-voice-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    const int descriptor = ::mkstemp(writable.data());
    if (descriptor < 0)
    {
        return std::nullopt;
    }
    ::close(descriptor);
    return std::filesystem::path(writable.data());
#else
    return std::nullopt;
#endif
}
} // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--check")
    {
        return 0;
    }
    if (argc != 3 || (std::string(argv[1]) != "--file" && std::string(argv[1]) != "--record"))
    {
        usage(argv[0]);
        return 2;
    }

    std::filesystem::path audio_path;
    bool temporary = false;
    if (std::string(argv[1]) == "--file")
    {
        audio_path = argv[2];
    }
    else
    {
        int duration = 0;
        if (!parse_duration(argv[2], &duration) || !VoiceInputRecorder::valid_duration(duration))
        {
            std::cerr << "Recording duration must be between 1 and 120 seconds.\n";
            return 2;
        }
        const auto temporary_path = temporary_audio_path();
        if (!temporary_path)
        {
            std::cerr << "Unable to create a temporary audio file.\n";
            return 1;
        }
        audio_path = *temporary_path;
        temporary = true;
        VoiceInputRecorder recorder;
        std::string recording_error;
        if (!recorder.record(audio_path, duration, &recording_error))
        {
            std::cerr << (recording_error.empty() ? "Voice recording failed." : recording_error) << '\n';
            std::error_code ignored;
            std::filesystem::remove(audio_path, ignored);
            return 1;
        }
    }

    std::ifstream input(audio_path, std::ios::binary);
    if (!input)
    {
        std::cerr << "Unable to open audio file.\n";
        if (temporary)
        {
            std::error_code ignored;
            std::filesystem::remove(audio_path, ignored);
        }
        return 1;
    }
    const std::string audio((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    LibsecretSecretStore secrets;
    std::string warning;
    const InputSettings settings = SettingsStore().load(secrets, &warning);
    if (!settings.voice.enabled || settings.voice.token.empty())
    {
        std::cerr << "Voice input is disabled or has no configured credential.\n";
        if (temporary)
        {
            std::error_code ignored;
            std::filesystem::remove(audio_path, ignored);
        }
        return 1;
    }
    // The provider only receives the voice block, so the user's configured transport budget has to be carried across
    // here; the provider then extends it by the size of the audio being uploaded.
    VoiceInputConfig voice = settings.voice;
    voice.connect_timeout = settings.online.connect_timeout;
    voice.total_timeout = settings.online.total_timeout;
    auto transport = std::make_shared<online::CurlHttpTransport>();
    VoiceInputProvider provider(transport);
    std::string error;
    auto text = provider.transcribe(audio, voice, [] { return false; }, &error);
    if (!text)
    {
        std::cerr << (error.empty() ? "Voice transcription failed." : error) << '\n';
        if (temporary)
        {
            std::error_code ignored;
            std::filesystem::remove(audio_path, ignored);
        }
        return 1;
    }
    if (voice.polish_enabled)
    {
        std::string polish_error;
        const auto polished = provider.polish(*text, voice, [] { return false; }, &polish_error);
        if (polished)
        {
            text = polished;
        }
        else if (!polish_error.empty())
        {
            std::cerr << "Voice polish unavailable; using the raw transcription: " << polish_error << '\n';
        }
    }
    if (temporary)
    {
        std::error_code ignored;
        std::filesystem::remove(audio_path, ignored);
    }
    std::cout << *text << '\n';
    return 0;
}
