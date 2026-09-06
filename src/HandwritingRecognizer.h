#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace metasequoia::linux_ime
{
struct HandwritingPoint
{
    double x = 0.0;
    double y = 0.0;
};

using HandwritingStroke = std::vector<HandwritingPoint>;

// Recognition spawns an external OCR process, so every request is bounded by a wall-clock deadline.
inline constexpr std::chrono::milliseconds kHandwritingRecognitionTimeout{4000};

enum class HandwritingStatus
{
    Ok,
    NoStrokes,
    InvalidCanvas,
    TemporaryFileFailed,
    BackendMissing,
    BackendFailed,
    BackendTimedOut,
    NoCandidates,
};

// The status lets a caller tell "install tesseract" apart from "nothing was drawn"; the message is displayable.
struct HandwritingResult
{
    HandwritingStatus status = HandwritingStatus::NoStrokes;
    std::vector<std::string> candidates;
    std::string message;
};

// Self-contained so a caller can move a request onto a worker thread and move the result back.
struct HandwritingRequest
{
    std::vector<HandwritingStroke> strokes;
    int width = 0;
    int height = 0;
    std::chrono::milliseconds timeout = kHandwritingRecognitionTimeout;
};

class HandwritingRecognizer
{
  public:
    static constexpr std::size_t kMaxCandidates = 12;

    static std::vector<std::string> parse_candidates(std::string_view output);

    // Blocks for at most request.timeout and keeps no shared state, so it is safe to run on a worker thread.
    HandwritingResult recognize(const HandwritingRequest &request) const;

    // Convenience form for callers that only need the candidate list; the reported error is empty on success.
    std::vector<std::string> recognize(const std::vector<HandwritingStroke> &strokes, int width, int height,
                                       std::string *error = nullptr) const;
};
} // namespace metasequoia::linux_ime
