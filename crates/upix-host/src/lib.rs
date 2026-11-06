//! upix-host — Rust port of `share/core/distributed/coordinator*` (PC side
//! of the phone↔PC offload protocol) and `share/core/serve/daemon.cpp` (the
//! warm-engine Unix-socket daemon).
//!
//! Porting contract: docs/rewrite/CONTRACT.md §host.

pub mod coordinator;
pub mod modelctl;
pub mod pairing;
pub mod seams;
pub mod serve;
pub mod transport_std;
pub mod wireio;

pub use coordinator::{
    run_stdin_approval_loop, spawn_approval_forwarder, ApprovalWait, Coordinator,
    CoordinatorConfig, CoordinatorError, PendingApproval,
};
pub use pairing::{build_connection_card, pairing_qr_payload, ConnectionCard};
pub use serve::{
    default_serve_socket_path, ensure_running, request, run_daemon, run_daemon_fg, status,
    stop,
    write_pid_file, DaemonStatus, ServeConfig, ServeDaemonHandle, ServeDevice, ServeOutcome,
    ServeRequestArgs, DEFAULT_IDLE_TIMEOUT_S,
};

pub use modelctl::{list_remote, RemoteModel};
pub mod modelctl_api {
    //! CONTRACT §6 pin surface: `modelctl::{list_remote, download, import}`.
    pub use crate::modelctl::{download, find_remote, import, list_remote, manifest_url, download_model, DEFAULT_MODEL_REPO};
}

pub use coordinator::Framer;
pub use seams::Backend;
