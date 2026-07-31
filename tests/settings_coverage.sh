#!/bin/sh
# Keeps arkcfg::knownSettings() honest.
#
# The deprecation warning at startup says "ark does not recognise this setting"
# -- which is only true if the list backing it actually matches what the code
# reads. A knob added to the source but not to the list would make ark warn
# about a setting that works. This check diffs the two and fails the suite if
# they drift.
set -e
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

used=$(grep -rhoE 'getenv\("ARK_[A-Z0-9_]+"\)' src/ | grep -oE 'ARK_[A-Z0-9_]+' | sort -u)
# Anchor on the DEFINITIONS, not every mention of the name -- a call site like
# `known = knownSettings()` would otherwise start a range that runs on into the
# next function and swallows its contents.
listed=$(sed -n '/^const std::vector<std::string>& knownSettings/,/^}/p' src/config_sync.cpp |
         grep -oE '"ARK_[A-Z0-9_]+"' | tr -d '"' | sort -u)
# A deprecated name is deliberately read-but-unlisted (or listed-but-unread).
# Commented-out template entries in that table don't count.
deprecated=$(sed -n '/^const std::vector<Deprecation>& deprecatedSettings/,/^}/p' src/config_sync.cpp |
             grep -v '^\s*//' |
             grep -oE '"ARK_[A-Z0-9_]+"' | tr -d '"' | sort -u)

missing=$(comm -23 <(echo "$used") <(echo "$listed"))
extra=$(comm -13 <(echo "$used") <(echo "$listed"))
[ -n "$deprecated" ] && extra=$(comm -23 <(echo "$extra") <(echo "$deprecated"))

fail=0
if [ -n "$missing" ]; then
    echo "FAIL: settings_coverage -- read by the code, absent from knownSettings():"
    echo "$missing" | sed 's/^/  /'
    fail=1
fi
if [ -n "$extra" ]; then
    echo "FAIL: settings_coverage -- in knownSettings(), never read:"
    echo "$extra" | sed 's/^/  /'
    fail=1
fi
[ $fail -eq 0 ] && echo "settings coverage: knownSettings() matches the source"
exit $fail
