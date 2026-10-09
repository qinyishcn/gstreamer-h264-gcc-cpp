#include "gst_experiment.hpp"
#include <gst/gst.h>
#include <iostream>
#include <stdexcept>

static void check(bool v, const char* msg) {
    if (!v) throw std::runtime_error(msg);
}
int main(int argc, char** argv) {
    gst_init(&argc, &argv);
    using namespace resilience;
    require_plugins();
    Clip c{"synthetic", 160, 96, 30, 1, {}};
    for (int t = 0; t < 90; ++t) {
        Frame f = black_i420(c.width, c.height);
        for (int y = 0; y < c.height; ++y)
            for (int x = 0; x < c.width; ++x)
                f[y * c.width + x] = 40 + ((x + y + t * 3) % 160);
        c.frames.push_back(std::move(f));
    }
    auto e = encode(c, 2, 30, 500, 1500);
    check(!e.packets.empty() && e.max_packet <= 1472, "RTP packet MTU bound");
    auto clean = decode(c, e, 0, 42, false);
    check(clean.frames.size() == c.frames.size(), "loss-free frame count");
    for (std::size_t i = 0; i < clean.frames.size(); ++i) {
        check(clean.frames[i].index == i, "loss-free PTS alignment");
        check(psnr(squared_error(c.frames[i], clean.frames[i].pixels, 0,
                  c.width * c.height), c.width * c.height) > 20,
              "loss-free reconstruction quality");
    }
    auto lost = decode(c, e, 1, 42, false);
    check(lost.frames.empty() && lost.lost == e.packets.size(), "100% loss");
    auto partial = decode(c, e, .02, 42, false);
    check(partial.lost > 0 && partial.frames.size() <= c.frames.size(), "partial loss decode");
    auto timeline = conceal(partial.frames, c.frames.size(), c.width, c.height, true);
    check(timeline.size() == 90, "conceal full source timeline");
    auto repeat = decode(c, e, .02, 42, false);
    check(repeat.lost == partial.lost && repeat.frames.size() == partial.frames.size(),
          "repeatable trial");
    for (std::size_t i = 0; i < repeat.frames.size(); ++i)
        check(repeat.frames[i].index == partial.frames[i].index &&
              repeat.frames[i].pixels == partial.frames[i].pixels,
              "repeatable decoded pixels");
    auto multi = encode(c, 4, 120, 3000, 1500);
    auto damaged_multi = decode(c, multi, .02, 42, false);
    check(conceal(damaged_multi.frames, 90, c.width, c.height, true).size() == 90,
          "duplicate PTS in damaged multi-slice video must not abort playout");
    if (argc == 2) {
        std::vector<DisplayFrame> original;
        for (const auto& f : c.frames) original.push_back({f, false, false, false});
        save_y4m(argv[1], c, original);
    }
    std::cout << "pipeline tests passed: " << e.packets.size() << " packets, "
              << partial.lost << " lost, " << partial.frames.size() << " decoded\n";
}
