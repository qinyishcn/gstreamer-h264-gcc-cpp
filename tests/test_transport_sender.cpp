#include "probe_delivery_estimator.h"
#include "transport_sender.h"
#include "test_support.h"

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace {

std::vector<uint8_t> MakePacket(
    size_t size, uint32_t timestamp = 0x01020304,
    uint32_t ssrc = 0x11223344, uint8_t payload_type = 96) {
    std::vector<uint8_t> packet(size, 0);
    if (size < 12) {
        return packet;
    }
    packet[0] = 0x80;
    packet[1] = payload_type;
    packet[4] = static_cast<uint8_t>(timestamp >> 24);
    packet[5] = static_cast<uint8_t>(timestamp >> 16);
    packet[6] = static_cast<uint8_t>(timestamp >> 8);
    packet[7] = static_cast<uint8_t>(timestamp);
    packet[8] = static_cast<uint8_t>(ssrc >> 24);
    packet[9] = static_cast<uint8_t>(ssrc >> 16);
    packet[10] = static_cast<uint8_t>(ssrc >> 8);
    packet[11] = static_cast<uint8_t>(ssrc);
    for (size_t i = 12; i < size; ++i) {
        packet[i] = static_cast<uint8_t>(i);
    }
    return packet;
}

int TestPreservesPacketsAndMetadata() {
    std::vector<std::vector<uint8_t>> datagrams;
    uint64_t now = 1000;
    gcc::TransportSender sender(
        [&datagrams](const uint8_t* data, size_t size) {
            datagrams.emplace_back(data, data + size);
            return true;
        },
        [&now]() { return now += 10; }, 4096);

    const std::vector<uint8_t> media =
        MakePacket(19, 0x12345678, 0xaabbccdd, 97);
    gcc::RtpHeader source;
    CHECK_TRUE(gcc::ParseRtpHeader(media.data(), media.size(), source));
    const std::vector<uint8_t> probe =
        gcc::BuildProbeRtpPacket(source, 42, 31);
    CHECK_TRUE(sender.EnqueueMedia(media));
    CHECK_TRUE(sender.EnqueueProbe(probe));
    CHECK_EQ(sender.ingress_bytes(), static_cast<size_t>(50));
    CHECK_EQ(sender.queued_bytes(), static_cast<size_t>(50));
    CHECK_EQ(sender.sent_packets(), static_cast<uint64_t>(0));

    std::vector<gcc::PacerEnqueue> paced;
    CHECK_EQ(sender.DrainIngress(
                 [&paced](uint32_t id, size_t size) {
                     paced.push_back({id, size});
                     return true;
                 }),
             static_cast<size_t>(2));
    CHECK_EQ(paced.size(), static_cast<size_t>(2));
    CHECK_EQ(paced[0].packet_id, static_cast<uint32_t>(0));
    CHECK_EQ(paced[0].size, static_cast<size_t>(19));
    CHECK_EQ(paced[1].packet_id, static_cast<uint32_t>(1));
    CHECK_EQ(paced[1].size, static_cast<size_t>(31));
    CHECK_EQ(sender.ingress_bytes(), static_cast<size_t>(0));
    CHECK_EQ(sender.pending_packets(), static_cast<size_t>(2));
    CHECK_EQ(sender.queued_bytes(), static_cast<size_t>(50));

    gcc::RtpHeader latest;
    CHECK_TRUE(sender.latest_media_header(latest));
    CHECK_EQ(latest.timestamp, static_cast<uint32_t>(0x12345678));
    CHECK_EQ(latest.ssrc, static_cast<uint32_t>(0xaabbccdd));
    CHECK_EQ(latest.payload_type, static_cast<uint8_t>(97));

    gcc::SentPacket first;
    gcc::SentPacket second;
    CHECK_TRUE(sender.SendPaced(paced[0].packet_id, first));
    CHECK_TRUE(sender.SendPaced(paced[1].packet_id, second));
    CHECK_EQ(first.transport_sequence, static_cast<uint16_t>(0));
    CHECK_EQ(first.size, static_cast<size_t>(19));
    CHECK_EQ(first.send_time_us, static_cast<uint64_t>(1010));
    CHECK_TRUE(!first.probe);
    CHECK_EQ(first.cumulative_sent_bytes, static_cast<uint64_t>(19));
    CHECK_EQ(first.generation, static_cast<uint64_t>(0));
    CHECK_EQ(second.transport_sequence, static_cast<uint16_t>(1));
    CHECK_EQ(second.size, static_cast<size_t>(31));
    CHECK_TRUE(second.probe);
    CHECK_EQ(second.cumulative_sent_bytes, static_cast<uint64_t>(50));
    CHECK_EQ(second.generation, static_cast<uint64_t>(1));
    gcc::SentPacketInfo lookup;
    CHECK_TRUE(sender.LookupSentPacket(1, lookup));
    CHECK_EQ(lookup.cumulative_sent_bytes, static_cast<uint64_t>(50));
    CHECK_TRUE(lookup.probe);
    CHECK_EQ(datagrams.size(), static_cast<size_t>(2));
    CHECK_EQ(datagrams[0].size(), static_cast<size_t>(19));
    CHECK_EQ(datagrams[1].size(), static_cast<size_t>(31));
    CHECK_EQ(datagrams[0][2], static_cast<uint8_t>(0));
    CHECK_EQ(datagrams[0][3], static_cast<uint8_t>(0));
    CHECK_EQ(datagrams[1][2], static_cast<uint8_t>(0));
    CHECK_EQ(datagrams[1][3], static_cast<uint8_t>(1));
    CHECK_EQ(datagrams[0][18], media[18]);
    CHECK_EQ(datagrams[1][30], probe[30]);
    CHECK_EQ(sender.queued_bytes(), static_cast<size_t>(0));
    CHECK_EQ(sender.sent_packets(), static_cast<uint64_t>(2));
    gcc::SentHistoryWatermark watermark;
    CHECK_TRUE(sender.GetSentHistoryWatermark(watermark));
    CHECK_TRUE(watermark.valid);
    CHECK_EQ(watermark.oldest_generation, static_cast<uint64_t>(0));
    CHECK_EQ(watermark.oldest_cumulative_sent_bytes,
             static_cast<uint64_t>(19));
    CHECK_EQ(watermark.oldest_history_ordinal, static_cast<uint32_t>(0));
    CHECK_EQ(watermark.newest_generation, static_cast<uint64_t>(1));
    CHECK_EQ(watermark.cumulative_sent_bytes, static_cast<uint64_t>(50));
    CHECK_EQ(watermark.newest_history_ordinal, static_cast<uint32_t>(1));
    return EXIT_SUCCESS;
}

int TestNoPacketSamplingCap() {
    size_t sent = 0;
    gcc::TransportSender sender(
        [&sent](const uint8_t*, size_t) {
            ++sent;
            return true;
        },
        []() { return uint64_t{1}; }, 150 * 20);
    for (size_t i = 0; i < 150; ++i) {
        CHECK_TRUE(sender.EnqueueMedia(MakePacket(20)));
    }
    std::vector<uint32_t> ids;
    CHECK_EQ(sender.DrainIngress(
                 [&ids](uint32_t id, size_t size) {
                     if (size != 20) {
                         return false;
                     }
                     ids.push_back(id);
                     return true;
                 }),
             static_cast<size_t>(150));
    for (uint32_t id : ids) {
        gcc::SentPacket packet;
        CHECK_TRUE(sender.SendPaced(id, packet));
        CHECK_EQ(packet.size, static_cast<size_t>(20));
    }
    CHECK_EQ(sent, static_cast<size_t>(150));
    return EXIT_SUCCESS;
}

int TestHistoryWatermarkAdvancesAfterBoundedEviction() {
    constexpr size_t kPackets = 2001U;
    gcc::TransportSender sender(
        [](const uint8_t*, size_t) { return true; },
        []() { return uint64_t{1}; }, kPackets * 12U);
    for (size_t i = 0; i < kPackets; ++i) {
        CHECK_TRUE(sender.EnqueueMedia(MakePacket(12)));
    }
    std::vector<uint32_t> ids;
    CHECK_EQ(sender.DrainIngress(
                 [&ids](uint32_t id, size_t) {
                     ids.push_back(id);
                     return true;
                 }),
             kPackets);
    for (uint32_t id : ids) {
        gcc::SentPacket packet;
        CHECK_TRUE(sender.SendPaced(id, packet));
    }
    gcc::SentHistoryWatermark watermark;
    CHECK_TRUE(sender.GetSentHistoryWatermark(watermark));
    CHECK_EQ(watermark.oldest_generation, static_cast<uint64_t>(1));
    CHECK_EQ(watermark.oldest_history_ordinal, static_cast<uint32_t>(1));
    CHECK_EQ(watermark.newest_generation, static_cast<uint64_t>(2000));
    CHECK_EQ(watermark.newest_history_ordinal,
             static_cast<uint32_t>(2000));
    CHECK_EQ(watermark.cumulative_sent_bytes,
             static_cast<uint64_t>(kPackets * 12U));
    return EXIT_SUCCESS;
}

int TestNoFeedbackGuardClearsFromRealSendProgress() {
    constexpr size_t kMediaPackets = 2000U;
    gcc::TransportSender sender(
        [](const uint8_t*, size_t) { return true; },
        []() { return uint64_t{1}; },
        64U + kMediaPackets * 12U);
    gcc::RtpHeader source;
    const std::vector<uint8_t> media = MakePacket(12);
    CHECK_TRUE(gcc::ParseRtpHeader(media.data(), media.size(), source));
    CHECK_TRUE(sender.EnqueueProbe(
        gcc::BuildProbeRtpPacket(source, 0U, 64U)));
    uint32_t probe_id = 0U;
    CHECK_EQ(sender.DrainIngress(
                 [&probe_id](uint32_t id, size_t) {
                     probe_id = id;
                     return true;
                 }),
             static_cast<size_t>(1));
    gcc::SentPacket probe;
    CHECK_TRUE(sender.SendPaced(probe_id, probe));
    CHECK_TRUE(probe.probe);

    gcc::ProbeDeliveryEstimator estimator;
    estimator.Start(2000U);
    estimator.OnProbePacketSent(probe);
    estimator.Finish();
    CHECK_TRUE(estimator.rate_events_guarded());

    for (size_t i = 0; i < kMediaPackets; ++i) {
        CHECK_TRUE(sender.EnqueueMedia(media));
    }
    std::vector<uint32_t> ids;
    CHECK_EQ(sender.DrainIngress(
                 [&ids](uint32_t id, size_t) {
                     ids.push_back(id);
                     return true;
                 }),
             kMediaPackets);
    for (uint32_t id : ids) {
        gcc::SentPacket sent;
        CHECK_TRUE(sender.SendPaced(id, sent));
    }
    gcc::SentHistoryWatermark watermark;
    CHECK_TRUE(sender.GetSentHistoryWatermark(watermark));
    estimator.AdvanceSendProgress(watermark);
    const bool subsequent_heartbeat_rate_related =
        estimator.rate_events_guarded();
    CHECK_TRUE(!subsequent_heartbeat_rate_related);
    return EXIT_SUCCESS;
}

int TestGenerationSaturatesWithoutWrapping() {
    gcc::TransportSender sender(
        [](const uint8_t*, size_t) { return true; },
        []() { return uint64_t{1}; }, 36U, 0U,
        UINT64_MAX - 1U, UINT64_MAX - 20U, UINT32_MAX);
    for (size_t i = 0; i < 3U; ++i) {
        CHECK_TRUE(sender.EnqueueMedia(MakePacket(12)));
    }
    std::vector<uint32_t> ids;
    CHECK_EQ(sender.DrainIngress(
                 [&ids](uint32_t id, size_t) {
                     ids.push_back(id);
                     return true;
                 }),
             static_cast<size_t>(3));
    gcc::SentPacket first;
    gcc::SentPacket second;
    gcc::SentPacket third;
    CHECK_TRUE(sender.SendPaced(ids[0], first));
    CHECK_TRUE(sender.SendPaced(ids[1], second));
    CHECK_TRUE(sender.SendPaced(ids[2], third));
    CHECK_EQ(first.generation, UINT64_MAX - 1U);
    CHECK_EQ(second.generation, UINT64_MAX);
    CHECK_EQ(third.generation, UINT64_MAX);
    CHECK_EQ(first.history_ordinal, UINT32_MAX);
    CHECK_EQ(second.history_ordinal, static_cast<uint32_t>(0));
    CHECK_EQ(third.history_ordinal, static_cast<uint32_t>(1));
    CHECK_EQ(first.cumulative_sent_bytes, UINT64_MAX - 8U);
    CHECK_EQ(second.cumulative_sent_bytes, UINT64_MAX);
    CHECK_EQ(third.cumulative_sent_bytes, UINT64_MAX);
    gcc::SentHistoryWatermark watermark;
    CHECK_TRUE(sender.GetSentHistoryWatermark(watermark));
    CHECK_EQ(watermark.newest_generation, UINT64_MAX);
    CHECK_EQ(watermark.newest_history_ordinal, static_cast<uint32_t>(1));
    return EXIT_SUCCESS;
}

int TestIngressBoundAndValidation() {
    gcc::TransportSender sender(
        [](const uint8_t*, size_t) { return true; },
        []() { return uint64_t{1}; }, 20);
    CHECK_TRUE(sender.EnqueueMedia(MakePacket(20)));
    CHECK_TRUE(!sender.EnqueueMedia(MakePacket(12)));
    CHECK_TRUE(!sender.EnqueueProbe(std::vector<uint8_t>(11, 0)));
    CHECK_EQ(sender.ingress_bytes(), static_cast<size_t>(20));
    CHECK_EQ(sender.queued_bytes(), static_cast<size_t>(20));
    CHECK_EQ(sender.dropped_packets(), static_cast<uint64_t>(2));
    CHECK_EQ(sender.DrainIngress(
                 [](uint32_t, size_t size) {
                     return size == 20;
                 }),
             static_cast<size_t>(1));
    return EXIT_SUCCESS;
}

int TestPacerFailuresErasePackets() {
    gcc::TransportSender sender(
        [](const uint8_t*, size_t) { return true; },
        []() { return uint64_t{1}; }, 100);
    CHECK_TRUE(sender.EnqueueMedia(MakePacket(12)));
    CHECK_TRUE(sender.EnqueueMedia(MakePacket(13)));
    CHECK_TRUE(sender.EnqueueMedia(MakePacket(14)));
    size_t calls = 0;
    CHECK_EQ(sender.DrainIngress(
                 [&calls](uint32_t, size_t) {
                     ++calls;
                     if (calls == 2) {
                         throw std::runtime_error("pacer");
                     }
                     return calls == 3;
                 }),
             static_cast<size_t>(1));
    CHECK_EQ(sender.pending_packets(), static_cast<size_t>(1));
    CHECK_EQ(sender.queued_bytes(), static_cast<size_t>(14));
    CHECK_EQ(sender.dropped_packets(), static_cast<uint64_t>(2));
    gcc::SentPacket out;
    CHECK_TRUE(!sender.SendPaced(0, out));
    CHECK_TRUE(!sender.SendPaced(1, out));
    CHECK_TRUE(sender.SendPaced(2, out));
    CHECK_EQ(sender.queued_bytes(), static_cast<size_t>(0));
    CHECK_EQ(sender.sent_packets(), static_cast<uint64_t>(1));
    return EXIT_SUCCESS;
}

int TestSendFailuresEraseWithoutHistory() {
    size_t calls = 0;
    gcc::TransportSender sender(
        [&calls](const uint8_t*, size_t) {
            ++calls;
            if (calls == 2) {
                throw std::runtime_error("send");
            }
            return false;
        },
        []() { return uint64_t{99}; }, 100);
    CHECK_TRUE(sender.EnqueueMedia(MakePacket(12)));
    CHECK_TRUE(sender.EnqueueMedia(MakePacket(12)));
    std::vector<uint32_t> ids;
    CHECK_EQ(sender.DrainIngress(
                 [&ids](uint32_t id, size_t) {
                     ids.push_back(id);
                     return true;
                 }),
             static_cast<size_t>(2));
    gcc::SentPacket out{777, 888, 999, true};
    CHECK_TRUE(!sender.SendPaced(ids[0], out));
    CHECK_EQ(out.transport_sequence, static_cast<uint16_t>(777));
    CHECK_TRUE(!sender.SendPaced(ids[0], out));
    uint64_t time = 0;
    CHECK_TRUE(!sender.LookupSendTime(0, time));
    CHECK_TRUE(!sender.SendPaced(ids[1], out));
    CHECK_TRUE(!sender.LookupSendTime(1, time));
    gcc::SentPacketInfo info;
    CHECK_TRUE(!sender.LookupSentPacket(0, info));
    CHECK_EQ(sender.pending_packets(), static_cast<size_t>(0));
    CHECK_EQ(sender.queued_bytes(), static_cast<size_t>(0));
    CHECK_EQ(sender.dropped_packets(), static_cast<uint64_t>(2));
    CHECK_EQ(sender.sent_packets(), static_cast<uint64_t>(0));
    return EXIT_SUCCESS;
}

int TestClockFailurePreventsSend() {
    size_t sends = 0;
    gcc::TransportSender empty_clock(
        [&sends](const uint8_t*, size_t) {
            ++sends;
            return true;
        },
        {}, 100);
    CHECK_TRUE(empty_clock.EnqueueMedia(MakePacket(12)));
    uint32_t empty_id = 0;
    CHECK_EQ(empty_clock.DrainIngress(
                 [&empty_id](uint32_t id, size_t) {
                     empty_id = id;
                     return true;
                 }),
             static_cast<size_t>(1));
    gcc::SentPacket out;
    CHECK_TRUE(!empty_clock.SendPaced(empty_id, out));
    CHECK_EQ(sends, static_cast<size_t>(0));
    CHECK_EQ(empty_clock.dropped_packets(), static_cast<uint64_t>(1));
    CHECK_EQ(empty_clock.queued_bytes(), static_cast<size_t>(0));

    gcc::TransportSender throwing_clock(
        [&sends](const uint8_t*, size_t) {
            ++sends;
            return true;
        },
        []() -> uint64_t { throw std::runtime_error("clock"); }, 100);
    CHECK_TRUE(throwing_clock.EnqueueMedia(MakePacket(12)));
    uint32_t throwing_id = 0;
    CHECK_EQ(throwing_clock.DrainIngress(
                 [&throwing_id](uint32_t id, size_t) {
                     throwing_id = id;
                     return true;
                 }),
             static_cast<size_t>(1));
    CHECK_TRUE(!throwing_clock.SendPaced(throwing_id, out));
    CHECK_EQ(sends, static_cast<size_t>(0));
    CHECK_EQ(throwing_clock.dropped_packets(), static_cast<uint64_t>(1));
    CHECK_EQ(throwing_clock.sent_packets(), static_cast<uint64_t>(0));
    uint64_t time = 0;
    CHECK_TRUE(!throwing_clock.LookupSendTime(
        static_cast<uint16_t>(throwing_id), time));
    return EXIT_SUCCESS;
}

int TestReentrantPacerConsumptionIsAccepted() {
    gcc::TransportSender* sender_ptr = nullptr;
    size_t sent = 0;
    gcc::TransportSender sender(
        [&sent](const uint8_t*, size_t) {
            ++sent;
            return true;
        },
        []() { return uint64_t{55}; }, 100);
    sender_ptr = &sender;

    CHECK_TRUE(sender.EnqueueMedia(MakePacket(12)));
    CHECK_EQ(sender.DrainIngress(
                 [sender_ptr](uint32_t id, size_t) {
                     gcc::SentPacket out;
                     if (!sender_ptr->SendPaced(id, out)) {
                         return true;
                     }
                     return false;
                 }),
             static_cast<size_t>(1));
    CHECK_EQ(sender.dropped_packets(), static_cast<uint64_t>(0));

    CHECK_TRUE(sender.EnqueueMedia(MakePacket(13)));
    CHECK_EQ(sender.DrainIngress(
                 [sender_ptr](uint32_t id, size_t) -> bool {
                     gcc::SentPacket out;
                     if (!sender_ptr->SendPaced(id, out)) {
                         return true;
                     }
                     throw std::runtime_error("after consumption");
                 }),
             static_cast<size_t>(1));
    CHECK_EQ(sent, static_cast<size_t>(2));
    CHECK_EQ(sender.sent_packets(), static_cast<uint64_t>(2));
    CHECK_EQ(sender.dropped_packets(), static_cast<uint64_t>(0));
    CHECK_EQ(sender.pending_packets(), static_cast<size_t>(0));
    CHECK_EQ(sender.queued_bytes(), static_cast<size_t>(0));
    return EXIT_SUCCESS;
}

int TestProbeAdmissionRequiresGccProbe() {
    gcc::TransportSender sender(
        [](const uint8_t*, size_t) { return true; },
        []() { return uint64_t{1}; }, 100);
    const std::vector<uint8_t> media = MakePacket(20);
    CHECK_TRUE(!sender.EnqueueProbe(media));

    std::vector<uint8_t> arbitrary_padding = MakePacket(20);
    arbitrary_padding[0] |= 0x20;
    arbitrary_padding.back() = 1;
    gcc::RtpHeader padded_header;
    CHECK_TRUE(gcc::ParseRtpHeader(
        arbitrary_padding.data(), arbitrary_padding.size(), padded_header));
    CHECK_TRUE(!gcc::IsProbeRtpPacket(
        arbitrary_padding.data(), arbitrary_padding.size()));
    CHECK_TRUE(!sender.EnqueueProbe(arbitrary_padding));

    gcc::RtpHeader media_header;
    CHECK_TRUE(gcc::ParseRtpHeader(
        media.data(), media.size(), media_header));
    CHECK_TRUE(sender.EnqueueProbe(
        gcc::BuildProbeRtpPacket(media_header, 7, 20)));
    CHECK_EQ(sender.queued_bytes(), static_cast<size_t>(20));
    CHECK_EQ(sender.dropped_packets(), static_cast<uint64_t>(2));
    return EXIT_SUCCESS;
}

int TestReentrantSendCannotSendTwice() {
    gcc::TransportSender* sender_ptr = nullptr;
    uint32_t active_id = 0;
    bool nested_result = true;
    size_t sends = 0;
    gcc::TransportSender sender(
        [&](const uint8_t*, size_t) {
            ++sends;
            gcc::SentPacket nested;
            nested_result = sender_ptr->SendPaced(active_id, nested);
            return true;
        },
        []() { return uint64_t{5}; }, 100);
    sender_ptr = &sender;
    CHECK_TRUE(sender.EnqueueMedia(MakePacket(12)));
    CHECK_EQ(sender.DrainIngress(
                 [&active_id](uint32_t id, size_t) {
                     active_id = id;
                     return true;
                 }),
             static_cast<size_t>(1));
    gcc::SentPacket out;
    CHECK_TRUE(sender.SendPaced(active_id, out));
    CHECK_TRUE(!nested_result);
    CHECK_EQ(sends, static_cast<size_t>(1));
    CHECK_TRUE(!sender.SendPaced(active_id, out));
    CHECK_TRUE(!sender.SendPaced(active_id + 100, out));
    return EXIT_SUCCESS;
}

int TestSequenceWrapKeepsPacketIdsMonotonic() {
    gcc::TransportSender sender(
        [](const uint8_t*, size_t) { return true; },
        []() { return uint64_t{1}; }, 12);
    uint32_t previous_id = 0;
    for (uint32_t i = 0; i <= 65536; ++i) {
        CHECK_TRUE(sender.EnqueueMedia(MakePacket(12)));
        uint32_t id = 0;
        CHECK_EQ(sender.DrainIngress(
                     [&id](uint32_t packet_id, size_t) {
                         id = packet_id;
                         return true;
                     }),
                 static_cast<size_t>(1));
        CHECK_EQ(id, i);
        if (i != 0) {
            CHECK_EQ(id, previous_id + 1);
        }
        gcc::SentPacket out;
        CHECK_TRUE(sender.SendPaced(id, out));
        CHECK_EQ(out.transport_sequence, static_cast<uint16_t>(i));
        previous_id = id;
    }
    return EXIT_SUCCESS;
}

int TestPacketIdWrapsModuloUint32() {
    gcc::TransportSender sender(
        [](const uint8_t*, size_t) { return true; },
        []() { return uint64_t{1}; }, 24, UINT32_MAX);
    CHECK_TRUE(sender.EnqueueMedia(MakePacket(12)));
    CHECK_TRUE(sender.EnqueueMedia(MakePacket(12)));
    std::vector<uint32_t> ids;
    CHECK_EQ(sender.DrainIngress(
                 [&ids](uint32_t id, size_t) {
                     ids.push_back(id);
                     return true;
                 }),
             static_cast<size_t>(2));
    CHECK_EQ(ids[0], UINT32_MAX);
    CHECK_EQ(ids[1], static_cast<uint32_t>(0));
    gcc::SentPacket before_wrap;
    gcc::SentPacket after_wrap;
    CHECK_TRUE(sender.SendPaced(ids[0], before_wrap));
    CHECK_TRUE(sender.SendPaced(ids[1], after_wrap));
    CHECK_EQ(before_wrap.transport_sequence, UINT16_MAX);
    CHECK_EQ(after_wrap.transport_sequence, static_cast<uint16_t>(0));
    return EXIT_SUCCESS;
}

int TestHistoryEvictsAfterTwoThousand() {
    uint64_t now = 100;
    gcc::TransportSender sender(
        [](const uint8_t*, size_t) { return true; },
        [&now]() { return ++now; }, 12);
    for (uint32_t i = 0; i < 2001; ++i) {
        CHECK_TRUE(sender.EnqueueMedia(MakePacket(12)));
        uint32_t id = 0;
        CHECK_EQ(sender.DrainIngress(
                     [&id](uint32_t packet_id, size_t) {
                         id = packet_id;
                         return true;
                     }),
                 static_cast<size_t>(1));
        gcc::SentPacket out;
        CHECK_TRUE(sender.SendPaced(id, out));
    }
    uint64_t send_time = 0;
    CHECK_TRUE(!sender.LookupSendTime(0, send_time));
    CHECK_TRUE(sender.LookupSendTime(1, send_time));
    CHECK_EQ(send_time, static_cast<uint64_t>(102));
    CHECK_TRUE(sender.LookupSendTime(2000, send_time));
    CHECK_EQ(send_time, static_cast<uint64_t>(2101));
    return EXIT_SUCCESS;
}

int TestHistoryEvictionKeepsNewestWrappedSequence() {
    constexpr uint32_t kInitialPacketId = 65534;
    constexpr uint64_t kConstantSendTime = 777;
    gcc::TransportSender sender(
        [](const uint8_t*, size_t) { return true; },
        []() { return kConstantSendTime; }, 12, kInitialPacketId);

    CHECK_TRUE(sender.EnqueueMedia(MakePacket(12)));
    uint32_t first_id = 0;
    CHECK_EQ(sender.DrainIngress(
                 [&first_id](uint32_t packet_id, size_t) {
                     first_id = packet_id;
                     return true;
                 }),
             static_cast<size_t>(1));
    CHECK_EQ(first_id, kInitialPacketId);
    gcc::SentPacket first;
    CHECK_TRUE(sender.SendPaced(first_id, first));
    CHECK_EQ(first.transport_sequence, static_cast<uint16_t>(65534));

    // Advance the 32-bit ID without adding send-history entries. The next
    // accepted packet then has the same low 16-bit transport sequence.
    for (uint32_t i = 0; i < 65535; ++i) {
        CHECK_TRUE(sender.EnqueueMedia(MakePacket(12)));
        CHECK_EQ(sender.DrainIngress(
                     [](uint32_t, size_t) { return false; }),
                 static_cast<size_t>(0));
    }

    CHECK_TRUE(sender.EnqueueMedia(MakePacket(12)));
    uint32_t duplicate_id = 0;
    CHECK_EQ(sender.DrainIngress(
                 [&duplicate_id](uint32_t packet_id, size_t) {
                     duplicate_id = packet_id;
                     return true;
                 }),
             static_cast<size_t>(1));
    CHECK_EQ(duplicate_id, kInitialPacketId + 65536);
    gcc::SentPacket duplicate;
    CHECK_TRUE(sender.SendPaced(duplicate_id, duplicate));
    CHECK_EQ(duplicate.transport_sequence, first.transport_sequence);

    // Reach 2001 successful sends. Evicting the old generation must not
    // remove the newer map record with the same sequence and timestamp.
    for (uint32_t i = 0; i < 1999; ++i) {
        CHECK_TRUE(sender.EnqueueMedia(MakePacket(12)));
        uint32_t id = 0;
        CHECK_EQ(sender.DrainIngress(
                     [&id](uint32_t packet_id, size_t) {
                         id = packet_id;
                         return true;
                     }),
                 static_cast<size_t>(1));
        gcc::SentPacket out;
        CHECK_TRUE(sender.SendPaced(id, out));
    }

    uint64_t send_time = 0;
    CHECK_TRUE(sender.LookupSendTime(
        duplicate.transport_sequence, send_time));
    CHECK_EQ(send_time, kConstantSendTime);
    return EXIT_SUCCESS;
}

int TestUdpFactoryValidatesConfiguration() {
    CHECK_TRUE(!gcc::CreateUdpSendFunction("", 5000));
    CHECK_TRUE(!gcc::CreateUdpSendFunction("127.0.0.1", 0));
    CHECK_TRUE(!gcc::CreateUdpSendFunction("not-an-ipv4-address", 5000));
    const gcc::TransportSender::SendFunction send =
        gcc::CreateUdpSendFunction("127.0.0.1", 5000);
    CHECK_TRUE(static_cast<bool>(send));
    CHECK_TRUE(!send(nullptr, 1));
    CHECK_TRUE(!send(reinterpret_cast<const uint8_t*>(""), 0));
    return EXIT_SUCCESS;
}

static_assert(!std::is_copy_constructible<gcc::TransportSender>::value,
              "TransportSender must not be copy constructible");
static_assert(!std::is_copy_assignable<gcc::TransportSender>::value,
              "TransportSender must not be copy assignable");
static_assert(!std::is_move_constructible<gcc::TransportSender>::value,
              "TransportSender must not be move constructible");
static_assert(!std::is_move_assignable<gcc::TransportSender>::value,
              "TransportSender must not be move assignable");

}  // namespace

int main() {
    CHECK_EQ(TestPreservesPacketsAndMetadata(), EXIT_SUCCESS);
    CHECK_EQ(TestNoPacketSamplingCap(), EXIT_SUCCESS);
    CHECK_EQ(TestHistoryWatermarkAdvancesAfterBoundedEviction(),
             EXIT_SUCCESS);
    CHECK_EQ(TestNoFeedbackGuardClearsFromRealSendProgress(),
             EXIT_SUCCESS);
    CHECK_EQ(TestGenerationSaturatesWithoutWrapping(), EXIT_SUCCESS);
    CHECK_EQ(TestIngressBoundAndValidation(), EXIT_SUCCESS);
    CHECK_EQ(TestPacerFailuresErasePackets(), EXIT_SUCCESS);
    CHECK_EQ(TestSendFailuresEraseWithoutHistory(), EXIT_SUCCESS);
    CHECK_EQ(TestClockFailurePreventsSend(), EXIT_SUCCESS);
    CHECK_EQ(TestReentrantPacerConsumptionIsAccepted(), EXIT_SUCCESS);
    CHECK_EQ(TestProbeAdmissionRequiresGccProbe(), EXIT_SUCCESS);
    CHECK_EQ(TestReentrantSendCannotSendTwice(), EXIT_SUCCESS);
    CHECK_EQ(TestSequenceWrapKeepsPacketIdsMonotonic(), EXIT_SUCCESS);
    CHECK_EQ(TestPacketIdWrapsModuloUint32(), EXIT_SUCCESS);
    CHECK_EQ(TestHistoryEvictsAfterTwoThousand(), EXIT_SUCCESS);
    CHECK_EQ(TestHistoryEvictionKeepsNewestWrappedSequence(), EXIT_SUCCESS);
    CHECK_EQ(TestUdpFactoryValidatesConfiguration(), EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
