#include "mediaOptimizer/video_optimizer.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <system_error>

namespace fs = std::filesystem;

namespace mediaopt {
namespace {

struct VideoInfo {
    double average_fps = 0.0;
    double nominal_fps = 0.0;
    double duration = 0.0;
    int width = 0;
    int height = 0;
    std::string codec;
    std::uintmax_t file_size = 0;
};

std::string quote_argument(const std::string& value) {
#ifdef _WIN32
    std::string result = "\"";
    for (char c : value) {
        if (c == '"') result += "\\\"";
        else result += c;
    }
    result += "\"";
    return result;
#else
    std::string result = "'";
    for (char c : value) {
        if (c == '\'') result += "'\\''";
        else result += c;
    }
    result += "'";
    return result;
#endif
}

std::string run_capture(const std::string& command) {
    std::string output;

#ifdef _WIN32
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif

    if (!pipe) {
        throw std::runtime_error("Impossible de lancer FFprobe.");
    }

    char buffer[512];

    while (fgets(buffer, sizeof(buffer), pipe)) {
        output += buffer;
    }

#ifdef _WIN32
    int status = _pclose(pipe);
#else
    int status = pclose(pipe);
#endif

    if (status != 0) {
        throw std::runtime_error("FFprobe a échoué.");
    }

    return output;
}

std::unordered_map<std::string, std::string>
parse_probe_output(const std::string& output) {
    std::unordered_map<std::string, std::string> values;
    std::istringstream stream(output);
    std::string line;

    while (std::getline(stream, line)) {
        const auto separator = line.find('=');

        if (separator != std::string::npos) {
            values[line.substr(0, separator)] =
                line.substr(separator + 1);
        }
    }

    return values;
}

double parse_fps(const std::string& value) {
    if (value.empty() || value == "N/A") return 0.0;

    try {
        const auto separator = value.find('/');

        if (separator == std::string::npos) {
            return std::stod(value);
        }

        const double numerator = std::stod(value.substr(0, separator));
        const double denominator = std::stod(value.substr(separator + 1));

        return denominator == 0.0 ? 0.0 : numerator / denominator;
    }
    catch (...) {
        return 0.0;
    }
}

double parse_double(const std::string& value) {
    try {
        return std::stod(value);
    }
    catch (...) {
        return 0.0;
    }
}

int parse_int(const std::string& value) {
    try {
        return std::stoi(value);
    }
    catch (...) {
        return 0;
    }
}

VideoInfo analyze_video(const fs::path& input) {
    const std::string command =
        "ffprobe -v error -select_streams v:0 "
        "-show_entries stream=codec_name,width,height,avg_frame_rate,r_frame_rate "
        "-show_entries format=duration "
        "-of default=noprint_wrappers=1 " +
        quote_argument(input.string());

    const auto values = parse_probe_output(run_capture(command));

    VideoInfo info;
    auto get = [&values](const std::string& key) -> std::string {
        const auto it = values.find(key);
        return it == values.end() ? "" : it->second;
    };

    info.average_fps = parse_fps(get("avg_frame_rate"));
    info.nominal_fps = parse_fps(get("r_frame_rate"));
    info.width = parse_int(get("width"));
    info.height = parse_int(get("height"));
    info.duration = parse_double(get("duration"));
    info.codec = get("codec_name");

    std::error_code ec;
    info.file_size = fs::file_size(input, ec);

    if (ec) {
        throw std::runtime_error("Impossible de lire la taille du fichier.");
    }

    return info;
}

} // namespace

Result optimize_video(
    const fs::path& input,
    const fs::path& output,
    const VideoConfig& config
) {
    Result result;
    result.media_type = MediaType::Video;
    result.input_path = input;
    result.output_path = output;

    try {
        if (!fs::exists(input) || !fs::is_regular_file(input)) {
            throw std::runtime_error("Fichier vidéo introuvable ou invalide.");
        }

        if (fs::absolute(input).lexically_normal() ==
            fs::absolute(output).lexically_normal()) {
            throw std::runtime_error(
                "Le fichier de sortie doit être différent de l'original."
            );
        }

        if (config.max_fps <= 0.0 || config.crf < 0 || config.crf > 51) {
            throw std::runtime_error("Configuration vidéo invalide.");
        }

        const VideoInfo info = analyze_video(input);

        const double fps = info.average_fps > 0.0
            ? info.average_fps
            : info.nominal_fps;

        if (fps <= 0.0) {
            throw std::runtime_error("Impossible de déterminer les FPS.");
        }

        if (!output.parent_path().empty()) {
            fs::create_directories(output.parent_path());
        }

        const bool reduce_fps = fps > config.max_fps + 0.01;

        std::string command = "ffmpeg -hide_banner -y";

        if (!reduce_fps) {
            command += " -copyts";
        }

        command += " -i " + quote_argument(input.string());

        if (reduce_fps) {
            command += " -r " + std::to_string(config.max_fps);
        }
        else {
            command += " -fps_mode passthrough";
            command += " -enc_time_base:v demux";
        }

        command += " -c:v libx264 -crf " + std::to_string(config.crf);
        command += " -preset medium -c:a copy -movflags +faststart ";
        command += quote_argument(output.string());

        const int status = std::system(command.c_str());

        if (status != 0) {
            std::error_code ec;
            fs::remove(output, ec);
            throw std::runtime_error("L'encodage FFmpeg a échoué.");
        }

        result.input_size = info.file_size;
        result.output_size = fs::file_size(output);

        if (result.output_size < result.input_size) {
            result.status = Status::Optimized;
            result.reason = "Vidéo optimisée.";
        }
        else {
            std::error_code ec;
            fs::copy_file(
                input,
                output,
                fs::copy_options::overwrite_existing,
                ec
            );

            if (ec) {
                throw std::runtime_error(
                    "Impossible de conserver le fichier original : " + ec.message()
                );
            }

            result.status = Status::OriginalKept;
            result.output_size = fs::file_size(output);
            result.reason = "Optimisation inutile : original conservé.";
        }

        return result;
    }
    catch (const std::exception& error) {
        result.status = Status::Error;
        result.error = error.what();
        return result;
    }
}

} // namespace mediaopt
