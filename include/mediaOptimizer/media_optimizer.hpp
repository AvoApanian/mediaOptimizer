#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace mediaopt {

enum class MediaType {
    Image,
    Video,
    Audio,
    Unknown
};

enum class Status {
    Optimized,
    OriginalKept,
    Error
};

struct ImageConfig {
    int max_dim = 2560;
    int jpeg_quality = 85;
    int webp_quality = 80;
    int png_level = 9;
    double min_psnr_db = 22.0;
    bool no_copy = false;
};

struct VideoConfig {
    double max_fps = 60.0;
    int crf = 23;
};

struct AudioConfig {
    int bitrate_kbps = 32;
    int sample_rate = 48000;
    int channels = 0;
    std::string application = "voip";
    bool vbr = true;
    int frame_duration_ms = 20;
};

struct Config {
    ImageConfig image;
    VideoConfig video;
    AudioConfig audio;
};

struct Result {
    Status status = Status::Error;
    MediaType media_type = MediaType::Unknown;

    std::filesystem::path input_path;
    std::filesystem::path output_path;

    std::uintmax_t input_size = 0;
    std::uintmax_t output_size = 0;

    std::string reason;
    std::string error;

    [[nodiscard]] bool success() const noexcept {
        return status != Status::Error;
    }

    [[nodiscard]] bool was_optimized() const noexcept {
        return status == Status::Optimized;
    }
};

[[nodiscard]] Result optimize(
    const std::filesystem::path& input,
    const std::filesystem::path& output,
    const Config& config = {}
);

} // namespace mediaopt
