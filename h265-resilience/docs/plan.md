# Implementation and verification plan

1. Write C++ tests for deterministic packet loss, IP MTU calculation, PSNR,
   timestamp alignment, hold-last concealment and initial black output. Observe
   missing implementation failure, implement and run tests.
2. Build a small GStreamer RAII pipeline layer. Encode one reference clip into
   real RTP packets, cache each GOP/slice variant, and replay packet losses into
   software decoder. Check zero-loss count and bounded pipeline completion.
3. Run synthetic 0%, partial and 100% loss smoke tests before dataset tests.
4. Download bounded prefixes of public Y4M sequences with provenance and hashes;
   test 6 modes x 7 rates x 3 seeds x 2 clips. Retain per-trial and per-frame CSV.
5. Generate Markdown and self-contained HTML/SVG report and representative
   impaired Y4M video. Verify all rows, all timeline slots, and finite metrics.
6. Document build/run instructions, exact environment, limitations, and measured
   tradeoffs. Deliver executable source, results and report without publishing.

Commands (WSL Ubuntu):

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```
