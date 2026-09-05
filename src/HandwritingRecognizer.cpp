#include "HandwritingRecognizer.h"

#include <glib.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
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
constexpr int kMaximumCanvasDimension = 2048;
constexpr int kStrokeRadius = 3;

void set_error(std::string *error, const char *message)
{
    if (error != nullptr)
    {
        *error = message;
    }
}

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

std::vector<std::uint8_t> render_pgm(const std::vector<HandwritingStroke> &strokes, int width, int height)
{
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 255);
    for (const auto &stroke : strokes)
    {
        if (stroke.empty())
        {
            continue;
        }
        if (stroke.size() == 1)
        {
            draw_disk(pixels, width, height, static_cast<int>(std::lround(stroke.front().x)),
                      static_cast<int>(std::lround(stroke.front().y)));
            continue;
        }
        for (std::size_t index = 1; index < stroke.size(); ++index)
        {
            draw_line(pixels, width, height, stroke[index - 1], stroke[index]);
        }
    }
    return pixels;
}

bool run_tesseract(const std::filesystem::path &input, std::string &output, int *exit_code)
{
    int pipe_fds[2] = {-1, -1};
    if (::pipe(pipe_fds) != 0)
    {
        return false;
    }
    const pid_t child = ::fork();
    if (child < 0)
    {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        return false;
    }
    if (child == 0)
    {
        ::close(pipe_fds[0]);
        ::dup2(pipe_fds[1], STDOUT_FILENO);
        ::close(pipe_fds[1]);
        ::execlp("tesseract", "tesseract", input.c_str(), "stdout", "--psm", "10", "-l", "chi_sim+eng",
                 static_cast<char *>(nullptr));
        _exit(127);
    }
    ::close(pipe_fds[1]);
    output.clear();
    char buffer[1024];
    while (output.size() < 64 * 1024)
    {
        const ssize_t count = ::read(pipe_fds[0], buffer, sizeof(buffer));
        if (count > 0)
        {
            output.append(buffer, static_cast<std::size_t>(count));
            continue;
        }
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        break;
    }
    ::close(pipe_fds[0]);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR)
    {
    }
    if (exit_code != nullptr)
    {
        *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
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

std::vector<std::string> HandwritingRecognizer::recognize(const std::vector<HandwritingStroke> &strokes, int width,
                                                          int height, std::string *error) const
{
    if (error != nullptr)
    {
        error->clear();
    }
    if (strokes.empty())
    {
        set_error(error, "Draw at least one handwriting stroke.");
        return {};
    }
    if (width < kMinimumCanvasDimension || height < kMinimumCanvasDimension || width > kMaximumCanvasDimension ||
        height > kMaximumCanvasDimension)
    {
        set_error(error, "Handwriting canvas dimensions were invalid.");
        return {};
    }
    const auto input = temporary_path();
    if (!input)
    {
        set_error(error, "Unable to create a temporary handwriting image.");
        return {};
    }
    const std::filesystem::path input_path = *input;
    std::error_code ignored;
    const auto pixels = render_pgm(strokes, width, height);
    {
        std::ofstream file(input_path, std::ios::binary | std::ios::trunc);
        file << "P5\n" << width << ' ' << height << "\n255\n";
        file.write(reinterpret_cast<const char *>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
        if (!file)
        {
            std::filesystem::remove(input_path, ignored);
            set_error(error, "Unable to write the temporary handwriting image.");
            return {};
        }
    }
    std::string output;
    int exit_code = -1;
    const bool succeeded = run_tesseract(input_path, output, &exit_code);
    std::filesystem::remove(input_path, ignored);
    if (!succeeded)
    {
        set_error(error,
                  exit_code == 127
                      ? "Tesseract handwriting backend is unavailable; install tesseract-ocr and tesseract-ocr-chi-sim."
                      : "Tesseract handwriting recognition failed.");
        return {};
    }
    const auto candidates = parse_candidates(output);
    if (candidates.empty())
    {
        set_error(error, "The handwriting backend returned no candidates.");
    }
    return candidates;
}
} // namespace metasequoia::linux_ime
