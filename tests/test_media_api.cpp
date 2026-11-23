#include "mediaOptimizer/media_optimizer.hpp"

#include <iostream>

int main() {
    mediaopt::Config config;
    config.video.crf = 0;

    const auto result = mediaopt::optimize(
        "/home/avo/Downloads/WhatsApp Video 2026-10-29 at 17.51.59.mp4",
        "/home/avo/Documents/mediaOptimizer/build/api_video_output.mp4",
        config
    );

    std::cout << "Success: " << result.success() << '\n';
    std::cout << "Optimized: " << result.was_optimized() << '\n';
    std::cout << "Input bytes: " << result.input_size << '\n';
    std::cout << "Output bytes: " << result.output_size << '\n';
    std::cout << "Reason: " << result.reason << '\n';
    std::cout << "Error: " << result.error << '\n';

    return result.success() ? 0 : 1;
}
