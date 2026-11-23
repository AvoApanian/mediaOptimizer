#include "mediaOptimizer/video_optimizer.hpp"

namespace mediaopt {

Result optimize_video(
    const std::filesystem::path& input,
    const std::filesystem::path& output,
    const VideoConfig&
) {
    Result result;
    result.media_type = MediaType::Video;
    result.input_path = input;
    result.output_path = output;
    result.error = "Android video backend is not implemented yet.";
    return result;
}

}
