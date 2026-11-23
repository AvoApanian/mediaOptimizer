#include "mediaOptimizer/image_optimizer.hpp"

#include <iostream>

int main() {
    mediaopt::ImageConfig config;

    const auto result = mediaopt::optimize_image(
        "/home/avo/Pictures/black.jpg",
        "/home/avo/Documents/mediaOptimizer/build/test_output.jpg",
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
