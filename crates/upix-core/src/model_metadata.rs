//! model_metadata — Rust port of `engine/core/model_metadata.{hpp,cpp}`:
//! curated scene labels for the model catalog.

/// Scene label + recommendation flag for a catalog model. Empty scene =
/// not a catalog model (user import) → no badge anywhere.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ModelSceneInfo {
    pub scene: String,
    pub recommended: bool,
}

/// Matching rules (verbatim from the C++):
///   - Patterns are lowercase; the name is normalized the same way
///     (lowercase, '_' and ' ' → '-'), so "Real-ESRGANv3-anime_x2" and
///     "animesharp x4" both resolve.
///   - Substring match, table ordered most-specific first: "real-cugan" is
///     checked before the 1x "denoise" nets so Real-CUGAN's -denoise variants
///     keep their anime label instead of the photo-restoration one, and
///     "real-esrgan-x4plus-anime" precedes "real-esrgan-x4plus".
///   - `recommended` mirrors the picks models/README.md calls out as first
///     choices ("the default recommendation in both apps", "best photo
///     default on the phone", "first pick for PC-offloaded photo jobs",
///     "the community's default all-rounder", "recommended starting point").
const SCENE_RULES: [(&str, &str, bool); 28] = [
    // Real-ESRGAN v3 compacts — the default recommendation in both apps.
    ("real-esrganv3-anime", "动漫", true),
    ("real-esrganv3-general", "照片", true),
    // x4plus family; the anime variant must be tested before the photo base.
    ("real-esrgan-x4plus-anime", "动漫", false),
    ("real-esrgan-x4plus", "照片", true),
    ("real-esrgan-x2plus", "照片", false),
    // Real-CUGAN — anime specialist (before the 1x "-denoise" restores).
    ("real-cugan", "动漫", false),
    // Phhofm photo compacts / 4x nets.
    ("nomosuni-compact", "照片", false),
    ("parimg-compact", "照片", false),
    ("nomosuni-span", "照片", false),
    ("lsdir-compact", "照片", false),
    ("nomos2-otf", "照片", false),
    ("nomos8ksc", "照片", false),
    ("nomoswebphoto", "照片", false),
    // 1x restoration nets — clean in place, no resize (scale already says x1).
    ("dejpg", "照片·修复", false),
    ("denoise", "照片·修复", false),
    ("deh264", "视频·修复", false),
    ("archivist-medium", "动漫·修复", true),
    ("archivist-rough", "动漫·修复", false),
    ("nes-composite", "游戏·修复", false),
    ("genesis-cleanup", "游戏·修复", false),
    // Community anime restoration ladder (both HFA2k variants in one rule).
    ("hfa2k", "动漫", false),
    ("modernspanimation", "动漫", false),
    // Community classics.
    ("ultrasharp", "综合", true),
    ("remacri", "综合", true),
    ("animesharp", "动漫", true),
    ("animejanai", "动漫", false),
    ("nmkd-siax", "综合", false), // photos & textures
    ("bsrgan", "照片", false),
];

/// Classify a model name into its scene badge. Unknown → empty info.
pub fn classify_model_scene(name: &str) -> ModelSceneInfo {
    let key = normalize_name(name);
    for (pattern, scene, recommended) in SCENE_RULES {
        if key.contains(pattern) {
            return ModelSceneInfo { scene: scene.to_string(), recommended };
        }
    }
    ModelSceneInfo::default()
}

/// Lowercase, '_' and ' ' → '-' (ASCII only, matching the C++).
pub(crate) fn normalize_name(name: &str) -> String {
    let mut key = String::with_capacity(name.len());
    for c in name.chars() {
        if c == '_' || c == ' ' {
            key.push('-');
        } else {
            key.push(c.to_ascii_lowercase());
        }
    }
    key
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn catalog_names_resolve_to_labels() {
        assert_eq!(classify_model_scene("Real-ESRGANv3-anime-x2"),
            ModelSceneInfo { scene: "动漫".into(), recommended: true });
        assert_eq!(classify_model_scene("real-esrganv3-general-x4"),
            ModelSceneInfo { scene: "照片".into(), recommended: true });
    }

    #[test]
    fn most_specific_rule_wins() {
        // anime variant before the photo base
        assert_eq!(classify_model_scene("real-esrgan-x4plus-anime").scene, "动漫");
        assert_eq!(classify_model_scene("real-esrgan-x4plus").scene, "照片");
        // real-cugan before the 1x "denoise" rule
        assert_eq!(classify_model_scene("real-cugan-denoise-x2").scene, "动漫");
    }

    #[test]
    fn separators_and_case_are_normalized() {
        assert_eq!(classify_model_scene("Animesharp x4").scene, "动漫");
        assert_eq!(classify_model_scene("Real_ESRGANv3_anime_x2").scene, "动漫");
    }

    #[test]
    fn unknown_names_get_no_badge() {
        assert_eq!(classify_model_scene("totally-custom-net"), ModelSceneInfo::default());
        assert_eq!(classify_model_scene(""), ModelSceneInfo::default());
    }
}
