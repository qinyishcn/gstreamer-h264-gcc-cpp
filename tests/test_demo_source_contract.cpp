#include "test_support.h"

#include <fstream>
#include <iterator>
#include <string>

namespace {

bool Contains(const std::string& source, const char* text) {
    return source.find(text) != std::string::npos;
}

size_t Count(const std::string& source, const char* text) {
    size_t count = 0U;
    size_t position = 0U;
    while ((position = source.find(text, position)) != std::string::npos) {
        ++count;
        position += std::char_traits<char>::length(text);
    }
    return count;
}

std::string Read(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.good()) {
        std::fprintf(stderr, "could not read %s\n", path);
        std::exit(EXIT_FAILURE);
    }
    return std::string{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}

}  // namespace

int main(int argc, char* argv[]) {
    CHECK_TRUE(argc == 3);
    for (int index = 1; index < argc; ++index) {
        const std::string source = Read(argv[index]);

        CHECK_TRUE(!Contains(source, "probe_acked_kbps = std::max"));
        CHECK_TRUE(!Contains(
            source, "probe_acked_kbps = stats.acked_bitrate_kbps"));
        CHECK_TRUE(Contains(
            source, "probe_acked_kbps =\n"
                    "                static_cast<uint32_t>(loss_adjusted_delivery)"));
    }
    const std::string source = Read(argv[2]);
    CHECK_TRUE(!Contains(source, "razor_tx.on_send(tx_seq, 1400)"));
    CHECK_TRUE(!Contains(source, "ReceivedFeedback"));
    CHECK_TRUE(!Contains(source, "smoothed_loss"));
    CHECK_TRUE(!Contains(source, "\"udpsink\""));
    CHECK_TRUE(Contains(source, "TransportSender"));
    CHECK_TRUE(Contains(source, "RatePolicy"));
    CHECK_TRUE(Contains(source, "ProbeController"));
    CHECK_TRUE(Contains(source, "FeedbackPacket"));
    CHECK_TRUE(Contains(source, "MediaSequenceTranslator"));
    CHECK_TRUE(Contains(source, "demo_stats encoder_kbps="));
    CHECK_TRUE(Count(source, "razor_tx.set_bitrates(") >= 2U);
    return 0;
}
