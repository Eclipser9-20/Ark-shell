#!/usr/bin/env bash
# tests/run_tests.sh — runs every tests/cases/*.ark through ./ark, diffs vs .expected
set -u
cd "$(dirname "$0")/.."
fail=0
for ark_file in tests/cases/*.ark; do
    name=$(basename "$ark_file" .ark)
    expected="tests/cases/${name}.expected"
    actual=$(./ark < "$ark_file" 2>&1)
    want=$(cat "$expected")
    if [[ "$actual" == "$want" ]]; then
        echo "PASS: $name"
    else
        echo "FAIL: $name"
        echo "  expected: $want"
        echo "  actual:   $actual"
        fail=1
    fi
done

# Source-level check: the settings list that backs ark's "unrecognised setting"
# warning must match the settings the code actually reads.
bash tests/settings_coverage.sh || fail=1

exit $fail
