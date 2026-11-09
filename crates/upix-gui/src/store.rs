//! Reactive application state (`Store` mirroring `desktop/src/app_state.hpp`).
//!
//! Because `Signal<T>` and `Stored<T>` in `nsh_core` implement `Copy`, `Store`
//! itself is `#[derive(Clone, Copy)]`, making it ergonomic to pass into any
//! closure without `.clone()`.

use std::collections::HashSet;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{mpsc, Arc};
use std::time::Duration;

use nsh_core::prelude::*;

use crate::engine_bridge::{
    ChromaSubsampling, EngineEvent, JobBackend, JobRequest, OutputFormat,
};
use crate::host_bridge::{
    ApprovalReply, ApprovalRequest, HostConfig, HostEvent, HostHandle, HostService,
};
use crate::models_local::{self, ModelGroup, ModelVariant};
use crate::nav::NavTab;

/// Per-image status in the Workbench batch filmstrip.
#[derive(Clone, Debug, PartialEq)]
pub enum ItemStatus {
    Ready,
    Running(f32),
    Done {
        output_path: PathBuf,
        out_w: u32,
        out_h: u32,
    },
    Skipped,
    Failed(String),
}

/// One queued input image in the Workbench.
#[derive(Clone, Debug, PartialEq)]
pub struct InputItem {
    pub path: PathBuf,
    pub filename: String,
    pub width: u32,
    pub height: u32,
    pub status: ItemStatus,
}

/// Overall batch job phase.
#[derive(Clone, Debug, PartialEq)]
pub enum JobPhase {
    Idle,
    Running {
        item_index: usize,
        item_total: usize,
        fraction: f32,
        stage: String,
    },
    Done {
        completed: usize,
        skipped: usize,
        elapsed_ms: u64,
    },
    Cancelled,
    Failed(String),
}

enum ModelTaskEvent {
    RemoteList(Result<Vec<String>, String>),
    DownloadDone {
        name: String,
        result: Result<(), String>,
    },
}

/// Central reactive store for the `upix` Darkroom Instrument UI.
#[derive(Clone, Copy)]
pub struct Store {
    // Navigation
    pub nav: Signal<NavTab>,

    // Workbench inputs & canvas
    pub inputs: Signal<Vec<InputItem>>,
    pub active_input: Signal<usize>,
    pub split_pos: Signal<f32>,

    // Model selection & floating picker modal
    pub selected_model: Signal<String>,
    pub show_model_picker: Signal<bool>,
    pub picker_search: Signal<String>,

    // Output & Encoder settings
    pub format: Signal<OutputFormat>,
    pub quality: Signal<i32>,
    pub chroma: Signal<ChromaSubsampling>,
    pub avif_speed: Signal<i32>,
    pub custom_out_dir: Signal<Option<PathBuf>>,

    // Engine & Warm Daemon settings
    pub use_gpu: Signal<bool>,
    pub tile_size: Signal<i32>,
    pub daemon_running: Signal<bool>,
    pub daemon_loading: Signal<bool>,
    pub spinner_tick: Signal<u8>,
    daemon_rx: Stored<Option<mpsc::Receiver<bool>>>,
    pub force: Signal<bool>,

    // Active job state
    pub job_phase: Signal<JobPhase>,
    /// Set when the last job failed because the selected model is not on disk.
    /// The failure banner offers "下载模型" only in that case (a generic retry
    /// loops forever otherwise).
    pub job_missing_model: Signal<Option<String>>,
    job_rx: Stored<Option<mpsc::Receiver<EngineEvent>>>,
    job_cancel: Stored<Option<Arc<AtomicBool>>>,
    backend: Stored<Arc<dyn JobBackend>>,

    // Models Hub state
    pub models_dir: Stored<PathBuf>,
    pub installed_groups: Signal<Vec<ModelGroup>>,
    pub installed_set: Signal<HashSet<String>>,
    pub remote_groups: Signal<Vec<ModelGroup>>,
    pub remote_stems: Signal<Vec<String>>,
    pub remote_loading: Signal<bool>,
    pub downloading_models: Signal<HashSet<String>>,
    pub models_status: Signal<String>,
    model_rx: Stored<Option<mpsc::Receiver<ModelTaskEvent>>>,
    model_tx: Stored<mpsc::Sender<ModelTaskEvent>>,

    // Remote Host state
    host_service: Stored<Arc<dyn HostService>>,
    host_rx: Stored<Option<mpsc::Receiver<HostEvent>>>,
    host_handle: Stored<Option<HostHandle>>,
    pub host_running: Signal<bool>,
    pub host_port: Signal<u16>,
    pub host_auto_approve: Signal<bool>,
    pub host_ips: Signal<Vec<String>>,
    pub host_ip_idx: Signal<usize>,
    pub show_ip_picker: Signal<bool>,
    pub host_fingerprint: Signal<String>,
    pub host_pubkey_b64: Signal<String>,
    pub host_qr_payload: Signal<String>,
    pub host_qr_uri: Signal<String>,
    pub host_logs: Signal<Vec<String>>,
    pub host_pending_approval: Signal<Option<ApprovalRequest>>,
    pub host_trusted: Signal<Vec<String>>,
}

impl Default for Store {
    fn default() -> Self {
        Self::new()
    }
}

impl Store {
    pub fn new() -> Self {
        #[cfg(feature = "integration")]
        let backend: Arc<dyn JobBackend> = Arc::new(crate::engine_bridge::RealBackend);
        #[cfg(not(feature = "integration"))]
        let backend: Arc<dyn JobBackend> = Arc::new(crate::engine_bridge::MockBackend);

        #[cfg(feature = "integration")]
        let host_service: Arc<dyn HostService> = Arc::new(crate::host_bridge::RealHostService);
        #[cfg(not(feature = "integration"))]
        let host_service: Arc<dyn HostService> = Arc::new(crate::host_bridge::MockHostService);

        Self::with_services(backend, host_service)
    }

    pub fn with_services(
        backend: Arc<dyn JobBackend>,
        host_service: Arc<dyn HostService>,
    ) -> Self {
        let dir = models_local::models_dir();
        let (installed_groups, installed_set) = models_local::scan_installed_groups(&dir);
        let default_model = installed_groups
            .iter()
            .flat_map(|g| g.items.iter())
            .find(|m| m.name == "NomosUni-SPAN-x2")
            .or_else(|| installed_groups.iter().flat_map(|g| g.items.iter()).next())
            .map(|m| m.name.clone())
            .unwrap_or_else(|| "NomosUni-SPAN-x2".to_string());

        let fallback_stems: Vec<String> = models_local::FALLBACK_CATALOG
            .iter()
            .map(|s| (*s).to_string())
            .collect();
        let initial_remote_groups =
            models_local::build_remote_groups(&fallback_stems, &dir, &installed_set);

        let (model_tx, model_rx) = mpsc::channel();

        let daemon_initially_up = {
            #[cfg(feature = "integration")]
            {
                let sock = upix_host::serve::default_serve_socket_path();
                upix_host::serve::status(&sock).running
            }
            #[cfg(not(feature = "integration"))]
            {
                false
            }
        };

        let (init_fp, init_pk, init_ips) = crate::host_bridge::initial_host_identity();

        Self {
            nav: signal(NavTab::Workbench),

            inputs: signal(Vec::new()),
            active_input: signal(0),
            split_pos: signal(0.545),

            selected_model: signal(default_model),
            show_model_picker: signal(false),
            picker_search: signal(String::new()),

            format: signal(OutputFormat::Follow),
            quality: signal(90),
            chroma: signal(ChromaSubsampling::Yuv420),
            avif_speed: signal(6),
            custom_out_dir: signal(None),

            use_gpu: signal(true),
            tile_size: signal(0),
            daemon_running: signal(daemon_initially_up),
            daemon_loading: signal(false),
            spinner_tick: signal(0),
            daemon_rx: stored(None),
            force: signal(false),

            job_phase: signal(JobPhase::Idle),
            job_missing_model: signal(None),
            job_rx: stored(None),
            job_cancel: stored(None),
            backend: stored(backend),

            models_dir: stored(dir),
            installed_groups: signal(installed_groups),
            installed_set: signal(installed_set),
            remote_groups: signal(initial_remote_groups),
            remote_stems: signal(fallback_stems),
            remote_loading: signal(false),
            downloading_models: signal(HashSet::new()),
            models_status: signal(String::new()),
            model_rx: stored(Some(model_rx)),
            model_tx: stored(model_tx),

            host_service: stored(host_service),
            host_rx: stored(None),
            host_handle: stored(None),
            host_running: signal(false),
            host_port: signal(9527),
            host_auto_approve: signal(false),
            host_ips: signal(init_ips),
            host_ip_idx: signal(0),
            show_ip_picker: signal(false),
            host_fingerprint: signal(init_fp),
            host_pubkey_b64: signal(init_pk),
            host_qr_payload: signal(String::new()),
            host_qr_uri: signal(String::new()),
            host_logs: signal(Vec::new()),
            host_pending_approval: signal(None),
            host_trusted: signal(crate::host_bridge::trust_list()),
        }
    }

    // ── Workbench input management ──────────────────────────────────────────

    pub fn add_paths(self, paths: &[PathBuf]) {
        let mut list = self.inputs.get();
        let was_empty = list.is_empty();
        for p in paths {
            if !p.is_file() {
                continue;
            }
            let ext = p
                .extension()
                .and_then(|e| e.to_str())
                .map(|s| s.to_ascii_lowercase())
                .unwrap_or_default();
            if !matches!(ext.as_str(), "png" | "jpg" | "jpeg" | "webp" | "avif") {
                continue;
            }
            if list.iter().any(|it| &it.path == p) {
                continue;
            }
            let filename = p
                .file_name()
                .map(|n| n.to_string_lossy().into_owned())
                .unwrap_or_else(|| "image".into());
            let (w, h) = crate::engine_bridge::probe_dimensions(p).unwrap_or((0, 0));
            list.push(InputItem {
                path: p.clone(),
                filename,
                width: w,
                height: h,
                status: ItemStatus::Ready,
            });
        }
        if was_empty && !list.is_empty() {
            self.active_input.set(0);
        }
        self.inputs.set(list);
    }

    pub fn remove_input(self, idx: usize) {
        let mut list = self.inputs.get();
        if idx >= list.len() {
            return;
        }
        list.remove(idx);
        let len = list.len();
        self.inputs.set(list);
        let cur = self.active_input.get();
        if len == 0 {
            self.active_input.set(0);
        } else if cur >= len {
            self.active_input.set(len - 1);
        }
    }

    pub fn clear_inputs(self) {
        self.inputs.set(Vec::new());
        self.active_input.set(0);
        if !matches!(self.job_phase.get(), JobPhase::Running { .. }) {
            self.job_phase.set(JobPhase::Idle);
        }
    }

    /// Returns metadata for the currently selected model (or constructs a default).
    pub fn current_model_variant(self) -> ModelVariant {
        let name = self.selected_model.get();
        for g in self.installed_groups.get() {
            for it in g.items {
                if it.name == name {
                    return it;
                }
            }
        }
        for g in self.remote_groups.get() {
            for it in g.items {
                if it.name == name {
                    return it;
                }
            }
        }
        let scale = models_local::scale_from_filename(&name).unwrap_or(2);
        let short_name = models_local::short_name(&name);
        let dir = self.models_dir.with(|d| d.clone());
        ModelVariant {
            param_path: dir.join(format!("{name}.param")),
            bin_path: dir.join(format!("{name}.bin")),
            name,
            short_name,
            scale,
            scene: "通用".into(),
            recommended: false,
            installed: false,
            size_bytes: 0,
        }
    }

    // ── Job execution ───────────────────────────────────────────────────────

    pub fn is_job_running(self) -> bool {
        matches!(self.job_phase.get(), JobPhase::Running { .. })
    }

    pub fn start_job(self) {
        if self.is_job_running() {
            return;
        }
        let mut list = self.inputs.get();
        if list.is_empty() {
            return;
        }
        for it in &mut list {
            it.status = ItemStatus::Ready;
        }
        let paths: Vec<PathBuf> = list.iter().map(|it| it.path.clone()).collect();
        self.inputs.set(list);

        let cancel = Arc::new(AtomicBool::new(false));
        let (tx, rx) = mpsc::channel();
        let _ = self.job_cancel.replace(Some(cancel.clone()));
        let _ = self.job_rx.replace(Some(rx));

        let total = paths.len();
        self.job_missing_model.set(None);
        self.job_phase.set(JobPhase::Running {
            item_index: 0,
            item_total: total,
            fraction: 0.0,
            stage: "准备超分任务…".into(),
        });

        let req = JobRequest {
            inputs: paths,
            model_name: self.selected_model.get(),
            models_dir: self.models_dir.with(|d| d.clone()),
            format: self.format.get(),
            quality: self.quality.get(),
            chroma: self.chroma.get(),
            avif_speed: self.avif_speed.get(),
            custom_out_dir: self.custom_out_dir.get(),
            use_gpu: self.use_gpu.get(),
            tile_size: self.tile_size.get(),
            force: self.force.get(),
            use_daemon: self.daemon_running.get(),
            cancel,
        };
        self.backend.with(|b| b.spawn(req, tx));
    }

    pub fn cancel_job(self) {
        self.job_cancel.with(|slot| {
            if let Some(c) = slot {
                c.store(true, Ordering::Relaxed);
            }
        });
    }

    // ── Warm Daemon (`upix_host::serve`) ────────────────────────────────────

    pub fn refresh_daemon_status(self) {
        #[cfg(feature = "integration")]
        {
            let sock = upix_host::serve::default_serve_socket_path();
            self.daemon_running
                .set(upix_host::serve::status(&sock).running);
        }
    }

    pub fn toggle_daemon(self, enable: bool) {
        if self.daemon_loading.get() {
            return;
        }
        #[cfg(feature = "integration")]
        {
            let sock = upix_host::serve::default_serve_socket_path();
            if enable {
                self.daemon_loading.set(true);
                let dir = self.models_dir.with(|d| d.clone());
                let model = self.selected_model.get();
                let dev = if self.use_gpu.get() {
                    upix_host::serve::ServeDevice::Gpu
                } else {
                    upix_host::serve::ServeDevice::Cpu
                };
                let (tx, rx) = mpsc::channel();
                let _ = self.daemon_rx.replace(Some(rx));
                std::thread::spawn(move || {
                    let _ = upix_host::serve::ensure_running(
                        &upix_host::transport_std::StdUdsFactory,
                        &sock,
                        &dir,
                        &model,
                        dev,
                        1800,
                        Duration::from_secs(6),
                    );
                    let up = upix_host::serve::status(&sock).running;
                    let _ = tx.send(up);
                });
            } else {
                let _ = upix_host::serve::stop(&sock);
                self.daemon_running.set(false);
                self.daemon_loading.set(false);
            }
        }
        #[cfg(not(feature = "integration"))]
        {
            if enable {
                self.daemon_loading.set(true);
                let (tx, rx) = mpsc::channel();
                let _ = self.daemon_rx.replace(Some(rx));
                std::thread::spawn(move || {
                    std::thread::sleep(Duration::from_millis(450));
                    let _ = tx.send(true);
                });
            } else {
                self.daemon_running.set(false);
                self.daemon_loading.set(false);
            }
        }
    }

    // ── Models Hub actions ──────────────────────────────────────────────────

    pub fn rescan_models(self) {
        let dir = self.models_dir.with(|d| d.clone());
        let (groups, set) = models_local::scan_installed_groups(&dir);
        let remote = models_local::build_remote_groups(&self.remote_stems.get(), &dir, &set);
        self.installed_groups.set(groups);
        self.installed_set.set(set);
        self.remote_groups.set(remote);
    }

    pub fn fetch_remote_catalog(self) {
        if self.remote_loading.get() {
            return;
        }
        self.remote_loading.set(true);
        self.models_status.set("正在拉取远端模型清单…".into());
        let tx = self.model_tx.with(|t| t.clone());
        std::thread::spawn(move || {
            #[cfg(feature = "integration")]
            let res = upix_host::modelctl::list_remote(upix_host::modelctl::DEFAULT_MODEL_REPO)
                .map(|v| v.into_iter().map(|m| m.name).collect());
            #[cfg(not(feature = "integration"))]
            let res = Ok(models_local::FALLBACK_CATALOG
                .iter()
                .map(|s| (*s).to_string())
                .collect());
            let _ = tx.send(ModelTaskEvent::RemoteList(res));
        });
    }

    pub fn download_model(self, name: String) {
        let mut dl = self.downloading_models.get();
        if dl.contains(&name) {
            return;
        }
        dl.insert(name.clone());
        self.downloading_models.set(dl);
        self.models_status.set(format!("正在下载 {name}…"));

        let dir = self.models_dir.with(|d| d.clone());
        let tx = self.model_tx.with(|t| t.clone());
        std::thread::spawn(move || {
            #[cfg(feature = "integration")]
            let res = upix_host::modelctl::download(
                &name,
                &dir,
                upix_host::modelctl::DEFAULT_MODEL_REPO,
                None,
            )
            .map(|_| ());
            #[cfg(not(feature = "integration"))]
            let res = {
                let _ = std::fs::create_dir_all(&dir);
                let _ = std::fs::write(dir.join(format!("{name}.param")), b"7767517\n0 0\n");
                let _ = std::fs::write(dir.join(format!("{name}.bin")), b"\x00\x00\x00\x00");
                Ok(())
            };
            let _ = tx.send(ModelTaskEvent::DownloadDone { name, result: res });
        });
    }

    pub fn delete_model(self, name: &str) {
        let dir = self.models_dir.with(|d| d.clone());
        match models_local::delete_local_model(&dir, name) {
            Ok(()) => {
                self.models_status.set(format!("已删除本地模型 {name}"));
                self.rescan_models();
            }
            Err(e) => self.models_status.set(e),
        }
    }

    pub fn import_model_files(self, files: &[PathBuf]) {
        let dir = self.models_dir.with(|d| d.clone());
        let lines = models_local::import_files(files, &dir);
        if let Some(last) = lines.last() {
            self.models_status.set(last.clone());
        }
        self.rescan_models();
    }

    // ── Remote Host actions ─────────────────────────────────────────────────

    pub fn start_host(self) {
        if self.host_running.get() {
            return;
        }
        let cfg = HostConfig {
            port: self.host_port.get(),
            auto_approve: self.host_auto_approve.get(),
            models_dir: self.models_dir.with(|d| d.clone()),
        };
        let started = self.host_service.with(|s| s.start(cfg));
        let _ = self.host_handle.replace(Some(started.handle));
        let _ = self.host_rx.replace(Some(started.events));
        self.host_running.set(true);
    }

    pub fn stop_host(self) {
        self.host_handle.with(|slot| {
            if let Some(h) = slot {
                h.stop.store(true, Ordering::Relaxed);
            }
        });
    }

    pub fn select_host_ip(self, idx: usize) {
        let ips = self.host_ips.get();
        if idx >= ips.len() {
            return;
        }
        self.host_ip_idx.set(idx);
        self.show_ip_picker.set(false);
        let ip = &ips[idx];
        let (payload, uri) = crate::host_bridge::build_qr_for_ip(
            ip,
            self.host_port.get(),
            &self.host_fingerprint.get(),
            &self.host_pubkey_b64.get(),
        );
        self.host_qr_payload.set(payload);
        self.host_qr_uri.set(uri);
    }

    pub fn cycle_host_ip(self) {
        let ips = self.host_ips.get();
        if ips.len() <= 1 {
            return;
        }
        let next = (self.host_ip_idx.get() + 1) % ips.len();
        self.select_host_ip(next);
    }

    pub fn answer_approval(self, reply: ApprovalReply) {
        self.host_handle.with(|slot| {
            if let Some(h) = slot {
                let _ = h.reply.send(reply);
            }
        });
        self.host_pending_approval.set(None);
        self.host_trusted.set(crate::host_bridge::trust_list());
    }

    pub fn remove_trusted(self, idx: usize) {
        let _ = crate::host_bridge::remove_trusted_client(idx);
        self.host_trusted.set(crate::host_bridge::trust_list());
    }

    pub fn clear_trusted(self) {
        let _ = crate::host_bridge::clear_trust_list();
        self.host_trusted.set(Vec::new());
    }

    // ── Background channel drain ────────────────────────────────────────────

    /// Returns true when any background worker (job, daemon warmup, catalog download, or host)
    /// is actively producing events or animating.
    pub fn has_background_activity(self) -> bool {
        self.daemon_loading.get_untracked()
            || matches!(
                self.job_phase.get_untracked(),
                JobPhase::Running { .. }
            )
            || self.remote_loading.get_untracked()
            || !self.downloading_models.with_untracked(|s| s.is_empty())
            || self.host_running.get_untracked()
    }

    pub fn drain(self) {
        self.drain_job();
        self.drain_models();
        self.drain_host();
        self.drain_daemon();
    }

    fn drain_daemon(self) {
        let mut finished = None;
        self.daemon_rx.with(|slot| {
            if let Some(rx) = slot {
                while let Ok(up) = rx.try_recv() {
                    finished = Some(up);
                }
            }
        });
        if let Some(up) = finished {
            self.daemon_loading.set(false);
            self.daemon_running.set(up);
            let _ = self.daemon_rx.replace(None);
        }
    }

    fn drain_job(self) {
        let mut events = Vec::new();
        self.job_rx.with(|slot| {
            if let Some(rx) = slot {
                while let Ok(ev) = rx.try_recv() {
                    events.push(ev);
                }
            }
        });

        for ev in events {
            match ev {
                EngineEvent::Progress {
                    item_index,
                    item_total,
                    fraction,
                    stage,
                } => {
                    let mut list = self.inputs.get();
                    if let Some(it) = list.get_mut(item_index) {
                        it.status = ItemStatus::Running(fraction);
                    }
                    self.inputs.set(list);
                    self.job_phase.set(JobPhase::Running {
                        item_index,
                        item_total,
                        fraction,
                        stage,
                    });
                }
                EngineEvent::ItemDone(out) => {
                    let mut list = self.inputs.get();
                    if let Some(it) = list.get_mut(out.index) {
                        if out.skipped {
                            it.status = ItemStatus::Skipped;
                        } else {
                            it.status = ItemStatus::Done {
                                output_path: out.output_path,
                                out_w: out.out_w,
                                out_h: out.out_h,
                            };
                        }
                    }
                    self.inputs.set(list);
                }
                EngineEvent::BatchDone {
                    completed,
                    skipped,
                    elapsed_ms,
                } => {
                    self.job_phase.set(JobPhase::Done {
                        completed,
                        skipped,
                        elapsed_ms,
                    });
                    let _ = self.job_rx.replace(None);
                    let _ = self.job_cancel.replace(None);
                }
                EngineEvent::Cancelled => {
                    self.job_phase.set(JobPhase::Cancelled);
                    let _ = self.job_rx.replace(None);
                    let _ = self.job_cancel.replace(None);
                }
                EngineEvent::Failed(msg) => {
                    // "模型文件缺失" / "model not found" are recoverable by
                    // installing the model; anything else is not, so the
                    // banner must not offer a download that cannot help.
                    let missing = msg.contains("模型文件缺失")
                        || msg.contains("model not found")
                        || msg.contains("No models found")
                        || msg.contains("no models found");
                    self.job_missing_model
                        .set(missing.then(|| self.selected_model.get()));
                    self.job_phase.set(JobPhase::Failed(msg));
                    let _ = self.job_rx.replace(None);
                    let _ = self.job_cancel.replace(None);
                }
            }
        }
    }

    fn drain_models(self) {
        let mut events = Vec::new();
        self.model_rx.with(|slot| {
            if let Some(rx) = slot {
                while let Ok(ev) = rx.try_recv() {
                    events.push(ev);
                }
            }
        });

        for ev in events {
            match ev {
                ModelTaskEvent::RemoteList(res) => {
                    self.remote_loading.set(false);
                    match res {
                        Ok(stems) => {
                            let count = stems.len();
                            self.remote_stems.set(stems);
                            self.rescan_models();
                            self.models_status
                                .set(format!("已同步远端模型清单（共 {count} 项）"));
                        }
                        Err(e) => {
                            self.models_status
                                .set(format!("拉取远端清单失败（使用内置目录）：{e}"));
                        }
                    }
                }
                ModelTaskEvent::DownloadDone { name, result } => {
                    let mut dl = self.downloading_models.get();
                    dl.remove(&name);
                    self.downloading_models.set(dl);
                    match result {
                        Ok(()) => {
                            self.models_status.set(format!("下载完成：{name}"));
                            self.rescan_models();
                        }
                        Err(e) => {
                            self.models_status.set(format!("下载 {name} 失败：{e}"));
                        }
                    }
                }
            }
        }
    }

    fn drain_host(self) {
        let mut events = Vec::new();
        self.host_rx.with(|slot| {
            if let Some(rx) = slot {
                while let Ok(ev) = rx.try_recv() {
                    events.push(ev);
                }
            }
        });

        for ev in events {
            match ev {
                HostEvent::Started {
                    port,
                    ips,
                    fingerprint,
                    pubkey_b64,
                    qr_payload,
                    qr_data_uri,
                } => {
                    let prev_ip = self
                        .host_ips
                        .get_untracked()
                        .get(self.host_ip_idx.get_untracked())
                        .cloned();
                    let chosen_idx = prev_ip
                        .as_deref()
                        .and_then(|target| ips.iter().position(|x| x == target))
                        .unwrap_or(0);
                    self.host_port.set(port);
                    self.host_ips.set(ips.clone());
                    self.host_ip_idx.set(chosen_idx);
                    self.host_fingerprint.set(fingerprint.clone());
                    self.host_pubkey_b64.set(pubkey_b64.clone());
                    if chosen_idx > 0 {
                        if let Some(ip) = ips.get(chosen_idx) {
                            let (p, u) = crate::host_bridge::build_qr_for_ip(
                                ip,
                                port,
                                &fingerprint,
                                &pubkey_b64,
                            );
                            self.host_qr_payload.set(p);
                            self.host_qr_uri.set(u);
                        } else {
                            self.host_qr_payload.set(qr_payload);
                            self.host_qr_uri.set(qr_data_uri);
                        }
                    } else {
                        self.host_qr_payload.set(qr_payload);
                        self.host_qr_uri.set(qr_data_uri);
                    }
                    self.host_trusted.set(crate::host_bridge::trust_list());
                }
                HostEvent::Log(line) => {
                    let mut logs = self.host_logs.get();
                    logs.push(line);
                    if logs.len() > 200 {
                        let excess = logs.len() - 200;
                        logs.drain(0..excess);
                    }
                    self.host_logs.set(logs);
                }
                HostEvent::Approval(req) => {
                    self.host_pending_approval.set(Some(req));
                }
                HostEvent::Stopped => {
                    self.host_running.set(false);
                    self.host_pending_approval.set(None);
                    let _ = self.host_rx.replace(None);
                    let _ = self.host_handle.replace(None);
                }
            }
        }
    }
}

/// Converts a `file://` URI (from drag-and-drop) into a local `PathBuf`.
pub fn file_uri_path(uri: &str) -> Option<PathBuf> {
    let raw = uri.strip_prefix("file://").unwrap_or(uri);
    let decoded = percent_decode(raw);
    let path = Path::new(&decoded);
    if path.exists() {
        Some(path.to_path_buf())
    } else {
        None
    }
}

fn percent_decode(s: &str) -> String {
    let bytes = s.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' && i + 2 < bytes.len() {
            if let (Some(h), Some(l)) = (hex_val(bytes[i + 1]), hex_val(bytes[i + 2])) {
                out.push((h << 4) | l);
                i += 3;
                continue;
            }
        }
        out.push(bytes[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

fn hex_val(b: u8) -> Option<u8> {
    match b {
        b'0'..=b'9' => Some(b - b'0'),
        b'a'..=b'f' => Some(b - b'a' + 10),
        b'A'..=b'F' => Some(b - b'A' + 10),
        _ => None,
    }
}
