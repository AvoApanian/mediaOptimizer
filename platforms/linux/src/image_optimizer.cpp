#include "mediaOptimizer/image_optimizer.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

struct LegacyConfig {
    int max_dim = 2560;
    int jpeg_quality = 85;
    int webp_quality = 80;
    int png_level = 9;
    bool no_copy = false;
    double min_psnr_db = 22.0;
};

static std::string u8(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

static fs::path path_from_utf8(const std::string& s) {
#ifdef _WIN32
#if __cplusplus >= 202002L
    return fs::path(std::u8string(s.begin(), s.end()));
#else
    return fs::u8path(s);
#endif
#else
    return fs::path(s);
#endif
}

#ifdef _WIN32
static std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string wide_to_utf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}
#endif

static std::vector<std::string> get_args(int argc, char* argv[]) {
    std::vector<std::string> args;
#ifdef _WIN32
    (void)argc; (void)argv;
    int n = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &n);
    if (wargv) {
        for (int i = 0; i < n; ++i) args.push_back(wide_to_utf8(wargv[i]));
        LocalFree(wargv);
    }
#else
    for (int i = 0; i < argc; ++i) args.push_back(argv[i]);
#endif
    return args;
}

#ifdef _WIN32

static std::wstring quote_win(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += L'"';
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            out += c;
            backslashes = 0;
        }
    }
    out.append(backslashes * 2, L'\\');
    out += L'"';
    return out;
}

static int run_process(const std::vector<std::string>& args, std::string* out, bool merge_stderr = false) {
    std::wstring cmd;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) cmd += L' ';
        cmd += quote_win(utf8_to_wide(args[i]));
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);

    if (out) {
        if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        si.hStdOutput = wr;
        if (merge_stderr) si.hStdError = wr;
    }

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    if (out) CloseHandle(wr);
    if (!ok) {
        if (out) CloseHandle(rd);
        return -1;
    }

    if (out) {
        char tmp[4096];
        DWORD n = 0;
        while (ReadFile(rd, tmp, sizeof(tmp), &n, nullptr) && n > 0) out->append(tmp, n);
        CloseHandle(rd);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    if (!GetExitCodeProcess(pi.hProcess, &code)) code = (DWORD)-1;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(code);
}

#else

static int run_process(const std::vector<std::string>& args, std::string* out, bool merge_stderr = false) {
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    int fds[2] = {-1, -1};
    if (out && pipe(fds) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        if (out) {
            close(fds[0]);
            close(fds[1]);
        }
        return -1;
    }

    if (pid == 0) {
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) {
            dup2(nul, STDIN_FILENO);
            if (nul > STDERR_FILENO) close(nul);
        }
        if (out) {
            dup2(fds[1], STDOUT_FILENO);
            if (merge_stderr) dup2(fds[1], STDERR_FILENO);
            close(fds[0]);
            close(fds[1]);
        }
        execvp(argv[0], argv.data());
        _exit(127);
    }

    if (out) {
        close(fds[1]);
        char tmp[4096];
        for (;;) {
            ssize_t n = read(fds[0], tmp, sizeof(tmp));
            if (n > 0) out->append(tmp, static_cast<size_t>(n));
            else if (n < 0 && errno == EINTR) continue;
            else break;
        }
        close(fds[0]);
    }

    int status = 0;
    pid_t w;
    do {
        w = waitpid(pid, &status, 0);
    } while (w < 0 && errno == EINTR);
    if (w < 0) return -1;

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

#endif

static bool replace_file(const fs::path& from, const fs::path& to, std::string& err) {
#ifdef _WIN32
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        err = "MoveFileEx a echoue (code " + std::to_string(GetLastError()) + ")";
        return false;
    }
    return true;
#else
    if (std::rename(from.c_str(), to.c_str()) != 0) {
        err = std::strerror(errno);
        return false;
    }
    return true;
#endif
}

static unsigned long current_pid() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return (unsigned long)getpid();
#endif
}

static bool create_temp_file(const fs::path& output, fs::path& tmp, std::string& err) {
#ifdef _WIN32
    for (int i = 0; i < 100; ++i) {
        tmp = output.parent_path() /
              (".optimg-" + std::to_string(current_pid()) + "-" +
               std::to_string(i) + output.extension().string());

        HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);

        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            return true;
        }

        DWORD code = GetLastError();
        if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS) {
            err = "impossible de creer le fichier temporaire (code " +
                  std::to_string(code) + ")";
            return false;
        }
    }
#else
    for (int i = 0; i < 100; ++i) {
        tmp = output.parent_path() /
              (".optimg-" + std::to_string(current_pid()) + "-" +
               std::to_string(i) + output.extension().string());

        int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);

        if (fd >= 0) {
            close(fd);
            return true;
        }

        if (errno != EEXIST) {
            err = "impossible de creer le fichier temporaire : " +
                  std::string(std::strerror(errno));
            return false;
        }
    }
#endif

    err = "impossible de trouver un nom temporaire libre";
    return false;
}

struct TempCopy {
    fs::path path;
    bool active = false;
    ~TempCopy() {
        if (active) {
            std::error_code ec;
            fs::remove(path, ec);
        }
    }
};

static bool safe_source(const fs::path& input, const fs::path& output, TempCopy& tc, fs::path& src,
                        std::string& err) {
    src = input;
    if (input.filename().string().find('%') == std::string::npos) return true;

    fs::path temp_name;
    fs::path temp_base = output.parent_path() / ("input" + input.extension().string());

    if (!create_temp_file(temp_base, tc.path, err)) {
        err = "creation de la copie temporaire impossible : " + err;
        return false;
    }

    tc.active = true;

    std::error_code ec;
    fs::copy_file(input, tc.path, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        err = "copie temporaire de l'entree impossible : " + ec.message();
        return false;
    }

    src = tc.path;
    return true;
}

struct ImageInfo {
    int width = 0;
    int height = 0;
    std::string codec;
    std::string pix_fmt;
    std::string format_name;
    long long frames = -1;
    std::uintmax_t file_size = 0;
};

static std::unordered_map<std::string, std::string> parse_probe_output(const std::string& output) {
    std::unordered_map<std::string, std::string> values;
    std::istringstream ss(output);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto sep = line.find('=');
        if (sep != std::string::npos) values[line.substr(0, sep)] = line.substr(sep + 1);
    }
    return values;
}

static long long parse_ll(const std::string& v, long long fallback) {
    try {
        size_t used = 0;
        long long x = std::stoll(v, &used);
        return used == v.size() ? x : fallback;
    } catch (...) {
        return fallback;
    }
}

static bool analyze_image(const fs::path& input, ImageInfo& info, std::string& err) {
    std::string out;
    int code = run_process({"ffprobe", "-v", "quiet", "-count_frames",
                            "-select_streams", "v:0", "-show_entries",
                            "stream=codec_name,pix_fmt,width,height,nb_read_frames:format=format_name",
                            "-of", "default=noprint_wrappers=1", "-i", u8(fs::absolute(input))},
                           &out);
    if (code == 127 || code == -1) {
        err = "ffprobe introuvable ou impossible a lancer";
        return false;
    }
    if (code != 0) {
        err = "ffprobe a echoue (code " + std::to_string(code) + ") : fichier invalide ou corrompu";
        return false;
    }

    auto v = parse_probe_output(out);
    info.codec = v["codec_name"];
    info.pix_fmt = v["pix_fmt"];
    info.format_name = v["format_name"];
    info.width = (int)parse_ll(v["width"], 0);
    info.height = (int)parse_ll(v["height"], 0);
    info.frames = parse_ll(v["nb_read_frames"], -1);

    if (info.codec.empty() || info.format_name.empty() || info.width <= 0 || info.height <= 0) {
        err = "aucune image valide trouvee dans le fichier";
        return false;
    }

    std::error_code ec;
    info.file_size = fs::file_size(input, ec);
    if (ec) {
        err = "impossible de lire la taille du fichier";
        return false;
    }
    return true;
}

static void print_image_info(const ImageInfo& i) {
    std::cout << "\n=== IMAGE ANALYSIS ===\n"
              << "Codec      : " << i.codec << " (" << i.pix_fmt << ")\n"
              << "Conteneur   : " << i.format_name << "\n"
              << "Resolution  : " << i.width << "x" << i.height << "\n"
              << "Images      : " << (i.frames >= 0 ? std::to_string(i.frames) : "inconnu") << "\n"
              << "File size   : " << i.file_size << " bytes\n"
              << "======================\n\n";
}

static bool is_still_container(const std::string& fmt) {
    if (fmt == "image2") return true;
    return fmt.size() > 5 && fmt.compare(fmt.size() - 5, 5, "_pipe") == 0;
}

static std::string read_head(const fs::path& p, size_t n) {
    std::ifstream f(p, std::ios::binary);
    std::string buf(n, '\0');
    f.read(&buf[0], (std::streamsize)n);
    buf.resize((size_t)f.gcount());
    return buf;
}

static bool webp_flag(const fs::path& p, unsigned char mask) {
    std::string h = read_head(p, 32);
    return h.size() >= 21 && h.compare(0, 4, "RIFF") == 0 && h.compare(8, 4, "WEBP") == 0 &&
           h.compare(12, 4, "VP8X") == 0 && (static_cast<unsigned char>(h[20]) & mask);
}

static bool webp_is_animated(const fs::path& p) { return webp_flag(p, 0x02); }

static bool has_icc(const fs::path& p, const std::string& codec) {
    if (codec == "webp") return webp_flag(p, 0x20);
    std::string h = read_head(p, 512 * 1024);
    if (codec == "mjpeg") return h.find(std::string("ICC_PROFILE\0", 12)) != std::string::npos;
    if (codec == "png") {
        size_t idat = h.find("IDAT");
        size_t iccp = h.find("iCCP");
        return iccp != std::string::npos && (idat == std::string::npos || iccp < idat);
    }
    return false;
}

static bool jpeg_is_multi_picture(const fs::path& p) {
    std::string h = read_head(p, 256 * 1024);
    return h.find(std::string("MPF\0", 4)) != std::string::npos;
}

static const int QSCALE_EQUIV_QUALITY[30] = {
    94, 92, 87, 85, 82, 78, 75, 73, 69, 66, 62, 60, 59, 54, 51, 48, 47, 43, 40, 40, 37, 37, 35, 34, 31, 31, 30, 29, 28, 26};

static int jpeg_qscale(int quality) {
    quality = std::clamp(quality, 1, 100);
    int best = 2, best_diff = 1000;
    for (int i = 0; i < 30; ++i) {
        int diff = std::abs(QSCALE_EQUIV_QUALITY[i] - quality);
        if (diff < best_diff) {
            best_diff = diff;
            best = i + 2;
        }
    }
    return best;
}

static std::vector<std::string> encoder_args(const std::string& codec, const LegacyConfig& cfg) {
    if (codec == "mjpeg")
        return {"-c:v", "mjpeg", "-q:v", std::to_string(jpeg_qscale(cfg.jpeg_quality)), "-huffman", "optimal"};
    if (codec == "png")
        return {"-c:v", "png", "-compression_level", std::to_string(cfg.png_level)};
    if (codec == "webp")
        return {"-c:v", "libwebp", "-lossless", "0", "-quality", std::to_string(cfg.webp_quality)};
    return {};
}

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static bool extension_matches(const std::string& codec, const fs::path& out) {
    std::string ext = lower(out.extension().string());
    if (codec == "mjpeg") return ext == ".jpg" || ext == ".jpeg";
    if (codec == "png") return ext == ".png";
    if (codec == "webp") return ext == ".webp";
    return false;
}

enum class LegacyOutcome { Optimized, OriginalKept, Error };

struct LegacyResult {
    LegacyOutcome outcome = LegacyOutcome::Error;
    std::string reason;
    fs::path output_path;
};

static LegacyResult fail(const std::string& why) {
    LegacyResult r;
    r.outcome = LegacyOutcome::Error;
    r.reason = why;
    return r;
}

static LegacyResult keep_original(const fs::path& input, const fs::path& output, const LegacyConfig& cfg,
                            const std::string& reason) {
    LegacyResult r;
    r.reason = reason;

    if (cfg.no_copy) {
        r.outcome = LegacyOutcome::OriginalKept;
        r.output_path = input;
        return r;
    }

    ImageInfo info;
    std::string analyze_error;
    fs::path destination = output;

    if (analyze_image(input, info, analyze_error)) {
        if (info.codec == "mjpeg")
            destination.replace_extension(".jpg");
        else if (info.codec == "png")
            destination.replace_extension(".png");
        else if (info.codec == "webp")
            destination.replace_extension(".webp");
        else
            destination.replace_extension(input.extension());
    } else {
        destination.replace_extension(input.extension());
    }

    fs::path tmp;
    std::string temp_err;
    if (!create_temp_file(destination, tmp, temp_err))
        return fail("creation du fichier temporaire impossible : " + temp_err);

    std::error_code ec;
    fs::copy_file(input, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return fail("erreur de copie : " + ec.message());
    }

    std::string err;
    if (!replace_file(tmp, destination, err)) {
        fs::remove(tmp, ec);
        return fail("impossible d'ecrire la sortie : " + err);
    }

    r.outcome = LegacyOutcome::OriginalKept;
    r.output_path = destination;
    return r;
}

static bool validate_structure(const fs::path& path, const ImageInfo& in, const LegacyConfig& cfg, bool reduced,
                               ImageInfo& out, std::string& err) {
    std::error_code ec;
    if (!fs::exists(path, ec) || fs::file_size(path, ec) == 0) {
        err = "fichier de sortie absent ou vide";
        return false;
    }

    std::string perr;
    if (!analyze_image(path, out, perr)) {
        err = "fichier de sortie illisible : " + perr;
        return false;
    }
    if (out.codec != in.codec) {
        err = "format de sortie different (" + out.codec + " au lieu de " + in.codec + ")";
        return false;
    }
    if (out.frames != 1) {
        err = "la sortie ne contient pas exactement une image";
        return false;
    }

    const double L = std::max(in.width, in.height), S = std::min(in.width, in.height);
    const double OL = std::max(out.width, out.height), OS = std::min(out.width, out.height);

    if (reduced) {
        if (OL != cfg.max_dim || std::fabs(OS - S * cfg.max_dim / L) > 1.0) {
            err = "dimensions de sortie inattendues : " + std::to_string(out.width) + "x" +
                  std::to_string(out.height);
            return false;
        }
    } else if (OL != L || OS != S) {
        err = "les dimensions ont change alors qu'elles devaient etre conservees : " +
              std::to_string(out.width) + "x" + std::to_string(out.height);
        return false;
    }
    return true;
}

static double measure_psnr(const fs::path& in, const fs::path& out, const ImageInfo& out_info) {
    const std::string size = std::to_string(out_info.width) + ":" + std::to_string(out_info.height);
    std::string log;
    int code = run_process({"ffmpeg", "-hide_banner", "-nostdin", "-i",
                            u8(fs::absolute(in)), "-i", u8(fs::absolute(out)),
                            "-lavfi", "[0:v]scale=" + size + ":flags=bicubic[a];[a][1:v]psnr", "-f", "null", "-"},
                           &log, /*merge_stderr=*/true);
    if (code != 0) return -1.0;
    size_t pos = log.rfind("average:");
    if (pos == std::string::npos) return -1.0;
    return std::strtod(log.c_str() + pos + 8, nullptr);
}

static bool validate_fidelity(const fs::path& input, const fs::path& tmp, const ImageInfo& in,
                              const ImageInfo& out, const LegacyConfig& cfg, std::string& why) {
    if (has_icc(input, in.codec) && !has_icc(tmp, in.codec)) {
        why = "le profil colorimetrique ICC serait perdu";
        return false;
    }
    double psnr = measure_psnr(input, tmp, out);
    if (psnr < 0) {
        why = "controle visuel impossible (mesure PSNR indisponible)";
        return false;
    }
    if (psnr < cfg.min_psnr_db) {
        why = "controle visuel echoue (PSNR " + std::to_string(psnr) + " dB < " +
              std::to_string(cfg.min_psnr_db) + " dB)";
        return false;
    }
    return true;
}

static LegacyResult optimize_image(const fs::path& original, const fs::path& output, const LegacyConfig& cfg) {
    ImageInfo info;
    std::string err;

    TempCopy tempcopy;
    fs::path input;
    if (!safe_source(original, output, tempcopy, input, err)) return fail(err);

    if (webp_is_animated(input))
        return keep_original(original, output, cfg, "image animee (WebP anime) non geree");

    if (!analyze_image(input, info, err)) return fail("analyse : " + err);
    print_image_info(info);

    if (!is_still_container(info.format_name)) {
        if (info.codec == "gif" || info.codec == "apng")
            return keep_original(original, output, cfg, "image animee (" + info.codec + ") non geree");
        return fail("ce fichier n'est pas une image (conteneur '" + info.format_name +
                    "', codec '" + info.codec + "') : refuse");
    }

    const auto enc = encoder_args(info.codec, cfg);
    if (enc.empty())
        return keep_original(original, output, cfg, "format '" + info.codec + "' non gere");

    if (info.frames != 1)
        return keep_original(original, output, cfg,
                           info.frames > 1 ? "image animee" : "image animee ou non verifiable");

    if (info.codec == "mjpeg") {
        if (info.pix_fmt.rfind("yuv", 0) != 0)
            return keep_original(original, output, cfg,
                                 "JPEG '" + info.pix_fmt + "' (CMYK / niveaux de gris) : couleurs non preservables");
        if (jpeg_is_multi_picture(input))
            return keep_original(original, output, cfg, "JPEG multi-images (MPO / gain map HDR) : les autres images seraient perdues");
    }
    if (info.codec == "webp" && has_icc(input, "webp"))
        return keep_original(original, output, cfg, "WebP avec profil ICC (serait perdu par ffmpeg)");

    if (!extension_matches(info.codec, output))
        return fail("l'extension de sortie doit correspondre au format d'entree (" + info.codec + ")");

    const int longest = std::max(info.width, info.height);
    const bool reduce_size = longest > cfg.max_dim;

    fs::path tmp;
    if (!create_temp_file(output, tmp, err))
        return fail("creation du fichier temporaire impossible : " + err);

    std::vector<std::string> args = {"ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin", "-y",
                                     "-i", u8(fs::absolute(input)),
                                     "-map", "0:v:0"};

    if (reduce_size) {
        const std::string m = std::to_string(cfg.max_dim);
        std::cout << "Cote max " << longest << " > " << cfg.max_dim << " -> reduction a " << cfg.max_dim
                  << " px (proportions conservees).\n";
        args.push_back("-vf");
        args.push_back("scale=w='if(gt(iw,ih)," + m + ",-1)':h='if(gt(iw,ih),-1," + m + ")':flags=lanczos");
    } else {
        std::cout << "Cote max " << longest << " <= " << cfg.max_dim
                  << " -> dimensions conservees (pas de scale), autres optimisations seulement.\n";
    }

    args.insert(args.end(), enc.begin(), enc.end());
    args.insert(args.end(), {"-frames:v", "1", "-update", "1", u8(fs::absolute(tmp))});

    std::cout << "Commande :";
    for (const auto& a : args) std::cout << " [" << a << "]";
    std::cout << "\n";

    std::error_code ec;
    std::string ffmpeg_log;
    int code = run_process(args, &ffmpeg_log, /*merge_stderr=*/true);
    if (code != 0) {
        fs::remove(tmp, ec);
        return fail("ffmpeg a echoue (code " + std::to_string(code) + ") " + ffmpeg_log);
    }

    ImageInfo out_info;
    if (!validate_structure(tmp, info, cfg, reduce_size, out_info, err)) {
        fs::remove(tmp, ec);
        return fail("verification : " + err);
    }

    std::string why;
    if (!validate_fidelity(input, tmp, info, out_info, cfg, why)) {
        fs::remove(tmp, ec);
        return keep_original(original, output, cfg, why);
    }

    const std::uintmax_t out_size = fs::file_size(tmp, ec);

    if (!reduce_size && out_size >= info.file_size) {
        fs::remove(tmp, ec);
        return keep_original(original, output, cfg, "le reencodage n'est pas plus leger que l'original");
    }

    if (!replace_file(tmp, output, err)) {
        fs::remove(tmp, ec);
        return fail("impossible d'ecrire la sortie : " + err);
    }

    LegacyResult r;
    r.outcome = LegacyOutcome::Optimized;
    if (reduce_size && out_size >= info.file_size)
        r.reason = "attention : image reduite mais fichier plus gros que l'original";
    return r;
}


namespace mediaopt {

Result optimize_image(
    const std::filesystem::path& input,
    const std::filesystem::path& output,
    const ImageConfig& config
) {
    LegacyConfig legacy_config;
    legacy_config.max_dim = config.max_dim;
    legacy_config.jpeg_quality = config.jpeg_quality;
    legacy_config.webp_quality = config.webp_quality;
    legacy_config.png_level = config.png_level;
    legacy_config.min_psnr_db = config.min_psnr_db;
    legacy_config.no_copy = config.no_copy;

    const LegacyResult legacy = ::optimize_image(input, output, legacy_config);

    Result result;
    result.media_type = MediaType::Image;
    result.input_path = input;
    result.output_path = legacy.output_path.empty()
        ? output
        : legacy.output_path;

    std::error_code ec;
    if (std::filesystem::exists(input, ec))
        result.input_size = std::filesystem::file_size(input, ec);

    ec.clear();
    if (std::filesystem::exists(result.output_path, ec))
        result.output_size = std::filesystem::file_size(result.output_path, ec);

    switch (legacy.outcome) {
    case LegacyOutcome::Optimized:
        result.status = Status::Optimized;
        result.reason = legacy.reason;
        break;

    case LegacyOutcome::OriginalKept:
        result.status = Status::OriginalKept;
        result.reason = legacy.reason;
        break;

    case LegacyOutcome::Error:
        result.status = Status::Error;
        result.error = legacy.reason;
        break;
    }

    return result;
}

} // namespace mediaopt
