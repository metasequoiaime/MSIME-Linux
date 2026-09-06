#include "HandwritingRecognizer.h"

#include <glib.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace metasequoia::linux_ime
{
namespace
{
constexpr int kMinimumCanvasDimension = 32;
// Strokes are normalized into a fixed OCR bitmap, so this only rejects a nonsensical coordinate space.
constexpr int kMaximumCanvasDimension = 16384;
constexpr int kRenderExtent = 256;
constexpr int kRenderMargin = 24;
constexpr int kStrokeRadius = 4;
constexpr std::size_t kMaximumOutputBytes = 64 * 1024;
constexpr int kChildPollIntervalMilliseconds = 5;
constexpr std::chrono::milliseconds kKillGrace{500};

struct StrokeBounds
{
    double min_x = 0.0;
    double min_y = 0.0;
    double max_x = 0.0;
    double max_y = 0.0;
};

enum class ChildState
{
    Exited,
    Running,
    Lost,
};

enum class BackendOutcome
{
    Ok,
    SpawnFailed,
    Missing,
    Failed,
    TimedOut,
};

std::string trim(std::string_view value)
{
    std::size_t first = 0;
    while (first < value.size() &&
           (value[first] == ' ' || value[first] == '\t' || value[first] == '\r' || value[first] == '\n'))
    {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first &&
           (value[last - 1] == ' ' || value[last - 1] == '\t' || value[last - 1] == '\r' || value[last - 1] == '\n'))
    {
        --last;
    }
    return std::string(value.substr(first, last - first));
}

void draw_pixel(std::vector<std::uint8_t> &pixels, int width, int height, int x, int y)
{
    if (x < 0 || y < 0 || x >= width || y >= height)
    {
        return;
    }
    pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = 0;
}

void draw_disk(std::vector<std::uint8_t> &pixels, int width, int height, int x, int y)
{
    for (int dy = -kStrokeRadius; dy <= kStrokeRadius; ++dy)
    {
        for (int dx = -kStrokeRadius; dx <= kStrokeRadius; ++dx)
        {
            if (dx * dx + dy * dy <= kStrokeRadius * kStrokeRadius)
            {
                draw_pixel(pixels, width, height, x + dx, y + dy);
            }
        }
    }
}

void draw_line(std::vector<std::uint8_t> &pixels, int width, int height, HandwritingPoint first,
               HandwritingPoint second)
{
    const double distance = std::hypot(second.x - first.x, second.y - first.y);
    const int steps = std::max(1, static_cast<int>(std::ceil(distance)));
    for (int step = 0; step <= steps; ++step)
    {
        const double fraction = static_cast<double>(step) / static_cast<double>(steps);
        draw_disk(pixels, width, height, static_cast<int>(std::lround(first.x + (second.x - first.x) * fraction)),
                  static_cast<int>(std::lround(first.y + (second.y - first.y) * fraction)));
    }
}

bool usable_point(HandwritingPoint point)
{
    return std::isfinite(point.x) && std::isfinite(point.y);
}

HandwritingPoint clamp_point(HandwritingPoint point, int width, int height)
{
    return {std::clamp(point.x, 0.0, static_cast<double>(width)),
            std::clamp(point.y, 0.0, static_cast<double>(height))};
}

std::optional<StrokeBounds> stroke_bounds(const std::vector<HandwritingStroke> &strokes, int width, int height)
{
    std::optional<StrokeBounds> bounds;
    for (const auto &stroke : strokes)
    {
        for (const auto &raw : stroke)
        {
            if (!usable_point(raw))
            {
                continue;
            }
            const HandwritingPoint point = clamp_point(raw, width, height);
            if (!bounds)
            {
                bounds = StrokeBounds{point.x, point.y, point.x, point.y};
                continue;
            }
            bounds->min_x = std::min(bounds->min_x, point.x);
            bounds->min_y = std::min(bounds->min_y, point.y);
            bounds->max_x = std::max(bounds->max_x, point.x);
            bounds->max_y = std::max(bounds->max_y, point.y);
        }
    }
    return bounds;
}

// Normalizing keeps the bitmap independent of the widget allocation and hands tesseract a cropped glyph.
std::vector<HandwritingStroke> normalize_strokes(const std::vector<HandwritingStroke> &strokes, int width, int height)
{
    const auto bounds = stroke_bounds(strokes, width, height);
    if (!bounds)
    {
        return {};
    }
    const double span_x = bounds->max_x - bounds->min_x;
    const double span_y = bounds->max_y - bounds->min_y;
    // A single dot has no span, so the one pixel floor avoids both a division by zero and a full bitmap blot.
    const double span = std::max({span_x, span_y, 1.0});
    const double scale = static_cast<double>(kRenderExtent - 2 * kRenderMargin) / span;
    const double center = static_cast<double>(kRenderExtent) / 2.0;
    const double offset_x = center - (bounds->min_x + span_x / 2.0) * scale;
    const double offset_y = center - (bounds->min_y + span_y / 2.0) * scale;
    std::vector<HandwritingStroke> normalized;
    normalized.reserve(strokes.size());
    for (const auto &stroke : strokes)
    {
        HandwritingStroke mapped;
        mapped.reserve(stroke.size());
        for (const auto &raw : stroke)
        {
            if (!usable_point(raw))
            {
                continue;
            }
            const HandwritingPoint point = clamp_point(raw, width, height);
            mapped.push_back({point.x * scale + offset_x, point.y * scale + offset_y});
        }
        if (!mapped.empty())
        {
            normalized.push_back(std::move(mapped));
        }
    }
    return normalized;
}

std::optional<std::filesystem::path> temporary_path()
{
    std::string pattern = (std::filesystem::temp_directory_path() / "metasequoia-ime-handwriting-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    const int descriptor = ::mkstemp(writable.data());
    if (descriptor < 0)
    {
        return std::nullopt;
    }
    ::close(descriptor);
    return std::filesystem::path(writable.data());
}

std::vector<std::uint8_t> render_pgm(const std::vector<HandwritingStroke> &strokes)
{
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kRenderExtent) * static_cast<std::size_t>(kRenderExtent),
                                     255);
    for (const auto &stroke : strokes)
    {
        if (stroke.empty())
        {
            continue;
        }
        if (stroke.size() == 1)
        {
            draw_disk(pixels, kRenderExtent, kRenderExtent, static_cast<int>(std::lround(stroke.front().x)),
                      static_cast<int>(std::lround(stroke.front().y)));
            continue;
        }
        for (std::size_t index = 1; index < stroke.size(); ++index)
        {
            draw_line(pixels, kRenderExtent, kRenderExtent, stroke[index - 1], stroke[index]);
        }
    }
    return pixels;
}

int remaining_milliseconds(std::chrono::steady_clock::time_point deadline)
{
    const long long remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0)
    {
        return 0;
    }
    if (remaining > static_cast<long long>(std::numeric_limits<int>::max()))
    {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(remaining);
}

ChildState reap_child(pid_t child, std::chrono::steady_clock::time_point deadline, int &status)
{
    for (;;)
    {
        const pid_t reaped = ::waitpid(child, &status, WNOHANG);
        if (reaped == child)
        {
            return ChildState::Exited;
        }
        if (reaped < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return ChildState::Lost;
        }
        if (remaining_milliseconds(deadline) <= 0)
        {
            return ChildState::Running;
        }
        // Nothing is left to wait on after stdout closes, and a GTK process must keep SIGCHLD for itself.
        ::poll(nullptr, 0, kChildPollIntervalMilliseconds);
    }
}

void terminate_child(pid_t child)
{
    // The child leads its own group, so this also reaches the children of a `tesseract` wrapper script.
    ::kill(-child, SIGKILL);
    ::kill(child, SIGKILL);
    int status = 0;
    // A child wedged in uninterruptible I/O outlives SIGKILL, so abandon it rather than pin the caller.
    reap_child(child, std::chrono::steady_clock::now() + kKillGrace, status);
}

BackendOutcome run_tesseract(const std::filesystem::path &input, std::chrono::milliseconds timeout, std::string &output)
{
    int pipe_fds[2] = {-1, -1};
    if (::pipe(pipe_fds) != 0)
    {
        return BackendOutcome::SpawnFailed;
    }
    const pid_t child = ::fork();
    if (child < 0)
    {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        return BackendOutcome::SpawnFailed;
    }
    if (child == 0)
    {
        ::setpgid(0, 0);
        ::close(pipe_fds[0]);
        ::dup2(pipe_fds[1], STDOUT_FILENO);
        ::close(pipe_fds[1]);
        ::execlp("tesseract", "tesseract", input.c_str(), "stdout", "--psm", "10", "-l", "chi_sim+eng",
                 static_cast<char *>(nullptr));
        _exit(127);
    }
    ::close(pipe_fds[1]);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    output.clear();
    bool expired = false;
    char buffer[1024];
    while (output.size() < kMaximumOutputBytes)
    {
        const int remaining = remaining_milliseconds(deadline);
        if (remaining <= 0)
        {
            expired = true;
            break;
        }
        struct pollfd descriptor = {};
        descriptor.fd = pipe_fds[0];
        descriptor.events = POLLIN;
        const int ready = ::poll(&descriptor, 1, remaining);
        if (ready < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }
        if (ready == 0)
        {
            expired = true;
            break;
        }
        const ssize_t count = ::read(pipe_fds[0], buffer, sizeof(buffer));
        if (count > 0)
        {
            output.append(buffer, static_cast<std::size_t>(count));
            continue;
        }
        if (count < 0 && (errno == EINTR || errno == EAGAIN))
        {
            continue;
        }
        break;
    }
    ::close(pipe_fds[0]);
    int status = 0;
    if (!expired)
    {
        const ChildState state = reap_child(child, deadline, status);
        if (state == ChildState::Lost)
        {
            return BackendOutcome::Failed;
        }
        expired = state == ChildState::Running;
    }
    if (expired)
    {
        terminate_child(child);
        return BackendOutcome::TimedOut;
    }
    if (!WIFEXITED(status))
    {
        return BackendOutcome::Failed;
    }
    if (WEXITSTATUS(status) == 0)
    {
        return BackendOutcome::Ok;
    }
    return WEXITSTATUS(status) == 127 ? BackendOutcome::Missing : BackendOutcome::Failed;
}

HandwritingResult make_result(HandwritingStatus status, const char *message)
{
    HandwritingResult result;
    result.status = status;
    result.message = message;
    return result;
}

HandwritingResult recognize_strokes(const std::vector<HandwritingStroke> &strokes, int width, int height,
                                    std::chrono::milliseconds timeout)
{
    if (strokes.empty())
    {
        return make_result(HandwritingStatus::NoStrokes, "Draw at least one handwriting stroke.");
    }
    if (width < kMinimumCanvasDimension || height < kMinimumCanvasDimension || width > kMaximumCanvasDimension ||
        height > kMaximumCanvasDimension)
    {
        return make_result(HandwritingStatus::InvalidCanvas, "Handwriting canvas dimensions were invalid.");
    }
    const auto normalized = normalize_strokes(strokes, width, height);
    if (normalized.empty())
    {
        return make_result(HandwritingStatus::NoStrokes, "Draw at least one handwriting stroke.");
    }
    const auto input = temporary_path();
    if (!input)
    {
        return make_result(HandwritingStatus::TemporaryFileFailed, "Unable to create a temporary handwriting image.");
    }
    const std::filesystem::path input_path = *input;
    std::error_code ignored;
    const auto pixels = render_pgm(normalized);
    {
        std::ofstream file(input_path, std::ios::binary | std::ios::trunc);
        file << "P5\n" << kRenderExtent << ' ' << kRenderExtent << "\n255\n";
        file.write(reinterpret_cast<const char *>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
        if (!file)
        {
            std::filesystem::remove(input_path, ignored);
            return make_result(HandwritingStatus::TemporaryFileFailed,
                               "Unable to write the temporary handwriting image.");
        }
    }
    std::string output;
    const auto bounded_timeout = timeout > std::chrono::milliseconds::zero() ? timeout : kHandwritingRecognitionTimeout;
    const BackendOutcome outcome = run_tesseract(input_path, bounded_timeout, output);
    std::filesystem::remove(input_path, ignored);
    switch (outcome)
    {
    case BackendOutcome::Missing:
        return make_result(
            HandwritingStatus::BackendMissing,
            "Tesseract handwriting backend is unavailable; install tesseract-ocr and tesseract-ocr-chi-sim.");
    case BackendOutcome::TimedOut:
        return make_result(HandwritingStatus::BackendTimedOut,
                           "Tesseract handwriting recognition did not answer in time and was stopped.");
    case BackendOutcome::SpawnFailed:
    case BackendOutcome::Failed:
        return make_result(HandwritingStatus::BackendFailed, "Tesseract handwriting recognition failed.");
    case BackendOutcome::Ok:
        break;
    }
    HandwritingResult result;
    result.candidates = HandwritingRecognizer::parse_candidates(output);
    if (result.candidates.empty())
    {
        result.status = HandwritingStatus::NoCandidates;
        result.message = "The handwriting backend returned no candidates.";
        return result;
    }
    result.status = HandwritingStatus::Ok;
    return result;
}
} // namespace

std::vector<std::string> HandwritingRecognizer::parse_candidates(std::string_view output)
{
    if (!g_utf8_validate(output.data(), static_cast<gssize>(output.size()), nullptr))
    {
        return {};
    }
    std::vector<std::string> candidates;
    std::size_t offset = 0;
    while (offset < output.size() && candidates.size() < kMaxCandidates)
    {
        while (offset < output.size() && g_ascii_isspace(static_cast<guchar>(output[offset])))
        {
            ++offset;
        }
        const std::size_t start = offset;
        while (offset < output.size() && !g_ascii_isspace(static_cast<guchar>(output[offset])))
        {
            ++offset;
        }
        if (start == offset)
        {
            continue;
        }
        const std::string candidate = trim(output.substr(start, offset - start));
        if (!candidate.empty() && std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
        {
            candidates.push_back(candidate);
        }
    }
    return candidates;
}

HandwritingResult HandwritingRecognizer::recognize(const HandwritingRequest &request) const
{
    return recognize_strokes(request.strokes, request.width, request.height, request.timeout);
}

std::vector<std::string> HandwritingRecognizer::recognize(const std::vector<HandwritingStroke> &strokes, int width,
                                                          int height, std::string *error) const
{
    HandwritingResult result = recognize_strokes(strokes, width, height, kHandwritingRecognitionTimeout);
    if (error != nullptr)
    {
        *error = result.status == HandwritingStatus::Ok ? std::string() : result.message;
    }
    return std::move(result.candidates);
}
} // namespace metasequoia::linux_ime
