#pragma once

#include <cstddef>
#include <string>

namespace metasequoia::linux_ime
{
// What one metasequoia-ime-voice run has to become on screen. The tool prints the transcription on stdout and every
// failure reason on stderr, and a toolbar started from its .desktop file has both wired to the session journal, so this
// is the only place that can turn either into something the user can read or paste.
struct VoiceToolbarResult
{
    bool copied = false;
    std::string text;
    std::string message;
};

inline VoiceToolbarResult voice_toolbar_result(bool successful, const std::string &standard_output,
                                               const std::string &standard_error)
{
    const auto trim = [](const std::string &value) {
        const std::size_t first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
        {
            return std::string();
        }
        return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    };
    VoiceToolbarResult result;
    const std::string text = trim(standard_output);
    if (successful && !text.empty())
    {
        result.copied = true;
        result.text = text;
        result.message = "语音转写已复制到剪贴板：" + text;
        return result;
    }
    const std::string failure = trim(standard_error);
    if (!failure.empty())
    {
        result.message = failure;
    }
    else
    {
        result.message = successful ? "语音转写没有返回文本。" : "语音转写失败。";
    }
    return result;
}
} // namespace metasequoia::linux_ime
