#include "../image_internal.hpp"

#include <webp/decode.h>
#include <webp/encode.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace mediaopt::web::detail {

bool decode_webp(
    ImageView input,
    std::uint64_t max_pixels,
    DecodedImage& output,
    std::string& error
) {
    if (!input.valid()) {
        error = "Invalid WebP input buffer.";
        return false;
    }

    int width = 0;
    int height = 0;

    if (!WebPGetInfo(input.data, input.size, &width, &height)) {
        error = "Invalid WebP image.";
        return false;
    }

    if (width <= 0 || height <= 0 ||
        max_pixels == 0 ||
        static_cast<std::uint64_t>(width) >
            max_pixels / static_cast<std::uint64_t>(height)) {
        error = "WebP dimensions exceed the configured pixel limit.";
        return false;
    }

    if (width > std::numeric_limits<int>::max() / 4) {
        error = "WebP image stride exceeds the platform limit.";
        return false;
    }

    const std::uint64_t pixel_count =
        static_cast<std::uint64_t>(width) *
        static_cast<std::uint64_t>(height);

    if (pixel_count >
        static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max() / 4)) {
        error = "WebP image is too large for this platform.";
        return false;
    }

    const std::size_t expected_size =
        static_cast<std::size_t>(pixel_count) * 4;

    const int stride = width * 4;

    std::vector<std::uint8_t> pixels;

    try {
        pixels.resize(expected_size);
    } catch (...) {
        error = "Not enough memory to decode WebP.";
        return false;
    }

    if (WebPDecodeRGBAInto(
            input.data,
            input.size,
            pixels.data(),
            pixels.size(),
            stride) == nullptr) {
        error = "WebP pixel decoding failed.";
        return false;
    }

    output.width = static_cast<std::uint32_t>(width);
    output.height = static_cast<std::uint32_t>(height);
    output.pixels = std::move(pixels);

    error.clear();
    return true;
}

bool encode_webp(
    const DecodedImage& input,
    int quality,
    std::vector<std::uint8_t>& output,
    std::string& error
) {
    if (input.width == 0 || input.height == 0) {
        error = "Invalid image dimensions for WebP encoding.";
        return false;
    }

    if (quality < 0 || quality > 100) {
        error = "WebP quality must be between 0 and 100.";
        return false;
    }

    if (input.width >
        static_cast<std::uint32_t>(
            std::numeric_limits<int>::max() / 4)) {
        error = "WebP image stride exceeds the platform limit.";
        return false;
    }

    const std::uint64_t max_size =
        static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max());

    if (static_cast<std::uint64_t>(input.width) >
        max_size / 4 / input.height) {
        error = "Image is too large for this platform.";
        return false;
    }

    const std::size_t expected_size =
        static_cast<std::size_t>(input.width) *
        static_cast<std::size_t>(input.height) * 4;

    if (input.pixels.size() != expected_size) {
        error = "Invalid RGBA pixel buffer size.";
        return false;
    }

    std::uint8_t* raw_output = nullptr;

    const std::size_t encoded_size = WebPEncodeRGBA(
        input.pixels.data(),
        static_cast<int>(input.width),
        static_cast<int>(input.height),
        static_cast<int>(input.width * 4),
        static_cast<float>(quality),
        &raw_output
    );

    std::unique_ptr<std::uint8_t, decltype(&WebPFree)> encoded(
        raw_output,
        &WebPFree
    );

    if (encoded_size == 0 || !encoded) {
        error = "WebP encoding failed.";
        return false;
    }

    std::vector<std::uint8_t> result;

    try {
        result.assign(
            encoded.get(),
            encoded.get() + encoded_size
        );
    } catch (...) {
        error = "Not enough memory to store encoded WebP.";
        return false;
    }

    output.swap(result);

    error.clear();
    return true;
}

}
