#include "test_support.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

#ifdef _WIN32
using Socket = SOCKET;
using SocketLength = int;
const Socket kInvalidSocket = INVALID_SOCKET;
void CloseSocket(Socket socket) { closesocket(socket); }
int ProcessId() { return _getpid(); }
#else
using Socket = int;
using SocketLength = socklen_t;
const Socket kInvalidSocket = -1;
void CloseSocket(Socket socket) { close(socket); }
int ProcessId() { return static_cast<int>(getpid()); }
#endif

uint16_t PickPortPair() {
    for (int attempt = 0; attempt < 100; ++attempt) {
        Socket first = socket(AF_INET, SOCK_DGRAM, 0);
        if (first == kInvalidSocket) continue;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(first, reinterpret_cast<sockaddr*>(&address),
                 sizeof(address)) != 0) {
            CloseSocket(first);
            continue;
        }
        SocketLength size = sizeof(address);
        if (getsockname(first, reinterpret_cast<sockaddr*>(&address),
                        &size) != 0) {
            CloseSocket(first);
            continue;
        }
        const uint16_t port = ntohs(address.sin_port);
        if (port == 0 || port == 65535) {
            CloseSocket(first);
            continue;
        }
        Socket second = socket(AF_INET, SOCK_DGRAM, 0);
        if (second == kInvalidSocket) {
            CloseSocket(first);
            continue;
        }
        address.sin_port = htons(static_cast<uint16_t>(port + 1));
        const bool pair_available =
            bind(second, reinterpret_cast<sockaddr*>(&address),
                 sizeof(address)) == 0;
        CloseSocket(second);
        CloseSocket(first);
        if (pair_available) return port;
    }
    return 0;
}

std::string ShellQuote(const std::string& value) {
#ifdef _WIN32
    std::string quoted = "\"";
    for (char character : value) {
        if (character == '"') quoted += '\\';
        quoted += character;
    }
    return quoted + '"';
#else
    std::string quoted = "'";
    for (char character : value) {
        quoted += character == '\'' ? "'\\''" : std::string(1, character);
    }
    return quoted + '\'';
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

std::map<std::string, uint64_t> ParseStats(const std::string& line) {
    std::map<std::string, uint64_t> fields;
    std::istringstream stream(line);
    std::string token;
    stream >> token;
    while (stream >> token) {
        const size_t separator = token.find('=');
        if (separator == std::string::npos) continue;
        const std::string key = token.substr(0, separator);
        const std::string value = token.substr(separator + 1);
        char* end = nullptr;
        const unsigned long long number =
            std::strtoull(value.c_str(), &end, 10);
        if (end != value.c_str() && *end == '\0') {
            fields[key] = static_cast<uint64_t>(number);
        }
    }
    return fields;
}

uint64_t Require(const std::map<std::string, uint64_t>& fields,
                 const char* name) {
    const auto found = fields.find(name);
    CHECK_TRUE(found != fields.end());
    return found->second;
}

size_t Count(const std::string& text, const std::string& needle) {
    size_t count = 0U;
    size_t position = 0U;
    while ((position = text.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

}  // namespace

int main(int argc, char* argv[]) {
    CHECK_TRUE(argc == 2);
#ifdef _WIN32
    WSADATA data;
    CHECK_TRUE(WSAStartup(MAKEWORD(2, 2), &data) == 0);
#endif
    const uint16_t port = PickPortPair();
#ifdef _WIN32
    WSACleanup();
#endif
    CHECK_TRUE(port != 0);

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
    const uint64_t nonce = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string output =
        root + "/gcc-demo-runtime-" + std::to_string(ProcessId()) + "-" +
        std::to_string(nonce) + ".log";
    struct RemoveOutput {
        std::string path;
        ~RemoveOutput() { std::remove(path.c_str()); }
    } remove_output{output};

    const std::string command =
        ShellQuote(argv[1]) + " --duration 5 --bitrate 1000 --port " +
        std::to_string(port) +
        " --probe-first-scale 2.25 --probe-next-scale 1.75"
        " --probe-duration-ms 120 --probe-success-ratio 0.80"
        " --probe-retry-ms 2500 --probe-max-kbps 7000 >" +
        ShellQuote(output) + " 2>&1";
    const int status = std::system(command.c_str());

    std::ifstream input(output, std::ios::binary);
    CHECK_TRUE(input.good());
    const std::string log{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    if (ExitCode(status) != 0) std::fprintf(stderr, "%s", log.c_str());
    CHECK_TRUE(ExitCode(status) == 0);
    CHECK_TRUE(log.find(
        "probe_config first_scale=2.250 next_scale=1.750 "
        "duration_ms=120 success_ratio=0.800 retry_ms=2500 "
        "max_kbps=7000") != std::string::npos);
    CHECK_TRUE(log.find(
        "gcc_transition event=probe_state from=idle to=probing "
        "probe_kbps=2250") != std::string::npos);
    CHECK_TRUE(Count(
        log, "gcc_transition event=rate reason=normal") <= 6U);

    const std::string marker = "demo_stats ";
    const size_t stats_start = log.rfind(marker);
    CHECK_TRUE(stats_start != std::string::npos);
    const size_t stats_end = log.find('\n', stats_start);
    const std::string stats_line =
        log.substr(stats_start, stats_end - stats_start);
    const std::map<std::string, uint64_t> fields = ParseStats(stats_line);
    Require(fields, "encoder_kbps");
    Require(fields, "target_kbps");
    Require(fields, "pacing_kbps");
    Require(fields, "acked_kbps");
    Require(fields, "loss_q8");
    Require(fields, "rtt_ms");
    CHECK_TRUE(Require(fields, "media_sent") > 100U);
    Require(fields, "probe_sent");
    CHECK_TRUE(Require(fields, "probe_cluster_sent") > 0U);
    CHECK_TRUE(Require(fields, "feedback") > 50U);
    CHECK_TRUE(Require(fields, "feedback_matched") > 50U);
    CHECK_TRUE(Require(fields, "frames") > 0U);
    CHECK_TRUE(Require(fields, "arrivals") > 0U);
    CHECK_TRUE(Require(fields, "arrival_overflow") == 0U);
    CHECK_TRUE(Require(fields, "media_reorder_drops") == 0U);
    CHECK_TRUE(Require(fields, "ingress_bytes") == 0U);
    CHECK_TRUE(Require(fields, "pacer_queue_ms") == 0U);
    CHECK_TRUE(stats_line.find("probe_state=") != std::string::npos);

    std::string lower = log;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    CHECK_TRUE(lower.find("fu jump") == std::string::npos);
    CHECK_TRUE(lower.find("data lost") == std::string::npos);
    CHECK_TRUE(lower.find("map failure") == std::string::npos);
    CHECK_TRUE(lower.find("gstreamer-critical") == std::string::npos);
    CHECK_TRUE(lower.find("gstreamer critical") == std::string::npos);
    CHECK_TRUE(lower.find("razor decode error") == std::string::npos);
    CHECK_TRUE(lower.find("razor: decode error") == std::string::npos);
    CHECK_TRUE(lower.find("timed out draining") == std::string::npos);
    CHECK_TRUE(lower.find("drain timeout") == std::string::npos);
    return 0;
}
