#include "../src/chrome.h"
#include <cassert>
#include <iostream>

static void test_hwstats_plausible_ranges() {
    HwStats hw = getHwStats();
    assert(hw.load1 >= 0.0);
    assert(hw.memTotalGB > 0.0);       // every real Mac has nonzero total RAM
    assert(hw.memUsedGB >= 0.0);
    assert(hw.memUsedGB <= hw.memTotalGB + 1.0); // +1 slack for rounding
}

int main() {
    test_hwstats_plausible_ranges();
    std::cout << "all chrome hwstats tests passed\n";
}
