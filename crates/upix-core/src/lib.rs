//! upix-core — Rust port of the `share/core` orchestration layer: tile
//! pipeline, tile-size optimizer, engine pool/setup, model manager/graph,
//! processing config. The inference backend contract lives in [`engine`].
//!
//! Porting contract: docs/rewrite/CONTRACT.md §core.

pub mod bicubic;
pub mod config;
pub mod engine;
pub mod engine_pool;
pub mod engine_setup;
pub mod model_catalog;
pub mod model_families;
pub mod model_graph;
pub mod model_manager;
pub mod model_metadata;
pub mod tile_optimizer;
pub mod tile_processor;

#[cfg(test)]
pub(crate) mod mock;

// CONTRACT §3 API pins re-exported at the crate root.
pub use config::{ProcessingConfig, resolve_output_path, sanitize_model_for_filename};
pub use engine_setup::{EngineStrategy, initialize_engine, switch_model};
pub use model_families::{FamilyGroup, FamilyItem, groups as model_family_groups, model_stem, short_model_name};
pub use model_graph::{detect_from_param, scale_from_filename};
pub use model_manager::ModelManager;
pub use tile_optimizer::TileSizeOptimizer;
pub use tile_processor::{ScaledImage, TileProcessor, TileProcessorError, process_image_u8};

/// Free-function form of [`TileProcessor::compute_tile_layout`] (CONTRACT pin).
pub fn compute_tile_layout(width: i32, height: i32, tile: i32, overlap: i32) -> Vec<engine::TileRegion> {
    TileProcessor::compute_tile_layout(width, height, tile, overlap)
}
