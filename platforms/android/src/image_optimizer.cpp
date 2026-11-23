#include "mediaOptimizer/image_optimizer.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include <fcntl.h>
#include <setjmp.h>
#include <sys/stat.h>
#include <unistd.h>

#include <jpeglib.h>
#include <png.h>

#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/encode.h>

namespace mediaopt {
namespace {

constexpr std::uintmax_t MAX_INPUT_BYTES = 128ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t MAX_PIXELS = 100ULL * 1000ULL * 1000ULL;

enum class Format {
    JPEG,
    PNG,
    WEBP,
    Unsupported
};

struct Pixels {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

struct JpegError {
    jpeg_error_mgr base;
    jmp_buf jump;
};

void jpeg_error_exit(j_common_ptr info) {
    auto* error = reinterpret_cast<JpegError*>(info->err);
    longjmp(error->jump, 1);
}

bool dimensions_valid(int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }

    const auto pixels =
        static_cast<std::uint64_t>(width) *
        static_cast<std::uint64_t>(height);

    return pixels <= MAX_PIXELS;
}

bool read_file(
    const std::filesystem::path& path,
    std::vector<std::uint8_t>& data,
    std::string& error
) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);

    if (ec || size == 0 || size > MAX_INPUT_BYTES) {
        error = "Input file is empty, inaccessible or exceeds 128 MiB.";
        return false;
    }

    std::ifstream file(path, std::ios::binary);

    if (!file) {
        error = "Unable to open input file.";
        return false;
    }

    try {
        data.resize(static_cast<std::size_t>(size));
    } catch (...) {
        error = "Not enough memory to read input file.";
        return false;
    }

    file.read(
        reinterpret_cast<char*>(data.data()),
        static_cast<std::streamsize>(data.size())
    );

    if (!file || static_cast<std::size_t>(file.gcount()) != data.size()) {
        error = "Unable to read the complete input file.";
        return false;
    }

    return true;
}

Format detect_format(const std::vector<std::uint8_t>& data) {
    static constexpr std::array<std::uint8_t, 8> png_signature = {
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A
    };

    if (data.size() >= png_signature.size() &&
        std::equal(
            png_signature.begin(),
            png_signature.end(),
            data.begin()
        )) {
        return Format::PNG;
    }

    if (data.size() >= 3 &&
        data[0] == 0xFF &&
        data[1] == 0xD8 &&
        data[2] == 0xFF) {
        return Format::JPEG;
    }

    if (data.size() >= 12 &&
        std::memcmp(data.data(), "RIFF", 4) == 0 &&
        std::memcmp(data.data() + 8, "WEBP", 4) == 0) {
        return Format::WEBP;
    }

    return Format::Unsupported;
}

bool jpeg_metadata_present(const std::vector<std::uint8_t>& data) {
    std::size_t pos = 2;

    while (pos + 1 < data.size()) {
        if (data[pos] != 0xFF) {
            return true;
        }

        while (pos < data.size() && data[pos] == 0xFF) {
            ++pos;
        }

        if (pos >= data.size()) {
            return true;
        }

        const std::uint8_t marker = data[pos++];

        if (marker == 0xD9 || marker == 0xDA) {
            return false;
        }

        if (marker == 0xD8 ||
            marker == 0x01 ||
            (marker >= 0xD0 && marker <= 0xD7)) {
            continue;
        }

        if (pos + 2 > data.size()) {
            return true;
        }

        const std::size_t length =
            (static_cast<std::size_t>(data[pos]) << 8) |
            data[pos + 1];

        if (length < 2 || length > data.size() - pos) {
            return true;
        }

        // Conservatively preserve APP1-APP15 and COM markers.
        // APP0 (JFIF) is allowed.
        if ((marker >= 0xE1 && marker <= 0xEF) ||
            marker == 0xFE) {
            return true;
        }

        pos += length;
    }

    return true;
}

bool png_metadata_or_animation_present(
    const std::vector<std::uint8_t>& data,
    bool& is_16_bit
) {
    is_16_bit = false;

    if (data.size() < 33) {
        return true;
    }

    is_16_bit = data[24] == 16;

    std::size_t pos = 8;

    while (pos + 12 <= data.size()) {
        const std::uint32_t length =
            (static_cast<std::uint32_t>(data[pos]) << 24) |
            (static_cast<std::uint32_t>(data[pos + 1]) << 16) |
            (static_cast<std::uint32_t>(data[pos + 2]) << 8) |
            static_cast<std::uint32_t>(data[pos + 3]);

        if (length > data.size() - pos - 12) {
            return true;
        }

        const char* type =
            reinterpret_cast<const char*>(data.data() + pos + 4);

        const auto is_chunk = [type](const char* name) {
            return std::memcmp(type, name, 4) == 0;
        };

        if (
            is_chunk("acTL") ||
            is_chunk("iCCP") ||
            is_chunk("eXIf") ||
            is_chunk("sRGB") ||
            is_chunk("gAMA") ||
            is_chunk("cHRM") ||
            is_chunk("cICP") ||
            is_chunk("mDCV") ||
            is_chunk("cLLI") ||
            is_chunk("tEXt") ||
            is_chunk("zTXt") ||
            is_chunk("iTXt")
        ) {
            return true;
        }

        pos += static_cast<std::size_t>(length) + 12;

        if (is_chunk("IEND")) {
            return false;
        }
    }

    return true;
}


struct JpegDecodeState {
    jpeg_decompress_struct decoder{};
    JpegError error{};
    unsigned char* rgba = nullptr;
    unsigned char* row = nullptr;
    bool created = false;
};

void cleanup_jpeg_state(JpegDecodeState* state) {
    if (!state) {
        return;
    }

    if (state->created) {
        jpeg_destroy_decompress(&state->decoder);
        state->created = false;
    }

    std::free(state->row);
    std::free(state->rgba);
    delete state;
}

bool decode_jpeg_raw(
    const std::vector<std::uint8_t>& data,
    unsigned char** output,
    int* output_width,
    int* output_height
) {
    *output = nullptr;
    *output_width = 0;
    *output_height = 0;

    auto* state = new (std::nothrow) JpegDecodeState{};

    if (!state) {
        return false;
    }

    state->decoder.err = jpeg_std_error(&state->error.base);
    state->error.base.error_exit = jpeg_error_exit;

    if (setjmp(state->error.jump)) {
        cleanup_jpeg_state(state);
        return false;
    }

    state->created = true;
    jpeg_create_decompress(&state->decoder);

    jpeg_mem_src(
        &state->decoder,
        data.data(),
        static_cast<unsigned long>(data.size())
    );

    if (jpeg_read_header(&state->decoder, TRUE) != JPEG_HEADER_OK) {
        cleanup_jpeg_state(state);
        return false;
    }

    if (state->decoder.jpeg_color_space != JCS_RGB &&
        state->decoder.jpeg_color_space != JCS_YCbCr) {
        cleanup_jpeg_state(state);
        return false;
    }

    state->decoder.out_color_space = JCS_RGB;

    if (!jpeg_start_decompress(&state->decoder)) {
        cleanup_jpeg_state(state);
        return false;
    }

    if (state->decoder.output_width > static_cast<JDIMENSION>(INT_MAX) ||
        state->decoder.output_height > static_cast<JDIMENSION>(INT_MAX)) {
        cleanup_jpeg_state(state);
        return false;
    }

    const int width = static_cast<int>(state->decoder.output_width);
    const int height = static_cast<int>(state->decoder.output_height);

    if (!dimensions_valid(width, height) ||
        state->decoder.output_components != 3) {
        cleanup_jpeg_state(state);
        return false;
    }

    const std::size_t w = static_cast<std::size_t>(width);
    const std::size_t h = static_cast<std::size_t>(height);

    if (w > std::numeric_limits<std::size_t>::max() / 4 ||
        h > std::numeric_limits<std::size_t>::max() / (w * 4) ||
        w > std::numeric_limits<std::size_t>::max() / 3) {
        cleanup_jpeg_state(state);
        return false;
    }

    const std::size_t row_size = w * 3;
    const std::size_t rgba_size = w * h * 4;

    state->row = static_cast<unsigned char*>(std::malloc(row_size));
    state->rgba = static_cast<unsigned char*>(std::malloc(rgba_size));

    if (!state->row || !state->rgba) {
        cleanup_jpeg_state(state);
        return false;
    }

    while (state->decoder.output_scanline < state->decoder.output_height) {
        JSAMPROW row_pointer = state->row;

        if (jpeg_read_scanlines(&state->decoder, &row_pointer, 1) != 1) {
            cleanup_jpeg_state(state);
            return false;
        }

        const std::size_t y =
            static_cast<std::size_t>(state->decoder.output_scanline - 1);

        for (int x = 0; x < width; ++x) {
            const std::size_t src = static_cast<std::size_t>(x) * 3;
            const std::size_t dst = (y * w + static_cast<std::size_t>(x)) * 4;

            state->rgba[dst] = state->row[src];
            state->rgba[dst + 1] = state->row[src + 1];
            state->rgba[dst + 2] = state->row[src + 2];
            state->rgba[dst + 3] = 255;
        }
    }

    if (!jpeg_finish_decompress(&state->decoder)) {
        cleanup_jpeg_state(state);
        return false;
    }

    jpeg_destroy_decompress(&state->decoder);
    state->created = false;

    std::free(state->row);
    state->row = nullptr;

    *output = state->rgba;
    *output_width = width;
    *output_height = height;

    state->rgba = nullptr;
    delete state;

    return true;
}

bool decode_jpeg(
    const std::vector<std::uint8_t>& data,
    Pixels& pixels,
    std::string& reason
) {
    unsigned char* decoded = nullptr;
    int width = 0;
    int height = 0;

    if (!decode_jpeg_raw(data, &decoded, &width, &height)) {
        reason = "JPEG decoding failed or its format is unsupported.";
        return false;
    }

    const std::size_t decoded_size =
        static_cast<std::size_t>(width) *
        static_cast<std::size_t>(height) * 4;

    try {
        pixels.rgba.assign(decoded, decoded + decoded_size);
    } catch (...) {
        std::free(decoded);
        reason = "Not enough memory to decode JPEG.";
        return false;
    }

    std::free(decoded);

    pixels.width = width;
    pixels.height = height;

    return true;
}

bool decode_png(
    const std::vector<std::uint8_t>& data,
    Pixels& pixels,
    std::string& reason
) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;

    if (!png_image_begin_read_from_memory(
            &image,
            data.data(),
            data.size())) {
        reason = "Invalid PNG image.";
        return false;
    }

    if (!dimensions_valid(
            static_cast<int>(image.width),
            static_cast<int>(image.height))) {
        png_image_free(&image);
        reason = "PNG dimensions are too large.";
        return false;
    }

    image.format = PNG_FORMAT_RGBA;

    try {
        pixels.width = static_cast<int>(image.width);
        pixels.height = static_cast<int>(image.height);
        pixels.rgba.resize(PNG_IMAGE_SIZE(image));
    } catch (...) {
        png_image_free(&image);
        reason = "Not enough memory to decode PNG.";
        return false;
    }

    if (!png_image_finish_read(
            &image,
            nullptr,
            pixels.rgba.data(),
            0,
            nullptr)) {
        png_image_free(&image);
        reason = "PNG pixel decoding failed.";
        return false;
    }

    png_image_free(&image);
    return true;
}

bool decode_webp(
    const std::vector<std::uint8_t>& data,
    Pixels& pixels,
    std::string& reason
) {
    WebPBitstreamFeatures features{};

    if (WebPGetFeatures(
            data.data(),
            data.size(),
            &features) != VP8_STATUS_OK) {
        reason = "Invalid WebP image.";
        return false;
    }

    if (features.has_animation) {
        reason = "Animated WebP is preserved without conversion.";
        return false;
    }

    if (!dimensions_valid(features.width, features.height)) {
        reason = "WebP dimensions are too large.";
        return false;
    }

    WebPData webp_data{data.data(), data.size()};
    WebPDemuxer* demux = WebPDemux(&webp_data);

    if (!demux) {
        reason = "Unable to inspect WebP metadata.";
        return false;
    }

    const bool has_metadata =
        WebPDemuxGetI(demux, WEBP_FF_FORMAT_FLAGS) &
        (ICCP_FLAG | EXIF_FLAG | XMP_FLAG);

    WebPDemuxDelete(demux);

    if (has_metadata) {
        reason = "WebP contains metadata that this backend does not preserve.";
        return false;
    }

    std::uint8_t* decoded = WebPDecodeRGBA(
        data.data(),
        data.size(),
        &pixels.width,
        &pixels.height
    );

    if (!decoded) {
        reason = "WebP pixel decoding failed.";
        return false;
    }

    try {
        const std::size_t size =
            static_cast<std::size_t>(pixels.width) *
            static_cast<std::size_t>(pixels.height) * 4;

        pixels.rgba.assign(decoded, decoded + size);
    } catch (...) {
        WebPFree(decoded);
        reason = "Not enough memory to decode WebP.";
        return false;
    }

    WebPFree(decoded);
    return true;
}

void resize_pixels(Pixels& pixels, int max_dim) {
    if (max_dim <= 0 ||
        (pixels.width <= max_dim && pixels.height <= max_dim)) {
        return;
    }

    const double scale = std::min(
        static_cast<double>(max_dim) / pixels.width,
        static_cast<double>(max_dim) / pixels.height
    );

    const int new_width = std::max(
        1,
        static_cast<int>(std::round(pixels.width * scale))
    );

    const int new_height = std::max(
        1,
        static_cast<int>(std::round(pixels.height * scale))
    );

    std::vector<std::uint8_t> resized(
        static_cast<std::size_t>(new_width) *
        static_cast<std::size_t>(new_height) * 4
    );

    for (int y = 0; y < new_height; ++y) {
        const int source_y = std::min(
            pixels.height - 1,
            static_cast<int>(
                static_cast<std::int64_t>(y) *
                pixels.height / new_height
            )
        );

        for (int x = 0; x < new_width; ++x) {
            const int source_x = std::min(
                pixels.width - 1,
                static_cast<int>(
                    static_cast<std::int64_t>(x) *
                    pixels.width / new_width
                )
            );

            const std::size_t src =
                (static_cast<std::size_t>(source_y) *
                 pixels.width + source_x) * 4;

            const std::size_t dst =
                (static_cast<std::size_t>(y) *
                 new_width + x) * 4;

            std::copy_n(
                pixels.rgba.data() + src,
                4,
                resized.data() + dst
            );
        }
    }

    pixels.width = new_width;
    pixels.height = new_height;
    pixels.rgba = std::move(resized);
}

double calculate_psnr(
    const std::vector<std::uint8_t>& original,
    const std::vector<std::uint8_t>& decoded
) {
    if (original.size() != decoded.size() || original.empty()) {
        return 0.0;
    }

    long double squared_error = 0.0;

    for (std::size_t i = 0; i < original.size(); ++i) {
        const long double difference =
            static_cast<int>(original[i]) -
            static_cast<int>(decoded[i]);

        squared_error += difference * difference;
    }

    const long double mse =
        squared_error / original.size();

    if (mse == 0.0) {
        return std::numeric_limits<double>::infinity();
    }

    return static_cast<double>(
        10.0L * std::log10((255.0L * 255.0L) / mse)
    );
}

bool encode_webp(
    const Pixels& pixels,
    int quality,
    std::vector<std::uint8_t>& output,
    std::string& reason
) {
    WebPConfig config{};

    if (!WebPConfigPreset(
            &config,
            WEBP_PRESET_DEFAULT,
            static_cast<float>(quality))) {
        reason = "Unable to initialize WebP encoder.";
        return false;
    }

    config.quality = static_cast<float>(quality);
    config.method = 6;

    if (!WebPValidateConfig(&config)) {
        reason = "Invalid WebP quality configuration.";
        return false;
    }

    WebPPicture picture{};

    if (!WebPPictureInit(&picture)) {
        reason = "Unable to initialize WebP picture.";
        return false;
    }

    picture.width = pixels.width;
    picture.height = pixels.height;

    WebPMemoryWriter writer;
    WebPMemoryWriterInit(&writer);

    picture.writer = WebPMemoryWrite;
    picture.custom_ptr = &writer;

    if (!WebPPictureImportRGBA(
            &picture,
            pixels.rgba.data(),
            pixels.width * 4)) {
        WebPMemoryWriterClear(&writer);
        WebPPictureFree(&picture);
        reason = "Unable to import RGBA pixels into WebP encoder.";
        return false;
    }

    const bool encoded = WebPEncode(&config, &picture) != 0;

    if (!encoded || !writer.mem || writer.size == 0) {
        WebPMemoryWriterClear(&writer);
        WebPPictureFree(&picture);
        reason = "WebP encoding failed.";
        return false;
    }

    try {
        output.assign(writer.mem, writer.mem + writer.size);
    } catch (...) {
        WebPMemoryWriterClear(&writer);
        WebPPictureFree(&picture);
        reason = "Not enough memory for WebP output.";
        return false;
    }

    WebPMemoryWriterClear(&writer);
    WebPPictureFree(&picture);

    return true;
}

bool write_atomic(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& data,
    std::string& error
) {
    static unsigned int counter = 0;

    const auto temporary = std::filesystem::path(
        path.string() + ".tmp-" +
        std::to_string(getpid()) + "-" +
        std::to_string(++counter)
    );

    const int fd = open(
        temporary.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
        S_IRUSR | S_IWUSR
    );

    if (fd < 0) {
        error = "Unable to create temporary output file.";
        return false;
    }

    bool write_ok = true;
    std::size_t written = 0;

    while (written < data.size()) {
        const ssize_t count = write(
            fd,
            data.data() + written,
            data.size() - written
        );

        if (count < 0 && errno == EINTR) {
            continue;
        }

        if (count <= 0) {
            write_ok = false;
            break;
        }

        written += static_cast<std::size_t>(count);
    }

    bool sync_ok = false;

    if (write_ok) {
        sync_ok = fsync(fd) == 0;
    }

    const bool close_ok = close(fd) == 0;

    if (!write_ok || !sync_ok || !close_ok) {
        unlink(temporary.c_str());
        error = "Unable to write or finalize temporary WebP file.";
        return false;
    }

    if (link(temporary.c_str(), path.c_str()) != 0) {
        const int saved_errno = errno;
        unlink(temporary.c_str());

        if (saved_errno == EEXIST) {
            error = "Output file already exists; refusing to overwrite it.";
        } else {
            error = "Unable to publish WebP output safely.";
        }

        return false;
    }

    if (unlink(temporary.c_str()) != 0) {
        error = "WebP output was published, but temporary file cleanup failed.";
        return false;
    }

    return true;
}

Result keep_original(
    const std::filesystem::path& input,
    std::uintmax_t input_size,
    const std::string& reason
) {
    Result result;
    result.status = Status::OriginalKept;
    result.media_type = MediaType::Image;
    result.input_path = input;
    result.output_path = input;
    result.input_size = input_size;
    result.output_size = input_size;
    result.reason = reason;
    return result;
}

} // namespace

Result optimize_image(
    const std::filesystem::path& input,
    const std::filesystem::path& output,
    const ImageConfig& config
) {
    std::error_code ec;
    const auto input_size = std::filesystem::file_size(input, ec);

    if (ec || input_size == 0 || input_size > MAX_INPUT_BYTES) {
        Result result;
        result.media_type = MediaType::Image;
        result.input_path = input;
        result.output_path = input;
        result.error = "Input file is inaccessible, empty or exceeds 128 MiB.";
        return result;
    }

    if (config.webp_quality < 0 || config.webp_quality > 100 ||
        config.max_dim < 0 ||
        !std::isfinite(config.min_psnr_db) ||
        config.min_psnr_db < 0.0) {
        Result result;
        result.media_type = MediaType::Image;
        result.input_path = input;
        result.output_path = input;
        result.error = "Invalid image optimization configuration.";
        return result;
    }

    std::vector<std::uint8_t> data;
    std::string error;

    if (!read_file(input, data, error)) {
        Result result;
        result.media_type = MediaType::Image;
        result.input_path = input;
        result.output_path = input;
        result.error = error;
        return result;
    }

    const Format format = detect_format(data);

    if (format == Format::Unsupported) {
        return keep_original(
            input,
            input_size,
            "Unsupported image format. Original preserved."
        );
    }

    bool is_16_bit = false;

    if (format == Format::JPEG &&
        jpeg_metadata_present(data)) {
        return keep_original(
            input,
            input_size,
            "JPEG contains metadata or markers that are not preserved."
        );
    }

    if (format == Format::PNG &&
        png_metadata_or_animation_present(data, is_16_bit)) {
        return keep_original(
            input,
            input_size,
            is_16_bit
                ? "16-bit PNG or PNG metadata/animation cannot be safely preserved."
                : "PNG metadata or animation cannot be safely preserved."
        );
    }

    Pixels pixels;

    bool decoded = false;

    switch (format) {
        case Format::JPEG:
            decoded = decode_jpeg(data, pixels, error);
            break;

        case Format::PNG:
            decoded = decode_png(data, pixels, error);
            break;

        case Format::WEBP:
            decoded = decode_webp(data, pixels, error);
            break;

        default:
            break;
    }

    if (!decoded) {
        return keep_original(
            input,
            input_size,
            error.empty() ? "Image could not be decoded safely." : error
        );
    }

    const int original_width = pixels.width;
    const int original_height = pixels.height;

    resize_pixels(pixels, config.max_dim);

    const bool was_resized =
        pixels.width != original_width ||
        pixels.height != original_height;

    const auto expected_width = pixels.width;
    const auto expected_height = pixels.height;
    const auto reference_pixels = pixels.rgba;

    std::vector<std::uint8_t> encoded;

    if (!encode_webp(
            pixels,
            config.webp_quality,
            encoded,
            error)) {
        return keep_original(input, input_size, error);
    }

    int decoded_width = 0;
    int decoded_height = 0;

    std::uint8_t* verification = WebPDecodeRGBA(
        encoded.data(),
        encoded.size(),
        &decoded_width,
        &decoded_height
    );

    if (!verification) {
        return keep_original(
            input,
            input_size,
            "Encoded WebP failed verification."
        );
    }

    if (decoded_width != expected_width ||
        decoded_height != expected_height) {
        WebPFree(verification);

        return keep_original(
            input,
            input_size,
            "Encoded WebP dimensions do not match the expected dimensions."
        );
    }

    const std::size_t verification_size =
        static_cast<std::size_t>(decoded_width) *
        static_cast<std::size_t>(decoded_height) * 4;

    std::vector<std::uint8_t> decoded_pixels;

    try {
        decoded_pixels.assign(
            verification,
            verification + verification_size
        );
    } catch (...) {
        WebPFree(verification);

        return keep_original(
            input,
            input_size,
            "Not enough memory to verify WebP output."
        );
    }

    WebPFree(verification);

    const double psnr = calculate_psnr(
        reference_pixels,
        decoded_pixels
    );

    if (psnr < config.min_psnr_db) {
        return keep_original(
            input,
            input_size,
            "WebP quality is below the configured minimum PSNR."
        );
    }

    if (encoded.size() >= input_size && !was_resized) {
        return keep_original(
            input,
            input_size,
            "WebP output is not smaller than the original."
        );
    }

    std::filesystem::path output_path = output;

    if (output_path.empty()) {
        Result result;
        result.media_type = MediaType::Image;
        result.input_path = input;
        result.error = "Invalid output path.";
        return result;
    }

    output_path.replace_extension(".webp");

    if (!write_atomic(output_path, encoded, error)) {
        Result result;
        result.media_type = MediaType::Image;
        result.input_path = input;
        result.output_path = input;
        result.error = error;
        return result;
    }

    Result result;
    result.status = Status::Optimized;
    result.media_type = MediaType::Image;
    result.input_path = input;
    result.output_path = output_path;
    result.input_size = input_size;
    result.output_size = encoded.size();
    result.reason = "Image converted to WebP and verified successfully.";

    return result;
}

} // namespace mediaopt
