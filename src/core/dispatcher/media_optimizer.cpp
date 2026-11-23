#include "mediaOptimizer/media_optimizer.hpp"
#include "mediaOptimizer/image_optimizer.hpp"
#include "mediaOptimizer/video_optimizer.hpp"
#include "mediaOptimizer/audio_optimizer.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>

namespace mediaopt {
namespace {

std::string lowercase_extension(const std::filesystem::path& path) {
    std::string extension = path.extension().string();

    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        }
    );

    return extension;
}

}

Result optimize(
    const std::filesystem::path& input,
    const std::filesystem::path& output,
    const Config& config
) {
    static const std::unordered_set<std::string> image_extensions = {
        ".jpg", ".jpeg", ".png", ".webp"
    };

    static const std::unordered_set<std::string> video_extensions = {
        ".mp4", ".m4v", ".mov", ".mkv", ".avi", ".webm", ".3gp"
    };

    static const std::unordered_set<std::string> audio_extensions = {
        ".m4a", ".aac", ".mp3", ".wav", ".flac",
        ".ogg", ".opus", ".oga", ".wma", ".aiff", ".aif", ".amr"
    };

    const std::string extension = lowercase_extension(input);

    if (image_extensions.find(extension) != image_extensions.end()) {
        return optimize_image(input, output, config.image);
    }

    if (video_extensions.find(extension) != video_extensions.end()) {
        return optimize_video(input, output, config.video);
    }

    if (audio_extensions.find(extension) != audio_extensions.end()) {
        return optimize_audio(input, output, config.audio);
    }

    Result result;
    result.input_path = input;
    result.output_path = output;
    result.error = "Format de média non pris en charge : " + extension;
    return result;
}

}
