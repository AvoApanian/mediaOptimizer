#include "mediaOptimizer/audio_optimizer.hpp"

namespace mediaopt {

Result optimize_audio(
    const std::filesystem::path& input,
    const std::filesystem::path& output,
    const AudioConfig&
) {
    Result result;
    result.media_type = MediaType::Audio;
    result.input_path = input;
    result.output_path = output;
    result.error = "Android audio backend is not implemented yet.";
    return result;
}

}
