#!/usr/bin/env bash
# DESIGN_LANGUAGE.md §11 greps. Token definitions live in ui/theme/.
set -euo pipefail

cd "$(dirname "$0")/.."
UI="apps/app/src/main/java/io/upix/ui"
fail=0

# Drop theme definitions; all screen code is checked.
filter() {
    grep -v '/theme/' || true
}

# Each scan guards its leftmost grep: under `set -e` + `pipefail` a grep that
# selects nothing exits 1, which would abort the script (with no ok/FAIL line)
# precisely when the tree is clean. `filter` already ends in `|| true`.
hits="$( { grep -rn --include='*.kt' 'Color(0x' "$UI" || true; } | filter)"
if [[ -n "$hits" ]]; then
    echo "FAIL: Color(0x outside ui/theme"
    printf '%s\n' "$hits"
    fail=1
else
    echo "ok: Color(0x"
fi

hits="$( { grep -rn --include='*.kt' 'RoundedCornerShape(' "$UI" || true; } | filter)"
if [[ -n "$hits" ]]; then
    echo "FAIL: RoundedCornerShape( outside ui/theme"
    printf '%s\n' "$hits"
    fail=1
else
    echo "ok: RoundedCornerShape("
fi

hits="$( { grep -rnE --include='*.kt' 'shadowElevation|defaultElevation|tonalElevation' "$UI" || true; } | filter)"
if [[ -n "$hits" ]]; then
    echo "FAIL: elevation APIs"
    printf '%s\n' "$hits"
    fail=1
else
    echo "ok: elevation APIs"
fi

hits="$( { grep -rnE --include='*.kt' '\.sp\b' "$UI" || true; } | filter)"
if [[ -n "$hits" ]]; then
    echo "FAIL: raw .sp outside ui/theme"
    printf '%s\n' "$hits"
    fail=1
else
    echo "ok: .sp"
fi

hits="$( { grep -rnE --include='*.kt' 'FontWeight\.' "$UI" || true; } | filter)"
if [[ -n "$hits" ]]; then
    echo "FAIL: FontWeight.* outside ui/theme"
    printf '%s\n' "$hits"
    fail=1
else
    echo "ok: FontWeight.*"
fi

hits="$( { grep -rnE --include='*.kt' 'Icons\.(Rounded|Sharp|TwoTone)\.' "$UI" || true; } | filter)"
if [[ -n "$hits" ]]; then
    echo "FAIL: forbidden icon families (Rounded/Sharp/TwoTone)"
    printf '%s\n' "$hits"
    fail=1
else
    echo "ok: icon families"
fi

hits="$( { grep -rnE --include='*.kt' 'typography\.(display|headline)' "$UI" || true; } | filter)"
if [[ -n "$hits" ]]; then
    echo "FAIL: banned typography slots (display/headline)"
    printf '%s\n' "$hits"
    fail=1
else
    echo "ok: typography slots"
fi

exit "$fail"
