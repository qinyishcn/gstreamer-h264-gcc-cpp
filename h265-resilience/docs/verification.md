# Verification evidence (2026-10-08)

Environment: WSL2 Ubuntu, GCC 15.2, C++17, CMake, GStreamer 1.28.2.
Data preparation and independent metric check: Windows FFmpeg 8.1.

Publication check (2026-10-09): a fresh Release build of the repository copy
passed core, pipeline and live_udp (3/3, 5.72 seconds). The published matrix
contains 252 trial rows, 75,600 frame rows and 84 summary groups. The copied PDF
matches the delivered PDF byte-for-byte (SHA256
`b03e52df0c3308e61d240b1fd7cf0f0ccfdbf74542a00872622a449e24178343`).
Raw source media, per-trial Y4M examples and build outputs are excluded from Git.

## Source files

- FourPeople WebM: https://media.xiph.org/video/derf/webm/FourPeople_1280x720_60.webm
  - SHA256: `1e4a5df7e67ae985370321cc2c91b8595f7e3433ad8ee1e84da7a26ff5254deb`
- KristenAndSara WebM: https://media.xiph.org/video/derf/webm/KristenAndSara_1280x720_60.webm
  - SHA256: `7d32a6d6864804b986832272ed570a2e4b43552d5958fc1f614d1e7e0fbd2436`

Both are listed public domain by Xiph. Fixed references are the first 300 frames
of the WebM-decoded 720p I420 images at 60fps, not original lossless masters.

## Automated tests

`ctest --test-dir build --output-on-failure`: core, pipeline, live_udp all passed.
Core tests cover known-answer PSNR, deterministic Bernoulli masks, 0% and 100%
loss, IPv4 MTU accounting, missing first/last frame concealment, no future-frame
use, duplicate timestamps and out-of-range corrupted timestamps.

Pipeline tests check actual encoding/decoding, zero-loss count and alignment,
MTU bounds, partial and total loss, and bitwise repeated decode equality for a
fixed source/encoded stream and mask. Damaged four-slice AUs are also exercised.

UDP smoke log example: 594 packet attempts, 7 injected losses; receiver produced
89 decoder outputs, 146 display frames, 71 repeated frames, startup_no_image=0.
Live scheduling changes those exact receiver counts slightly between runs;
the test checks actual loss and successful nonempty sustained output, not an
unrealistic exact scheduling count. The receiver continued holding after sender EOS.

## Matrix

Final data: `results/final/trials.csv` and `results/final/frames.csv`.
2 clips x 6 modes x 7 rates x 3 seeds = 252 trials, 84 groups, 75,600 scored slots.
The report generator checks trial uniqueness, complete groups, finite PSNR,
zero-loss complete output, MTU, and timeline accounting before writing the report.

## Independent FFmpeg PSNR calculation

Commands (same pattern for both clips):

```
ffmpeg -i data/FourPeople.y4m \
  -i results/final/examples/FourPeople_slices_short_gop_hold_loss2_seed42.y4m \
  -lavfi psnr -f null -
```

- FourPeople: Y=16.738555 dB; YUV=18.467864 dB.
- KristenAndSara: Y=15.384173 dB; YUV=17.041005 dB.

Both agree with the C++ trial values to displayed precision. Comparison images
were inspected: native damaged output can become mostly gray; four-slice mode
retains some intact regions but can still be badly damaged. Do not interpret
successful decode as correct/high-quality reconstruction.

## Scope limits

No hardware decoder qualification, native Windows application build, jitter/RTT
PSNR model, burst-loss model, codec-internal motion/spatial concealment redesign,
RTCP PLI, retransmission or FEC results are claimed. Existing H.264/GCC files
remain untouched.
