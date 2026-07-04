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

    CHECK_TRUE(gcc::RewriteRtpSequence(
        packet.data(), packet.size(), 0x0102));
    CHECK_EQ(packet[2], static_cast<uint8_t>(0x01));
    CHECK_EQ(packet[3], static_cast<uint8_t>(0x02));
    return EXIT_SUCCESS;
}

#define CHECK_TRANSLATION(translator, input, expected)                        \
    do {                                                                      \
        uint16_t translated_sequence = 0U;                                    \
        CHECK_TRUE((translator).TranslateMedia(                               \
            static_cast<uint16_t>(input), translated_sequence));              \
        CHECK_EQ(                                                             \
            translated_sequence, static_cast<uint16_t>(expected));            \
    } while (false)

int TestMediaSequenceTranslation() {
    gcc::MediaSequenceTranslator translator;
    CHECK_TRANSLATION(translator, 100, 100);
    translator.OnProbe(101);
    translator.OnProbe(102);
    CHECK_TRANSLATION(translator, 103, 101);

    gcc::MediaSequenceTranslator media_gap;
    CHECK_TRANSLATION(media_gap, 100, 100);
    CHECK_TRANSLATION(media_gap, 102, 102);

    gcc::MediaSequenceTranslator duplicate_and_reordered;
    CHECK_TRANSLATION(duplicate_and_reordered, 100, 100);
    duplicate_and_reordered.OnProbe(101);
    duplicate_and_reordered.OnProbe(101);
    duplicate_and_reordered.OnProbe(99);
    CHECK_TRANSLATION(duplicate_and_reordered, 102, 101);

    gcc::MediaSequenceTranslator wrap;
    CHECK_TRANSLATION(wrap, 0xfffe, 0xfffe);
    wrap.OnProbe(0xffff);
    wrap.OnProbe(0x0000);
    CHECK_TRANSLATION(wrap, 0x0001, 0xffff);
    return EXIT_SUCCESS;
}

int TestMediaSequenceTranslationHandlesProbeReordering() {
    gcc::MediaSequenceTranslator skips_only_earlier_probes;
    CHECK_TRANSLATION(skips_only_earlier_probes, 100, 100);
    skips_only_earlier_probes.OnProbe(101);
    skips_only_earlier_probes.OnProbe(103);
    CHECK_TRANSLATION(skips_only_earlier_probes, 102, 101);
    CHECK_TRANSLATION(skips_only_earlier_probes, 104, 102);

    gcc::MediaSequenceTranslator reordered_probes;
    CHECK_TRANSLATION(reordered_probes, 100, 100);
    reordered_probes.OnProbe(102);
    reordered_probes.OnProbe(101);
    CHECK_TRANSLATION(reordered_probes, 103, 101);
    return EXIT_SUCCESS;
}

int TestMediaSequenceTranslationPreservesPublishedMappings() {
    gcc::MediaSequenceTranslator future_probe;
    CHECK_TRANSLATION(future_probe, 100, 100);
    future_probe.OnProbe(105);
    CHECK_TRANSLATION(future_probe, 102, 102);
    CHECK_TRANSLATION(future_probe, 106, 105);

    gcc::MediaSequenceTranslator late_probe;
    CHECK_TRANSLATION(late_probe, 100, 100);
    CHECK_TRANSLATION(late_probe, 103, 103);
    late_probe.OnProbe(102);
    CHECK_TRANSLATION(late_probe, 104, 104);

    gcc::MediaSequenceTranslator reordered_media;
    CHECK_TRANSLATION(reordered_media, 100, 100);
    CHECK_TRANSLATION(reordered_media, 105, 105);
    CHECK_TRANSLATION(reordered_media, 103, 103);
    reordered_media.OnProbe(104);
    CHECK_TRANSLATION(reordered_media, 106, 106);

    gcc::MediaSequenceTranslator duplicate_probe;
    CHECK_TRANSLATION(duplicate_probe, 100, 100);
    duplicate_probe.OnProbe(101);
    duplicate_probe.OnProbe(101);
    CHECK_TRANSLATION(duplicate_probe, 102, 101);

    gcc::MediaSequenceTranslator media_gap;
    CHECK_TRANSLATION(media_gap, 100, 100);
    media_gap.OnProbe(101);
    CHECK_TRANSLATION(media_gap, 104, 103);
    return EXIT_SUCCESS;
}

int TestMediaSequenceTranslationHandlesWrapAndEviction() {
    gcc::MediaSequenceTranslator wrap;
    CHECK_TRANSLATION(wrap, 65533, 65533);
    wrap.OnProbe(0);
    wrap.OnProbe(65534);
    CHECK_TRANSLATION(wrap, 65535, 65534);
    CHECK_TRANSLATION(wrap, 1, 65535);

    gcc::MediaSequenceTranslator eviction;
    CHECK_TRANSLATION(eviction, 0, 0);
    for (uint16_t sequence = 1; sequence <= 5000; ++sequence) {
        eviction.OnProbe(sequence);
    }
    uint16_t untranslated = 123U;
    CHECK_TRUE(!eviction.TranslateMedia(0, untranslated));
    CHECK_EQ(untranslated, static_cast<uint16_t>(123));
    CHECK_TRANSLATION(eviction, 5001, 1);
    CHECK_TRANSLATION(eviction, 5002, 2);

    gcc::MediaSequenceTranslator boundary;
    CHECK_TRANSLATION(boundary, 0, 0);
    for (uint16_t sequence = 1; sequence <= 5000; ++sequence) {
        boundary.OnProbe(sequence);
    }
    CHECK_TRUE(!boundary.TranslateMedia(904, untranslated));
    CHECK_TRANSLATION(boundary, 905, 1);
    return EXIT_SUCCESS;
}

int TestMediaSequenceTranslationStartsNearWrap() {
    gcc::MediaSequenceTranslator media_first;
    CHECK_TRANSLATION(media_first, 1, 1);
    media_first.OnProbe(65535);
    CHECK_TRANSLATION(media_first, 2, 2);

    gcc::MediaSequenceTranslator probe_before_wrap;
    probe_before_wrap.OnProbe(65535);
    CHECK_TRANSLATION(probe_before_wrap, 0, 65535);

    gcc::MediaSequenceTranslator probes_across_wrap;
    probes_across_wrap.OnProbe(0);
    probes_across_wrap.OnProbe(65535);
    CHECK_TRANSLATION(probes_across_wrap, 1, 65535);
    return EXIT_SUCCESS;
}

int TestProbePacket() {
    gcc::RtpHeader source;
    source.payload_type = 96;
    source.timestamp = 0x01020304;
    source.ssrc = 0x11223344;

    const std::vector<uint8_t> packet =
        gcc::BuildProbeRtpPacket(source, 0x2222, 267);
    CHECK_EQ(packet.size(), static_cast<size_t>(267));

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
    CHECK_EQ(packet.back(), static_cast<uint8_t>(255));
    CHECK_EQ(
        packet.size() - static_cast<size_t>(packet.back()),
        parsed.header_size);
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
        gcc::BuildProbeRtpPacket(source, 1, 1200);
    CHECK_EQ(maximum_packet.size(), static_cast<size_t>(1200));
    CHECK_TRUE(gcc::IsProbeRtpPacket(
        maximum_packet.data(), maximum_packet.size()));
    CHECK_EQ(maximum_packet.back(), static_cast<uint8_t>(255));
    CHECK_EQ(maximum_packet[945], static_cast<uint8_t>('G'));

    std::vector<uint8_t> payload_then_padding = packet;
    payload_then_padding.back() = 1;
    CHECK_TRUE(!gcc::IsProbeRtpPacket(
        payload_then_padding.data(), payload_then_padding.size()));

    CHECK_TRUE(gcc::BuildProbeRtpPacket(source, 1, 16).empty());
    CHECK_TRUE(gcc::BuildProbeRtpPacket(source, 1, 1201).empty());
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
    packet[20] = 5;
    CHECK_TRUE(gcc::IsProbeRtpPacket(packet.data(), packet.size()));

    packet[0] = 0x81;
    CHECK_TRUE(!gcc::IsProbeRtpPacket(packet.data(), packet.size()));
    return EXIT_SUCCESS;
}

}  // namespace

int main() {
    CHECK_EQ(TestParseBasicPacket(), EXIT_SUCCESS);
    CHECK_EQ(TestRewriteSequence(), EXIT_SUCCESS);
    CHECK_EQ(TestMediaSequenceTranslation(), EXIT_SUCCESS);
    CHECK_EQ(
        TestMediaSequenceTranslationHandlesProbeReordering(),
        EXIT_SUCCESS);
    CHECK_EQ(
        TestMediaSequenceTranslationPreservesPublishedMappings(),
        EXIT_SUCCESS);
    CHECK_EQ(
        TestMediaSequenceTranslationHandlesWrapAndEviction(),
        EXIT_SUCCESS);
    CHECK_EQ(
        TestMediaSequenceTranslationStartsNearWrap(),
        EXIT_SUCCESS);
    CHECK_EQ(TestProbePacket(), EXIT_SUCCESS);
    CHECK_EQ(TestRejectsShortPacket(), EXIT_SUCCESS);
    CHECK_EQ(TestRejectsWrongVersion(), EXIT_SUCCESS);
    CHECK_EQ(TestCsrcBounds(), EXIT_SUCCESS);
    CHECK_EQ(TestExtensionBounds(), EXIT_SUCCESS);
    CHECK_EQ(TestPaddingBounds(), EXIT_SUCCESS);
    CHECK_EQ(TestProbeRecognitionUsesValidatedHeaderSize(), EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
