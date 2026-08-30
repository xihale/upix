#!/usr/bin/env bash
# DESIGN_LANGUAGE.md §11 greps. Token definitions live in ui/theme/;
# BottomActionDock still has two known hits (pill shape + dock shadow) and
# is allowlisted so this stays a regression gate, not a born-red job.
set -euo pipefail

cd "$(dirname "$0")/.."
UI="apps/app/src/main/java/io/upix/ui"
fail=0

# Drop theme definitions and the one allowlisted file.
filter() {
    grep -v '/theme/' | grep -v '/screens/home/BottomActionDock.kt' || true
}

hits="$(grep -rn --include='*.kt' 'Color(0x' "$UI" | filter)"
if [[ -n "$hits" ]]; then
    echo "FAIL: Color(0x outside ui/theme"
    printf '%s\n' "$hits"
    fail=1
else
    echo "ok: Color(0x"
fi

hits="$(grep -rn --include='*.kt' 'RoundedCornerShape(' "$UI" | filter)"
if [[ -n "$hits" ]]; then
    echo "FAIL: RoundedCornerShape( outside ui/theme"
    printf '%s\n' "$hits"
    fail=1
else
    echo "ok: RoundedCornerShape("
fi

hits="$(grep -rnE --include='*.kt' 'shadowElevation|defaultElevation|tonalElevation' "$UI" | filter)"
if [[ -n "$hits" ]]; then
    echo "FAIL: elevation APIs"
    printf '%s\n' "$hits"
    fail=1
else
    echo "ok: elevation APIs"
fi

exit "$fail"
