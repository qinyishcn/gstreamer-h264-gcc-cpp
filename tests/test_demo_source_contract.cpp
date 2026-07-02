#include "test_support.h"

#include <fstream>
#include <iterator>
#include <string>

namespace {

bool Contains(const std::string& source, const char* text) {
    return source.find(text) != std::string::npos;
}

}  // namespace

int main(int argc, char* argv[]) {
    CHECK_TRUE(argc == 2);
    std::ifstream input(argv[1], std::ios::binary);
    CHECK_TRUE(input.good());
    const std::string source{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};

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
    return 0;
}
