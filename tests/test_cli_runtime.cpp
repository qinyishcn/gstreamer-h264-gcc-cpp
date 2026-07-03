#include "test_support.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

std::string Quote(const std::string& value) {
#ifdef _WIN32
    return "\"" + value + "\"";
#else
    std::string quoted = "'";
    for (char character : value) {
        quoted += character == '\'' ? "'\\''" : std::string(1, character);
    }
    return quoted + "'";
#endif
}

int ProcessId() {
#ifdef _WIN32
    return _getpid();
#else
    return static_cast<int>(getpid());
#endif
}

int ExitCode(int status) {
#ifdef _WIN32
    return status;
#else
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
#endif
}

std::string Read(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.good()) {
        std::fprintf(stderr, "could not read %s\n", path.c_str());
        std::exit(EXIT_FAILURE);
    }
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

struct Result {
    int exit_code;
    std::string output;
    std::string error;
};

Result Run(const std::string& executable, const std::string& arguments,
           const std::string& name) {
    const char* temporary_root =
#ifdef _WIN32
        std::getenv("TEMP");
#else
        std::getenv("TMPDIR");
#endif
    const std::string root =
        temporary_root != nullptr ? temporary_root :
#ifdef _WIN32
        ".";
#else
        "/tmp";
#endif
    const std::string prefix =
        root + "/gcc-cli-" + std::to_string(ProcessId()) + "-" + name;
    const std::string output_path = prefix + ".out";
    const std::string error_path = prefix + ".err";
    const std::string command =
        Quote(executable) + " " + arguments + " >" + Quote(output_path) +
        " 2>" + Quote(error_path);
    const int status = std::system(command.c_str());
    const Result result{
        ExitCode(status), Read(output_path), Read(error_path)};
    std::remove(output_path.c_str());
    std::remove(error_path.c_str());
    return result;
}

int CheckExecutable(const std::string& executable,
                    const std::string& name, int port) {
    const std::string safe_tail =
        " --duration 1 --port " + std::to_string(port);
    const Result help = Run(
        executable, "--help" + safe_tail, name + "-help");
    CHECK_TRUE(help.exit_code == 0);
    CHECK_TRUE(help.output.find("Usage:") != std::string::npos);
    const char* options[] = {
        "--probe-first-scale", "--probe-next-scale",
        "--probe-duration-ms", "--probe-success-ratio",
        "--probe-retry-ms", "--probe-max-kbps"};
    for (const char* option : options) {
        CHECK_TRUE(help.output.find(option) != std::string::npos);
    }
    CHECK_TRUE(help.output.find("0 disables") != std::string::npos);
    CHECK_TRUE(help.output.find("Feedback") == std::string::npos);
    CHECK_TRUE(help.output.find("_stats") == std::string::npos);
    CHECK_TRUE(help.error.empty());

    const Result unknown = Run(
        executable, "--unknown" + safe_tail, name + "-unknown");
    CHECK_TRUE(unknown.exit_code != 0);
    CHECK_TRUE(unknown.error.find("unknown option") != std::string::npos);

    const Result missing = Run(
        executable,
        "--probe-first-scale --duration 1 --port " + std::to_string(port),
        name + "-missing");
    CHECK_TRUE(missing.exit_code != 0);
    CHECK_TRUE(missing.error.find("missing value") != std::string::npos);
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char* argv[]) {
    CHECK_TRUE(argc == 3);
    CHECK_TRUE(CheckExecutable(argv[1], "sender", 46120) == EXIT_SUCCESS);
    CHECK_TRUE(CheckExecutable(argv[2], "demo", 46130) == EXIT_SUCCESS);
    return 0;
}
