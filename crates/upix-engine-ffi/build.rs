//! Builds the workspace's only C++ FFI island: the ncnn/Vulkan engine subset
//! (git-mv'd from share/ into csrc/upix/) plus the extern "C" shim.
//!
//! Engine sources are compiled verbatim — no semantic edits. The share/ tree
//! still supplies `engine.hpp`, `tile_processor.{hpp,cpp}`, `model_graph.{hpp,cpp}`
//! and `utils/benchmark.{hpp,cpp}` at build time (the engine's declared deps);
//! see CONTRACT §7 and the delivery report for the cutover note.

use std::env;
use std::fs;
use std::path::{Path, PathBuf};

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"));
    let csrc = manifest_dir.join("csrc");
    let workspace = manifest_dir
        .parent()
        .and_then(Path::parent)
        .expect("workspace root")
        .to_path_buf();
    let share = workspace.join("share");

    for dir in [
        csrc.join("upix/core"),
        csrc.join("upix/utils"),
        share.join("core"),
        share.join("utils"),
    ] {
        println!("cargo:rerun-if-changed={}", dir.display());
    }

    // --- ncnn static library discovery -------------------------------------
    // CONTRACT §1: xmake cache ~/.xmake/packages/n/ncnn/20260526/<hash> with
    // multiple platform variants; manifest.txt tells them apart. UPIX_NCNN_DIR
    // overrides discovery entirely.
    let ncnn_dir = match env::var("UPIX_NCNN_DIR") {
        Ok(dir) => PathBuf::from(dir),
        Err(_) => discover_ncnn().unwrap_or_else(|| {
            let home = env::var("HOME").unwrap_or_else(|_| "$HOME".into());
            panic!(
                "no linux/x86_64 ncnn 20260526 found in {home}/.xmake/packages \
                 (need shared=false, vulkan=true, online_spirv=false); set UPIX_NCNN_DIR"
            )
        }),
    };
    let ncnn_include = ncnn_dir.join("include");
    let ncnn_lib = ncnn_dir.join("lib");
    println!("cargo:rerun-if-changed={}", ncnn_lib.join("libncnn.a").display());
    assert!(
        ncnn_include.join("ncnn/platform.h").is_file(),
        "ncnn include dir not found under {}",
        ncnn_dir.display()
    );
    assert!(
        ncnn_lib.join("libncnn.a").is_file(),
        "libncnn.a not found under {}",
        ncnn_dir.display()
    );

    // --- compile ------------------------------------------------------------
    // Engine sources must stay pristine: they compile against
    //   -I share      -> engine.hpp, tile_processor*, model_graph*, benchmark*
    //   -I csrc/upix  -> moved tile_types/expected/logger + engine subset headers
    // Include order matters: moved csrc/upix/core/tile_types.hpp must shadow the
    // deleted share copy only via fallthrough, never the reverse.
    let mut build = cc::Build::new();
    build
        .cpp(true)
        .flag_if_supported("-std=c++23")
        .include(&csrc)
        .include(&csrc.join("upix"))
        .include(&csrc.join("upix/core"))
        .include(&share)
        .include(&share.join("core"))
        .include(&ncnn_include)
        .include(ncnn_include.join("ncnn")); // engine uses unqualified <net.h> etc.

    let sources = [
        csrc.join("shim.cpp"),
        csrc.join("upix/core/ncnn_engine.cpp"),
        csrc.join("upix/core/ncnn_gpu_init.cpp"),
        csrc.join("upix/core/ncnn_gpu_pipeline.cpp"),
        csrc.join("upix/core/spirv_pack.cpp"),
        share.join("core/tile_processor.cpp"),
        share.join("core/model_graph.cpp"),
        share.join("utils/benchmark.cpp"),
    ];
    for src in &sources {
        println!("cargo:rerun-if-changed={}", src.display());
        build.file(src);
    }
    build.compile("upix_engine_ffi");

    // --- link ---------------------------------------------------------------
    // Static ncnn + its transitive needs (simpleomp is bundled; vulkan is
    // dlopen'd at runtime but linked for presence per CONTRACT).
    println!("cargo:rustc-link-search=native={}", ncnn_lib.display());
    println!("cargo:rustc-link-lib=static=ncnn");
    println!("cargo:rustc-link-lib=dylib=vulkan");
    println!("cargo:rustc-link-lib=dylib=deflate"); // spirv_pack zlib inflate
    println!("cargo:rustc-link-lib=dylib=z");
    println!("cargo:rustc-link-lib=dylib=pthread");
    println!("cargo:rustc-link-lib=dylib=dl");
    println!("cargo:rustc-link-lib=dylib=stdc++"); // ncnn + engine C++ runtime
}

/// Scans the xmake package cache for the linux/x86_64 ncnn variant this
/// project pins: shared=false, vulkan=true, online_spirv=false, simpleomp=true
/// (see xmake.lua `ncnn~runtime`; the online_spirv=true variant is the
/// glslang-linked toolchain build and must not be linked into the runtime).
fn discover_ncnn() -> Option<PathBuf> {
    let home = env::var("HOME").ok()?;
    let base = PathBuf::from(home)
        .join(".xmake/packages/n/ncnn/20260526");
    let mut hits: Vec<PathBuf> = Vec::new();
    for entry in fs::read_dir(&base).ok()? {
        let dir = entry.ok()?.path();
        let manifest = dir.join("manifest.txt");
        let Ok(text) = fs::read_to_string(&manifest) else {
            continue; // some cache dirs (e.g. reference entries) have no manifest
        };
        let raw = |key: &str| -> Option<String> {
            text.split('\n').find_map(|line| {
                let line = line.trim().trim_end_matches(',');
                let (k, v) = line.split_once('=')?;
                (k.trim() == key).then(|| v.trim().trim_matches('"').to_string())
            })
        };
        if raw("plat").as_deref() != Some("linux")
            || raw("arch").as_deref() != Some("x86_64")
            || raw("shared").as_deref() != Some("false")
            || raw("vulkan").as_deref() != Some("true")
            || raw("online_spirv").as_deref() != Some("false")
        {
            continue;
        }
        hits.push(dir);
    }
    hits.sort();
    hits.into_iter().next()
}
