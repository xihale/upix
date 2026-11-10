//! `upix model` — local catalog + GitHub release model management.
//!
//!   model list                     scan the models dir, print name/scale/size
//!   model list-remote [--repo R]   fetch the release manifest, print catalog
//!   model download NAME [...]      download model(s) into the models dir
//!   model import FILE [...]        copy local .param/.bin pairs into the dir

use std::path::PathBuf;

use upix_host::modelctl_api as modelctl;
use upix_host::RemoteModel;

use crate::args::ModelArgs;
use crate::common::{default_models_dir, emit_json_line, json_escape};
use crate::logging;

/// Case/dash-insensitive name match against the manifest (same normalization
/// idea as ModelManager::get_model).
fn find_remote<'a>(models: &'a [RemoteModel], name: &str) -> Option<&'a RemoteModel> {
    modelctl::find_remote(models, name)
}

fn mb(bytes: u64) -> String {
    format!("{:.1} MB", bytes as f64 / 1024.0 / 1024.0)
}

/// Manifest-shaped size, or "—" when the entry came from the embedded catalog
/// (sizes are per-release facts, so the offline list cannot know them; printing
/// "0.0 MB" there read like a truncated download).
fn size_label(bytes: u64) -> String {
    if bytes == 0 { "—".to_string() } else { mb(bytes) }
}

/// "[动漫] ★" style annotation for catalog models; empty for user imports.
fn scene_suffix(name: &str) -> String {
    let meta = upix_core::model_metadata::classify_model_scene(name);
    if meta.scene.is_empty() {
        return String::new();
    }
    if meta.recommended {
        format!(" [{}] ★", meta.scene)
    } else {
        format!(" [{}]", meta.scene)
    }
}

/// One-line legend so the tags/stars are self-explanatory in a terminal.
fn print_scene_legend() {
    println!("  ([场景] 适用：动漫/照片/综合/修复… · ★ 推荐)");
}

fn cmd_model_list(a: &ModelArgs) -> i32 {
    if a.json {
        logging::enable_machine_output();
    }
    let dir = if a.models_dir.is_empty() {
        PathBuf::from(default_models_dir())
    } else {
        PathBuf::from(&a.models_dir)
    };
    let mut manager = upix_core::ModelManager::new(dir.clone());

    // Machine-readable form: names only, and quiet — the logger writes to
    // stdout by design (machine mode reroutes it to stderr), so any
    // diagnostic would pollute what scripts and shell completions consume.
    // Missing/unscannable dir → no output at all.
    if a.names_only {
        if !dir.is_dir() {
            return 0;
        }
        if let Ok(names) = manager.scan_models() {
            for m in &names {
                println!("{}", m.name);
            }
        }
        return 0;
    }

    let models = manager.scan_models();
    let models = match models {
        Ok(m) => m,
        Err(e) => {
            log::error!(target: "Model", "Failed to scan models: {}", e.message);
            return 1;
        }
    };

    if a.json {
        // Same fields the desktop catalog shows; `size` keeps the human
        // formatting so the GUI needs no formatting logic.
        let mut json = format!(
            r#"{{"modelsDir":"{}","installed":["#,
            json_escape(&dir.to_string_lossy())
        );
        for (i, m) in models.iter().enumerate() {
            if i > 0 {
                json.push(',');
            }
            let meta = upix_core::model_metadata::classify_model_scene(&m.name);
            json.push_str(&format!(
                r#"{{"name":"{}","scale":{},"size":"{} MB","scene":"{}","recommended":{}}}"#,
                json_escape(&m.name),
                m.scale_factor,
                m.total_size / 1024 / 1024,
                json_escape(&meta.scene),
                meta.recommended
            ));
        }
        json.push_str("]}");
        emit_json_line(json);
        return 0;
    }

    println!("Installed models ({}):", dir.display());
    if models.is_empty() {
        println!("  (none — try `upix model download <name>` or `model list-remote`)");
        return 0;
    }
    for m in &models {
        println!(
            "  {} ({}x, {} MB){}",
            m.name,
            m.scale_factor,
            m.total_size / 1024 / 1024,
            scene_suffix(&m.name)
        );
    }
    print_scene_legend();
    0
}

fn cmd_model_list_remote(a: &ModelArgs) -> i32 {
    if a.json {
        logging::enable_machine_output();
    }
    // Same fallback the GUI uses: without the release manifest a listing is
    // still answerable from the embedded catalog, so an offline machine can
    // see what exists (downloading itself still needs the network).
    let (remote, fallback) = modelctl::list_remote_or_catalog(&a.repo);

    if a.json {
        let mut json = format!(
            r#"{{"manifestUrl":"{}","source":"{}","remote":["#,
            json_escape(&modelctl::manifest_url(&a.repo)),
            if fallback.is_some() { "catalog" } else { "release" }
        );
        for (i, m) in remote.iter().enumerate() {
            if i > 0 {
                json.push(',');
            }
            let meta = upix_core::model_metadata::classify_model_scene(&m.name);
            json.push_str(&format!(
                r#"{{"name":"{}","scale":{},"size":"{}","scene":"{}","recommended":{}}}"#,
                json_escape(&m.name),
                m.scale,
                size_label(m.size_bytes),
                json_escape(&meta.scene),
                meta.recommended
            ));
        }
        json.push_str("]}");
        emit_json_line(json);
        if let Some(err) = &fallback {
            log::warn!(target: "Model", "Release manifest unavailable, listing the built-in catalog: {err}");
        }
        return 0;
    }

    if let Some(err) = &fallback {
        log::warn!(target: "Model", "Release manifest unavailable, listing the built-in catalog: {err}");
        log::warn!(target: "Model", "Sizes/URLs need the release; `model download` will fail until the network is back");
    }
    println!(
        "Models available in the release ({}):",
        modelctl::manifest_url(&a.repo)
    );
    if remote.is_empty() {
        println!("  (manifest is empty)");
    }
    for m in &remote {
        println!(
            "  {} ({}x, {}){}",
            m.name,
            m.scale,
            size_label(m.size_bytes),
            scene_suffix(&m.name)
        );
    }
    print_scene_legend();
    println!("\nDownload with: upix model download <name>");
    0
}

fn cmd_model_download(a: &ModelArgs) -> i32 {
    if a.json {
        logging::enable_machine_output();
    }
    if a.names.is_empty() {
        log::error!(
            target: "Model",
            "model download requires at least one model name (see `model list-remote`)"
        );
        return 1;
    }

    let remote = match modelctl::list_remote(&a.repo) {
        Ok(r) => r,
        Err(e) => {
            // Downloading needs per-release URLs, so there is no offline path
            // here — but the message must name one the user can take.
            log::error!(target: "Model", "Cannot reach the model release: {e}");
            log::error!(
                target: "Model",
                "Check the network/proxy, or install files you already have with `upix model import <file.param> <file.bin>`"
            );
            return 1;
        }
    };

    let dir = if a.models_dir.is_empty() {
        PathBuf::from(default_models_dir())
    } else {
        PathBuf::from(&a.models_dir)
    };
    let mut failures = 0;
    for name in &a.names {
        let Some(m) = find_remote(&remote, name) else {
            log::error!(target: "Model", "Model not found in release: {name}");
            failures += 1;
            continue;
        };

        // Machine consumers key the event stream per model; build the event
        // prefix once (name is escaped exactly once).
        let event_prefix = if a.json {
            format!(r#"{{"t":"download","name":"{}","pct":"#, json_escape(&m.name))
        } else {
            String::new()
        };
        if !a.json {
            print!("Downloading {} ({}x, {}) ... ", m.name, m.scale, mb(m.size_bytes));
            use std::io::Write;
            let _ = std::io::stdout().flush();
        }
        let last_cell = std::cell::Cell::new(-1i32);
        let last_cell2 = &last_cell;
        let prefix = event_prefix.clone();
        let json_mode = a.json;
        let progress_cb = move |p: &upix_host::modelctl::DownloadProgress| {
            if p.total > 0 {
                let pct = (p.downloaded * 100 / p.total) as i32;
                let prev = last_cell2.replace(pct);
                if pct != prev && pct % 10 == 0 {
                    if json_mode {
                        emit_json_line(format!("{prefix}{pct}}}"));
                    } else {
                        print!("{pct}% ");
                        use std::io::Write;
                        let _ = std::io::stdout().flush();
                    }
                }
            }
        };
        let result = modelctl::download_model(m, &dir, Some(&progress_cb));
        if !result.ok {
            if !a.json {
                println!();
            }
            log::error!(target: "Model", "Download failed: {}", result.message);
            if a.json {
                // Terminal event: without it a machine consumer that keyed on
                // the progress stream cannot tell a failure from a stall.
                emit_json_line(format!(
                    r#"{{"t":"download","name":"{}","error":"{}"}}"#,
                    json_escape(&m.name),
                    json_escape(&result.message)
                ));
            }
            failures += 1;
            continue;
        }
        if a.json {
            // The callback is throttled to 10% steps and may never see 100.
            if last_cell.get() < 100 {
                emit_json_line(format!("{event_prefix}100}}"));
            }
        } else {
            println!("done\n  {}\n  {}", result.param_path.display(), result.bin_path.display());
        }
    }
    if failures == 0 {
        0
    } else {
        1
    }
}

/// Import local .param/.bin files (user-provided models): each file is copied
/// into the models dir; pairs are matched by stem. Only .param/.bin — a .zip
/// is not accepted.
fn cmd_model_import(a: &ModelArgs) -> i32 {
    if a.json {
        logging::enable_machine_output();
    }
    if a.files.is_empty() {
        log::error!(
            target: "Model",
            "model import requires at least one .param/.bin file path"
        );
        return 1;
    }

    let dir = if a.models_dir.is_empty() {
        PathBuf::from(default_models_dir())
    } else {
        PathBuf::from(&a.models_dir)
    };
    if let Err(e) = std::fs::create_dir_all(&dir) {
        log::error!(target: "Model", "Cannot create models directory {}: {e}", dir.display());
        return 1;
    }

    let files: Vec<String> = a.files.clone();
    match modelctl::import(
        &files.iter().map(PathBuf::from).collect::<Vec<_>>(),
        &dir,
    ) {
        Ok(installed) => {
            for (param, _bin) in &installed {
                let stem = param
                    .file_stem()
                    .map(|s| s.to_string_lossy().into_owned())
                    .unwrap_or_default();
                println!("Imported {stem}");
            }
            if a.json {
                let mut json = String::from(r#"{"ok":true,"imported":["#);
                for (i, (param, _)) in installed.iter().enumerate() {
                    if i > 0 {
                        json.push(',');
                    }
                    let stem = param
                        .file_stem()
                        .map(|s| s.to_string_lossy().into_owned())
                        .unwrap_or_default();
                    json.push_str(&format!(r#""{}""#, json_escape(&stem)));
                }
                json.push_str("]}");
                emit_json_line(json);
            }
            0
        }
        Err(e) => {
            log::error!(target: "Model", "{e}");
            if a.json {
                emit_json_line(format!(
                    r#"{{"ok":false,"error":"{}"}}"#,
                    json_escape(&e)
                ));
            }
            1
        }
    }
}

pub fn run(a: &ModelArgs) -> i32 {
    match a.subcommand.as_str() {
        "list" => cmd_model_list(a),
        "list-remote" => cmd_model_list_remote(a),
        "download" => cmd_model_download(a),
        "import" => cmd_model_import(a),
        other => {
            log::error!(target: "Model", "Unknown model subcommand: {other}");
            1
        }
    }
}
