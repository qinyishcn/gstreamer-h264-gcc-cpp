#include "test_support.h"

#include <fstream>
#include <iterator>
#include <string>

namespace {

std::string Read(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.good()) {
        std::fprintf(stderr, "could not read %s\n", path);
        std::exit(EXIT_FAILURE);
    }
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

bool Contains(const std::string& text, const char* value) {
    return text.find(value) != std::string::npos;
}

}  // namespace

int main(int argc, char* argv[]) {
    CHECK_TRUE(argc == 3);
    const std::string cmake = Read(argv[1]);
    const std::string readme = Read(argv[2]);

    CHECK_TRUE(Contains(cmake, "ENABLE_SANITIZERS"));
    CHECK_TRUE(Contains(cmake, "ENABLE_TSAN"));
    CHECK_TRUE(Contains(cmake, "-fsanitize=address,undefined"));
    CHECK_TRUE(Contains(cmake, "-fsanitize=thread"));

    CHECK_TRUE(Contains(readme, "协议 v4"));
    CHECK_TRUE(Contains(readme, "16 字节"));
    CHECK_TRUE(Contains(readme, "appsink"));
    CHECK_TRUE(Contains(readme, "TransportSender"));
    CHECK_TRUE(Contains(readme, "pacer"));
    CHECK_TRUE(Contains(readme, "C++14"));
    CHECK_TRUE(Contains(readme, "C11"));
    CHECK_TRUE(Contains(readme, "gcc_stats"));
    CHECK_TRUE(Contains(readme, "receiver_stats"));
    CHECK_TRUE(Contains(readme, "demo_stats"));
    CHECK_TRUE(Contains(readme, "gcc_transition"));
    CHECK_TRUE(!Contains(readme, "协议格式（v3）"));
    CHECK_TRUE(!Contains(readme, "→ udpsink"));
    CHECK_TRUE(!Contains(readme, "20% max step"));
    CHECK_TRUE(!Contains(readme, "RTT = now_ms - fb.rx_ts_ms"));
    CHECK_TRUE(Contains(
        readme,
        "docker run --rm --cap-add=NET_ADMIN h264-gcc-test"));

    return 0;
}
