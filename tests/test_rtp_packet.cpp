#include "rtp_packet.h"
#include "test_support.h"

#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {

std::vector<uint8_t> BasicPacket() {
    return {
        0x80, 0x60, 0x12, 0x34,
        0x01, 0x02, 0x03, 0x04,
        0x11, 0x22, 0x33, 0x44,
        0xde, 0xad,
    };
}

int TestParseBasicPacket() {
    const std::vector<uint8_t> packet = BasicPacket();
    gcc::RtpHeader header;

    CHECK_TRUE(gcc::ParseRtpHeader(packet.data(), packet.size(), header));
    CHECK_TRUE(!header.padding);
    CHECK_TRUE(!header.marker);
    CHECK_EQ(header.payload_type, static_cast<uint8_t>(96));
    CHECK_EQ(header.sequence, static_cast<uint16_t>(0x1234));
    CHECK_EQ(header.timestamp, static_cast<uint32_t>(0x01020304));
    CHECK_EQ(header.ssrc, static_cast<uint32_t>(0x11223344));
    CHECK_EQ(header.header_size, static_cast<size_t>(12));
    return EXIT_SUCCESS;
}

int TestRewriteSequence() {
    std::vector<uint8_t> packet = BasicPacket();
    CHECK_TRUE(gcc::RewriteRtpSequence(packet, 0xabcd));

    gcc::RtpHeader header;
    CHECK_TRUE(gcc::ParseRtpHeader(packet.data(), packet.size(), header));
    CHECK_EQ(header.sequence, static_cast<uint16_t>(0xabcd));
    return EXIT_SUCCESS;
}

int TestProbePacket() {
    gcc::RtpHeader source;
    source.payload_type = 96;
    source.timestamp = 0x01020304;
    source.ssrc = 0x11223344;

    const std::vector<uint8_t> packet =
        gcc::BuildProbeRtpPacket(source, 0x2222, 1200);
    CHECK_EQ(packet.size(), static_cast<size_t>(1200));

    gcc::RtpHeader parsed;
    CHECK_TRUE(gcc::ParseRtpHeader(packet.data(), packet.size(), parsed));
    CHECK_TRUE(parsed.padding);
    CHECK_EQ(parsed.payload_type, source.payload_type);
    CHECK_EQ(parsed.timestamp, source.timestamp);
    CHECK_EQ(parsed.ssrc, source.ssrc);
    CHECK_EQ(parsed.sequence, static_cast<uint16_t>(0x2222));
    CHECK_EQ(packet[parsed.header_size], static_cast<uint8_t>('G'));
    CHECK_EQ(packet[parsed.header_size + 1], static_cast<uint8_t>('C'));
    CHECK_EQ(packet[parsed.header_size + 2], static_cast<uint8_t>('C'));
    CHECK_EQ(packet[parsed.header_size + 3], static_cast<uint8_t>('P'));
    CHECK_EQ(packet.back(), static_cast<uint8_t>(1));
    CHECK_TRUE(gcc::IsProbeRtpPacket(packet.data(), packet.size()));

    std::vector<uint8_t> wrong_magic = packet;
    wrong_magic[12] = 'X';
    CHECK_TRUE(!gcc::IsProbeRtpPacket(wrong_magic.data(), wrong_magic.size()));

    const std::vector<uint8_t> minimum_packet =
        gcc::BuildProbeRtpPacket(source, 1, 17);
    CHECK_EQ(minimum_packet.size(), static_cast<size_t>(17));
    CHECK_TRUE(gcc::IsProbeRtpPacket(
        minimum_packet.data(), minimum_packet.size()));

    const std::vector<uint8_t> maximum_packet =
        gcc::BuildProbeRtpPacket(source, 1, 1472);
    CHECK_EQ(maximum_packet.size(), static_cast<size_t>(1472));
    CHECK_TRUE(gcc::IsProbeRtpPacket(
        maximum_packet.data(), maximum_packet.size()));

    CHECK_TRUE(gcc::BuildProbeRtpPacket(source, 1, 16).empty());
    CHECK_TRUE(gcc::BuildProbeRtpPacket(source, 1, 1473).empty());
    return EXIT_SUCCESS;
}

int TestRejectsShortPacket() {
    std::vector<uint8_t> packet(11, 0);
    gcc::RtpHeader header;
    CHECK_TRUE(!gcc::ParseRtpHeader(packet.data(), packet.size(), header));
    CHECK_TRUE(!gcc::RewriteRtpSequence(packet, 1));
    return EXIT_SUCCESS;
}

int TestRejectsWrongVersion() {
    std::vector<uint8_t> packet = BasicPacket();
    packet[0] = 0x40;
    gcc::RtpHeader header;
    CHECK_TRUE(!gcc::ParseRtpHeader(packet.data(), packet.size(), header));
    return EXIT_SUCCESS;
}

int TestCsrcBounds() {
    std::vector<uint8_t> packet = BasicPacket();
    packet[0] = 0x81;
    packet.resize(12);
    gcc::RtpHeader header;
    CHECK_TRUE(!gcc::ParseRtpHeader(packet.data(), packet.size(), header));

    packet.resize(16, 0);
    CHECK_TRUE(gcc::ParseRtpHeader(packet.data(), packet.size(), header));
    CHECK_EQ(header.header_size, static_cast<size_t>(16));
    return EXIT_SUCCESS;
}

int TestExtensionBounds() {
    std::vector<uint8_t> packet = BasicPacket();
    packet[0] = 0x90;
    packet.resize(15);
    gcc::RtpHeader header;
    CHECK_TRUE(!gcc::ParseRtpHeader(packet.data(), packet.size(), header));

    packet.resize(16, 0);
    packet[14] = 0;
    packet[15] = 1;
    CHECK_TRUE(!gcc::ParseRtpHeader(packet.data(), packet.size(), header));

    packet.resize(20, 0);
    CHECK_TRUE(gcc::ParseRtpHeader(packet.data(), packet.size(), header));
    CHECK_EQ(header.header_size, static_cast<size_t>(20));
    return EXIT_SUCCESS;
}

int TestPaddingBounds() {
    std::vector<uint8_t> packet = BasicPacket();
    packet[0] = 0xa0;
    packet.back() = 0;
    gcc::RtpHeader header;
    CHECK_TRUE(!gcc::ParseRtpHeader(packet.data(), packet.size(), header));

    packet.back() = 3;
    CHECK_TRUE(!gcc::ParseRtpHeader(packet.data(), packet.size(), header));

    packet.push_back(2);
    CHECK_TRUE(gcc::ParseRtpHeader(packet.data(), packet.size(), header));
    CHECK_TRUE(header.padding);
    return EXIT_SUCCESS;
}

int TestProbeRecognitionUsesValidatedHeaderSize() {
    std::vector<uint8_t> packet(21, 0);
    packet[0] = 0xa1;
    packet[1] = 96;
    packet[16] = 'G';
    packet[17] = 'C';
    packet[18] = 'C';
    packet[19] = 'P';
    packet[20] = 1;
    CHECK_TRUE(gcc::IsProbeRtpPacket(packet.data(), packet.size()));

    packet[0] = 0x81;
    CHECK_TRUE(!gcc::IsProbeRtpPacket(packet.data(), packet.size()));
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    CHECK_EQ(TestParseBasicPacket(), EXIT_SUCCESS);
    CHECK_EQ(TestRewriteSequence(), EXIT_SUCCESS);
    CHECK_EQ(TestProbePacket(), EXIT_SUCCESS);
    CHECK_EQ(TestRejectsShortPacket(), EXIT_SUCCESS);
    CHECK_EQ(TestRejectsWrongVersion(), EXIT_SUCCESS);
    CHECK_EQ(TestCsrcBounds(), EXIT_SUCCESS);
    CHECK_EQ(TestExtensionBounds(), EXIT_SUCCESS);
    CHECK_EQ(TestPaddingBounds(), EXIT_SUCCESS);
    CHECK_EQ(TestProbeRecognitionUsesValidatedHeaderSize(), EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
