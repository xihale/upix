//! The published model catalog — one list, every consumer.
//!
//! `model list-remote` (CLI), the GUI's 可下载 list and the offline fallback all
//! read this. It exists because the release manifest needs a network round trip:
//! without a cached copy, "what models are there?" was unanswerable on a plane
//! or behind a proxy, and the GUI carried its own hand-maintained list that had
//! drifted (it named models that were never published, e.g. `Animesharp-x4`).
//!
//! The list mirrors `models/README.md` plus the entries that shipped in the
//! release but are documented elsewhere; `catalog_is_sane` in the tests keeps
//! the names unique and scene-classifiable so a typo fails the build instead of
//! becoming a 404 in the UI.
//!
//! This is metadata only: sizes, digests and download URLs come from the live
//! manifest (`upix_host::modelctl`), since they change per release.

/// Every published model name, sorted. Extend `models/README.md` first, then
/// here — `catalog_covers_readme` checks the two agree.
pub const CATALOG: &[&str] = &[
    "AnimeJaNai-v2-Compact-x2",
    "AnimeSharp-x4",
    "Archivist-Medium-Compact-x1",
    "Archivist-Rough-Compact-x1",
    "BSRGAN-x2",
    "BSRGAN-x4",
    "DeH264-RealPLKSR-x1",
    "DeJPG-RealPLKSR-x1",
    "DeNoise-RealPLKSR-x1",
    "Genesis-Cleanup-Compact-x1",
    "HFA2k-LUDVAE-SPAN-x2",
    "HFA2k-x4",
    "LSDIR-Compact-v2-x4",
    "ModernSpanimation-x2",
    "NES-Composite-RGB-Compact-x1",
    "NMKD-Siax-x4",
    "Nomos2-OTF-x4",
    "Nomos8kSC-x4",
    "NomosUni-Compact-x2",
    "NomosUni-SPAN-x2",
    "NomosUni-SPAN-x4",
    "NomosWebPhoto-RealPLKSR-x4",
    "Parimg-Compact-x2",
    "Real-CUGAN-denoise-x2",
    "Real-CUGAN-denoise-x3",
    "Real-CUGAN-no-denoise-x2",
    "Real-ESRGAN-x2plus",
    "Real-ESRGAN-x4plus",
    "Real-ESRGAN-x4plus-anime",
    "Real-ESRGANv3-anime-x2",
    "Real-ESRGANv3-anime-x3",
    "Real-ESRGANv3-general-x4",
    "Remacri-x4",
    "UltraSharp-x4",
];

/// The catalog, for iterating in a stable order.
pub fn names() -> &'static [&'static str] {
    CATALOG
}

/// Exact-name membership (the release matcher is the one that folds case and
/// dashes; this is for "is this a model we publish?" checks).
pub fn is_known(name: &str) -> bool {
    CATALOG.iter().any(|n| *n == name)
}

/// Catalog entries the scene classifier marks as recommended for their domain.
pub fn recommended() -> impl Iterator<Item = &'static str> {
    CATALOG.iter().copied().filter(|n| {
        crate::model_metadata::classify_model_scene(n).recommended
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn catalog_is_sane() {
        let mut seen = std::collections::HashSet::new();
        for name in CATALOG {
            assert!(seen.insert(*name), "duplicate catalog entry: {name}");
            let meta = crate::model_metadata::classify_model_scene(name);
            assert!(
                !meta.scene.is_empty(),
                "catalog entry {name} has no scene — the UI would show a blank badge"
            );
            assert!(
                crate::model_graph::scale_from_filename(name).is_some(),
                "catalog entry {name} carries no -xN scale"
            );
        }
        assert!(is_known("AnimeJaNai-v2-Compact-x2"));
        assert!(!is_known("Animesharp-x4"));
        assert!(recommended().count() > 0);
    }

    #[test]
    fn catalog_covers_readme() {
        // The doc is the human-facing catalog; a model added there but not here
        // never reaches the offline list, which is how the previous fallback
        // drifted. Missing file = skip (published crates do not ship models/).
        let doc = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../models/README.md");
        let Ok(text) = std::fs::read_to_string(doc) else { return };
        let mut documented: Vec<&str> = text
            .lines()
            .filter_map(|l| {
                let rest = l.trim_start().strip_prefix("| `")?;
                let name = rest.split('`').next()?;
                // Rows also carry family shorthand and brace patterns
                // ("DeJPG", "Real-CUGAN-{denoise,…}-x2"); only whole names are
                // checkable, and a documented prefix still has to resolve.
                let whole = name.starts_with(char::is_alphabetic)
                    && !name.contains(['{', ' ', '/', ',']);
                whole.then_some(name)
            })
            .collect();
        documented.sort_unstable();
        documented.dedup();
        for name in documented {
            assert!(
                CATALOG.iter().any(|c| c.starts_with(name)),
                "models/README.md documents {name} but model_catalog::CATALOG has no such model"
            );
        }
    }
}
