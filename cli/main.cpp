
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace fs = std::filesystem;

constexpr double MAX_FPS = 60.0;
constexpr int CRF = 23;

#ifdef _WIN32
#define POPEN _popen
#define PCLOSE _pclose
#else
#define POPEN popen
#define PCLOSE pclose
#endif

struct VideoInfo {
    double average_fps = 0.0;
    double nominal_fps = 0.0;

    int width = 0;
    int height = 0;

    double duration = 0.0;
    std::string codec;

    std::uintmax_t file_size = 0;
};

static std::string quote(const std::string& s) {
    std::string out = "\"";

    for (char c : s) {
        if (c == '"') out += "\\\"";
        else out += c;
    }

    out += "\"";
    return out;
}

static std::string run_capture(const std::string& cmd) {
    std::string result;

    FILE* pipe = POPEN(cmd.c_str(), "r");
    if (!pipe) return result;

    char buf[256];

    while (fgets(buf, sizeof(buf), pipe)) {
        result += buf;
    }

    PCLOSE(pipe);
    return result;
}

static std::unordered_map<std::string, std::string>
parse_probe_output(const std::string& output) {
    std::unordered_map<std::string, std::string> values;

    std::size_t start = 0;

    while (start < output.size()) {
        std::size_t end = output.find('\n', start);

        if (end == std::string::npos) {
            end = output.size();
        }

        std::string line = output.substr(start, end - start);
        std::size_t separator = line.find('=');

        if (separator != std::string::npos) {
            std::string key = line.substr(0, separator);
            std::string value = line.substr(separator + 1);

            values[key] = value;
        }

        start = end + 1;
    }

    return values;
}

static double parse_fps(const std::string& value) {
    if (value.empty() || value == "N/A") return 0.0;

    try {
        std::size_t separator = value.find('/');

        if (separator == std::string::npos) {
            return std::stod(value);
        }

        double numerator = std::stod(value.substr(0, separator));
        double denominator = std::stod(value.substr(separator + 1));

        if (denominator == 0.0) return 0.0;

        return numerator / denominator;
    }
    catch (...) {
        return 0.0;
    }
}

static double parse_double(const std::string& value) {
    try {
        return std::stod(value);
    }
    catch (...) {
        return 0.0;
    }
}

static int parse_int(const std::string& value) {
    try {
        return std::stoi(value);
    }
    catch (...) {
        return 0;
    }
}

static VideoInfo analyze_video(const fs::path& input) {
    VideoInfo info;

    std::string cmd =
        "ffprobe -v error -select_streams v:0 "
        "-show_entries stream=codec_name,width,height,avg_frame_rate,r_frame_rate "
        "-show_entries format=duration "
        "-of default=noprint_wrappers=1 " +
        quote(input.string());

    std::string output = run_capture(cmd);

    if (output.empty()) {
        throw std::runtime_error("Impossible de lire les informations FFprobe.");
    }

    auto values = parse_probe_output(output);

    info.average_fps = parse_fps(values["avg_frame_rate"]);
    info.nominal_fps = parse_fps(values["r_frame_rate"]);

    info.width = parse_int(values["width"]);
    info.height = parse_int(values["height"]);

    info.duration = parse_double(values["duration"]);
    info.codec = values["codec_name"];

    std::error_code ec;
    info.file_size = fs::file_size(input, ec);

    if (ec) {
        throw std::runtime_error("Impossible de lire la taille du fichier.");
    }

    return info;
}

static void print_video_info(const VideoInfo& info) {
    std::cout << "\n=== VIDEO ANALYSIS ===\n";

    std::cout << "Codec       : " << info.codec << "\n";
    std::cout << "Resolution  : "
              << info.width << "x" << info.height << "\n";

    std::cout << "Average FPS : "
              << info.average_fps << "\n";

    std::cout << "Nominal FPS : "
              << info.nominal_fps << "\n";

    std::cout << "Duration    : "
              << info.duration << " seconds\n";

    std::cout << "File size   : "
              << info.file_size << " bytes\n";

    std::cout << "======================\n\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0]
                  << " input.mp4 [output.mp4]\n";
        return 1;
    }

    const fs::path input = argv[1];

    if (!fs::exists(input) || !fs::is_regular_file(input)) {
        std::cerr << "Fichier introuvable ou invalide : "
                  << input << "\n";
        return 1;
    }

    fs::path output;

    if (argc >= 3) {
        output = argv[2];
    }
    else {
        output = input.parent_path() /
            (input.stem().string() + "_out" + input.extension().string());
    }

    if (fs::absolute(input).lexically_normal() ==
        fs::absolute(output).lexically_normal()) {
        std::cerr << "Le fichier de sortie doit etre different de l'original.\n";
        return 1;
    }

    VideoInfo info;

    try {
        info = analyze_video(input);
    }
    catch (const std::exception& e) {
        std::cerr << "Erreur d'analyse : " << e.what() << "\n";
        return 1;
    }

    print_video_info(info);

    double fps = info.average_fps > 0.0
        ? info.average_fps
        : info.nominal_fps;

    if (fps <= 0.0) {
        std::cerr << "Impossible de determiner les FPS.\n";
        return 1;
    }

    std::cout << "FPS utilise pour la decision : "
              << fps << "\n";

    const bool reduce_fps = fps > MAX_FPS + 0.01;

    std::string cmd = "ffmpeg -y";

    if (!reduce_fps) {
        cmd += " -copyts";
    }

    cmd += " -i " + quote(input.string());

    if (reduce_fps) {
        std::cout << "Reduction a " << MAX_FPS << " FPS...\n";
        cmd += " -r " + std::to_string((int)MAX_FPS);
    }
    else {
        std::cout << "FPS <= " << MAX_FPS
                  << " -> timestamps et cadence conserves.\n";
        cmd += " -fps_mode passthrough";
        cmd += " -enc_time_base:v demux";
    }

    cmd += " -c:v libx264 -crf " + std::to_string(CRF) +
           " -preset medium -c:a copy -movflags +faststart " +
           quote(output.string());

    std::cout << "Commande : " << cmd << "\n";

    int ret = std::system(cmd.c_str());

    if (ret != 0) {
        std::cerr << "Erreur FFmpeg (code " << ret << ")\n";
        return 1;
    }

    std::cout << "\nVideo de sortie : " << output << "\n";

    return 0;
}
