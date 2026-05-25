#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include "parsers/YSMParser.hpp"
#include "platform/PlatformCompat.hpp"
#include "CLI11.hpp"
#include <exception>
#include "parsers/exceptions/ParserException.hpp"

namespace fs = std::filesystem;

#ifndef YSM_PARSER_VERSION
#define YSM_PARSER_VERSION "0.0.0-dev"
#endif

#if defined(__EMSCRIPTEN__)
#define YSM_WASM_TARGET 1
#else
#define YSM_WASM_TARGET 0
#endif

namespace {

constexpr std::size_t kProgressBarWidth = 32;
constexpr std::chrono::milliseconds kProgressRefreshInterval(90);

struct FileTask {
    fs::path input_file;
    fs::path target_output_dir;
    std::string relative_input;
    uintmax_t size_bytes = 0;
    bool size_known = false;
};

struct RunSummary {
    std::size_t total = 0;
    std::size_t succeeded = 0;
    std::size_t failed = 0;
};

struct ProcessResult {
    bool success = false;
    std::string file_name;
    std::string output_dir;
    std::string error_message;
    double elapsed_seconds = 0.0;
};

struct ConsoleTheme {
    bool ansi = false;

    const char* reset() const { return ansi ? "\x1b[0m" : ""; }
    const char* dim() const { return ansi ? "\x1b[2m" : ""; }
    const char* accent() const { return ansi ? "\x1b[96m" : ""; }
    const char* strong() const { return ansi ? "\x1b[1;97m" : ""; }
    const char* success() const { return ansi ? "\x1b[92m" : ""; }
    const char* warning() const { return ansi ? "\x1b[93m" : ""; }
    const char* error() const { return ansi ? "\x1b[91m" : ""; }
};

std::string to_lower_ascii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

std::string format_size(uintmax_t size_bytes) {
    static constexpr const char* kUnits[] = { "B", "KB", "MB", "GB", "TB" };

    double value = static_cast<double>(size_bytes);
    std::size_t unit_index = 0;

    while (value >= 1024.0 && unit_index + 1 < std::size(kUnits)) {
        value /= 1024.0;
        ++unit_index;
    }

    std::ostringstream oss;
    if (unit_index == 0) {
        oss << static_cast<uintmax_t>(value) << ' ' << kUnits[unit_index];
    }
    else {
        oss << std::fixed << std::setprecision(value >= 100.0 ? 0 : (value >= 10.0 ? 1 : 2))
            << value << ' ' << kUnits[unit_index];
    }
    return oss.str();
}

std::string format_seconds(double seconds) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(seconds >= 10.0 ? 1 : 2) << seconds << 's';
    return oss.str();
}

std::string escape_profile_field(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
        case '\\':
            escaped += "\\\\";
            break;
        case '\t':
            escaped += "\\t";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        default:
            escaped.push_back(ch);
            break;
        }
    }
    return escaped;
}

void emit_profile_line(const std::string& file_name, int version, const std::string& stage, double milliseconds) {
    std::ostringstream ms;
    ms << std::fixed << std::setprecision(3) << milliseconds;
    std::cerr << "YSM_PROFILE"
        << "\tfile=" << escape_profile_field(file_name)
        << "\tversion=" << version
        << "\tstage=" << escape_profile_field(stage)
        << "\tms=" << ms.str()
        << '\n';
}

std::string make_relative_utf8(const fs::path& path, const fs::path& base) {
    std::error_code ec;
    const fs::path relative_path = fs::relative(path, base, ec);
    return PathUtils::path_to_utf8(ec ? path : relative_path);
}

std::vector<FileTask> collect_file_tasks(const fs::path& input_root, const fs::path& output_root) {
    std::vector<FileTask> tasks;

    for (const auto& entry : fs::recursive_directory_iterator(input_root)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        if (to_lower_ascii(entry.path().extension().string()) != ".ysm") {
            continue;
        }

        std::error_code size_ec;
        const uintmax_t size_bytes = entry.file_size(size_ec);

        tasks.push_back(FileTask{
            entry.path(),
            output_root / entry.path().stem(),
            make_relative_utf8(entry.path(), input_root),
            size_bytes,
            !size_ec
        });
    }

    std::sort(tasks.begin(), tasks.end(), [](const FileTask& lhs, const FileTask& rhs) {
        return PathUtils::path_to_utf8(lhs.input_file) < PathUtils::path_to_utf8(rhs.input_file);
    });

    return tasks;
}

bool has_duplicate_output_directories(const std::vector<FileTask>& tasks) {
    std::unordered_set<std::string> seen;
    seen.reserve(tasks.size());

    for (const auto& task : tasks) {
        const std::string output_key = PathUtils::path_to_utf8(task.target_output_dir);
        if (!seen.insert(output_key).second) {
            return true;
        }
    }

    return false;
}

unsigned int resolve_worker_count(unsigned int requested_threads, std::size_t task_count) {
    if (task_count == 0) {
        return 1;
    }

#if YSM_WASM_TARGET
    (void)requested_threads;
    return 1;
#else
    unsigned int worker_count = requested_threads;
    if (worker_count == 0) {
        worker_count = std::thread::hardware_concurrency();
        if (worker_count == 0) {
            worker_count = 1;
        }
    }

    return std::max(1u, std::min(worker_count, static_cast<unsigned int>(task_count)));
#endif
}

void print_big_file_banner(const ConsoleTheme& theme) {
    static const std::vector<std::string> kBanner = {
        "FFFFF  IIIII  L       EEEEE    IIIII  N   N  FFFFF   OOO  ",
        "F        I    L       E          I    NN  N  F      O   O ",
        "FFFF     I    L       EEEE       I    N N N  FFFF   O   O ",
        "F        I    L       E          I    N  NN  F      O   O ",
        "F      IIIII  LLLLLL  EEEEE    IIIII  N   N  F       OOO  "
    };

    std::cout << '\n';
    for (const auto& line : kBanner) {
        std::cout << theme.accent() << line << theme.reset() << '\n';
    }
}

void print_box(std::ostream& os, const ConsoleTheme& theme, const std::string& title, const std::vector<std::string>& lines) {
    std::size_t width = title.size();
    for (const auto& line : lines) {
        width = std::max(width, line.size());
    }

    const std::string border(width + 2, '-');
    os << '/' << border << "\\\n";
    os << "| " << theme.strong() << std::left << std::setw(static_cast<int>(width)) << title << theme.reset() << " |\n";
    os << "| " << std::string(width, '-') << " |\n";
    for (const auto& line : lines) {
        os << "| " << std::left << std::setw(static_cast<int>(width)) << line << " |\n";
    }
    os << '\\' << border << "/\n";
}

class ScopedStdoutSilencer {
public:
    explicit ScopedStdoutSilencer(bool enabled)
        : enabled_(enabled) {
#if YSM_WASM_TARGET
        enabled_ = false;
        return;
#endif
        if (!enabled_) {
            return;
        }

        null_stream_.open(PlatformCompat::null_device_path());
        previous_cout_buffer_ = std::cout.rdbuf(null_stream_.rdbuf());

        duplicated_stdout_fd_ = PlatformCompat::dup_fd(PlatformCompat::fileno_of(stdout));
        if (duplicated_stdout_fd_ == -1) {
            std::cout.rdbuf(previous_cout_buffer_);
            throw std::runtime_error("Failed to duplicate stdout handle.");
        }

        std::fflush(stdout);

#ifdef _WIN32
        FILE* redirected_stdout = nullptr;
        if (freopen_s(&redirected_stdout, PlatformCompat::null_device_path(), "w", stdout) != 0) {
            PlatformCompat::close_fd(duplicated_stdout_fd_);
            duplicated_stdout_fd_ = -1;
            std::cout.rdbuf(previous_cout_buffer_);
            throw std::runtime_error("Failed to redirect stdout to the null device.");
        }
#else
        if (std::freopen(PlatformCompat::null_device_path(), "w", stdout) == nullptr) {
            PlatformCompat::close_fd(duplicated_stdout_fd_);
            duplicated_stdout_fd_ = -1;
            std::cout.rdbuf(previous_cout_buffer_);
            throw std::runtime_error("Failed to redirect stdout to the null device.");
        }
#endif
    }

    ScopedStdoutSilencer(const ScopedStdoutSilencer&) = delete;
    ScopedStdoutSilencer& operator=(const ScopedStdoutSilencer&) = delete;

    ~ScopedStdoutSilencer() {
        if (!enabled_) {
            return;
        }

        std::fflush(stdout);
        if (duplicated_stdout_fd_ != -1) {
            PlatformCompat::dup2_fd(duplicated_stdout_fd_, PlatformCompat::fileno_of(stdout));
            PlatformCompat::close_fd(duplicated_stdout_fd_);
        }
        std::clearerr(stdout);

        if (previous_cout_buffer_ != nullptr) {
            std::cout.rdbuf(previous_cout_buffer_);
        }
    }

private:
    bool enabled_ = false;
    std::ofstream null_stream_;
    std::streambuf* previous_cout_buffer_ = nullptr;
    int duplicated_stdout_fd_ = -1;
};

class ProgressRenderer {
public:
    ProgressRenderer(std::size_t total, ConsoleTheme theme)
        : total_(total), theme_(theme) {
    }

    ~ProgressRenderer() {
        if (worker_.joinable()) {
            stop_requested_ = true;
            worker_.join();
            std::lock_guard<std::mutex> lock(mutex_);
            clear_line_locked();
            last_line_width_ = 0;
        }
    }

    void start() {
        if (total_ == 0) {
            return;
        }

#if YSM_WASM_TARGET
        std::lock_guard<std::mutex> lock(mutex_);
        render_locked(frame_counter_++);
        std::cerr << '\n';
        last_line_width_ = 0;
        return;
#else
        stop_requested_ = false;
        worker_ = std::thread([this]() { run(); });
#endif
    }

    void begin_file(std::string current_name) {
        if (total_ == 0) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        current_name_ = std::move(current_name);
        ++active_count_;
#if YSM_WASM_TARGET
        render_locked(frame_counter_++);
        std::cerr << '\n';
        last_line_width_ = 0;
#endif
    }

    void finish_file() {
        if (total_ == 0) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        ++completed_;
        if (active_count_ != 0) {
            --active_count_;
        }
#if YSM_WASM_TARGET
        render_locked(frame_counter_++);
        std::cerr << '\n';
        last_line_width_ = 0;
#endif
    }

    void log_line(const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_line_locked();
        std::cerr << message << '\n';
        last_line_width_ = 0;
    }

    void stop() {
        if (total_ == 0) {
            return;
        }

#if YSM_WASM_TARGET
        std::lock_guard<std::mutex> lock(mutex_);
        render_locked(frame_counter_++);
        std::cerr << '\n';
        last_line_width_ = 0;
        return;
#else
        stop_requested_ = true;
        if (worker_.joinable()) {
            worker_.join();
        }

        std::lock_guard<std::mutex> lock(mutex_);
        clear_line_locked();
        render_locked(0);
        std::cerr << '\n';
        last_line_width_ = 0;
#endif
    }

private:
    void run() {
        std::size_t frame = 0;
        while (!stop_requested_) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                render_locked(frame++);
            }
            std::this_thread::sleep_for(kProgressRefreshInterval);
        }
    }

    void clear_line_locked() {
        if (theme_.ansi) {
            std::cerr << "\r\x1b[2K";
            return;
        }

        std::cerr << '\r';
        if (last_line_width_ != 0) {
            std::cerr << std::string(last_line_width_, ' ') << '\r';
        }
    }

    void render_locked(std::size_t frame) {
        const std::size_t safe_total = total_ == 0 ? 1 : total_;
        const double percent = 100.0 * static_cast<double>(completed_) / static_cast<double>(safe_total);
        const std::size_t filled = (completed_ * kProgressBarWidth) / safe_total;
        const char spinner_frames[] = { '|', '/', '-', '\\' };
        const bool active = active_count_ != 0;
        const char spinner = active ? spinner_frames[frame % std::size(spinner_frames)] : '=';

        std::ostringstream plain;
        plain << '[' << std::string(filled, '#') << std::string(kProgressBarWidth - filled, '.')
              << "] "
              << std::fixed << std::setprecision(1) << std::setw(5) << percent << "% "
              << "| " << completed_ << '/' << total_
              << " | active " << active_count_
              << " | " << spinner << ' '
              << (current_name_.empty() ? "waiting..." : current_name_);

        const std::string plain_line = plain.str();
        const std::size_t previous_line_width = last_line_width_;

        if (!theme_.ansi) {
            std::cerr << '\r';
            if (previous_line_width != 0) {
                std::cerr << std::string(previous_line_width, ' ') << '\r';
            }
        }
        else {
            clear_line_locked();
        }
        std::cerr << '\r';
        if (theme_.ansi) {
            std::cerr << theme_.accent() << '['
                      << theme_.success() << std::string(filled, '#')
                      << theme_.dim() << std::string(kProgressBarWidth - filled, '.')
                      << theme_.accent() << ']'
                      << theme_.reset() << ' '
                      << theme_.strong() << std::fixed << std::setprecision(1) << std::setw(5) << percent << '%' << theme_.reset()
                      << ' ' << theme_.dim() << "| " << completed_ << '/' << total_ << " | active " << active_count_ << " | " << theme_.reset();

            if (active) {
                std::cerr << theme_.warning() << spinner << theme_.reset() << ' ';
            }
            else {
                std::cerr << theme_.success() << spinner << theme_.reset() << ' ';
            }
            std::cerr << current_name_;
        }
        else {
            std::cerr << plain_line;
        }
        last_line_width_ = plain_line.size();
        std::cerr.flush();
    }

    std::size_t total_ = 0;
    ConsoleTheme theme_;
    std::atomic<bool> stop_requested_ = false;
    std::thread worker_;
    std::mutex mutex_;
    std::size_t completed_ = 0;
    std::size_t active_count_ = 0;
    std::string current_name_;
    std::size_t last_line_width_ = 0;
    std::size_t frame_counter_ = 0;
};

} // namespace

#include "algorithms/CryptoAlgorithms.hpp"
int main(int argc, char** argv) {
    PlatformCompat::init_console_utf8();

    ConsoleTheme theme;
    theme.ansi = PlatformCompat::enable_virtual_terminal(stdout) |
                 PlatformCompat::enable_virtual_terminal(stderr);

    CLI::App app{ "YSM File Decryptor" };
    // On Windows, argv from main() is encoded in the system ANSI code page,
    // so paths containing characters outside that code page (common with
    // CJK usernames in the Tauri sidecar's temp dir) get mangled before
    // std::filesystem ever sees them. ensure_utf8 re-reads the original
    // UTF-16 command line via GetCommandLineW and re-encodes as UTF-8.
    argv = app.ensure_utf8(argv);
    app.set_version_flag("--version", YSM_PARSER_VERSION, "Show version information and exit.");

    std::string input_dir_path;
    std::string base_output_dir;
    bool verbose = false;
    bool debug = false;
    bool formatJson = false;
    bool profile = false;
    unsigned int requested_threads = 0;

    app.add_option("-i,--input", input_dir_path, "Path to the input directory.")
        ->required();
    app.add_option("-o,--output", base_output_dir, "Path to the base output directory.")
        ->required();
    app.add_flag("-v,--verbose", verbose, "Show detailed per-file banners and parser logs.");
    app.add_flag("-d,--debug", debug, "Export all binary products (Only for V3).");
    app.add_flag("-f,--format", formatJson, "Format all json (Only for V3).");
    app.add_flag("--profile", profile, "Emit machine-readable per-file timing lines to stderr.");
    app.add_option("-j,--threads", requested_threads, "Number of files to process in parallel. 0 = auto.");

#if YSM_WASM_TARGET
    requested_threads = 1;
#endif

    CLI11_PARSE(app, argc, argv);

    try {
        const fs::path input_root = PathUtils::utf8_to_path(input_dir_path);
        const fs::path output_root = PathUtils::utf8_to_path(base_output_dir);

        if (!fs::exists(input_root)) {
            throw std::runtime_error("Input directory does not exist: " + input_dir_path);
        }
        if (!fs::is_directory(input_root)) {
            throw std::runtime_error("Input path is not a directory: " + input_dir_path);
        }

        if (fs::exists(output_root) && !fs::is_directory(output_root)) {
            throw std::runtime_error("Output path is not a directory: " + base_output_dir);
        }
        if (!fs::exists(output_root)) {
            fs::create_directories(output_root);
        }

        const std::vector<FileTask> tasks = collect_file_tasks(input_root, output_root);
        if (tasks.empty()) {
            std::cout << "[ YSMParser ] No .ysm files found in " << PathUtils::path_to_utf8(input_root) << '\n';
            return 0;
        }

        unsigned int worker_count = resolve_worker_count(requested_threads, tasks.size());
#if YSM_WASM_TARGET
        if (requested_threads > 1) {
            std::cerr << "[ YSMParser ] WebAssembly target forces --threads 1.\n";
        }
#endif
        if (verbose && worker_count > 1) {
            std::cerr << "[ YSMParser ] Verbose mode requires ordered output; forcing --threads 1.\n";
            worker_count = 1;
        }
        if (worker_count > 1 && has_duplicate_output_directories(tasks)) {
            std::cerr << "[ YSMParser ] Duplicate output directory names detected; forcing --threads 1 to avoid collisions.\n";
            worker_count = 1;
        }

        RunSummary summary;
        summary.total = tasks.size();

        ProgressRenderer progress(summary.total, theme);
        if (!verbose) {
            progress.log_line("[ YSMParser ] Found " + std::to_string(summary.total) + " .ysm file(s).");
            progress.start();
        }

        const auto batch_start = std::chrono::steady_clock::now();
        std::vector<ProcessResult> results(tasks.size());

        auto process_task = [&](std::size_t index) -> ProcessResult {
            const FileTask& task = tasks[index];
            ProcessResult result;
            result.file_name = PathUtils::path_to_utf8(task.input_file.filename());
            result.output_dir = PathUtils::path_to_utf8(task.target_output_dir);

            const std::string utf8_input = PathUtils::path_to_utf8(task.input_file);
            const auto file_start = std::chrono::steady_clock::now();

            if (verbose) {
                print_big_file_banner(theme);
                std::vector<std::string> info_lines;
                info_lines.emplace_back("Index   : " + std::to_string(index + 1) + "/" + std::to_string(tasks.size()));
                info_lines.emplace_back("Threads : " + std::to_string(worker_count));
                info_lines.emplace_back("File    : " + result.file_name);
                info_lines.emplace_back("Input   : " + task.relative_input);
                info_lines.emplace_back("Output  : " + result.output_dir);
                info_lines.emplace_back("Size    : " + (task.size_known ? format_size(task.size_bytes) : std::string("unknown")));
                print_box(std::cout, theme, "INFO / per-file details", info_lines);
                std::cout << theme.dim() << "[ YSMParser ] Preparing parser..." << theme.reset() << '\n';
            }

            std::unique_ptr<YSMParser> parser;
            try {
                if (!fs::exists(task.target_output_dir)) {
                    fs::create_directories(task.target_output_dir);
                }

                parser = YSMParserFactory::Create(utf8_input);
                parser->setVerbose(verbose);
                parser->setDebug(debug);
                parser->setFormatJson(formatJson);
                parser->setProfile(profile);
                const int version = parser->getYSGPVersion();

                if (verbose) {
                    std::cout << theme.accent() << "[ YSMParser ]" << theme.reset()
                        << " Detected version: " << theme.strong() << version << theme.reset() << '\n';
                    std::cout << theme.dim() << "[ YSMParser ] Parsing payload..." << theme.reset() << '\n';
                }

                auto stage_start = std::chrono::steady_clock::now();
                parser->parse();
                const double parse_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - stage_start
                ).count();
                if (profile) {
                    emit_profile_line(result.file_name, version, "parse_total", parse_ms);
                    for (const auto& entry : parser->profileEntries()) {
                        emit_profile_line(result.file_name, version, entry.stage, entry.milliseconds);
                    }
                }

                if (verbose) {
                    std::cout << theme.dim() << "[ YSMParser ] Exporting resources..." << theme.reset() << '\n';
                }
                stage_start = std::chrono::steady_clock::now();
                parser->saveToDirectory(result.output_dir);
                const double export_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - stage_start
                ).count();
                if (profile) {
                    emit_profile_line(result.file_name, version, "export", export_ms);
                }

                result.success = true;
            }
            catch (const std::exception& e) {
                result.error_message = e.what();
                if (debug && parser) {
                    try {
                        parser->saveToDirectory(result.output_dir);
                    }
                    catch (...) {
                    }
                }
            }
            catch (...) {
                result.error_message = "Caught an unknown or cross-boundary exception.";
                if (debug && parser) {
                    try {
                        parser->saveToDirectory(result.output_dir);
                    }
                    catch (...) {
                    }
                }
            }

            result.elapsed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - file_start).count();

            if (verbose) {
                if (result.success) {
                    print_box(std::cout, theme, "RESULT", {
                        "Status : SUCCESS",
                        "File   : " + result.file_name,
                        "Time   : " + format_seconds(result.elapsed_seconds),
                        "Saved  : " + result.output_dir
                    });
                }
                else {
                    print_box(std::cerr, theme, "RESULT", {
                        "Status : FAILED",
                        "File   : " + result.file_name,
                        std::string("Error  : ") + result.error_message
                    });
                }
            }

            return result;
        };

        {
            std::unique_ptr<ScopedStdoutSilencer> quiet_stdout;
            if (!verbose) {
                quiet_stdout = std::make_unique<ScopedStdoutSilencer>(true);
            }

            if (worker_count == 1) {
                for (std::size_t index = 0; index < tasks.size(); ++index) {
                    if (!verbose) {
                        progress.begin_file(PathUtils::path_to_utf8(tasks[index].input_file.filename()));
                    }

                    results[index] = process_task(index);

                    if (!verbose && !results[index].success) {
                        progress.log_line("[ YSMParser ] Failed: " + results[index].file_name + " -> " + results[index].error_message);
                    }
                    if (!verbose) {
                        progress.finish_file();
                    }
                }
            }
            else {
                std::atomic<std::size_t> next_index{ 0 };
                std::vector<std::thread> workers;
                workers.reserve(worker_count);

                for (unsigned int worker = 0; worker < worker_count; ++worker) {
                    workers.emplace_back([&]() {
                        while (true) {
                            const std::size_t index = next_index.fetch_add(1);
                            if (index >= tasks.size()) {
                                break;
                            }

                            progress.begin_file(PathUtils::path_to_utf8(tasks[index].input_file.filename()));
                            results[index] = process_task(index);

                            if (!results[index].success) {
                                progress.log_line("[ YSMParser ] Failed: " + results[index].file_name + " -> " + results[index].error_message);
                            }
                            progress.finish_file();
                        }
                    });
                }

                for (auto& worker : workers) {
                    worker.join();
                }
            }
        }

        for (const auto& result : results) {
            if (result.success) {
                ++summary.succeeded;
            }
            else {
                ++summary.failed;
            }
        }

        const double batch_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - batch_start).count();

        if (!verbose) {
            progress.stop();
        }

        print_box(std::cout, theme, "SUMMARY", {
            "Total    : " + std::to_string(summary.total),
            "Success  : " + std::to_string(summary.succeeded),
            "Failed   : " + std::to_string(summary.failed),
            "Threads  : " + std::to_string(worker_count),
            "Input    : " + PathUtils::path_to_utf8(input_root),
            "Output   : " + PathUtils::path_to_utf8(output_root),
            "Duration : " + format_seconds(batch_seconds)
        });
    }
    catch (const std::exception& e) {
        std::cerr << "[ YSMParser ] Critical Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
