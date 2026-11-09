// Shared helpers used across multiple CLI subcommands.
//
// These are the functions and globals that used to live at the top of the old
// monolithic main.cpp. They are kept here so each cmd_*.cpp can `#include` them
// without dragging in unrelated subcommand logic.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <sys/types.h>  // pid_t

#include "utils/image.hpp"
#include "utils/network.hpp"  // MAX_NETWORK_PAYLOAD

#include "core/config.hpp"
#include "core/distributed/connection_card.hpp"
#include "core/model_manager.hpp"
#include "core/serve/daemon_control.hpp"

namespace upix {
class ServeDaemon;
struct ProcessingProgress;
}  // namespace upix

namespace args { struct Process; }

// Maximum payload size for network messages (100 MB).
using upix::MAX_NETWORK_PAYLOAD;

// ── Global run-state ────────────────────────────────────────────────────────
// Set false by signal handlers; long-running loops (process, host, daemon)
// poll these to support graceful shutdown.
extern std::atomic<bool> g_running;
extern std::atomic<bool> g_cancel_requested;

// Used by the `daemon` subcommand so SIGINT/SIGTERM (e.g. from `daemon stop`)
// actually wake the daemon's accept() loop. signal_handler only sets g_running,
// which ServeDaemon::run() does not observe. Set to the running daemon by the
// daemon start path; null otherwise.
extern upix::ServeDaemon* g_daemon;

// ── Signal handlers ─────────────────────────────────────────────────────────
void signal_handler(int);
void daemon_signal_handler(int);

// ── Defaults ────────────────────────────────────────────────────────────────
// The one models-dir policy lives in share (core/model_manager.hpp) — the
// desktop and the daemon resolve the same path without asking the CLI.
inline auto default_models_dir() -> std::string {
    return upix::default_models_dir();
}

// Is `pid` a live process that is actually our serve daemon? Verifies the
// executable (and on Linux the command line) so pid files can be trusted.
// Lives in share now (core/serve/daemon_control.hpp) — the desktop's status
// page and the CLI trust the same check.
inline auto pid_is_upix_daemon(pid_t pid) -> bool {
    return upix::pid_is_upix_daemon(pid);
}

// ── Image / format helpers ──────────────────────────────────────────────────
// Both moved into legacy/share/core/config.hpp (the batch core + desktop single-file
// runs share them); kept here under the historical names.
inline auto infer_format_from_extension(const std::string& path) -> std::string {
    return upix::infer_format_from_extension(path);
}

inline auto derive_default_output(const std::string& input_path,
                                  const std::string& model_name,
                                  bool input_is_dir) -> std::string {
    return upix::derive_default_output(input_path, model_name, input_is_dir);
}

// Render a progress line to stderr:  45.2% [12/27 tiles, ETA 3.2s]
void print_progress_stderr(const upix::ProcessingProgress& prog,
                           std::chrono::steady_clock::time_point start_time);

// ── Machine-readable output ─────────────────────────────────────────────────
// JSON event mode for GUI/script consumers: `--json` on model/daemon/host,
// `UPIX_PROGRESS=json` on the process paths. Events are one compact JSON
// object per '\n'-terminated line on stdout; the Logger moves to stderr so
// the event stream stays parseable. Progress numbers are identical to the
// human path — only the encoding differs.
auto machine_output_enabled() -> bool;
void enable_machine_output();  // idempotent; also routes Logger to stderr

// Escape a UTF-8 string for embedding in a JSON string literal (quotes,
// backslashes, control chars; bytes >= 0x20 pass through untouched).
auto json_escape(std::string_view s) -> std::string;

// ── Interruptible stdin ─────────────────────────────────────────────────────
// Read one line from stdin without ever blocking past a poll tick, so a signal
// can end the wait: std::signal() installs its handler with SA_RESTART, so a
// blocked std::getline() is restarted rather than returning EINTR — pressing
// Ctrl+C at a prompt would otherwise look like a hang until Enter is pressed.
// `still_waiting` is re-checked every tick (the caller passes its shutdown
// condition); when it returns false, or stdin is at EOF with nothing buffered,
// the wait is abandoned and the function returns false (callers treat that as
// "no approval").
// Returns true when a complete line was read; the trailing '\n' is stripped.
inline constexpr size_t kMaxPromptLineBytes = 4096;  // bound a silent/hostile writer
auto read_line_interruptible(std::string& line,
                             const std::function<bool()>& still_waiting) -> bool;

// Write one machine event line to stdout and flush (GUI consumers read
// incrementally; pairing prompts block on stdin right after an event).
void emit_json_line(const std::string& json);

// Tile-progress update: machine JSON event when enabled, else the human
// `\r` line on stderr. Producers: inline single image, daemon delegate,
// directory batch — one call site each.
void report_progress(const upix::ProcessingProgress& prog,
                     std::chrono::steady_clock::time_point start_time);

// Normalize model input channels, preserving RGBA for the tile alpha bypass.
// Logs and returns false on failure.
auto normalize_channels_for_model(upix::Image& image,
                                  int expected_channels,
                                  const std::string& image_path) -> bool;

// Defaults -> config file -> explicitly supplied CLI flags. The recommendation
// is deferred until an engine exists, and only applies without a loaded config
// or an explicit tile size. A loaded file owns its defaulted fields too.
struct EffectiveProcessingConfig {
    upix::ProcessingConfig config;
    bool recommend_tile_size = true;
};
auto resolve_processing_config(const args::Process& a) -> EffectiveProcessingConfig;

// ── QR / connection card ────────────────────────────────────────────────────
// Render text as a 2-row-per-line block of Unicode half-blocks.
auto generate_qr_ascii(const std::string& text) -> std::string;

// Card data + builder moved to share (core/distributed/connection_card.hpp) —
// the desktop builds the identical card in-process now (SPEC-frontends S1).
using ConnectionCard = upix::ConnectionCard;
inline auto build_connection_card(uint16_t port,
                                  const std::array<uint8_t, 32>& identity_pubkey)
    -> ConnectionCard {
    return upix::build_connection_card(port, identity_pubkey);
}

// Print the connection card shown to the user: all local IPv4 addresses
// (first non-loopback one is used as the QR default), the identity fingerprint,
// and a QR code carrying the JSON the Android app's JoinInviteParser.parseJsonInvite()
// understands (v2: fingerprint + base64 pubkey). Shared by `host start` and
// `host info` to stay in sync.
void print_connection_card(uint16_t port, const std::array<uint8_t, 32>& identity_pubkey);
