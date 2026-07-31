#pragma once
#include <string>
#include <utility>
#include <vector>

// ── ark.config maintenance ───────────────────────────────────────────────────
// Keeps a user's ark.config current with the shipped template WITHOUT ever
// touching what they wrote.
//
// Two independent jobs:
//
//  1. ADDITIVE SYNC. When a new ark release documents settings that didn't
//     exist when the user's config was written, those lines get APPENDED. The
//     merge is append-only by construction -- it never rewrites, reorders, or
//     deletes a byte of the existing file, so an alias or function the user
//     edited stays exactly as they left it even when the template ships a
//     different version of the same name.
//
//     Deleting a suggestion is respected. A ledger of everything ever offered
//     lives beside the config; an entry is added at most once, so removing a
//     line you don't want does not get undone on the next start.
//
//  2. DEPRECATION WARNINGS. Any ARK_* setting the user ACTIVELY assigns (a
//     commented-out example doesn't count) that ark no longer understands is
//     reported at startup, with the replacement when there is one and a
//     nearest-match suggestion when it looks like a typo.

namespace arkcfg {

// Every ARK_* setting this build understands. Kept in sync with the source by
// tests/settings_coverage.sh, which fails the suite if the code reads a knob
// that isn't listed here (or lists one nothing reads).
const std::vector<std::string>& knownSettings();

// A setting that used to exist. `replacement` is empty when it was simply
// removed; otherwise it names what to use instead.
struct Deprecation {
    std::string name;
    std::string replacement;
    std::string note;
};
const std::vector<Deprecation>& deprecatedSettings();

struct Report {
    std::vector<std::string> added;          // template entries appended
    std::vector<std::string> warnings;       // one line per deprecated/unknown setting
    std::string backupPath;                  // written iff something was appended
    bool wrote = false;
};

// Run both jobs against `configPath`. `dryRun` reports what WOULD change and
// writes nothing. A missing config file is not an error -- it just means there
// is nothing to sync (creation is `ark --setup`'s job).
Report syncConfig(const std::string& configPath, bool dryRun);

// Exposed for testing: which template entries are missing from `configText`,
// given a ledger of entries already offered.
std::vector<std::string> missingEntries(const std::string& templateText,
                                        const std::string& configText,
                                        const std::vector<std::string>& ledger);

// Exposed for testing: deprecation/unknown warnings for a config's text.
std::vector<std::string> settingWarnings(const std::string& configText);

} // namespace arkcfg
