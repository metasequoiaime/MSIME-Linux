#pragma once

#include "online/HttpTransport.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace metasequoia::linux_ime
{
struct VoiceInputConfig
{
    bool enabled = false;
    std::string provider = "openai";
    std::string endpoint = "https://api.openai.com/v1/audio/transcriptions";
    std::string model = "whisper-1";
    std::string language = "zh";
    std::string token;
};

class VoiceInputProvider
{
  public:
    explicit VoiceInputProvider(std::shared_ptr<online::HttpTransport> transport,
                                bool allow_insecure_loopback_for_tests = false);

    std::optional<std::string> transcribe(std::string_view audio, const VoiceInputConfig &config,
                                          const online::CancellationCheck &cancelled,
                                          std::string *error = nullptr) const;
    static std::optional<std::string> parse_transcription(std::string_view response);

  private:
    std::shared_ptr<online::HttpTransport> transport_;
    bool allow_insecure_loopback_for_tests_ = false;
};

class VoiceInputRecorder
{
  public:
    static bool valid_duration(int seconds);
    bool record(const std::filesystem::path &output, int seconds, std::string *error = nullptr) const;
};
} // namespace metasequoia::linux_ime
