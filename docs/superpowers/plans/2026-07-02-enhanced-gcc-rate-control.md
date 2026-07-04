# Enhanced GCC Rate Control Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace sampled RTP accounting with accurate paced per-packet transport, make congestion decreases immediate, and add active RTP probing so available bandwidth can be discovered quickly.

**Architecture:** GStreamer ends at `appsink`; a C++14 `TransportSender` queues complete RTP packets and gives them to the existing razor pacer. A serialized control thread owns all razor calls, feedback processing, RTT updates, bitrate decisions, and probe state. The receiver observes media and probe RTP packets before depayloading and serializes arrival events into razor.

**Tech Stack:** C++14, existing vendored razor C API, GStreamer 1.16+, POSIX/WinSock UDP, CMake/CTest, Docker with `tc netem`.

---

## Scope and file map

New application code is C++14. Existing files under `vendor/razor` remain C.

**Create:**

- `include/rtp_packet.h` — safe RTP parsing, sequence rewriting, and probe packet construction.
- `src/rtp_packet.cpp` — implementation of RTP helpers.
- `include/transport_sender.h` — bounded media ingress queue, paced packet cache, UDP send abstraction, and send-time history.
- `src/transport_sender.cpp` — transport implementation and platform UDP handling.
- `include/rate_policy.h` — asymmetric encoder-target policy.
- `src/rate_policy.cpp` — immediate decrease, held normal increase, and probe acceptance.
- `include/probe_controller.h` — probe state and input/output structures.
- `src/probe_controller.cpp` — startup and recovery probe state machine.
- `include/feedback_packet.h` — protocol-v4 feedback value type and codec.
- `src/feedback_packet.cpp` — bounds-checked network-byte-order codec and RTT helper.
- `tests/test_support.h` — assertion helpers used in Release builds.
- `tests/test_rtp_packet.cpp`
- `tests/test_transport_sender.cpp`
- `tests/test_feedback_packet.cpp`
- `tests/test_rate_policy.cpp`
- `tests/test_probe_controller.cpp`
- `tests/test_razor_feedback.cpp`

**Modify:**

- `CMakeLists.txt` — add the C++ support library and CTest executables.
- `include/razor_wrapper.h` — expose native pacer enqueue/callback/configuration in C++.
- `vendor/razor/cc/sender_congestion_controller.c` — remove duplicated feedback processing.
- `vendor/razor/cc/sender_congestion_controller.h` — expose pacing-rate setter.
- `vendor/razor/cc/razor_api.h` — expose read-only sender diagnostics.
- `vendor/razor/cc/razor_callback.h` — add pacing-rate function pointer.
- `vendor/razor/cc/razor_api.c` — bind the pacing-rate function.
- `vendor/razor/estimator/estimator_common.h`
- `vendor/razor/estimator/estimator_common.c` — reject truncated razor feedback.
- `vendor/razor/remb/remb_sender.c` — handle the checked decoder result.
- `vendor/razor/pacing/pace_sender.h`
- `vendor/razor/pacing/pace_sender.c` — replace hard-coded factor with explicit pacing rate.
- `include/feedback_channel.h` — send and receive encoded protocol-v4 messages.
- `src/sender.cpp` — use `appsink`, `TransportSender`, serialized razor control, `RatePolicy`, and `ProbeController`.
- `src/receiver.cpp` — serialize packet arrivals and drop recognized probe packets before H.264 depayloading.
- `src/demo.cpp` — use the same sender/receiver components rather than the sampled-sequence path.
- `docker-test.sh` — add capacity-ramp, capacity-drop, recovery, and random-loss acceptance checks.
- `README.md` — document new diagnostics and probe behavior.

## Task 1: Add CTest and safe RTP helpers

**Files:**

- Create: `tests/test_support.h`
- Create: `tests/test_rtp_packet.cpp`
- Create: `include/rtp_packet.h`
- Create: `src/rtp_packet.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add a Release-safe test assertion helper**

Create `tests/test_support.h`:

```cpp
#pragma once

#include <cstdlib>
#include <iostream>

#define CHECK_TRUE(expr)                                                     \
    do {                                                                     \
        if (!(expr)) {                                                       \
            std::cerr << __FILE__ << ":" << __LINE__                         \
                      << " check failed: " #expr << std::endl;                \
            return EXIT_FAILURE;                                             \
        }                                                                    \
    } while (false)

#define CHECK_EQ(actual, expected) CHECK_TRUE((actual) == (expected))
```

- [ ] **Step 2: Write failing RTP helper tests**

Create `tests/test_rtp_packet.cpp`:

```cpp
#include "rtp_packet.h"
#include "test_support.h"

#include <cstdint>
#include <vector>

int main() {
    std::vector<uint8_t> media = {
        0x80, 96, 0x12, 0x34,
        0x01, 0x02, 0x03, 0x04,
        0x11, 0x22, 0x33, 0x44,
        0x65, 0xaa
    };

    gcc::RtpHeader header{};
    CHECK_TRUE(gcc::ParseRtpHeader(media.data(), media.size(), header));
    CHECK_EQ(header.sequence, 0x1234);
    CHECK_EQ(header.timestamp, 0x01020304u);
    CHECK_EQ(header.ssrc, 0x11223344u);
    CHECK_EQ(header.payload_type, 96);
    CHECK_TRUE(!header.padding);

    CHECK_TRUE(gcc::RewriteRtpSequence(media, 0xabcd));
    CHECK_EQ(media[2], 0xab);
    CHECK_EQ(media[3], 0xcd);

    const auto probe = gcc::BuildProbeRtpPacket(header, 0x2222, 1200);
    CHECK_EQ(probe.size(), 1200u);
    gcc::RtpHeader probe_header{};
    CHECK_TRUE(gcc::ParseRtpHeader(probe.data(), probe.size(), probe_header));
    CHECK_TRUE(gcc::IsProbeRtpPacket(probe.data(), probe.size()));
    CHECK_EQ(probe_header.sequence, 0x2222);
    CHECK_EQ(probe_header.ssrc, header.ssrc);

    std::vector<uint8_t> short_packet(11);
    CHECK_TRUE(!gcc::ParseRtpHeader(short_packet.data(), short_packet.size(),
                                    header));
    return EXIT_SUCCESS;
}
```

- [ ] **Step 3: Register the failing test**

Add to `CMakeLists.txt`:

```cmake
include(CTest)

add_library(gcc_support STATIC
    src/rtp_packet.cpp
)
target_include_directories(gcc_support PUBLIC include)

if(BUILD_TESTING)
    add_executable(test_rtp_packet tests/test_rtp_packet.cpp)
    target_include_directories(test_rtp_packet PRIVATE include tests)
    target_link_libraries(test_rtp_packet gcc_support)
    add_test(NAME rtp_packet COMMAND test_rtp_packet)
endif()
```

Run:

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --target test_rtp_packet -j
```

Expected: compilation fails because `rtp_packet.h` and its functions do not
exist.

- [ ] **Step 4: Implement the RTP helper**

Create `include/rtp_packet.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gcc {

struct RtpHeader {
    bool padding = false;
    bool marker = false;
    uint8_t payload_type = 0;
    uint16_t sequence = 0;
    uint32_t timestamp = 0;
    uint32_t ssrc = 0;
    size_t header_size = 0;
};

bool ParseRtpHeader(const uint8_t* data, size_t size, RtpHeader& out);
bool RewriteRtpSequence(std::vector<uint8_t>& packet, uint16_t sequence);
std::vector<uint8_t> BuildProbeRtpPacket(
    const RtpHeader& media_header, uint16_t sequence, size_t packet_size);
bool IsProbeRtpPacket(const uint8_t* data, size_t size);

}  // namespace gcc
```

Implement `src/rtp_packet.cpp` with these fixed rules:

```cpp
namespace {
constexpr uint8_t kRtpVersion = 2;
constexpr uint8_t kProbeMagic[] = {'G', 'C', 'C', 'P'};
constexpr size_t kFixedHeaderSize = 12;
constexpr size_t kMinProbeSize = kFixedHeaderSize + sizeof(kProbeMagic) + 1;
constexpr size_t kMaxUdpPayload = 1472;
}
```

`ParseRtpHeader()` must validate RTP version, CSRC count, extension length, and
padding count before reading. `BuildProbeRtpPacket()` must:

- reject sizes outside `[kMinProbeSize, kMaxUdpPayload]` by returning `{}`;
- copy payload type, timestamp, and SSRC from the latest media header;
- set the RTP padding bit;
- write `GCCP` immediately after the RTP header;
- zero-fill the remainder and use a final padding count of one.

`IsProbeRtpPacket()` must require both the padding bit and `GCCP` magic so
ordinary RTP padding is not discarded.

- [ ] **Step 5: Run and commit**

Run:

```bash
cmake --build build --target test_rtp_packet -j
ctest --test-dir build -R rtp_packet --output-on-failure
```

Expected: `1/1` tests pass.

Commit:

```bash
git add CMakeLists.txt include/rtp_packet.h src/rtp_packet.cpp \
  tests/test_support.h tests/test_rtp_packet.cpp
git commit -m "test: add safe RTP packet helpers"
```

## Task 2: Restore single-pass razor feedback processing

**Files:**

- Create: `tests/test_razor_feedback.cpp`
- Modify: `vendor/razor/cc/sender_congestion_controller.c:114-197`
- Modify: `vendor/razor/estimator/estimator_common.h`
- Modify: `vendor/razor/estimator/estimator_common.c`
- Modify: `vendor/razor/remb/remb_sender.c`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write a source-level regression test**

The duplicated block is a vendoring regression. Create
`tests/test_razor_feedback.cpp` that reads the configured source path and
asserts exactly one adapter pass and one loss-report call inside
`sender_on_feedback()`:

```cpp
#include "test_support.h"

#include <fstream>
#include <iterator>
#include <string>

static size_t Count(const std::string& text, const std::string& needle) {
    size_t count = 0;
    for (size_t pos = 0; (pos = text.find(needle, pos)) != std::string::npos;
         pos += needle.size()) {
        ++count;
    }
    return count;
}

int main(int argc, char** argv) {
    CHECK_EQ(argc, 2);
    std::ifstream file(argv[1]);
    CHECK_TRUE(file.good());
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    const auto begin = text.find("void sender_on_feedback");
    const auto end = text.find("void sender_cc_update_rtt", begin);
    CHECK_TRUE(begin != std::string::npos);
    CHECK_TRUE(end != std::string::npos);
    const std::string body = text.substr(begin, end - begin);
    CHECK_EQ(Count(body, "cc_feedback_on_feedback("), 1u);
    CHECK_EQ(Count(body, "bitrate_controller_on_report("), 1u);
    CHECK_EQ(Count(body, "bitrate_controller_on_remb("), 1u);
    return EXIT_SUCCESS;
}
```

Register it:

```cmake
add_executable(test_razor_feedback tests/test_razor_feedback.cpp)
target_include_directories(test_razor_feedback PRIVATE include tests)
target_link_libraries(test_razor_feedback razor Threads::Threads)
add_test(
    NAME razor_feedback
    COMMAND test_razor_feedback
            ${CMAKE_SOURCE_DIR}/vendor/razor/cc/sender_congestion_controller.c
)
```

- [ ] **Step 2: Run to prove the regression**

Run:

```bash
cmake --build build --target test_razor_feedback -j
ctest --test-dir build -R razor_feedback --output-on-failure
```

Expected: FAIL because the function contains two
`cc_feedback_on_feedback()` calls and two loss/REMB calls.

- [ ] **Step 3: Add a truncated-payload regression test**

Add to the same test:

```cpp
extern "C" {
#include "cf_stream.h"
#include "estimator_common.h"
}

static int VerifyTruncatedFeedbackIsRejected() {
    bin_stream_t stream;
    CHECK_EQ(bin_stream_init(&stream), 0);
    mach_uint8_write(&stream, proxy_ts_msg);
    bin_stream_rewind(&stream, 1);
    feedback_msg_t message{};
    CHECK_TRUE(feedback_msg_decode(&stream, &message) != 0);
    bin_stream_destroy(&stream);
    return EXIT_SUCCESS;
}
```

Call this function from `main()`. Change `feedback_msg_decode()` to return
`int`, where `0` means success and `-1` means malformed or truncated.

- [ ] **Step 4: Restore the upstream control flow and validate decode**

In `sender_on_feedback()` retain exactly:

```c
if ((msg.flag & proxy_ts_msg) == proxy_ts_msg) {
    if (cc_feedback_on_feedback(&cc->adapter, &msg, now_ts) <= 0)
        return;
    cur_alr = pace_get_limited_start_time(cc->pacer) > 0 ? 0 : -1;
    if (cc->was_in_alr == 0 && cur_alr != 0)
        ack_estimator_set_alrended(cc->ack, now_ts);
    cc->was_in_alr = cur_alr;
    ack_estimator_incoming(
        cc->ack, cc->adapter.packets, cc->adapter.num);
    init_bwe_result_null(bwe_result);
    bwe_result = delay_bwe_incoming(
        cc->bwe, cc->adapter.packets, cc->adapter.num,
        ack_estimator_bitrate_bps(cc->ack), now_ts);
    if (bwe_result.updated == 0)
        bitrate_controller_on_basedelay_result(
            cc->bitrate_controller, bwe_result.updated, bwe_result.probe,
            bwe_result.bitrate, cc->bwe->detector->state);
}
if ((msg.flag & remb_msg) == remb_msg)
    bitrate_controller_on_remb(cc->bitrate_controller, msg.remb);
if ((msg.flag & loss_info_msg) == loss_info_msg)
    bitrate_controller_on_report(
        cc->bitrate_controller, cc->rtt, now_ts,
        msg.fraction_loss, msg.packet_num,
        ack_estimator_bitrate_bps(cc->ack));
```

Delete the second proxy block and the duplicated REMB/loss blocks. Do not tune
any estimator constants in this commit.

Before using `msg`, require:

```c
if (feedback_msg_decode(&cc->strm, &msg) != 0)
    return;
```

Apply the same checked call in `remb_sender_on_feedback()` so changing the
shared decoder signature does not leave an unchecked caller.

In `feedback_msg_decode()`, check `bin_stream_available_read()` before each
flag-selected fixed field. Once `samples_num` is read, require:

```c
if (msg->samples_num > MAX_FEELBACK_COUNT ||
    bin_stream_available_read(strm) < msg->samples_num * 4)
    return -1;
```

Return zero only after every selected field has been decoded.

- [ ] **Step 5: Run and commit**

Run:

```bash
cmake --build build --target test_razor_feedback -j
ctest --test-dir build -R razor_feedback --output-on-failure
```

Expected: PASS.

Commit:

```bash
git add CMakeLists.txt tests/test_razor_feedback.cpp \
  vendor/razor/cc/sender_congestion_controller.c \
  vendor/razor/estimator/estimator_common.h \
  vendor/razor/estimator/estimator_common.c \
  vendor/razor/remb/remb_sender.c
git commit -m "fix: process razor feedback once"
```

## Task 3: Expose and configure the razor pacer

**Files:**

- Modify: `include/razor_wrapper.h:29-88`
- Modify: `vendor/razor/cc/razor_api.h`
- Modify: `vendor/razor/cc/razor_callback.h`
- Modify: `vendor/razor/cc/razor_api.c`
- Modify: `vendor/razor/cc/sender_congestion_controller.h`
- Modify: `vendor/razor/cc/sender_congestion_controller.c`
- Modify: `vendor/razor/pacing/pace_sender.h`
- Modify: `vendor/razor/pacing/pace_sender.c`
- Modify: `tests/test_razor_feedback.cpp`

- [ ] **Step 1: Extend the wrapper compile test**

Add a compile-only use to `tests/test_razor_feedback.cpp`:

```cpp
#include "razor_wrapper.h"

static void VerifyWrapperSurface() {
    razor::Sender sender(
        [](uint32_t, uint8_t, uint32_t) {},
        [](uint32_t, int, size_t, bool) {},
        /*queue_ms=*/250);
    sender.set_bitrates(200, 1000, 8000);
    sender.set_pacing_rate(1200);
    (void)sender.add_packet(7, false, 1200);
    (void)sender.pacer_queue_ms();
    const razor::SenderStats stats = sender.stats();
    (void)stats.acked_bitrate_kbps;
    (void)stats.delay_state;
    (void)stats.matched_feedback_packets;
}
```

Call `VerifyWrapperSurface()` from `main()`. Link the test to `razor`.

Run the build and expect compilation failure because the constructor and
methods do not exist.

- [ ] **Step 2: Add explicit pacing-rate support to razor**

Add to `pace_sender_t`:

```c
uint32_t pacing_factor_x100;
```

Initialize it to `120` and replace `k_default_pace_factor` calculations with:

```c
pace->pacing_bitrate_kpbs =
    SU_MAX(bitrate_bps / 1000, pace->min_sender_bitrate_kpbs) *
    pace->pacing_factor_x100 / 100;
```

Add:

```c
void pace_set_pacing_rate(pace_sender_t* pace, uint32_t bitrate_bps) {
    pace->pacing_bitrate_kpbs =
        SU_MAX(bitrate_bps / 1000, pace->min_sender_bitrate_kpbs);
}
```

Thread this through `sender_cc_set_pacing_rate()`, a new
`sender_set_pacing_rate_func` member on `razor_sender_t`, and the GCC branch in
`razor_sender_create()`.

- [ ] **Step 3: Add read-only GCC diagnostics**

Add to `razor_api.h`:

```c
typedef struct {
    uint32_t acked_bitrate_bps;
    int delay_state;
    uint64_t matched_feedback_packets;
    int64_t pacer_queue_ms;
} razor_sender_stats_t;

int razor_sender_get_stats(razor_sender_t* sender,
                           razor_sender_stats_t* stats);
```

Add `matched_feedback_packets` to `sender_cc_t` and increment it once after a
successful adapter pass:

```c
cc->matched_feedback_packets += cc->adapter.num;
```

For GCC senders, `razor_sender_get_stats()` reads the ACK estimator, delay
detector, matched counter, and pacer queue. It returns `-1` for null pointers
or unsupported sender types. Make `pace_queue_ms()` return zero when its queue
is empty so idle diagnostics do not report a huge duration.

- [ ] **Step 4: Extend the C++ wrapper**

Define:

```cpp
using PaceSendCallback =
    std::function<void(uint32_t packet_id, int retrans, size_t size,
                       bool padding)>;
```

The sender constructor passes a callback thunk and handler to
`razor_sender_create()`. Its signature accepts `queue_ms` with a default of
250 ms; the sender application passes 250 explicitly. Add:

```cpp
bool add_packet(uint32_t packet_id, bool retrans, size_t size);
void set_pacing_rate(uint32_t kbps);
int64_t pacer_queue_ms() const;
```

Also expose:

```cpp
struct SenderStats {
    uint32_t acked_bitrate_kbps = 0;
    int delay_state = 0;
    uint64_t matched_feedback_packets = 0;
    int64_t pacer_queue_ms = 0;
};

SenderStats stats() const;
```

All public bitrate values remain kbps at the wrapper boundary and are converted
to bps internally.

- [ ] **Step 5: Verify and commit**

Run:

```bash
cmake --build build --target razor test_razor_feedback -j
ctest --test-dir build -R razor_feedback --output-on-failure
```

Expected: PASS.

Commit:

```bash
git add include/razor_wrapper.h vendor/razor/cc vendor/razor/pacing \
  tests/test_razor_feedback.cpp
git commit -m "feat: expose configurable razor pacing"
```

## Task 4: Implement paced `TransportSender`

**Files:**

- Create: `include/transport_sender.h`
- Create: `src/transport_sender.cpp`
- Create: `tests/test_transport_sender.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write transport tests**

The test uses an injected send function and clock:

```cpp
std::vector<std::vector<uint8_t>> sent;
uint64_t now_us = 1000;
gcc::TransportSender transport(
    [&](const uint8_t* data, size_t size) {
        sent.emplace_back(data, data + size);
        return true;
    },
    [&] { return now_us; },
    64 * 1024);

CHECK_TRUE(transport.EnqueueMedia(MakeRtp(1, 100)));
CHECK_TRUE(transport.EnqueueMedia(MakeRtp(2, 700)));

std::vector<gcc::PacerEnqueue> enqueued;
transport.DrainIngress([&](uint32_t id, size_t size) {
    enqueued.push_back({id, size});
    return true;
});
CHECK_EQ(enqueued.size(), 2u);
CHECK_EQ(enqueued[0].size, 100u);
CHECK_EQ(enqueued[1].size, 700u);

gcc::SentPacket sent_info{};
CHECK_TRUE(transport.SendPaced(enqueued[0].packet_id, sent_info));
CHECK_EQ(sent_info.size, 100u);
CHECK_EQ(sent_info.send_time_us, 1000u);
CHECK_EQ(sent.size(), 1u);
```

Also test:

- 150 packets are all retained and sent, proving there is no 100 packets/s
  sampling cap;
- exact packet sizes are preserved;
- queue byte limit rejects new packets without corrupting retained packets;
- failed send does not produce a `SentPacket`;
- 16-bit RTP sequence wraps while 32-bit packet IDs remain monotonic.

- [ ] **Step 2: Run to verify failure**

Register:

```cmake
target_sources(gcc_support PRIVATE src/transport_sender.cpp)
target_link_libraries(gcc_support PUBLIC Threads::Threads)
add_executable(test_transport_sender tests/test_transport_sender.cpp)
target_include_directories(test_transport_sender PRIVATE include tests)
target_link_libraries(test_transport_sender gcc_support)
add_test(NAME transport_sender COMMAND test_transport_sender)
```

Build it and expect failure because `TransportSender` does not exist.

- [ ] **Step 3: Implement the interface**

Create these public values in `transport_sender.h`:

```cpp
struct PacerEnqueue {
    uint32_t packet_id;
    size_t size;
};

struct SentPacket {
    uint16_t transport_sequence;
    size_t size;
    uint64_t send_time_us;
    bool probe;
};

class TransportSender {
public:
    using SendFunction = std::function<bool(const uint8_t*, size_t)>;
    using ClockFunction = std::function<uint64_t()>;
    using PacerAddFunction = std::function<bool(uint32_t, size_t)>;

    TransportSender(SendFunction send, ClockFunction clock,
                    size_t max_ingress_bytes);
    bool EnqueueMedia(std::vector<uint8_t> packet);
    bool EnqueueProbe(std::vector<uint8_t> packet);
    size_t DrainIngress(const PacerAddFunction& add);
    bool SendPaced(uint32_t packet_id, SentPacket& out);
    bool LookupSendTime(uint16_t sequence, uint64_t& out_us) const;
    bool latest_media_header(RtpHeader& out) const;
    size_t ingress_bytes() const;
    size_t pending_packets() const;
    uint64_t dropped_packets() const;
};
```

Implementation requirements:

- protect only the GStreamer ingress deque with a mutex;
- access the paced packet map only from the control thread;
- assign packet IDs while draining on the control thread;
- rewrite the RTP sequence before storing the packet;
- retain the latest valid media RTP timestamp, SSRC, and payload type for probe
  construction;
- retain at most 2000 send-time history entries;
- erase paced packet bytes after successful or failed send;
- never call razor from this class.

Add a production factory that creates a UDP `SendFunction` using the same
POSIX/WinSock platform guards already used by `FeedbackChannel`.

If the pacer-add callback rejects a packet, remove that packet from the paced
map immediately and increment `dropped_packets`; do not leave unreachable
packet bytes cached.

- [ ] **Step 4: Run and commit**

Run:

```bash
cmake --build build --target test_transport_sender -j
ctest --test-dir build -R transport_sender --output-on-failure
```

Expected: PASS.

Commit:

```bash
git add CMakeLists.txt include/transport_sender.h src/transport_sender.cpp \
  tests/test_transport_sender.cpp
git commit -m "feat: add paced RTP transport sender"
```

## Task 5: Add protocol-v4 feedback and correct RTT

**Files:**

- Create: `include/feedback_packet.h`
- Create: `src/feedback_packet.cpp`
- Create: `tests/test_feedback_packet.cpp`
- Modify: `include/feedback_channel.h`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write feedback codec and RTT tests**

Test round-trip encoding:

```cpp
gcc::FeedbackPacket input;
input.sequence = 0x1234;
input.loss_q8 = 26;
input.receive_time_ms = 1000;
input.feedback_time_ms = 1012;
input.razor_payload = {1, 2, 3, 4};

const auto bytes = gcc::EncodeFeedbackPacket(input);
gcc::FeedbackPacket output;
CHECK_TRUE(gcc::DecodeFeedbackPacket(bytes.data(), bytes.size(), output));
CHECK_EQ(output.sequence, input.sequence);
CHECK_EQ(output.loss_q8, input.loss_q8);
CHECK_EQ(output.razor_payload, input.razor_payload);
```

Test malformed lengths and cross-clock RTT:

```cpp
uint32_t rtt_ms = 0;
CHECK_TRUE(gcc::ComputeNetworkRtt(
    /*sender_send_ms=*/500000,
    /*sender_feedback_ms=*/500085,
    /*receiver_receive_ms=*/900000,
    /*receiver_feedback_ms=*/900015,
    rtt_ms));
CHECK_EQ(rtt_ms, 70u);
```

Corrupt `payload_size`, truncate every header length, use unknown versions, and
assert decode returns false.

- [ ] **Step 2: Run to verify failure**

Register:

```cmake
target_sources(gcc_support PRIVATE src/feedback_packet.cpp)
add_executable(test_feedback_packet tests/test_feedback_packet.cpp)
target_include_directories(test_feedback_packet PRIVATE include tests)
target_link_libraries(test_feedback_packet gcc_support)
add_test(NAME feedback_packet COMMAND test_feedback_packet)
```

Build `test_feedback_packet`. Expected: missing codec symbols.

- [ ] **Step 3: Implement protocol v4**

Use this wire layout:

```text
magic              uint8   0xCC
version            uint8   4
sequence           uint16  network byte order
loss_q8            uint8
flags              uint8
razor_payload_size uint16  network byte order
receive_time_ms    uint32  network byte order
feedback_time_ms   uint32  network byte order
```

The fixed header is 16 bytes. Encode/decode fields manually; do not
`reinterpret_cast` a packed struct. Limit the razor payload to 256 bytes.

Implement RTT as:

```cpp
const uint32_t sender_elapsed = sender_feedback_ms - sender_send_ms;
const uint32_t receiver_residence =
    receiver_feedback_ms - receiver_receive_ms;
if (receiver_residence > sender_elapsed || sender_elapsed > 5000)
    return false;
out_rtt_ms = sender_elapsed - receiver_residence;
return out_rtt_ms > 0;
```

Modify `FeedbackChannel` to traffic only in `FeedbackPacket` values and reject
legacy v2/v3 datagrams instead of parsing them with the wrong header size.

Replace its single `rx_latest_` slot with a bounded FIFO:

```cpp
std::deque<FeedbackPacket> rx_queue_;
static constexpr size_t kMaxQueuedFeedback = 64;
```

The receive thread pushes decoded packets and drops the oldest only when the
bound is reached. `poll()` pops the oldest packet, allowing the control loop
to drain all pending feedback instead of silently overwriting intermediate
transport feedback.

Replace `LossTracker::get_loss_pct_x100()` with Q8 diagnostics:

```cpp
uint8_t get_loss_q8() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!initialized_ || expected_ == 0) return 0;
    double fraction = 1.0 -
        static_cast<double>(received_) / static_cast<double>(expected_);
    fraction = std::max(0.0, std::min(1.0, fraction));
    return static_cast<uint8_t>(fraction * 255.0 + 0.5);
}
```

This outer field is diagnostic only; razor's encoded loss report remains the
control input.

- [ ] **Step 4: Run and commit**

Run:

```bash
cmake --build build --target test_feedback_packet -j
ctest --test-dir build -R feedback_packet --output-on-failure
```

Expected: PASS.

Commit:

```bash
git add CMakeLists.txt include/feedback_packet.h src/feedback_packet.cpp \
  include/feedback_channel.h tests/test_feedback_packet.cpp
git commit -m "fix: calculate RTT within clock domains"
```

## Task 6: Add asymmetric rate policy

**Files:**

- Create: `include/rate_policy.h`
- Create: `src/rate_policy.cpp`
- Create: `tests/test_rate_policy.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write policy tests**

Define tests around this API:

```cpp
gcc::RatePolicy policy(/*min=*/200, /*max=*/8000,
                       /*media_utilization=*/0.92);
policy.Reset(/*network_kbps=*/4000, /*now_ms=*/0);

auto decrease = policy.Apply(
    {gcc::RateReason::DelayOveruse, 2500, 100, 1000});
CHECK_EQ(decrease.network_kbps, 2500);
CHECK_TRUE(decrease.changed);

auto guarded = policy.Apply(
    {gcc::RateReason::Loss, 500, 120, 1100});
CHECK_EQ(guarded.network_kbps, 1250);  // 50% of current 2500.

auto held = policy.Apply(
    {gcc::RateReason::Normal, 3000, 120, 1150});
CHECK_EQ(held.network_kbps, 1250);

auto probe = policy.Apply(
    {gcc::RateReason::ProbeAccepted, 3000, 120, 1200});
CHECK_EQ(probe.network_kbps, 3000);
CHECK_EQ(probe.encoder_kbps, 2760);
```

Also test minimum/maximum clamps, 8-percent-per-second ordinary increase, and
normal increase becoming available one RTT after a large decrease.

- [ ] **Step 2: Run to verify failure**

Register:

```cmake
target_sources(gcc_support PRIVATE src/rate_policy.cpp)
add_executable(test_rate_policy tests/test_rate_policy.cpp)
target_include_directories(test_rate_policy PRIVATE include tests)
target_link_libraries(test_rate_policy gcc_support)
add_test(NAME rate_policy COMMAND test_rate_policy)
```

Build and expect missing `RatePolicy`.

- [ ] **Step 3: Implement the policy**

Use:

```cpp
enum class RateReason {
    Normal,
    DelayOveruse,
    Loss,
    ProbeAccepted
};

struct RateInput {
    RateReason reason;
    uint32_t proposed_network_kbps;
    uint32_t rtt_ms;
    uint64_t now_ms;
};

struct RateDecision {
    bool changed;
    uint32_t network_kbps;
    uint32_t encoder_kbps;
};
```

Rules:

- decreases apply immediately but are no lower than 50 percent of the current
  value in one call;
- repeated decreases may continue on later calls;
- probe acceptance may increase immediately;
- normal increase is held until `last_major_decrease + max(rtt, 50 ms)`;
- normal growth cannot exceed `pow(1.08, elapsed_ms / 1000.0)`;
- encoder rate is rounded `network * 0.92`;
- all values are clamped to configured min/max.

- [ ] **Step 4: Run and commit**

Run the test and expect PASS.

Commit:

```bash
git add CMakeLists.txt include/rate_policy.h src/rate_policy.cpp \
  tests/test_rate_policy.cpp
git commit -m "feat: make congestion decreases immediate"
```

## Task 7: Add the probe state machine

**Files:**

- Create: `include/probe_controller.h`
- Create: `src/probe_controller.cpp`
- Create: `tests/test_probe_controller.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write deterministic probe tests**

Use a state machine with no sleeping:

```cpp
gcc::ProbeConfig config;
config.first_scale = 2.0;
config.next_scale = 1.5;
config.duration_ms = 100;
config.success_ratio = 0.75;
config.retry_interval_ms = 5000;
config.max_kbps = 8000;

gcc::ProbeController probe(config);
auto start = probe.OnStableTarget(/*target=*/1000, /*now=*/0);
CHECK_TRUE(start.start);
CHECK_EQ(start.probe_kbps, 2000);

probe.OnProbePacketSent(/*bytes=*/1200, /*now=*/20);
auto success = probe.OnFeedback(
    {/*acked=*/1700, /*loss=*/0.0, /*overusing=*/false, /*now=*/120});
CHECK_TRUE(success.accept);
CHECK_EQ(success.accepted_kbps, 1700);

auto second = probe.OnStableTarget(1700, 121);
CHECK_TRUE(second.start);
CHECK_EQ(second.probe_kbps, 2550);
```

Also test:

- ACK below 75 percent fails;
- loss above 10 percent aborts;
- delay overuse aborts;
- no retry before 5 seconds;
- max rate clamp;
- no overlapping probes.

- [ ] **Step 2: Run to verify failure**

Register:

```cmake
target_sources(gcc_support PRIVATE src/probe_controller.cpp)
add_executable(test_probe_controller tests/test_probe_controller.cpp)
target_include_directories(test_probe_controller PRIVATE include tests)
target_link_libraries(test_probe_controller gcc_support)
add_test(NAME probe_controller COMMAND test_probe_controller)
```

Build and expect missing `ProbeController`.

- [ ] **Step 3: Implement the state machine**

Expose:

```cpp
enum class ProbeState { Idle, Probing, WaitingForResult, Accepted, Failed };

struct ProbeAction {
    bool start = false;
    bool stop = false;
    bool accept = false;
    uint32_t probe_kbps = 0;
    uint32_t accepted_kbps = 0;
};
```

The controller decides only state and rate. `sender.cpp` is responsible for
calling `BuildProbeRtpPacket()` often enough to meet the requested byte budget:

```text
bytes_due = probe_kbps * elapsed_ms / 8
```

Subtract bytes already emitted and generate packets no larger than 1200 bytes.
Stop generation after 100 ms and wait for feedback until 300 ms after probe
start.

The control-loop generation is:

```cpp
gcc::RtpHeader media_header{};
if (probe_state == gcc::ProbeState::Probing &&
    transport.latest_media_header(media_header)) {
    const uint64_t desired_bytes =
        static_cast<uint64_t>(active_probe_kbps) *
        (now_ms - probe_start_ms) / 8;
    while (probe_bytes_enqueued + 1200 <= desired_bytes) {
        auto packet = gcc::BuildProbeRtpPacket(
            media_header, /* sequence is rewritten by transport */ 0, 1200);
        if (packet.empty() || !transport.EnqueueProbe(std::move(packet)))
            break;
        probe_bytes_enqueued += 1200;
    }
}
```

- [ ] **Step 4: Run and commit**

Run the test and expect PASS.

Commit:

```bash
git add CMakeLists.txt include/probe_controller.h src/probe_controller.cpp \
  tests/test_probe_controller.cpp
git commit -m "feat: add active GCC probe controller"
```

## Task 8: Refactor the sender onto `appsink` and the paced transport

**Files:**

- Modify: `src/sender.cpp:25-290`
- Modify: `CMakeLists.txt`
- Test: all unit tests

- [ ] **Step 1: Add an integration assertion for per-packet accounting**

Extend `docker-test.sh` to fail unless the final sender diagnostics satisfy:

```bash
media_sent=$(grep 'gcc_stats' sender.log | tail -1 |
  sed -n 's/.*media_sent=\\([0-9]*\\).*/\\1/p')
feedback_matched=$(grep 'gcc_stats' sender.log | tail -1 |
  sed -n 's/.*feedback_matched=\\([0-9]*\\).*/\\1/p')
test -n "$media_sent"
test "$media_sent" -gt 100
test "$feedback_matched" -gt 50
```

Run the baseline Docker test. Expected: FAIL because these diagnostics do not
exist.

- [ ] **Step 2: Replace `udpsink` with `appsink`**

Use:

```cpp
#include <gst/app/gstappsink.h>

appsink = gst_element_factory_make("appsink", "rtp-app-sink");
g_object_set(appsink,
    "emit-signals", TRUE,
    "sync", FALSE,
    "max-buffers", 64u,
    "drop", FALSE,
    nullptr);
gst_element_link_many(parser, payloader, appsink, nullptr);
```

Link support code into the applications:

```cmake
target_link_libraries(sender ${GST_LIBRARIES} razor gcc_support Threads::Threads)
target_link_libraries(receiver ${GST_LIBRARIES} razor gcc_support Threads::Threads)
target_link_libraries(demo ${GST_LIBRARIES} razor gcc_support Threads::Threads)
```

The `new-sample` callback maps the complete buffer, copies it into
`std::vector<uint8_t>`, unmaps immediately, and calls only
`TransportSender::EnqueueMedia()`.

Delete `last_rtp_seq`, `prev_rtp_seq`, `SenderProbeData`, `rtp_out_probe()`,
and the hard-coded `on_send(last_rtp_seq, 1400)` path.

- [ ] **Step 3: Wire the serialized control loop**

Construct razor with a pacing callback:

```cpp
razor::Sender* razor_ptr = nullptr;
razor::Sender razor_sender(
    bitrate_callback,
    [&](uint32_t packet_id, int, size_t, bool) {
        gcc::SentPacket sent{};
        if (transport.SendPaced(packet_id, sent)) {
            razor_ptr->on_send(sent.transport_sequence, sent.size);
        }
    });
razor_ptr = &razor_sender;
```

Each control-loop iteration:

1. drain all feedback datagrams;
2. update RTT by looking up feedback sequence send time;
3. call razor feedback;
4. drain media ingress into `razor_sender.add_packet()`;
5. generate due probe packets and drain them into razor;
6. call razor heartbeat every 5 ms;
7. apply `RatePolicy` decisions;
8. set encoder bitrate only when changed;
9. set normal or probe pacing rate;
10. emit diagnostics once per second.

No razor method may run in a GStreamer callback or feedback receive thread.

- [ ] **Step 4: Remove duplicate application loss adaptation**

Delete `smoothed_loss`, the extra loss factor, the 500-ms early return, and the
20-percent symmetric step. The razor callback converts the reported estimate
into a `RateInput` reason:

```cpp
const auto razor_stats = razor_sender.stats();
const auto reason =
    new_bitrate_kbps < current_network_kbps
        ? (loss_pct > 25 ? gcc::RateReason::Loss
                         : gcc::RateReason::DelayOveruse)
        : gcc::RateReason::Normal;
pending_rate_input = {reason, new_bitrate_kbps, rtt_ms, now_ms};
```

The control loop consumes this pending input; the callback does not touch
GStreamer. Use `razor_stats.acked_bitrate_kbps`,
`razor_stats.delay_state`, and `razor_stats.matched_feedback_packets` for
diagnostics and probe evaluation.

- [ ] **Step 5: Build, run baseline, and commit**

Run:

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
docker build -f Dockerfile.test -t h264-gcc-test .
docker run --rm h264-gcc-test
```

Expected: all unit tests pass; sender log reports more than 100 distinct media
packets and nonzero matched feedback.

Commit:

```bash
git add src/sender.cpp CMakeLists.txt docker-test.sh
git commit -m "feat: send every RTP packet through razor pacer"
```

## Task 9: Serialize receiver arrivals and handle probe RTP

**Files:**

- Modify: `src/receiver.cpp:29-215`
- Modify: `src/demo.cpp`
- Test: `tests/test_rtp_packet.cpp`

- [ ] **Step 1: Extend probe classification coverage**

Add to `test_rtp_packet.cpp`:

```cpp
auto corrupted = probe;
corrupted[12] = 'X';
CHECK_TRUE(!gcc::IsProbeRtpPacket(corrupted.data(), corrupted.size()));

auto ordinary_padding = media;
ordinary_padding[0] |= 0x20;
ordinary_padding.push_back(1);
CHECK_TRUE(!gcc::IsProbeRtpPacket(
    ordinary_padding.data(), ordinary_padding.size()));
```

Run and ensure the helper continues to pass before receiver changes.

- [ ] **Step 2: Queue arrival events from the pad probe**

Define in `receiver.cpp`:

```cpp
struct PacketArrival {
    uint16_t sequence;
    uint32_t arrival_ms;
    size_t size;
    bool probe;
};
```

The pad probe parses the packet, pushes `PacketArrival` under a short mutex,
updates no razor state, and returns:

```cpp
return arrival.probe ? GST_PAD_PROBE_DROP : GST_PAD_PROBE_OK;
```

Bound this queue to 4096 arrivals. If full, drop the newest event and increment
an arrival-overflow counter that is included in receiver diagnostics; never
allow the streaming thread to allocate an unbounded backlog.

The receiver control loop swaps the queue into a local deque and calls
`razor_rx.on_received()` for every event before calling heartbeat.

- [ ] **Step 3: Send accurate feedback metadata**

Track the most recently processed sequence and arrival time on the receiver
control thread. The razor feedback callback sends:

```cpp
gcc::FeedbackPacket packet;
packet.sequence = last_processed.sequence;
packet.loss_q8 = loss_tracker.get_loss_q8();
packet.receive_time_ms = last_processed.arrival_ms;
packet.feedback_time_ms = now_ms;
packet.razor_payload.assign(data, data + size);
feedback.send_feedback(packet);
```

Use the razor payload as the control loss source. Keep the outer Q8 loss only
as a diagnostic.

- [ ] **Step 4: Update `demo.cpp` to reuse the same path**

Remove its direct sampled `on_send(tx_seq, 1400)` and direct concurrent
receiver calls. Use `TransportSender`, the serialized arrival queue,
`RatePolicy`, and `ProbeController` exactly as sender/receiver do. Do not keep
a second demo-only congestion algorithm.

- [ ] **Step 5: Build, run, and commit**

Run:

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/demo --duration 20 --bitrate 1000
```

Expected: no decode errors from probe packets; feedback sequence is nonzero;
matched feedback grows continuously; ThreadSanitizer run in the next task has
no razor races.

Commit:

```bash
git add src/receiver.cpp src/demo.cpp tests/test_rtp_packet.cpp
git commit -m "feat: serialize receiver GCC feedback"
```

## Task 10: Add diagnostics and concurrency verification

**Files:**

- Modify: `src/sender.cpp`
- Modify: `src/receiver.cpp`
- Modify: `CMakeLists.txt`
- Modify: `README.md`

- [ ] **Step 1: Add structured diagnostics**

Print one sender line per second with stable key names:

```text
gcc_stats encoder_kbps=... target_kbps=... pacing_kbps=...
acked_kbps=... loss_q8=... rtt_ms=... delay_state=...
media_sent=... probe_sent=... feedback=... feedback_matched=...
pacer_queue_ms=... ingress_bytes=... probe_state=...
```

Print state transitions immediately but rate-limit repeated waiting and decode
errors to once per second.

- [ ] **Step 2: Add sanitizer build options**

Add:

```cmake
option(ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
option(ENABLE_TSAN "Enable thread sanitizer" OFF)

if(ENABLE_SANITIZERS)
    set(CMAKE_C_FLAGS
        "${CMAKE_C_FLAGS} -fsanitize=address,undefined -fno-omit-frame-pointer")
    set(CMAKE_CXX_FLAGS
        "${CMAKE_CXX_FLAGS} -fsanitize=address,undefined -fno-omit-frame-pointer")
    set(CMAKE_EXE_LINKER_FLAGS
        "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address,undefined")
endif()
if(ENABLE_TSAN)
    set(CMAKE_C_FLAGS
        "${CMAKE_C_FLAGS} -fsanitize=thread -fno-omit-frame-pointer")
    set(CMAKE_CXX_FLAGS
        "${CMAKE_CXX_FLAGS} -fsanitize=thread -fno-omit-frame-pointer")
    set(CMAKE_EXE_LINKER_FLAGS
        "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=thread")
endif()
```

Guard these options for GNU/Clang and non-Windows builds.

- [ ] **Step 3: Run sanitizers**

Run:

```bash
cmake -S . -B build-asan -DBUILD_TESTING=ON -DENABLE_SANITIZERS=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DBUILD_TESTING=ON -DENABLE_TSAN=ON
cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure
./build-tsan/demo --duration 10 --bitrate 1000
```

Expected: zero sanitizer reports and all tests pass.

- [ ] **Step 4: Document behavior and commit**

Update README with:

- C++14 application-code guarantee and retained razor C dependency;
- `appsink -> TransportSender -> Pacer -> UDP` architecture;
- probe defaults and command-line overrides;
- protocol v4 incompatibility with old sender/receiver binaries;
- diagnostic field definitions;
- how to enable sanitizer builds.

Commit:

```bash
git add CMakeLists.txt src/sender.cpp src/receiver.cpp README.md
git commit -m "docs: add GCC pacing diagnostics"
```

## Task 11: Add deterministic tc/netem acceptance scenarios

**Files:**

- Modify: `docker-test.sh`
- Modify: `Dockerfile.test` only if `iproute2` is not already installed by the
  test command.

- [ ] **Step 1: Add a reusable scenario runner**

Implement shell functions that:

- launch receiver and sender into separate logs;
- apply `tc qdisc replace` with rate, delay, and loss;
- change conditions at specified elapsed seconds;
- parse `gcc_stats` lines;
- fail with the relevant log tail when a condition is not met;
- always kill processes and remove qdiscs in a trap.

Do not parse human-readable “Bitrate adjusted” lines.

- [ ] **Step 2: Add ramp-up and recovery checks**

Scenarios:

```text
ramp_up:
  start=1000 kbps, capacity=8 Mbps
  assert target >= 5000 kbps by 5 seconds

recovery:
  capacity=1.5 Mbps for 8 seconds, then 6 Mbps
  assert target >= 4500 kbps within 8 seconds after restoration
```

- [ ] **Step 3: Add fast decrease and random-loss checks**

Scenarios:

```text
capacity_drop:
  start at 8 Mbps, drop to 1.5 Mbps
  assert target <= 2200 kbps within 1.5 seconds
  assert pacer_queue_ms <= 250

random_loss:
  capacity=6 Mbps, random loss=3 percent
  assert target does not remain below 1000 kbps for 3 consecutive samples

heavy_loss:
  loss=10 percent
  assert at least one immediate loss/decrease transition is logged
```

- [ ] **Step 4: Run the complete matrix**

Run:

```bash
docker build -f Dockerfile.test -t h264-gcc-test .
docker run --rm --cap-add NET_ADMIN h264-gcc-test \
  bash /app/docker-test.sh
```

Expected: all five named scenarios print `PASS`; command exits zero.

- [ ] **Step 5: Commit**

```bash
git add docker-test.sh Dockerfile.test
git commit -m "test: cover GCC ramp-up and fast backoff"
```

## Task 12: Final verification

**Files:**

- Verify all changed files.

- [ ] **Step 1: Run formatting and static checks**

Run:

```bash
git diff --check origin/main...HEAD
cmake -S . -B build-release -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
ctest --test-dir build-release --output-on-failure
```

Expected: no whitespace errors; build exits zero; all tests pass.

- [ ] **Step 2: Run the baseline demo**

Run:

```bash
./build-release/demo --duration 20 --bitrate 1000
```

Expected:

- feedback is continuously received;
- media and matched-feedback counts increase;
- at least one startup probe occurs;
- encoder rate follows 92 percent of network target;
- no GStreamer error or crash occurs.

- [ ] **Step 3: Run the network matrix**

Run the Task 11 Docker command again. Expected: all scenarios pass.

- [ ] **Step 4: Inspect the final change set**

Run:

```bash
git status --short
git log --oneline origin/main..HEAD
git diff --stat origin/main...HEAD
```

Expected: only intended files are changed, the worktree is clean, and each
task has a focused commit.

- [ ] **Step 5: Update design deviations**

If implementation required a deviation, add a dated “Implementation Notes”
section to
`docs/superpowers/specs/2026-07-02-gcc-enhanced-rate-control-design.md`
describing the tested reason and final behavior. If there was no deviation,
make no documentation-only change.
