//! The app store: one `Copy` struct of signals made once in the root, plus
//! the worker plumbing. Only these methods write state; pages read.
//!
//! Threading: engine/host work runs on `std::thread`s and reports through
//! `mpsc` channels. The shell's 100 ms tick (`drain`) moves events into
//! signals on the engine thread — the UI never blocks on a worker, and a
//! job's cancel is a shared `AtomicBool` checked at tile boundaries.

use std::path::PathBuf;
use std::sync::mpsc::{Receiver, Sender};
use std::sync::{Arc, atomic::AtomicBool};

use nsh_core::prelude::*;

use crate::engine_bridge::{
    JobEvent, JobSpec, OutputImage, SourceImage, Stage, UpscaleBackend,
};
use crate::host_bridge::{ApprovalReply, ApprovalRequest, HostEvent, HostHandle, HostService};
use crate::models_local::{self, ModelRow};
use crate::nav::Page;

/// One upscale job's lifecycle, as the progress page shows it.
#[derive(Clone, Debug, PartialEq)]
pub enum JobState {
    Idle,
    Running {
        stage: Stage,
        done: i32,
        total: i32,
        /// `unix_ms()` when the job started (ETA math).
        started_ms: f64,
    },
    Cancelled,
    Failed(String),
}

/// The host coordinator, as the Host page shows it.
#[derive(Clone, Debug, PartialEq)]
pub struct HostInfo {
    pub port: u16,
    pub ip: String,
    pub fingerprint: String,
    pub qr_payload: String,
    pub qr_data_uri: String,
}

/// `Copy`: closures and pages capture it like a signal.
#[derive(Clone, Copy)]
pub struct Store {
    // Shell
    pub page: Signal<Page>,

    // Home / job
    pub input: Signal<Option<SourceImage>>,
    pub format: Signal<String>,
    pub quality: Signal<f64>,
    pub scale: Signal<i32>,
    pub model: Signal<Option<String>>,
    pub job: Signal<JobState>,
    pub result: Signal<Option<OutputImage>>,
    job_rx: Stored<Option<Receiver<JobEvent>>>,
    cancel: Stored<Option<Arc<AtomicBool>>>,
    backend: Stored<Box<dyn UpscaleBackend>>,
    host_service: Stored<Box<dyn HostService>>,

    // Host
    pub host_running: Signal<bool>,
    pub host_info: Signal<Option<HostInfo>>,
    pub host_log: Signal<Vec<String>>,
    pub host_pending: Signal<Option<ApprovalRequest>>,
    pub host_port_draft: Signal<String>,
    pub host_auto_approve: Signal<bool>,
    pub trusted: Signal<Vec<String>>,
    host_rx: Stored<Option<Receiver<HostEvent>>>,
    host_handle: Stored<Option<HostHandle>>,

    // Models
    pub models_dir: Stored<PathBuf>,
    pub installed: Signal<Vec<ModelRow>>,
    /// ★ picks, persisted in app storage ("推荐" comes from the scene table).
    pub favorites: Signal<Vec<String>>,
    pub import_log: Signal<Vec<String>>,
    /// Name of the catalog row currently downloading, if any.
    pub downloading: Signal<Option<String>>,
    download_rx: Stored<Option<Receiver<String>>>,

    // Home draft state
    pub path_draft: Signal<String>,
}

impl Store {
    pub fn new() -> Self {
        let models_dir = models_local::models_dir();
        let installed = models_local::scan_installed(&models_dir);
        let favorites = nsh_core::storage::Storage::get_item("favModels")
            .map(|s| {
                s.split(',')
                    .map(str::to_owned)
                    .filter(|s| !s.is_empty())
                    .collect()
            })
            .unwrap_or_default();
        Self {
            page: signal(Page::Home),
            input: signal(None),
            format: signal("png".to_owned()),
            quality: signal(90.0),
            scale: signal(2),
            model: signal(None),
            job: signal(JobState::Idle),
            result: signal(None),
            job_rx: stored(None),
            cancel: stored(None),
            backend: stored(default_backend()),
            host_service: stored(default_host()),
            host_running: signal(false),
            host_info: signal(None),
            host_log: signal(Vec::new()),
            host_pending: signal(None),
            host_port_draft: signal(String::from("9177")),
            host_auto_approve: signal(false),
            trusted: signal(crate::host_bridge::trust_list()),
            host_rx: stored(None),
            host_handle: stored(None),
            models_dir: stored(models_dir.clone()),
            installed: signal(installed),
            favorites: signal(favorites),
            import_log: signal(Vec::new()),
            downloading: signal(None),
            download_rx: stored(None),
            path_draft: signal(String::new()),
        }
    }

    // ── Home ─────────────────────────────────────────────────────────────

    /// A picked/dropped/typed image becomes the source. Resets the previous
    /// result so 对比 never mixes two jobs.
    pub fn set_source(&self, path: PathBuf) {
        let mime = match path.extension().and_then(|e| e.to_str()).unwrap_or("").to_lowercase().as_str() {
            "jpg" | "jpeg" => "image/jpeg",
            "webp" => "image/webp",
            "gif" => "image/gif",
            _ => "image/png",
        };
        let bytes = match std::fs::read(&path) {
            Ok(b) => b,
            Err(e) => {
                log::warn!("读取失败 {}: {e}", path.display());
                return;
            }
        };
        let (width, height) = crate::engine_bridge::probe_dimensions(&path);
        self.input.set(Some(SourceImage {
            display_src: crate::engine_bridge::data_uri(&bytes, mime),
            path,
            width,
            height,
        }));
        self.result.set(None);
        self.job.set(JobState::Idle);
    }

    /// Starts a job on a worker thread; the progress page takes over.
    pub fn start_job(&self) {
        let Some(input) = self.input.get() else {
            return;
        };
        let (tx, rx) = std::sync::mpsc::channel();
        let cancel = Arc::new(AtomicBool::new(false));
        let spec = JobSpec {
            input_path: input.path.clone(),
            scale: self.scale.get(),
            format: self.format.get(),
            quality: self.quality.get() as i32,
            model: self.model.get(),
        };
        self.cancel.replace(Some(cancel.clone()));
        self.job_rx.replace(Some(rx));
        self.job.set(JobState::Running {
            stage: Stage::Loading,
            done: 0,
            total: 1,
            started_ms: nsh_core::time::unix_ms(),
        });
        self.backend.with(|b| b.start(spec, tx, cancel));
        self.page.set(Page::Progress);
    }

    /// The 取消 button: flag now, show 已取消 at once; the worker exits at
    /// the next tile boundary without emitting Done/Failed.
    pub fn cancel_job(&self) {
        if let Some(flag) = self.cancel.get() {
            flag.store(true, std::sync::atomic::Ordering::Relaxed);
        }
        if matches!(self.job.get(), JobState::Running { .. }) {
            self.job.set(JobState::Cancelled);
        }
    }

    // ── Host ─────────────────────────────────────────────────────────────

    pub fn start_host(&self) {
        if self.host_running.get() {
            return;
        }
        let port: u16 = self.host_port_draft.get().parse().unwrap_or(9177);
        let cfg = crate::host_bridge::HostConfig {
            port,
            auto_approve: self.host_auto_approve.get(),
            models_dir: self.models_dir.get().clone(),
        };
        let started = self.host_service.with(|svc| svc.start(cfg));
        self.host_handle.replace(Some(started.handle));
        self.host_rx.replace(Some(started.events));
        self.host_log.set(Vec::new());
        self.host_info.set(None);
        self.host_running.set(true);
    }

    pub fn stop_host(&self) {
        if let Some(handle) = self.host_handle.get() {
            handle
                .stop
                .store(true, std::sync::atomic::Ordering::Relaxed);
            // A pending dialog is decided as a rejection (stop() 拒绝未决审批).
            if self.host_pending.get().is_some() {
                let _ = handle.reply.send(ApprovalReply::Deny);
                self.host_pending.set(None);
            }
        }
    }

    pub fn answer_approval(&self, reply: ApprovalReply) {
        if let Some(handle) = self.host_handle.get() {
            let _ = handle.reply.send(reply);
        }
        if let Some(req) = self.host_pending.get() {
            let line = match reply {
                ApprovalReply::Allow => format!("已允许 {}", req.device),
                ApprovalReply::Deny => format!("已拒绝 {}", req.device),
            };
            self.host_log.update(|log| log.push(line));
        }
        self.host_pending.set(None);
    }

    // ── Models ───────────────────────────────────────────────────────────

    pub fn refresh_models(&self) {
        let dir = self.models_dir.get().clone();
        self.installed.set(models_local::scan_installed(&dir));
    }

    pub fn import_models(&self, files: &[PathBuf]) {
        let dir = self.models_dir.get().clone();
        let lines = models_local::import_files(files, &dir);
        self.import_log.update(|log| {
            log.extend(lines);
            if log.len() > 12 {
                log.drain(..log.len() - 12);
            }
        });
        self.refresh_models();
    }

    /// Release download via `upix_host::modelctl`. The button stays disabled
    /// until the worker's line is drained onto the import log.
    pub fn download_model(&self, name: &str) {
        #[cfg(not(feature = "integration"))]
        {
            self.note_import(format!("下载未编译进此构建: {name}"));
            return;
        }
        #[cfg(feature = "integration")]
        {
            if self.downloading.get().is_some() {
                return;
            }
            self.downloading.set(Some(name.to_owned()));
            self.note_import(format!("开始下载 {name}"));
            let dir = self.models_dir.get().clone();
            let name = name.to_owned();
            let (tx, rx) = std::sync::mpsc::channel();
            self.download_rx.replace(Some(rx));
            std::thread::spawn(move || {
                let line = match upix_host::modelctl::download(
                    &name,
                    &dir,
                    upix_host::modelctl::DEFAULT_MODEL_REPO,
                    None,
                ) {
                    Ok((param, _)) => format!("已下载 {name} → {}", param.display()),
                    Err(e) => format!("下载失败 {name}: {e}"),
                };
                let _ = tx.send(line);
            });
        }
    }

    fn note_import(&self, line: String) {
        self.import_log.update(|log| {
            log.push(line);
            if log.len() > 12 {
                log.drain(..log.len() - 12);
            }
        });
    }
    pub fn toggle_favorite(&self, name: &str) {
        self.favorites.update(|fav| {
            if let Some(at) = fav.iter().position(|f| f == name) {
                fav.remove(at);
            } else {
                fav.push(name.to_owned());
            }
            // Persist as a comma list; storage takes strings.
            let joined = fav.join(",");
            if let Err(full) = nsh_core::storage::Storage::set_item("favModels", joined) {
                log::warn!("收藏未持久化: {full}");
            }
        });
    }

    // ── The worker drain (shell tick) ────────────────────────────────────

    /// Moves pending worker events into signals. Called every 100 ms on the
    /// engine thread; `try_recv` never blocks. Job, host and download are
    /// independent: an idle job must not swallow host events.
    pub fn drain(&self) {
        self.drain_job();
        self.drain_host();
        self.drain_download();
    }

    fn drain_job(&self) {
        let Some(rx) = self.job_rx.with(|v| v.take()) else {
            return;
        };
        loop {
            match rx.try_recv() {
                Ok(JobEvent::Progress { stage, done, total }) => {
                    if let JobState::Running { started_ms, .. } = self.job.get() {
                        self.job.set(JobState::Running {
                            stage,
                            done,
                            total,
                            started_ms,
                        });
                    }
                }
                Ok(JobEvent::Done(output)) => {
                    self.result.set(Some(*output));
                    self.job.set(JobState::Idle);
                    self.job_rx.replace(None);
                    self.page.set(Page::Compare);
                    return;
                }
                Ok(JobEvent::Failed(message)) => {
                    if !matches!(self.job.get(), JobState::Cancelled) {
                        self.job.set(JobState::Failed(message));
                    }
                    self.job_rx.replace(None);
                    return;
                }
                Err(_) => {
                    self.job_rx.with(|slot| *slot = Some(rx));
                    return;
                }
            }
        }
    }

    fn drain_host(&self) {
        let Some(host_rx) = self.host_rx.with(|v| v.take()) else {
            return;
        };
        loop {
            match host_rx.try_recv() {
                Ok(HostEvent::Started {
                    port,
                    ip,
                    fingerprint,
                    qr_payload,
                    qr_data_uri,
                }) => {
                    self.host_info.set(Some(HostInfo {
                        port,
                        ip,
                        fingerprint,
                        qr_payload,
                        qr_data_uri,
                    }));
                }
                Ok(HostEvent::Log(line)) => {
                    self.host_log.update(|log| {
                        log.push(line);
                        if log.len() > 30 {
                            log.drain(..log.len() - 30);
                        }
                    });
                }
                Ok(HostEvent::Approval(req)) => {
                    if self.host_pending.get().is_none() {
                        self.host_pending.set(Some(req));
                    }
                }
                Ok(HostEvent::Stopped) => {
                    self.host_running.set(false);
                    self.host_handle.replace(None);
                    self.host_rx.replace(None);
                    self.host_pending.set(None);
                    return;
                }
                Err(_) => {
                    self.host_rx.with(|slot| *slot = Some(host_rx));
                    return;
                }
            }
        }
    }

    fn drain_download(&self) {
        let Some(rx) = self.download_rx.with(|v| v.take()) else {
            return;
        };
        match rx.try_recv() {
            Ok(line) => {
                self.note_import(line);
                self.downloading.set(None);
                self.download_rx.replace(None);
                self.refresh_models();
            }
            Err(std::sync::mpsc::TryRecvError::Empty) => {
                self.download_rx.with(|slot| *slot = Some(rx));
            }
            Err(std::sync::mpsc::TryRecvError::Disconnected) => {
                self.downloading.set(None);
                self.download_rx.replace(None);
            }
        }
    }
}

/// Sends a job event (worker side helper kept here so both backends share it).
pub type JobTx = Sender<JobEvent>;

fn default_backend() -> Box<dyn crate::engine_bridge::UpscaleBackend> {
    #[cfg(feature = "integration")]
    {
        Box::new(crate::engine_bridge::RealBackend)
    }
    #[cfg(not(feature = "integration"))]
    {
        Box::new(crate::engine_bridge::MockBackend)
    }
}

fn default_host() -> Box<dyn crate::host_bridge::HostService> {
    #[cfg(feature = "integration")]
    {
        Box::new(crate::host_bridge::RealHostService)
    }
    #[cfg(not(feature = "integration"))]
    {
        Box::new(crate::host_bridge::MockHostService)
    }
}
