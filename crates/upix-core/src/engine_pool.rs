//! EnginePool — Rust port of `share/core/engine_pool.{hpp,cpp}`.
//!
//! A fixed set of initialized, model-loaded engines with blocking
//! acquire/release. CPU engines split the machine's physical cores so
//! pool_size * threads_per_engine ~= physical cores.

use crate::engine::{DeviceType, Engine, EngineError};
use std::path::Path;
use std::sync::{Condvar, Mutex};

/// Physical (non-SMT) core count. On SMT/hybrid CPUs, ncnn's default of one
/// thread per logical CPU oversubscribes badly when several engines run
/// concurrently; the pool divides this count across its engines.
fn detect_physical_core_count() -> i32 {
    if let Ok(cpuinfo) = std::fs::read_to_string("/proc/cpuinfo") {
        // Distinct (physical id, core id) pairs, mirroring the C++ set<pair>.
        let mut cores: std::collections::BTreeSet<(i64, i64)> = Default::default();
        let mut physical_id: Option<i64> = None;
        for line in cpuinfo.lines() {
            if let Some(rest) = line.strip_prefix("physical id") {
                physical_id = rest
                    .split_once(':')
                    .and_then(|(_, v)| v.trim().parse().ok());
            } else if let Some(rest) = line.strip_prefix("core id") {
                if let Some((_, v)) = rest.split_once(':') {
                    if let Some(pid) = physical_id {
                        if let Ok(cid) = v.trim().parse() {
                            cores.insert((pid, cid));
                        }
                    }
                }
            }
        }
        if !cores.is_empty() {
            return cores.len() as i32;
        }
    }
    let logical = std::thread::available_parallelism()
        .map(|n| n.get() as i32)
        .unwrap_or(0);
    if logical > 0 {
        logical
    } else {
        1
    }
}

pub struct EnginePool {
    engines: Vec<Box<dyn Engine>>,
    /// Free-engine stack (indices into `engines`).
    available: Mutex<Vec<usize>>,
    available_cv: Condvar,
}

impl EnginePool {
    pub fn new(
        pool_size: usize,
        engine_factory: impl Fn() -> Box<dyn Engine>,
        param_path: &Path,
        bin_path: &Path,
        device_type: DeviceType,
    ) -> Result<Self, EngineError> {
        if pool_size == 0 {
            return Err(EngineError::new(1, "EnginePool size cannot be zero"));
        }

        // CPU engines split the machine's physical cores so that
        // pool_size * threads_per_engine ~= physical cores (ncnn guidance: at
        // most 8 threads per net). Without this, every engine spawns one
        // thread per logical CPU and N engines oversubscribe the machine
        // N-fold. The budget is computed for GPU pools too: a GPU pool whose
        // Vulkan init fails runs CPU engines, and those need the same cap.
        let physical_cores = detect_physical_core_count();
        let engines = pool_size as i32;
        let mut threads_per_engine =
            ((physical_cores + engines - 1) / engines).clamp(1, 8);
        if let Ok(env_t) = std::env::var("UPIX_CPU_THREADS_PER_ENGINE") {
            if let Ok(t) = env_t.trim().parse::<i32>() {
                threads_per_engine = t.clamp(1, 16);
            }
        }
        if device_type == DeviceType::Cpu {
            log::info!(target: "EnginePool",
                "CPU thread budget: {} physical cores / {} engines = {} inference threads per engine",
                physical_cores, pool_size, threads_per_engine);
        }

        let mut pool_engines: Vec<Box<dyn Engine>> = Vec::with_capacity(pool_size);
        for i in 0..pool_size {
            let mut engine = engine_factory();

            let init_res = engine.initialize(device_type);
            if init_res.is_err() {
                log::warn!(target: "EnginePool",
                    "Failed to initialize engine {} with specified device, falling back to CPU", i);
                if let Err(e) = engine.initialize(DeviceType::Cpu) {
                    return Err(EngineError::new(
                        1,
                        format!("Failed to initialize engine {}: {}", i, e.message),
                    ));
                }
            }

            // Keyed on the device the engine actually came up on, not the
            // requested one: a GPU pool whose Vulkan init failed falls back to
            // CPU, and those engines would otherwise keep ncnn's
            // one-thread-per-logical-CPU default — the N-fold oversubscription
            // this budget exists to prevent.
            if engine.current_device() == DeviceType::Cpu {
                engine.set_inference_threads(threads_per_engine);
            }

            // Load model
            if let Err(e) = engine.load_model(param_path, bin_path) {
                return Err(EngineError::new(
                    2,
                    format!("Failed to load model in engine {}: {}", i, e.message),
                ));
            }

            pool_engines.push(engine);
        }

        log::info!(target: "EnginePool", "Created pool with {} engines on {}",
            pool_size, if device_type == DeviceType::GpuVulkan { "GPU" } else { "CPU" });

        let available = (0..pool_size).rev().collect();
        Ok(Self { engines: pool_engines, available: Mutex::new(available), available_cv: Condvar::new() })
    }

    /// Wait for a free engine instead of throwing: callers are allowed to have
    /// more concurrent tasks than engines (e.g. a worker pool racing acquire).
    /// Use with [`PoolGuard`] for automatic release.
    pub fn acquire(&self) -> PoolGuard<'_> {
        let idx = self.acquire_index();
        PoolGuard { pool: self, idx, released: false }
    }

    /// Raw index acquire (the C++ `Engine*` shape). Release with
    /// [`EnginePool::release`] or a [`PoolGuard`].
    pub fn acquire_index(&self) -> usize {
        let mut available = self.available.lock().unwrap();
        while available.is_empty() {
            available = self.available_cv.wait(available).unwrap();
        }
        available.pop().unwrap()
    }

    /// Only pool-owned engines, and only once: an unchecked push lets a caller
    /// bug (double release, foreign pointer) grow the free list past
    /// pool_size, and acquire() then hands the same engine to two workers at
    /// once — two threads inside one ncnn::Net. The free list is a vector (a
    /// handful of entries) so the duplicate scan is cheap.
    pub fn release(&self, idx: usize) {
        {
            let mut available = self.available.lock().unwrap();
            if idx >= self.engines.len() {
                log::warn!(target: "EnginePool",
                    "release() called with an engine that is not in this pool; ignoring");
                return;
            }
            if available.contains(&idx) {
                log::warn!(target: "EnginePool",
                    "release() called twice for the same engine; ignoring");
                return;
            }
            available.push(idx);
        }
        self.available_cv.notify_one();
    }

    /// Borrow the engine behind an acquired index (tied to the pool borrow,
    /// so a PoolGuard deref keeps the pool's lifetime).
    pub fn engine<'p>(&'p self, idx: usize) -> &'p dyn Engine {
        self.engines[idx].as_ref()
    }

    pub fn pool_size(&self) -> usize {
        self.engines.len()
    }
}

/// RAII wrapper for an acquired engine: releases back to the pool on drop.
pub struct PoolGuard<'a> {
    pool: &'a EnginePool,
    idx: usize,
    released: bool,
}

impl<'a> PoolGuard<'a> {
    pub fn index(&self) -> usize {
        self.idx
    }

    /// Release early; a no-op after this when the guard drops.
    pub fn release(mut self) {
        self.released = true;
        self.pool.release(self.idx);
    }
}

impl<'a> std::ops::Deref for PoolGuard<'a> {
    type Target = dyn Engine + 'a;
    fn deref(&self) -> &Self::Target {
        self.pool.engine(self.idx)
    }
}

impl Drop for PoolGuard<'_> {
    fn drop(&mut self) {
        if !self.released {
            self.pool.release(self.idx);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mock::MockEngine;
    use std::path::Path;

    fn factory() -> Box<dyn Engine> {
        Box::new(MockEngine::new())
    }

    #[test]
    fn zero_size_is_rejected() {
        let err = match EnginePool::new(0, factory, Path::new("p"), Path::new("b"), DeviceType::Cpu)
        {
            Err(e) => e,
            Ok(_) => panic!("zero pool must be rejected"),
        };
        assert_eq!(err.code, 1);
    }

    #[test]
    fn acquire_release_roundtrip_and_blocking() {
        let pool =
            EnginePool::new(2, factory, Path::new("p"), Path::new("b"), DeviceType::Cpu).unwrap();
        assert_eq!(pool.pool_size(), 2);

        std::thread::scope(|s| {
            let a = pool.acquire();
            let b = pool.acquire();
            assert_ne!(a.index(), b.index(), "concurrent acquires must get distinct engines");

            // Third acquire has nothing free — release in a sibling thread
            // and it must unblock.
            let releaser = s.spawn({
                let b = b;
                move || {
                    std::thread::sleep(std::time::Duration::from_millis(20));
                    drop(b); // releases b back
                }
            });
            let _c = pool.acquire();
            releaser.join().unwrap();
            drop(a);
        });
    }

    #[test]
    fn double_release_is_ignored() {
        let pool =
            EnginePool::new(1, factory, Path::new("p"), Path::new("b"), DeviceType::Cpu).unwrap();
        let guard = pool.acquire();
        let idx = guard.index();
        pool.release(idx);
        pool.release(idx); // duplicate: must warn + ignore
        let g2 = pool.acquire();
        assert_eq!(g2.index(), idx);
        g2.release();
    }

    #[test]
    fn foreign_index_release_is_ignored() {
        let pool =
            EnginePool::new(1, factory, Path::new("p"), Path::new("b"), DeviceType::Cpu).unwrap();
        pool.release(42); // out of range: ignored
        pool.release(usize::MAX);
        let guard = pool.acquire();
        assert_eq!(guard.index(), 0);
    }

    #[test]
    fn gpu_request_falls_back_to_cpu_engines() {
        // MockEngine::new() rejects GpuVulkan initialize → the pool must fall
        // back per-engine instead of failing the whole pool.
        let pool =
            EnginePool::new(2, factory, Path::new("p"), Path::new("b"), DeviceType::GpuVulkan)
                .expect("fallback pool must build");
        let guard = pool.acquire();
        assert_eq!(guard.current_device(), DeviceType::Cpu);
    }
}
