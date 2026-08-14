#!/usr/bin/env bash
#
# android_test.sh — one-shot automated Android verification for CrossScale.
#
# Phases:
#   1. Unit tests       : ./gradlew testDebugUnitTest   (blocking)
#   2. Build debug APK  : ./gradlew assembleDebug         (blocking)
#   3. Lint             : ./gradlew lint                  (opt-in, non-blocking)
#   4. Device smoke     : if an adb device is connected:
#        install + launch + watch logcat for crash/ANR/native-fault, report.
#        Device-agnostic: identical on real devices and Waydroid.
#
# Usage:
#   ./scripts/android_test.sh                 # unit + build + smoke (if a device is up)
#   RUN_LINT=1 ./scripts/android_test.sh      # also run lint (non-blocking)
#   SMOKE_SECONDS=30 ./scripts/android_test.sh
#   ./scripts/android_test.sh --skip-device   # offline phases only
#
# Exit codes: 0 = all run phases passed; 1 = a blocking phase failed; 2 = usage.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APPS_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
APK="${APPS_DIR}/app/build/outputs/apk/debug/app-debug.apk"
PACKAGE="com.crossscale"
ACTIVITY="${PACKAGE}/.MainActivity"
SMOKE_SECONDS="${SMOKE_SECONDS:-20}"
RUN_LINT="${RUN_LINT:-0}"
SKIP_DEVICE=0

for a in "$@"; do
  case "$a" in
    --skip-device) SKIP_DEVICE=1 ;;
    --lint)        RUN_LINT=1 ;;
    -h|--help)     sed -n '2,24p' "$0"; exit 0 ;;
    *) echo "unknown arg: $a" >&2; exit 2 ;;
  esac
done

# Colors only on a tty so CI logs stay clean.
if [ -t 1 ]; then
  C_CYAN='\033[1;36m'; C_GREEN='\033[1;32m'; C_YELLOW='\033[1;33m'; C_RED='\033[1;31m'; C_OFF='\033[0m'
else
  C_CYAN=''; C_GREEN=''; C_YELLOW=''; C_RED=''; C_OFF=''
fi
phase() { printf '\n'"${C_CYAN}▸ %s${C_OFF}\n" "$*"; }
ok()    { printf "${C_GREEN}✓ %s${C_OFF}\n" "$*"; }
warn()  { printf "${C_YELLOW}⚠ %s${C_OFF}\n" "$*" >&2; }
die()   { printf "${C_RED}✗ %s${C_OFF}\n" "$*" >&2; exit 1; }

GRADLE="./gradlew"
cd "$APPS_DIR"

# --- Phase 1: unit tests ----------------------------------------------------
phase "Unit tests (testDebugUnitTest)"
$GRADLE testDebugUnitTest || die "unit tests failed"
ok "unit tests passed"

# --- Phase 2: build debug APK ----------------------------------------------
phase "Build debug APK (assembleDebug)"
$GRADLE assembleDebug || die "build failed"
[ -f "$APK" ] || die "APK not found: $APK"
ok "APK built: $(basename "$APK")"

# --- Phase 3: lint (opt-in, non-blocking) ----------------------------------
if [ "$RUN_LINT" = "1" ]; then
  phase "Lint (non-blocking)"
  if $GRADLE lint; then
    ok "lint passed"
  else
    warn "lint reported issues (non-blocking) — see app/build/reports/lint-results-debug.html"
  fi
fi

if [ "$SKIP_DEVICE" = "1" ]; then
  warn "--skip-device: device smoke skipped"
  printf "\n${C_GREEN}✔ All offline phases passed.${C_OFF}\n"
  exit 0
fi

# --- Phase 4: device smoke --------------------------------------------------
command -v adb >/dev/null 2>&1 || die "adb not found on PATH"
DEVICES="$(adb devices 2>/dev/null | awk 'NF>1 && $2=="device"{print $1}')"
DCOUNT="$(printf '%s\n' "$DEVICES" | grep -c . || true)"
if [ "$DCOUNT" -eq 0 ]; then
  warn "no adb device connected — skipping device smoke"
  printf "\n${C_GREEN}✔ Offline phases passed; connect a device to run the smoke test.${C_OFF}\n"
  exit 0
fi
if [ "$DCOUNT" -gt 1 ] && [ -z "${ANDROID_SERIAL:-}" ]; then
  die "multiple devices ($DCOUNT): set ANDROID_SERIAL to pick one, or disconnect the others"
fi
ok "adb device ready (${DCOUNT})${ANDROID_SERIAL:+ using $ANDROID_SERIAL}"

# Install. Auto-recover from a signature mismatch (e.g. an older build signed
# by a different keystore is already present) by uninstalling then reinstalling.
phase "Install APK"
INSTALL_LOG="$(mktemp -t crossscale_install_XXXXXX.log)"
if ! adb install -r -t "$APK" 2>&1 | tee "$INSTALL_LOG"; then
  if grep -q 'UPDATE_INCOMPATIBLE\|signatures do not match' "$INSTALL_LOG"; then
    warn "existing install has a different signature — uninstalling $PACKAGE and retrying"
    adb uninstall "$PACKAGE" >/dev/null 2>&1
    adb install -t "$APK" || die "adb install failed even after uninstall"
  else
    die "adb install failed (see $INSTALL_LOG)"
  fi
fi
rm -f "$INSTALL_LOG"

# Launch + observe for crashes/ANR/native faults.
phase "Launch & observe (${SMOKE_SECONDS}s)"
adb logcat -c
LAUNCH_OUT="$(adb shell am start -n "$ACTIVITY" 2>&1)"
echo "$LAUNCH_OUT" | sed 's/^/    /'
echo "$LAUNCH_OUT" | grep -qi 'Error\|Exception' && warn "am start reported an issue (see above)"

# Grace period before the first liveness probe (process is still spinning up).
sleep 2

CRASH=0
for i in $(seq 1 "$SMOKE_SECONDS"); do
  PID="$(adb shell pidof "$PACKAGE" 2>/dev/null | tr -d '\r')"
  if [ -z "$PID" ]; then
    warn "process gone after $((i + 1))s — likely crashed at startup"
    CRASH=1
    break
  fi
  sleep 1
done

# Dump the logcat buffer and scan for trouble markers.
SMOKE_LOG="$(mktemp -t crossscale_smoke_XXXXXX.log)"
adb logcat -d > "$SMOKE_LOG" 2>/dev/null
MARKERS='FATAL EXCEPTION|E AndroidRuntime|ANR in '"$PACKAGE"'|tombstoned|Fatal signal|signal [0-9]+ \(SIG(SEGV|ABRT|BUS)\)|Abort message'
HITS="$(grep -nE "$MARKERS" "$SMOKE_LOG" || true)"
if [ -n "$HITS" ]; then
  CRASH=1
  warn "crash/ANR/native-fault markers found in logcat:"
  printf '%s\n' "$HITS" | sed 's/^/    /' | head -40
fi

if [ "$CRASH" = "1" ]; then
  die "device smoke FAILED (app crashed/ANR). full logcat: $SMOKE_LOG"
fi
ok "app stayed alive ${SMOKE_SECONDS}s, no crash/ANR markers"
printf "\n${C_GREEN}✔ ALL PHASES PASSED (unit + build + device smoke).${C_OFF}\n"
echo "  logcat snapshot: $SMOKE_LOG"
