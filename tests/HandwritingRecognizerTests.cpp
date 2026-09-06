#include "HandwritingRecognizer.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

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

// A stub `tesseract` first on PATH keeps the backend cases deterministic without a real OCR install.

// This one reports the PGM header it was handed, which is how the tests observe the fixed OCR bitmap.
constexpr const char *kBitmapSizeStub = R"(#!/bin/sh
{
  read -r magic
  read -r width height
} < "$1"
printf '%sx%s\n' "$width" "$height"
)";

constexpr const char *kFailingStub = R"(#!/bin/sh
exit 1
)";

constexpr const char *kSilentStub = R"(#!/bin/sh
exit 0
)";

constexpr const char *kStalledStub = R"(#!/bin/sh
sleep 30
)";

std::filesystem::path make_temporary_directory(const char *pattern)
{
    const std::string path = (std::filesystem::temp_directory_path() / pattern).string();
    std::vector<char> writable(path.begin(), path.end());
    writable.push_back('\0');
    require(::mkdtemp(writable.data()) != nullptr, "Unable to create a temporary directory for the stub backend.");
    return std::filesystem::path(writable.data());
}

void write_stub(const std::filesystem::path &directory, const char *body)
{
    const std::filesystem::path script = directory / "tesseract";
    {
        std::ofstream file(script, std::ios::binary | std::ios::trunc);
        file << body;
        require(static_cast<bool>(file), "Unable to write the stub handwriting backend.");
    }
    std::filesystem::permissions(script, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
}

void use_path(const std::string &value)
{
    require(::setenv("PATH", value.c_str(), 1) == 0, "Unable to point PATH at the stub handwriting backend.");
}

HandwritingRequest make_request(int width, int height)
{
    HandwritingRequest request;
    request.strokes = {{{40.0, 40.0}, {200.0, 120.0}, {120.0, 200.0}}, {{60.0, 200.0}, {180.0, 60.0}}};
    request.width = width;
    request.height = height;
    return request;
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

        const char *inherited = ::getenv("PATH");
        const std::string inherited_path = inherited != nullptr ? inherited : "";
        const std::filesystem::path stub_directory =
            make_temporary_directory("metasequoia-ime-handwriting-stub-XXXXXX");
        const std::filesystem::path empty_directory =
            make_temporary_directory("metasequoia-ime-handwriting-empty-XXXXXX");
        use_path(inherited_path.empty() ? stub_directory.string() : stub_directory.string() + ':' + inherited_path);

        write_stub(stub_directory, kBitmapSizeStub);

        // A window wider than the old 2048 pixel ceiling must still reach the backend.
        const HandwritingResult wide = recognizer.recognize(make_request(5000, 900));
        require(wide.status != HandwritingStatus::InvalidCanvas,
                "A canvas wider than 2048 pixels was rejected, so recognition is dead on a maximized window.");
        require(wide.status == HandwritingStatus::Ok, "The stub handwriting backend did not answer a wide canvas.");
        require(wide.candidates.size() == 1 && wide.candidates.front() == "256x256",
                "A wide canvas was not normalized into the fixed handwriting bitmap.");
        require(wide.message.empty(), "A successful handwriting recognition still reported a message.");

        // The size the tools UI pins must stay accepted, and it must render the same bitmap as any other canvas.
        const HandwritingRequest shipped = make_request(400, 240);
        const auto shipped_candidates = recognizer.recognize(shipped.strokes, shipped.width, shipped.height, &error);
        require(error.empty(), "The shipped 400x240 handwriting canvas reported an error on success.");
        require(shipped_candidates.size() == 1 && shipped_candidates.front() == "256x256",
                "The shipped canvas was rendered at its own size instead of the fixed handwriting bitmap.");

        write_stub(stub_directory, kFailingStub);
        const HandwritingResult failed = recognizer.recognize(shipped);
        require(failed.status == HandwritingStatus::BackendFailed,
                "A backend that exited non-zero was not reported as a backend failure.");
        require(failed.candidates.empty() && !failed.message.empty(),
                "A failed handwriting recognition carried candidates or no message.");
        require(recognizer.recognize(shipped.strokes, shipped.width, shipped.height, &error).empty() && !error.empty(),
                "A failed handwriting recognition was not reported through the legacy error channel.");

        write_stub(stub_directory, kSilentStub);
        const HandwritingResult silent = recognizer.recognize(shipped);
        require(silent.status == HandwritingStatus::NoCandidates,
                "A backend that succeeded with no output was not distinguishable from a backend failure.");
        require(silent.candidates.empty() && !silent.message.empty(),
                "An empty handwriting recognition carried candidates or no message.");
        require(recognizer.recognize(shipped.strokes, shipped.width, shipped.height, &error).empty() && !error.empty(),
                "An empty handwriting recognition was not reported through the legacy error channel.");

        write_stub(stub_directory, kStalledStub);
        HandwritingRequest stalled = make_request(400, 240);
        stalled.timeout = std::chrono::milliseconds(300);
        const auto request_started = std::chrono::steady_clock::now();
        const HandwritingResult timed_out = recognizer.recognize(stalled);
        const auto request_elapsed = std::chrono::steady_clock::now() - request_started;
        require(timed_out.status == HandwritingStatus::BackendTimedOut,
                "A stalled handwriting backend was not stopped and reported as a timeout.");
        require(timed_out.candidates.empty() && !timed_out.message.empty(),
                "A timed out handwriting recognition carried candidates or no message.");
        require(request_elapsed < std::chrono::seconds(5),
                "Handwriting recognition outlived the deadline the request asked for.");

        // The overload the desktop tools shipped with must be bounded too, by the default deadline.
        const auto legacy_started = std::chrono::steady_clock::now();
        const auto legacy_candidates = recognizer.recognize(stalled.strokes, stalled.width, stalled.height, &error);
        const auto legacy_elapsed = std::chrono::steady_clock::now() - legacy_started;
        require(legacy_candidates.empty() && !error.empty(),
                "A stalled handwriting backend produced candidates or no error.");
        require(legacy_elapsed < std::chrono::seconds(15),
                "Handwriting recognition waited on a stalled backend instead of applying the default deadline.");

        use_path(empty_directory.string());
        const HandwritingResult unavailable = recognizer.recognize(shipped);
        require(unavailable.status == HandwritingStatus::BackendMissing,
                "An absent handwriting backend was not distinguishable from a backend failure.");
        require(unavailable.candidates.empty() && !unavailable.message.empty(),
                "An absent handwriting backend carried candidates or no message.");

        use_path(inherited_path);
        std::error_code ignored;
        std::filesystem::remove_all(stub_directory, ignored);
        std::filesystem::remove_all(empty_directory, ignored);

        std::cout << "Handwriting recognizer tests passed.\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
