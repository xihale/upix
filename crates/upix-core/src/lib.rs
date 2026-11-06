//! upix-core — Rust port of the `share/core` orchestration layer: tile
//! pipeline, tile-size optimizer, engine pool/setup, model manager/graph,
//! processing config. The inference backend contract lives in [`engine`].
//!
//! Porting contract: docs/rewrite/CONTRACT.md §core.

pub mod engine;
