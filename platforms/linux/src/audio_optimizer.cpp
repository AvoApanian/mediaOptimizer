#include "mediaOptimizer/audio_optimizer.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace mediaopt {
namespace {

struct AudioInfo {
    std::string codec;
    std::string format;
    int sample_rate = 0;
    int channels = 0;
    double duration = 0.0;
    bool has_audio = false;
    bool has_video = false;
};

static std::string path_to_utf8(const fs::path& path) {
    const auto value = path.u8string();
    return std::string(value.begin(), value.end());
}

#ifdef _WIN32

static std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) return {};

    const int size = MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0
    );

    if (size <= 0) return {};

    std::wstring result(static_cast<size_t>(size), L'\0');

    if (MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), size
    ) <= 0) {
        return {};
    }

    return result;
}

static std::wstring quote_windows_argument(const std::wstring& value) {
    if (!value.empty() &&
        value.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return value;
    }

    std::wstring result = L"\"";
    size_t backslashes = 0;

    for (wchar_t c : value) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result += L'"';
            backslashes = 0;
        } else {
            result.append(backslashes, L'\\');
            result += c;
            backslashes = 0;
        }
    }

    result.append(backslashes * 2, L'\\');
    result += L'"';
    return result;
}

#endif

static int run_process(
    const std::vector<std::string>& args,
    std::string* output,
    bool merge_stderr = false
) {
    if (args.empty()) return -1;

#ifdef _WIN32

    std::wstring command;

    for (size_t i = 0; i < args.size(); ++i) {
        if (i != 0) command += L' ';

        const std::wstring argument = utf8_to_wide(args[i]);
        if (argument.empty() && !args[i].empty()) return -1;

        command += quote_windows_argument(argument);
    }

    SECURITY_ATTRIBUTES security{
        sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE
    };

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    if (output) {
        if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) {
            return -1;
        }

        SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
        startup.hStdOutput = write_pipe;

        if (merge_stderr) {
            startup.hStdError = write_pipe;
        }
    }

    std::vector<wchar_t> command_buffer(command.begin(), command.end());
    command_buffer.push_back(L'\0');

    PROCESS_INFORMATION process{};

    const BOOL created = CreateProcessW(
        nullptr,
        command_buffer.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startup,
        &process
    );

    if (output && write_pipe) {
        CloseHandle(write_pipe);
    }

    if (!created) {
        if (read_pipe) CloseHandle(read_pipe);
        return -1;
    }

    if (output) {
        char buffer[4096];
        DWORD count = 0;

        while (ReadFile(read_pipe, buffer, sizeof(buffer), &count, nullptr) &&
               count > 0) {
            output->append(buffer, count);
        }

        CloseHandle(read_pipe);
    }

    WaitForSingleObject(process.hProcess, INFINITE);

    DWORD exit_code = 1;
    if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
        exit_code = static_cast<DWORD>(-1);
    }

    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);

    return static_cast<int>(exit_code);

#else

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);

    for (const auto& argument : args) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }

    argv.push_back(nullptr);

    int pipes[2] = {-1, -1};

    if (output && pipe(pipes) != 0) {
        return -1;
    }

    const pid_t pid = fork();

    if (pid < 0) {
        if (output) {
            close(pipes[0]);
            close(pipes[1]);
        }
        return -1;
    }

    if (pid == 0) {
        const int null_input = open("/dev/null", O_RDONLY);

        if (null_input >= 0) {
            dup2(null_input, STDIN_FILENO);
            if (null_input > STDERR_FILENO) close(null_input);
        }

        if (output) {
            dup2(pipes[1], STDOUT_FILENO);

            if (merge_stderr) {
                dup2(pipes[1], STDERR_FILENO);
            }

            close(pipes[0]);
            close(pipes[1]);
        }

        execvp(argv[0], argv.data());
        _exit(127);
    }

    if (output) {
        close(pipes[1]);

        char buffer[4096];

        for (;;) {
            const ssize_t count = read(pipes[0], buffer, sizeof(buffer));

            if (count > 0) {
                output->append(buffer, static_cast<size_t>(count));
            } else if (count < 0 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }

        close(pipes[0]);
    }

    int status = 0;
    pid_t waited;

    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);

    if (waited < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);

    return -1;

#endif
}

static double parse_number(const std::string& value) {
    if (value.empty() || value == "N/A") return 0.0;

    try {
        size_t used = 0;
        const double result = std::stod(value, &used);
        return used == value.size() ? result : 0.0;
    } catch (...) {
        return 0.0;
    }
}

static bool probe_media(
    const fs::path& path,
    AudioInfo& info,
    std::string& error
) {
    const std::string absolute_path = path_to_utf8(fs::absolute(path));
    std::string output;

    const int status = run_process({
        "ffprobe",
        "-v", "error",
        "-show_entries",
        "stream=codec_type,codec_name,sample_rate,channels:format=format_name,duration",
        "-of", "default=noprint_wrappers=0",
        absolute_path
    }, &output);

    if (status == 127 || status == -1) {
        error = "FFprobe introuvable ou impossible a lancer.";
        return false;
    }

    if (status != 0) {
        error = "FFprobe n'a pas pu analyser le fichier.";
        return false;
    }

    std::istringstream stream(output);
    std::string line;
    std::string section;

    bool selected_audio = false;

    std::string stream_codec_type;
    std::string stream_codec;
    int stream_sample_rate = 0;
    int stream_channels = 0;

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line == "[STREAM]") {
            section = "stream";
            stream_codec_type.clear();
            stream_codec.clear();
            stream_sample_rate = 0;
            stream_channels = 0;
            continue;
        }

        if (line == "[FORMAT]") {
            section = "format";
            continue;
        }

        if (line == "[/STREAM]") {
            if (stream_codec_type == "video") {
                info.has_video = true;
            } else if (
                stream_codec_type == "audio" &&
                !selected_audio
            ) {
                info.has_audio = true;
                info.codec = stream_codec;
                info.sample_rate = stream_sample_rate;
                info.channels = stream_channels;
                selected_audio = true;
            }

            section.clear();
            continue;
        }

        if (line == "[/FORMAT]") {
            section.clear();
            continue;
        }

        const size_t separator = line.find('=');
        if (separator == std::string::npos) continue;

        const std::string key = line.substr(0, separator);
        const std::string value = line.substr(separator + 1);

        if (section == "stream") {
            if (key == "codec_type") {
                stream_codec_type = value;
            } else if (key == "codec_name") {
                stream_codec = value;
            } else if (key == "sample_rate") {
                stream_sample_rate =
                    static_cast<int>(parse_number(value));
            } else if (key == "channels") {
                stream_channels =
                    static_cast<int>(parse_number(value));
            }
        } else if (section == "format") {
            if (key == "format_name") {
                info.format = value;
            } else if (key == "duration") {
                info.duration = parse_number(value);
            }
        }
    }

    if (!info.has_audio || info.codec.empty() || info.channels <= 0) {
        error = "Aucune piste audio exploitable n'a ete trouvee.";
        return false;
    }

    return true;
}

static std::string lowercase(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        }
    );

    return value;
}

static bool same_file(
    const fs::path& input,
    const fs::path& output,
    std::error_code& error
) {
    const fs::path absolute_input = fs::absolute(input).lexically_normal();
    const fs::path absolute_output = fs::absolute(output).lexically_normal();

    if (absolute_input == absolute_output) return true;

    error.clear();

    if (fs::exists(output, error) && !error) {
        error.clear();
        return fs::equivalent(input, output, error);
    }

    return false;
}

static unsigned long current_pid() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(getpid());
#endif
}

static bool create_temp_file(
    const fs::path& output,
    fs::path& temporary,
    std::string& error
) {
    const fs::path directory = output.parent_path().empty()
        ? fs::current_path()
        : output.parent_path();

    for (int attempt = 0; attempt < 100; ++attempt) {
        temporary = directory /
            (".mediaopt-audio-" + std::to_string(current_pid()) + "-" +
             std::to_string(attempt) + ".webm");

#ifdef _WIN32
        HANDLE file = CreateFileW(
            temporary.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY,
            nullptr
        );

        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            return true;
        }

        const DWORD code = GetLastError();

        if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS) {
            error = "Creation du fichier temporaire impossible (code " +
                    std::to_string(code) + ").";
            return false;
        }
#else
        const int file = open(
            temporary.c_str(),
            O_WRONLY | O_CREAT | O_EXCL,
            0600
        );

        if (file >= 0) {
            close(file);
            return true;
        }

        if (errno != EEXIST) {
            error = "Creation du fichier temporaire impossible : " +
                    std::string(std::strerror(errno));
            return false;
        }
#endif
    }

    error = "Impossible de trouver un nom temporaire libre.";
    return false;
}

static bool replace_file(
    const fs::path& temporary,
    const fs::path& output,
    std::string& error
) {
#ifdef _WIN32
    if (!MoveFileExW(
        temporary.c_str(),
        output.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
    )) {
        error = "Remplacement du fichier impossible (code " +
                std::to_string(GetLastError()) + ").";
        return false;
    }
#else
    if (std::rename(temporary.c_str(), output.c_str()) != 0) {
        error = std::strerror(errno);
        return false;
    }
#endif

    return true;
}

static bool valid_config(const AudioConfig& config, std::string& error) {
    if (config.bitrate_kbps < 6 || config.bitrate_kbps > 510) {
        error = "Le bitrate Opus doit etre compris entre 6 et 510 kbit/s.";
        return false;
    }

    if (config.sample_rate != 8000 &&
        config.sample_rate != 12000 &&
        config.sample_rate != 16000 &&
        config.sample_rate != 24000 &&
        config.sample_rate != 48000) {
        error = "Frequence audio invalide pour Opus.";
        return false;
    }

    if (config.channels < 0 || config.channels > 2) {
        error = "Le nombre de canaux doit etre 0, 1 ou 2.";
        return false;
    }

    if (config.application != "voip" &&
        config.application != "audio" &&
        config.application != "lowdelay") {
        error = "Application Opus invalide.";
        return false;
    }

    if (config.frame_duration_ms != 5 &&
        config.frame_duration_ms != 10 &&
        config.frame_duration_ms != 20 &&
        config.frame_duration_ms != 40 &&
        config.frame_duration_ms != 60 &&
        config.frame_duration_ms != 120) {
        error = "Duree de frame Opus invalide.";
        return false;
    }

    return true;
}

static bool validate_output(
    const fs::path& path,
    const AudioConfig& config,
    int expected_channels,
    AudioInfo& info,
    std::string& error
) {
    std::error_code ec;

    if (!fs::exists(path, ec) || ec ||
        !fs::is_regular_file(path, ec) || ec ||
        fs::file_size(path, ec) == 0 || ec) {
        error = "Le fichier WebM genere est absent ou vide.";
        return false;
    }

    if (!probe_media(path, info, error)) {
        error = "Validation FFprobe impossible : " + error;
        return false;
    }

    if (info.codec != "opus") {
        error = "Le codec de sortie n'est pas Opus.";
        return false;
    }

    if (info.format.find("webm") == std::string::npos) {
        error = "Le conteneur de sortie n'est pas WebM.";
        return false;
    }

    if (info.has_video) {
        error = "La sortie contient une piste video inattendue.";
        return false;
    }

    if (info.channels != expected_channels) {
        error = "Le nombre de canaux de sortie est inattendu.";
        return false;
    }

    if (info.sample_rate != config.sample_rate) {
        error = "La frequence d'echantillonnage de sortie est inattendue.";
        return false;
    }

    return true;
}

} // namespace

Result optimize_audio(
    const fs::path& input,
    const fs::path& output,
    const AudioConfig& config
) {
    Result result;
    result.media_type = MediaType::Audio;
    result.input_path = input;
    result.output_path = output;

    fs::path temporary;
    bool temporary_exists = false;

    auto cleanup = [&]() {
        if (temporary_exists) {
            std::error_code ec;
            fs::remove(temporary, ec);
            temporary_exists = false;
        }
    };

    auto fail = [&](const std::string& message) {
        cleanup();
        result.status = Status::Error;
        result.error = message;
        return result;
    };

    try {
        std::error_code ec;

        if (!fs::exists(input, ec) || ec ||
            !fs::is_regular_file(input, ec) || ec) {
            return fail("Fichier audio source introuvable ou invalide.");
        }

        result.input_size = fs::file_size(input, ec);

        if (ec || result.input_size == 0) {
            return fail("Impossible de lire le fichier source ou fichier vide.");
        }

        if (same_file(input, output, ec)) {
            return fail("Le fichier de sortie doit etre different de l'original.");
        }

        if (ec) {
            return fail("Impossible de verifier les chemins des fichiers : " +
                        ec.message());
        }

        if (lowercase(output.extension().string()) != ".webm") {
            return fail("Le fichier de sortie doit avoir l'extension .webm.");
        }

        std::string config_error;

        if (!valid_config(config, config_error)) {
            return fail(config_error);
        }

        AudioInfo input_info;

        if (!probe_media(input, input_info, config_error)) {
            return fail("Analyse de la source impossible : " + config_error);
        }

        const int output_channels = config.channels == 0
            ? input_info.channels
            : config.channels;

        if (output_channels < 1 || output_channels > 2) {
            return fail("La source doit contenir une ou deux voies audio.");
        }

        if (output_channels != input_info.channels &&
            !(input_info.channels == 1 && output_channels == 2) &&
            !(input_info.channels == 2 && output_channels == 1)) {
            return fail("Conversion du nombre de canaux non prise en charge.");
        }

        if (!output.parent_path().empty()) {
            fs::create_directories(output.parent_path(), ec);

            if (ec) {
                return fail("Impossible de creer le dossier de sortie : " +
                            ec.message());
            }
        }

        if (!create_temp_file(output, temporary, config_error)) {
            return fail(config_error);
        }

        temporary_exists = true;

        const fs::path absolute_input = fs::absolute(input);
        const fs::path absolute_temporary = fs::absolute(temporary);

        std::vector<std::string> arguments = {
            "ffmpeg",
            "-hide_banner",
            "-loglevel", "error",
            "-nostdin",
            "-y",
            "-i", path_to_utf8(absolute_input),
            "-map", "0:a:0",
            "-vn",
            "-sn",
            "-dn",
            "-c:a", "libopus",
            "-b:a", std::to_string(config.bitrate_kbps) + "k",
            "-ar", std::to_string(config.sample_rate),
            "-ac", std::to_string(output_channels),
            "-application", config.application,
            "-vbr", config.vbr ? "on" : "off",
            "-frame_duration", std::to_string(config.frame_duration_ms),
            "-f", "webm",
            path_to_utf8(absolute_temporary)
        };

        std::string log;

        const int encode_status = run_process(arguments, &log, true);

        if (encode_status == 127 || encode_status == -1) {
            return fail("FFmpeg introuvable ou impossible a lancer.");
        }

        if (encode_status != 0) {
            return fail("L'encodage FFmpeg a echoue : " + log);
        }

        AudioInfo output_info;

        if (!validate_output(
            temporary,
            config,
            output_channels,
            output_info,
            config_error
        )) {
            return fail(config_error);
        }

        result.output_size = fs::file_size(temporary, ec);

        if (ec) {
            return fail("Impossible de lire la taille du WebM genere.");
        }

        if (result.output_size >= result.input_size) {
            cleanup();

            result.status = Status::OriginalKept;
            result.output_path = input;
            result.output_size = result.input_size;
            result.reason =
                "La conversion Opus ne reduit pas la taille du fichier. "
                "L'original est conserve et aucun WebM n'est produit.";

            return result;
        }

        if (!replace_file(temporary, output, config_error)) {
            return fail("Impossible de finaliser le fichier WebM : " +
                        config_error);
        }

        temporary_exists = false;

        result.status = Status::Optimized;
        result.output_path = output;
        result.reason =
            "Audio converti en Opus dans un conteneur WebM.";

        return result;

    } catch (const std::exception& error) {
        return fail(error.what());
    } catch (...) {
        return fail("Erreur inconnue pendant l'optimisation audio.");
    }
}

} // namespace mediaopt
