#include "../image_internal.hpp"

#include <png.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace mediaopt::web::detail {

namespace {

bool png_structure_valid(ImageView input) noexcept {
    static constexpr std::uint8_t signature[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A
    };

    if (!input.valid() || input.size < sizeof(signature))
        return false;

    for (std::size_t i = 0; i < sizeof(signature); ++i) {
        if (input.data[i] != signature[i])
            return false;
    }

    std::size_t position = sizeof(signature);
    bool first_chunk = true;

    while (position <= input.size &&
           input.size - position >= 12) {
        const std::uint32_t length =
            (static_cast<std::uint32_t>(input.data[position]) << 24) |
            (static_cast<std::uint32_t>(input.data[position + 1]) << 16) |
            (static_cast<std::uint32_t>(input.data[position + 2]) << 8) |
            static_cast<std::uint32_t>(input.data[position + 3]);

        if (length > 0x7FFFFFFFU)
            return false;

        const std::uint8_t* type = input.data + position + 4;

        if (static_cast<std::uint64_t>(length) >
            input.size - position - 12)
            return false;

        if (first_chunk) {
            if (type[0] != 'I' || type[1] != 'H' ||
                type[2] != 'D' || type[3] != 'R' ||
                length != 13)
                return false;

            first_chunk = false;
        } else if (type[0] == 'I' && type[1] == 'H' &&
                   type[2] == 'D' && type[3] == 'R') {
            return false;
        }

        position += static_cast<std::size_t>(length) + 12;

        if (type[0] == 'I' && type[1] == 'E' &&
            type[2] == 'N' && type[3] == 'D') {
            return length == 0 && position == input.size;
        }
    }

    return false;
}

}

bool decode_png(
    ImageView input,
    std::uint64_t max_pixels,
    DecodedImage& output,
    std::string& error
) {
    if (!input.valid()) {
        error = "Invalid PNG input buffer.";
        return false;
    }

    if (!png_structure_valid(input)) {
        error = "Invalid or incomplete PNG structure.";
        return false;
    }

    png_image image{};
    image.version = PNG_IMAGE_VERSION;

    if (!png_image_begin_read_from_memory(
            &image,
            input.data,
            input.size)) {
        error = image.message[0] != '\0'
            ? image.message
            : "Invalid PNG image.";

        png_image_free(&image);
        return false;
    }

    const std::uint64_t width = image.width;
    const std::uint64_t height = image.height;

    if (width == 0 || height == 0 ||
        max_pixels == 0 ||
        width > max_pixels / height) {
        png_image_free(&image);
        error = "PNG dimensions exceed the configured pixel limit.";
        return false;
    }

    const std::uint64_t pixel_count = width * height;

    if (pixel_count >
        static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max() / 4)) {
        png_image_free(&image);
        error = "PNG image is too large for this platform.";
        return false;
    }

    const std::size_t expected_size =
        static_cast<std::size_t>(pixel_count) * 4;

    image.format = PNG_FORMAT_RGBA;

    const png_alloc_size_t png_size = PNG_IMAGE_SIZE(image);

    if (png_size != expected_size) {
        png_image_free(&image);
        error = "Unexpected PNG output buffer size.";
        return false;
    }

    std::vector<std::uint8_t> pixels;

    try {
        pixels.resize(expected_size);
    } catch (...) {
        png_image_free(&image);
        error = "Not enough memory to decode PNG.";
        return false;
    }

    if (!png_image_finish_read(
            &image,
            nullptr,
            pixels.data(),
            0,
            nullptr)) {
        error = image.message[0] != '\0'
            ? image.message
            : "PNG pixel decoding failed.";

        png_image_free(&image);
        return false;
    }

    png_image_free(&image);

    output.width = static_cast<std::uint32_t>(width);
    output.height = static_cast<std::uint32_t>(height);
    output.pixels = std::move(pixels);

    error.clear();
    return true;
}

}
