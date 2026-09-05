#include "HandwritingRecognizer.h"

#include <iostream>
#include <stdexcept>

using namespace metasequoia::linux_ime;

namespace
{
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
        const auto candidates = HandwritingRecognizer::parse_candidates("  你\n好\n你\n\n");
        require(candidates.size() == 2 && candidates[0] == "你" && candidates[1] == "好",
                "Handwriting candidates were not normalized or deduplicated.");
        require(HandwritingRecognizer::parse_candidates("\xff").empty(),
                "Invalid UTF-8 handwriting output was accepted.");

        HandwritingRecognizer recognizer;
        std::string error;
        require(recognizer.recognize({}, 400, 300, &error).empty() && !error.empty(),
                "Empty handwriting strokes were accepted.");
        require(recognizer.recognize({{{{10.0, 10.0}, {100.0, 100.0}}}}, 16, 16, &error).empty() && !error.empty(),
                "An undersized handwriting canvas was accepted.");

        std::cout << "Handwriting recognizer tests passed.\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
