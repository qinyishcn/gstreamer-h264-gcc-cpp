# H.265 RTP resilience experiment (2026-10-08)

## Goal and scope
Build a standalone C++17 GStreamer experiment beside the existing H.264/GCC
repository. Encoding, packet loss injection, temporal concealment, metric
calculation and report generation are all C++. Keep the previous GCC project
unchanged. This is a deterministic RTP packet transport experiment, not a claim
of a physical network or hardware decoder qualification.

## Design
Read public 8-bit 4:2:0 Y4M video, encode using x265enc, packetize using
rtph265pay, collect the real RTP packets, drop independently selected packets,
then replay surviving packets through rtph265depay/h265parse/avdec_h265.
Preserve source-frame timestamps. Compare strict_black, native_black,
native_hold, slices_hold, short_gop_hold and slices_short_gop_hold.

Use IPv4 IP MTU 1500: RTP buffer limit 1472 bytes, including RTP header, with
20-byte IPv4 and 8-byte UDP overhead. This is a maximum, not padding every packet
to 1500 bytes. Apply loss to all packets, including VPS/SPS/PPS and IDR packets.
No secret reliable startup or keyframe protection. Base GOP 120, short GOP 30,
single or four independent slices, fixed target bitrate 3000 kbit/s, no B frames,
closed GOP, repeated parameter sets at IDRs. Document measured bitrate.

Tolerant decoding allows corrupted output and unlimited consecutive decode
errors. It cannot reconstruct arbitrarily missing reference data or guarantee
a frame from every damaged access unit. Hold mode repeats the most recently
decoded frame; before any frame exists it displays legal-range black. Never
use a future decoded frame or skip a missing frame in the PSNR denominator.
Periodic IDR is the implemented recovery; RTCP feedback, retransmission, FEC
and motion-compensated decoder modification are outside this first experiment.

## Evaluation
Two 720p public-domain sequences: FourPeople and KristenAndSara, first 300 frames
at native 60fps. Due to slow raw-Y4M transfers, use official WebM distribution
decoded to a fixed reference; explicitly distinguish this from lossless masters.
Loss rates 0, 0.1%, 0.5%, 1%, 2%, 5%, 10%; seeds 17, 42, 2026.
Report every seed, actual packet loss, pooled-MSE PSNR-Y and PSNR-YUV, received
frames, inserted hold/black frames, corrupted-frame flags, longest no-output
interval, and RTP/IP-equivalent bitrate. Across-seed summaries show mean and
standard deviation, not unsupported confidence guarantees.

Acceptance: loss-free timestamp alignment outputs all source frames; each
trial is bounded and errors are reported; every source frame contributes to
metrics; 100% loss produces black rather than a crash; masks are reproducible;
packet size is checked; output includes source provenance, CSV, curves, and
viewable impaired videos. Results must be measured, never filled with estimates.
