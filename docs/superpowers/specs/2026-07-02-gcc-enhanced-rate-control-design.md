# Enhanced GCC Rate-Control Design

Date: 2026-07-02

## 1. Objective

Improve the current GStreamer H.264 sender so that it:

- raises its bitrate quickly when additional capacity becomes available;
- lowers its bitrate promptly when delay or loss indicates congestion;
- feeds razor accurate per-packet send information;
- uses pacing and active probing instead of relying only on encoder output;
- keeps all newly written application and transport code in C++14.

The existing vendored razor C implementation remains in place and is accessed
only through the C++ RAII wrapper.

## 2. Current Root Causes

### 2.1 Incomplete send history

The current sender stores only the latest RTP sequence number seen by the pad
probe. The main loop samples this value about once every 10 ms and reports a
hard-coded size of 1400 bytes to razor. At most approximately 100 packets per
second are therefore registered, even when the sender transmits many more.

This limits the synthetic acknowledged bitrate to approximately:

```text
100 packets/s * 1400 bytes * 8 = 1.12 Mbps
```

Razor's AIMD controller bounds increases by acknowledged throughput. Starting
at 2 Mbps can therefore prevent any increase.

### 2.2 Duplicated feedback processing

The vendored `sender_on_feedback()` contains a duplicated proxy/REMB/loss
processing block. The first proxy pass removes matched send-history entries.
The second proxy pass cannot match them and returns before the associated loss
information is processed.

### 2.3 No effective pacing or probing

GStreamer sends directly through `udpsink`. Razor's pacer is constructed
without a send callback and never owns the media packets. The sender cannot
generate padding traffic to verify that spare capacity exists, so a
low-complexity encoder can leave the controller permanently application
limited.

### 2.4 Symmetric application-level smoothing

The application applies an additional loss reduction, waits at least 500 ms
between changes, and limits both increases and decreases to 20 percent. This
duplicates razor's loss response and unnecessarily delays congestion
backoff.

### 2.5 Invalid cross-host RTT calculation

The current RTT estimate subtracts receiver and sender `steady_clock` values.
Those clocks do not share an epoch across hosts. The feedback sequence number
is also never updated, while the existing `SendTimeTracker` is unused.

### 2.6 Thread-safety violations

Razor explicitly requires callers to provide serialization. The current
receiver invokes `on_received()` from a GStreamer streaming thread while
calling `heartbeat()` from the main thread. Several sequence and packet
counters are also shared without synchronization.

## 3. Selected Architecture

The sender pipeline changes from:

```text
rtph264pay -> udpsink
```

to:

```text
rtph264pay -> appsink -> C++ TransportSender -> razor Pacer -> UDP socket
```

The receiver keeps `udpsrc` and its existing H.264 decode pipeline. A receiver
pad probe observes both media and padding packets before depayloading.

All razor sender operations run on one GCC control thread. GStreamer callbacks
only copy packet data into bounded, thread-safe queues.

## 4. Components

### 4.1 `TransportSender`

Add a C++14 `TransportSender` component with the following responsibilities:

- own the media UDP socket and destination address;
- accept complete RTP packets copied from the GStreamer `appsink`;
- allocate a monotonic 32-bit extended transport sequence;
- write the low 16 bits into the RTP sequence-number field;
- retain packet bytes by extended packet ID until the pacer sends them;
- enqueue packet ID, retransmission flag, and exact packet size into razor;
- send the packet from the razor pacing callback;
- after successful `sendto()`, call `razor.on_send(seq, actual_size)`;
- record the successful send time for RTT calculation;
- expose queue depth, queued bytes, sent packets, and dropped packets.

The GStreamer streaming thread must never call razor directly. It pushes
copied packets to a bounded ingress queue and returns promptly.

If either the ingress queue or razor pacing queue reaches its configured
limit, the sender logs the event and drops the newly arriving packet. Queue
limits must be based on a maximum residence time of 250 ms and a byte limit.
No unbounded queue is permitted.

### 4.2 Razor C++ wrapper

Extend `razor_wrapper.h` without adding new application-level C files:

- accept a C++ pacing callback in the `Sender` constructor;
- expose `add_packet(packet_id, retrans, size)`;
- expose pacing queue delay and queue size where available;
- keep C callbacks as small static thunks into C++ callables;
- document that every public method must run on the GCC control thread.

### 4.3 `ProbeController`

Add a C++14 probe state machine:

```text
Idle -> Probing -> WaitingForResult -> Accepted | Failed
```

It generates valid RTP padding-only packets with:

- the same SSRC and payload type as the media stream;
- the shared transport sequence space;
- a current media RTP timestamp;
- the RTP padding bit and a valid padding length;
- no H.264 media payload.

Probe packets use the same transport, pacer, send history, receiver arrival
tracking, and feedback path as media packets.

Initial defaults:

- first probe rate: `2.0 * current_target`;
- subsequent probe rate: between `1.5` and `2.0 * accepted_rate`;
- probe duration: 100 ms;
- minimum success ratio: acknowledged rate at least 75 percent of probe rate;
- maximum configured rate: 8 Mbps;
- retry after a stable interval: no earlier than 5 seconds;
- abort immediately on delay overuse or significant loss.

Probe parameters must be named constants or command-line configuration, not
scattered numeric literals.

### 4.4 Receiver probe handling

The receiver pad probe:

1. parses every RTP packet;
2. queues `{seq, arrival_time, actual_size, is_padding}`;
3. lets the serialized receiver control loop call razor `on_received()`;
4. returns `GST_PAD_PROBE_DROP` for padding-only packets after observation;
5. returns `GST_PAD_PROBE_OK` for H.264 media packets.

This prevents padding from reaching `rtph264depay` while still including it in
bandwidth estimation.

### 4.5 Serialized control loop

The sender control thread owns:

- razor heartbeat;
- draining media ingress packets into the pacer;
- pacing send callbacks;
- feedback parsing;
- RTT updates;
- probe state transitions;
- target bitrate decisions;
- encoder bitrate updates.

The receiver likewise serializes queued packet-arrival events and razor
heartbeat calls. No razor object is accessed concurrently.

## 5. Feedback and RTT

The receiver must populate the outer feedback sequence with the latest packet
covered by the feedback.

The sender records a local monotonic send time for each successfully sent
sequence. On feedback:

```text
network_rtt =
    sender_feedback_receive_time
    - sender_packet_send_time
    - receiver_residence_time

receiver_residence_time =
    receiver_feedback_send_time
    - receiver_packet_receive_time
```

Each subtraction uses timestamps from one clock domain, so arbitrary clock
offsets between hosts do not affect the result.

The outer loss field becomes Q8 loss (`0..255`) for diagnostics, matching
razor's loss representation. It is not used as a second independent bitrate
reduction.

Malformed feedback must be rejected before decoding:

- validate magic and protocol version;
- validate exact fixed-header availability;
- validate payload size against both the datagram length and configured
  maximum;
- convert multi-byte fields with explicit network byte order;
- reject feedback for unknown or expired sequence numbers.

## 6. Rate-Control Policy

### 6.1 Single source of truth

Razor produces the base network target. Remove the application-level
loss-proportional multiplication so loss is applied only once.

The encoder target is derived from the network target:

```text
encoder_target = network_target * media_utilization
media_utilization default = 0.92
```

This reserves capacity for RTP/UDP overhead, keyframes, and encoder variation.

### 6.2 Fast decrease

Delay overuse and loss-triggered decreases bypass the increase debounce:

- delay overuse uses approximately `0.85 * acknowledged_bitrate`;
- valid lower razor targets apply immediately;
- an isolated update cannot reduce more than 50 percent;
- repeated valid congestion updates may continue reducing;
- minimum configured bitrate remains enforced.

The 50-percent guard protects against malformed or transient feedback. It is
not a timer and must not delay subsequent congestion responses.

### 6.3 Controlled increase

Normal non-probe increases remain conservative:

- approximately 8 percent per second while far from capacity;
- additive increase near the estimated capacity;
- no unrestricted increase while application limited;
- no ordinary increase until at least one RTT after a major reduction.

A successful probe may bypass the ordinary increase slope and set the network
target to the validated probe result, bounded by configured maximum bitrate.

### 6.4 Pacing

Replace razor's hard-coded normal pacing factor of 3 with a configurable
factor:

- normal default: `1.2 * network_target`;
- probe period: explicit probe rate;
- never exceed the configured maximum probe rate;
- abort probing when pacing queue delay exceeds its limit.

## 7. Required Razor Corrections

Make only focused changes to vendored razor:

- restore `sender_on_feedback()` to one proxy block, one REMB block, and one
  loss block;
- make the pacing multiplier configurable rather than hard-coded;
- expose read-only diagnostic values needed by tests and logging;
- add length checks before decoding feedback;
- do not otherwise refactor or replace the estimator in this phase.

LossBasedBweV2 is explicitly out of scope until the corrected classic loss
path has measurable integration results.

## 8. Observability

Emit one structured diagnostic line per second:

```text
encoder_kbps target_kbps pacing_kbps acked_kbps
delay_state trend loss_pct rtt_ms
media_packets probe_packets feedback_packets matched_packets
pacer_queue_ms ingress_queue_bytes probe_state
```

Also log state transitions and causes:

- probe start, success, failure, timeout, or congestion abort;
- delay overuse and recovery;
- loss decrease;
- queue overflow;
- feedback decode rejection;
- encoder target change.

Logging must be rate-limited outside state transitions.

## 9. Testing

All new tests are C++14 tests integrated with CMake/CTest.

### 9.1 Unit tests

- registering N RTP packets preserves all N sequence numbers and exact sizes;
- combined proxy and loss feedback processes each section exactly once;
- pacing sends no more than its budget and does not burst after delayed
  heartbeats;
- probe state transitions cover success, loss failure, overuse abort, and
  timeout;
- probe padding packets are included in feedback and excluded from H.264
  depayloading;
- RTT remains correct when sender and receiver clocks have different
  simulated offsets;
- decreases bypass increase smoothing;
- the 50-percent single-update guard and minimum bitrate are enforced;
- malformed feedback is rejected without reading beyond the datagram.

### 9.2 Integration tests

Use the existing Docker/tc environment with deterministic scenarios:

1. Start at 1 Mbps on an 8 Mbps link. Active probes should produce a clear
   increase within approximately 5 seconds.
2. Drop capacity from 8 Mbps to 1.5 Mbps. The target should return to a
   reasonable range within approximately 1 second without an unbounded pacing
   queue.
3. Restore capacity from 1.5 Mbps to 6 Mbps. A recovery probe should reach at
   least 4.5 Mbps within 5 to 8 seconds.
4. Apply 3 percent random loss without a capacity reduction. The target must
   not collapse persistently to minimum bitrate.
5. Apply 10 percent loss or sustained queue growth. The target must decrease
   promptly.

Tests should assert ranges and deadlines rather than exact bitrate samples.

## 10. Implementation Sequence

1. Add regression tests for duplicated feedback and incomplete packet
   registration.
2. Fix duplicated feedback processing.
3. Add serialized per-packet registration with exact sizes.
4. Introduce `TransportSender` and enable razor pacing.
5. Correct RTT using send-history lookup.
6. Apply asymmetric increase/decrease policy and remove duplicate loss
   reduction.
7. Add `ProbeController` and receiver padding handling.
8. Add diagnostics.
9. Run unit and tc/netem integration tests before tuning parameters.

Each step must be verified independently. Probe and tuning work must not begin
until complete packet registration and loss feedback are demonstrated by
tests.

## 10.1 Implementation Notes (2026-07-03)

Integration testing exposed three cases where the verified implementation
needs more specific behavior than the initial design:

- A 100 ms probe cannot be validated reliably from razor's approximately
  500 ms smoothed acknowledged-rate window. `ProbeDeliveryEstimator` therefore
  validates the active probe from exact send history and receiver timestamps,
  using the lower of its send and receive interval rates. The acknowledged
  rate remains an input, and the accepted result is still capped by the active
  probe rate and configured maximum.
- Probe traffic can also produce delayed ordinary razor bitrate callbacks.
  Exact probe history ranges guard those callbacks until feedback or bounded
  send-history progress passes the final probe packet. The range and watermark
  comparisons include saturated counters and 32-bit ordinal wrap.
- Under sustained netem loss, corrected classic razor loss processing did not
  always emit a lower target even though the outer feedback loss was already
  high. A C++ `OuterLossBackoff` is therefore an emergency fallback: rolling
  256-packet loss strictly above Q8 value 25 halves the current target through
  `RatePolicy`, no more often than `max(250 ms, RTT)`. It is disabled for
  probe-related feedback and resets after low loss. This is not the removed
  proportional loss multiplier; it is a thresholded, rate-limited congestion
  event whose random-loss and heavy-loss behavior is covered by the tc/netem
  matrix.

The explicit probe pacing rate is restored by the C++ razor wrapper after
razor bitrate callbacks, because those callbacks internally recompute the
normal pacing estimate.

## 11. Non-Goals

- replacing razor with libwebrtc;
- implementing LossBasedBweV2;
- adding retransmission, NACK, FEC, or a jitter buffer;
- changing the H.264 codec or receiver display behavior;
- rewriting vendored razor from C to C++;
- supporting multiple simultaneous media streams in the first iteration.
