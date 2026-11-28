#pragma once

#include "mediaOptimizer/web_image_optimizer.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace mediaopt::web::detail {

struct DecodedImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;

    bool valid() const noexcept {
        const std::uint64_t expected =
            static_cast<std::uint64_t>(width) * height * 4;

        return width > 0 &&
               height > 0 &&
               expected == pixels.size();
    }
};

bool decode_jpeg(
    ImageView input,
    std::uint64_t max_pixels,
    DecodedImage& output,
    std::string& error
);

bool decode_png(
    ImageView input,
    std::uint64_t max_pixels,
    DecodedImage& output,
    std::string& error
);

bool decode_webp(
    ImageView input,
    std::uint64_t max_pixels,
    DecodedImage& output,
    std::string& error
);

bool encode_webp(
    const DecodedImage& input,
    int quality,
    std::vector<std::uint8_t>& output,
    std::string& error
);

}
