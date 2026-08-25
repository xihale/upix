#!/usr/bin/env bash
#
# test_remote_e2e.sh — end-to-end test of the phone↔PC distributed path.
#
# Brings up a PC coordinator (crossscale host --auto-approve), then runs
# the RemoteDistributedTest instrumented test on a connected device, which pairs
# with the coordinator and upscales a synthetic image through it over the
# encrypted protocol. Verifies the real remote logic: ECDH handshake, CONNECT
# pairing, CLIENT_SUBMIT/RESPONSE over EncryptedSession.
#
# Usage:
#   ./scripts/test_remote_e2e.sh                 # auto-detect LAN IP, port 9530
#   CS_HOST=192.168.1.10 ./scripts/test_remote_e2e.sh
#   PORT=9540 CS_MODEL=realesrgan-x4plus-x4 ./scripts/test_remote_e2e.sh
#
# Exit codes: 0 = both instrumented tests passed; 1 = setup or test failure.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLI_DIR="$ROOT/cli"
APPS_DIR="$ROOT/apps"
# xmake outputs to the project-root build dir ($(projectdir)/build/...), not
# cli/build. Prefer the fresh root build; fall back to a legacy cli/build copy.
CLI_BIN="$ROOT/build/linux/x86_64/release/crossscale-cli"
[ -x "$CLI_BIN" ] || CLI_BIN="$CLI_DIR/build/linux/x86_64/release/crossscale-cli"
APP_APK="$APPS_DIR/app/build/outputs/apk/debug/app-debug.apk"
TEST_APK="$APPS_DIR/app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk"
PACKAGE="com.crossscale"
TEST_RUNNER="$PACKAGE.test/androidx.test.runner.AndroidJUnitRunner"
PORT="${PORT:-9530}"
MODEL="${CS_MODEL:-Real-ESRGANv3-anime-x2}"
SRV_LOG="$(mktemp -t cs_e2e_server_XXXXXX.log)"

if [ -t 1 ]; then
  C_CYAN='\033[1;36m'; C_GREEN='\033[1;32m'; C_YELLOW='\033[1;33m'; C_RED='\033[1;31m'; C_OFF='\033[0m'
else
  C_CYAN=''; C_GREEN=''; C_YELLOW=''; C_RED=''; C_OFF=''
fi
phase() { printf '\n'"${C_CYAN}▸ %s${C_OFF}\n" "$*"; }
ok()    { printf "${C_GREEN}✓ %s${C_OFF}\n" "$*"; }
warn()  { printf "${C_YELLOW}⚠ %s${C_OFF}\n" "$*" >&2; }
die()   { printf "${C_RED}✗ %s${C_OFF}\n" "$*" >&2; exit 1; }

COORD_PID=""
cleanup() {
  if [ -n "$COORD_PID" ] && kill -0 "$COORD_PID" 2>/dev/null; then
    kill "$COORD_PID" 2>/dev/null
    wait "$COORD_PID" 2>/dev/null
    printf "${C_YELLOW}(stopped coordinator)${C_OFF}\n" >&2
  fi
}
trap cleanup EXIT

# ── Detect PC LAN IP (the phone connects to this) ─────────────────────────
detect_lan_ip() {
  if [ -n "${CS_HOST:-}" ]; then echo "$CS_HOST"; return; fi
  local ip=""
  if command -v hostname >/dev/null 2>&1; then
    ip=$(hostname -I 2>/dev/null | tr ' ' '\n' |
         grep -E '^([0-9]+\.){3}[0-9]+$' | grep -vE '^(127\.|169\.254\.)' | head -1)
  fi
  if [ -z "$ip" ] && command -v ip >/dev/null 2>&1; then
    ip=$(ip -4 -o addr show scope global 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -1)
  fi
  echo "$ip"
}

# ── Phase 0: preflight ─────────────────────────────────────────────────────
phase "Preflight"
command -v adb >/dev/null 2>&1 || die "adb not found on PATH"
[ -x "$CLI_BIN" ] || die "CLI not built; run: cd cli && xmake"

HOST="$(detect_lan_ip)"
[ -n "$HOST" ] || die "could not detect PC LAN IP (set CS_HOST=<ip>)"
ok "PC LAN IP: $HOST"

DCOUNT="$(adb devices 2>/dev/null | awk 'NF>1 && $2=="device"{c++} END{print c+0}')"
[ "$DCOUNT" -ge 1 ] || die "no adb device connected"
[ "$DCOUNT" -eq 1 ] || die "multiple devices ($DCOUNT); set ANDROID_SERIAL"
ok "device connected (${DCOUNT})"

# ── Phase 1: build debug + androidTest APKs ────────────────────────────────
phase "Build APKs (assembleDebug + assembleDebugAndroidTest)"
( cd "$APPS_DIR" && ./gradlew assembleDebug assembleDebugAndroidTest ) || die "gradle build failed"
[ -f "$APP_APK" ] || die "app APK missing: $APP_APK"
[ -f "$TEST_APK" ] || die "androidTest APK missing: $TEST_APK"
ok "APKs built"

# ── Phase 2: install both APKs (auto-recover from signature mismatch) ──────
phase "Install APKs"
install_recover() {
  local apk="$1"
  local log; log="$(mktemp -t cs_e2e_inst_XXXXXX.log)"
  if ! adb install -r -t "$apk" 2>&1 | tee "$log"; then
    if grep -q 'UPDATE_INCOMPATIBLE\|signatures do not match' "$log"; then
      warn "signature mismatch — reinstalling ${PACKAGE}"
      adb uninstall "$PACKAGE" >/dev/null 2>&1
      adb install -t "$apk" || { rm -f "$log"; return 1; }
    else
      rm -f "$log"; return 1
    fi
  fi
  rm -f "$log"
}
install_recover "$APP_APK" || die "failed to install app APK"
adb install -r -t "$TEST_APK" || die "failed to install androidTest APK"
ok "APKs installed"

# ── Phase 3: start PC coordinator (CWD=cli/ so ./models resolves) ──────────
phase "Start coordinator (port $PORT, --auto-approve, model $MODEL)"
# `exec` replaces the subshell with the CLI so $! is the coordinator PID
# itself (kill/trap then reaches it directly).
( cd "$CLI_DIR" && exec "$CLI_BIN" host -p "$PORT" --auto-approve ) >"$SRV_LOG" 2>&1 &
COORD_PID=$!
ok "coordinator PID $COORD_PID (log: $SRV_LOG)"

# Wait for readiness (up to 15s).
ready=0
for _ in $(seq 1 30); do
  kill -0 "$COORD_PID" 2>/dev/null || die "coordinator exited early — see $SRV_LOG"
  if grep -q "Coordinator running" "$SRV_LOG" 2>/dev/null; then ready=1; break; fi
  sleep 0.5
done
[ "$ready" = "1" ] || die "coordinator did not become ready — see $SRV_LOG"
ok "coordinator ready"

# ── Phase 4: run the instrumented remote test on the device ────────────────
phase "Run RemoteDistributedTest on device (host=$HOST port=$PORT model=$MODEL)"
OUT="$(adb shell am instrument -w \
  -e host "$HOST" -e port "$PORT" -e model "$MODEL" \
  -e class com.crossscale.RemoteDistributedTest \
  "$TEST_RUNNER" 2>&1)"
echo "$OUT" | sed 's/^/    /'

# am instrument prints "OK (<n> tests)" on success, "FAILURES" on failure.
if echo "$OUT" | grep -qE '^OK \([0-9]+ tests?\)'; then
  ok "instrumented remote tests PASSED"
  printf '\n%s\n' "$(echo "$OUT" | grep -E '^OK ')"
  exit 0
else
  die "instrumented remote tests FAILED (see output above; server log: $SRV_LOG)"
fi
