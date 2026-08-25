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
#include <optional>
#include <string>

#include "utils/image.hpp"
#include "utils/network.hpp"  // MAX_NETWORK_PAYLOAD

namespace crossscale {
class ServeDaemon;
struct ProcessingProgress;
}  // namespace crossscale

// Maximum payload size for network messages (100 MB).
using crossscale::MAX_NETWORK_PAYLOAD;

// ── Global run-state ────────────────────────────────────────────────────────
// Set false by signal handlers; long-running loops (process, host, daemon)
// poll these to support graceful shutdown.
extern std::atomic<bool> g_running;
extern std::atomic<bool> g_cancel_requested;

// Used by the `daemon` subcommand so SIGINT/SIGTERM (e.g. from `daemon stop`)
// actually wake the daemon's accept() loop. signal_handler only sets g_running,
// which ServeDaemon::run() does not observe. Set to the running daemon by the
// daemon start path; null otherwise.
extern crossscale::ServeDaemon* g_daemon;

// ── Signal handlers ─────────────────────────────────────────────────────────
void signal_handler(int);
void daemon_signal_handler(int);

// ── Defaults ────────────────────────────────────────────────────────────────
// $CROSSSCALE_MODELS_DIR > ./models (the --models-dir flag wins by being bound
// directly to each subcommand's models_dir field).
auto default_models_dir() -> std::string;

// ── Image / format helpers ──────────────────────────────────────────────────
// Infer output format ("png"/"jpg"/"webp"/"avif") from a file extension; "" if unknown.
auto infer_format_from_extension(const std::string& path) -> std::string;

// Default output location when none was given: `img.png` → `img_<model>.png`
// (sibling, input extension kept when it is a known output format, else .png);
// directory input `shots/` → `shots_<model>/` next to it.
auto derive_default_output(const std::string& input_path,
                           const std::string& model_name,
                           bool input_is_dir) -> std::string;

// Render a progress line to stderr:  45.2% [12/27 tiles, ETA 3.2s]
void print_progress_stderr(const crossscale::ProcessingProgress& prog,
                           std::chrono::steady_clock::time_point start_time);

// Convert `image` in place to `expected_channels`; logs + returns false on
// failure. No-op when channels already match.
auto normalize_channels_for_model(crossscale::Image& image,
                                  int expected_channels,
                                  const std::string& image_path) -> bool;

// ── QR / connection card ────────────────────────────────────────────────────
// Render text as a 2-row-per-line block of Unicode half-blocks.
auto generate_qr_ascii(const std::string& text) -> std::string;

// Print the connection card shown to the user: all local IPv4 addresses
// (first non-loopback one is used as the QR default), the identity fingerprint,
// and a QR code carrying the JSON the Android app's JoinInviteParser.parseJsonInvite()
// understands (v2: fingerprint + base64 pubkey). Shared by `host start` and
// `host info` to stay in sync.
void print_connection_card(uint16_t port, const std::array<uint8_t, 32>& identity_pubkey);
