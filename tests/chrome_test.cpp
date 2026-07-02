#include "../src/chrome.h"
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>

static void test_hwstats_plausible_ranges() {
    HwStats hw = getHwStats();
    assert(hw.load1 >= 0.0);
    assert(hw.memTotalGB > 0.0);       // every real Mac has nonzero total RAM
    assert(hw.memUsedGB >= 0.0);
    assert(hw.memUsedGB <= hw.memTotalGB + 1.0); // +1 slack for rounding
    assert(hw.cpuPercent >= 0.0 && hw.cpuPercent <= 100.0);
}

static void test_cpu_percent_uses_delta_between_calls() {
    // First call in the process has no prior sample -- verified by the
    // ranges test above already returning a valid (possibly 0) value.
    // Two back-to-back calls should each still land in [0, 100]; this is
    // mostly a smoke test that repeated sampling doesn't corrupt the static
    // previous-sample state (e.g. counter underflow from bad delta math).
    HwStats a = getHwStats();
    HwStats b = getHwStats();
    assert(a.cpuPercent >= 0.0 && a.cpuPercent <= 100.0);
    assert(b.cpuPercent >= 0.0 && b.cpuPercent <= 100.0);
}

static void test_find_git_branch_in_real_repo() {
    system("rm -rf /tmp/ark_chrome_test_repo");
    system("mkdir -p /tmp/ark_chrome_test_repo");
    system("cd /tmp/ark_chrome_test_repo && git init -q -b main "
           "&& git -c user.email=t@t.com -c user.name=t commit -q --allow-empty -m init");
    std::string branch = findGitBranch("/tmp/ark_chrome_test_repo");
    assert(branch == "main");
    system("rm -rf /tmp/ark_chrome_test_repo");
}

static void test_find_git_branch_from_subdirectory() {
    system("rm -rf /tmp/ark_chrome_test_repo2");
    system("mkdir -p /tmp/ark_chrome_test_repo2/sub/deeper");
    system("cd /tmp/ark_chrome_test_repo2 && git init -q -b feature-x "
           "&& git -c user.email=t@t.com -c user.name=t commit -q --allow-empty -m init");
    std::string branch = findGitBranch("/tmp/ark_chrome_test_repo2/sub/deeper");
    assert(branch == "feature-x"); // walks up parent directories to find .git
    system("rm -rf /tmp/ark_chrome_test_repo2");
}

static void test_find_git_branch_not_a_repo() {
    std::string branch = findGitBranch("/tmp");
    assert(branch.empty());
}

int main() {
    test_hwstats_plausible_ranges();
    test_cpu_percent_uses_delta_between_calls();
    test_find_git_branch_in_real_repo();
    test_find_git_branch_from_subdirectory();
    test_find_git_branch_not_a_repo();
    std::cout << "all chrome hwstats tests passed\n";
}
