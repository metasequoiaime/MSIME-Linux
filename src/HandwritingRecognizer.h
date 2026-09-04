#pragma once

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

class HandwritingRecognizer
{
  public:
    static constexpr std::size_t kMaxCandidates = 12;

    static std::vector<std::string> parse_candidates(std::string_view output);
    std::vector<std::string> recognize(const std::vector<HandwritingStroke> &strokes, int width, int height,
                                       std::string *error = nullptr) const;
};
} // namespace metasequoia::linux_ime
