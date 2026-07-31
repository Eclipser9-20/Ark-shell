// Tests for ark.config maintenance: the additive merge must never claim to
// need something the user already has, must respect a deleted suggestion, and
// must only warn about settings the user ACTIVELY sets.
#include "config_sync.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

static bool has(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& x : v) if (x == s) return true;
    return false;
}
static bool hasSubstr(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& x : v) if (x.find(s) != std::string::npos) return true;
    return false;
}

static const char* kTemplate =
    "# --- FEATURES ---\n"
    "# export ARK_GHOST_TEXT=0     # ghost text\n"
    "# export ARK_BANNER=0         # banner\n"
    "# alias la='ls -A'\n"
    "# mkcd() { mkdir -p \"$1\" && cd \"$1\"; }\n";

static void test_missing_reports_only_absent_entries() {
    std::string cfg = "# export ARK_GHOST_TEXT=0     # ghost text\n";
    auto m = arkcfg::missingEntries(kTemplate, cfg, {});
    assert(!has(m, "ARK_GHOST_TEXT"));   // already present, even though commented
    assert(has(m, "ARK_BANNER"));
    assert(has(m, "alias:la"));
    assert(has(m, "fn:mkcd"));
}

static void test_user_edited_function_is_not_re_offered() {
    // The user rewrote mkcd. The template's version must NOT be re-added --
    // that is the whole point of an additive merge.
    std::string cfg = "mkcd() { mkdir -p \"$1\" && cd \"$1\" && ls; }\n";
    auto m = arkcfg::missingEntries(kTemplate, cfg, {});
    assert(!has(m, "fn:mkcd"));
}

static void test_ledger_makes_deletion_stick() {
    std::string cfg = "";   // user deleted everything we offered
    auto m = arkcfg::missingEntries(kTemplate, cfg, {"ARK_BANNER", "fn:mkcd"});
    assert(!has(m, "ARK_BANNER"));
    assert(!has(m, "fn:mkcd"));
    assert(has(m, "ARK_GHOST_TEXT"));   // never offered, still missing
}

static void test_substring_names_do_not_collide() {
    // ARK_BANNER must not be considered "present" because ARK_BANNER_LOGO is.
    std::string cfg = "export ARK_BANNER_LOGO=ark\n";
    auto m = arkcfg::missingEntries(kTemplate, cfg, {});
    assert(has(m, "ARK_BANNER"));
}

static void test_commented_setting_never_warns() {
    // A commented-out example is not something the user set.
    auto w = arkcfg::settingWarnings("# export ARK_MADE_UP_THING=1\n");
    assert(w.empty());
}

static void test_active_unknown_setting_warns_with_suggestion() {
    auto w = arkcfg::settingWarnings("export ARK_GHOST_TEX=1\n");
    assert(w.size() == 1);
    assert(hasSubstr(w, "ARK_GHOST_TEX"));
    assert(hasSubstr(w, "did you mean ARK_GHOST_TEXT?"));
}

static void test_known_setting_never_warns() {
    auto w = arkcfg::settingWarnings("export ARK_GHOST_TEXT=0\nARK_BANNER=1\n");
    assert(w.empty());
}

static void test_warning_reports_line_number() {
    auto w = arkcfg::settingWarnings("# a comment\n\nexport ARK_NOPE_AT_ALL=1\n");
    assert(w.size() == 1);
    assert(hasSubstr(w, "ark.config:3:"));
}

static void test_each_unknown_reported_once() {
    auto w = arkcfg::settingWarnings("export ARK_NOPE_AT_ALL=1\nexport ARK_NOPE_AT_ALL=2\n");
    assert(w.size() == 1);
}

int main() {
    test_missing_reports_only_absent_entries();
    test_user_edited_function_is_not_re_offered();
    test_ledger_makes_deletion_stick();
    test_substring_names_do_not_collide();
    test_commented_setting_never_warns();
    test_active_unknown_setting_warns_with_suggestion();
    test_known_setting_never_warns();
    test_warning_reports_line_number();
    test_each_unknown_reported_once();
    std::cout << "all config-sync tests passed\n";
    return 0;
}
