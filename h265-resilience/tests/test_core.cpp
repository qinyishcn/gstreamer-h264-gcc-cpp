#include "resilience.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main() {
    using namespace resilience;
    require(rtp_limit(1500) == 1472, "IPv4 MTU accounting");
    bool rejected = false;
    try { rtp_limit(40); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "reject MTU too small for RTP");
    LossMask zero(0, 42), all(1, 42), a(.1, 17), b(.1, 17);
    int lost = 0;
    for (int i = 0; i < 10000; ++i) {
        require(!zero.drop(), "zero loss");
        require(all.drop(), "all loss includes startup");
        bool x = a.drop();
        require(x == b.drop(), "repeatable loss mask");
        lost += x;
    }
    require(lost > 850 && lost < 1150, "Bernoulli loss distribution");
    Frame original(12, 100), distorted(12, 90);
    require(std::abs(psnr(squared_error(original, distorted, 0, 8), 8) -
                     28.1308036087) < 1e-8, "PSNR known answer");
    require(std::isinf(psnr(0, 8)), "identical frame has infinite PSNR");
    auto black = black_i420(4, 2);
    require(black.size() == 12 && black[0] == 16 && black[8] == 128,
            "limited-range I420 startup black");
    std::vector<DecodedFrame> decoded{{1, Frame(12, 50), false},
                                       {3, Frame(12, 90), false}};
    auto timeline = conceal(decoded, 5, 4, 2, true);
    require(timeline[0].pixels == black, "do not use future frame");
    require(timeline[1].pixels[0] == 50 && !timeline[1].inserted, "exact alignment");
    require(timeline[2].pixels[0] == 50 && timeline[2].inserted, "hold last");
    require(timeline[4].pixels[0] == 90 && timeline[4].inserted, "hold tail");
    auto missing = conceal({}, 5, 4, 2, true);
    require(missing.size() == 5 && missing[4].pixels == black, "all-loss timeline");
    auto no_hold = conceal(decoded, 5, 4, 2, false);
    require(no_hold[2].pixels == black, "baseline black fallback");
    decoded.push_back({1, Frame(12, 200), true});
    auto duplicates = conceal(decoded, 5, 4, 2, true);
    require(duplicates[1].pixels[0] == 50, "first output wins duplicate PTS");
    decoded.push_back({5, Frame(12, 250), true});
    auto outside = conceal(decoded, 5, 4, 2, true);
    require(outside.size() == 5 && outside[4].pixels[0] == 90,
            "discard corrupt decoder timestamp outside source timeline");
    require(frame_index(2 * 1000000000ULL / 60, 60, 1) == 2,
            "nanosecond rounding must not shift frame");
    std::cout << "core tests passed\n";
}
