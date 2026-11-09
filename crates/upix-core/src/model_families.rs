//! model_families — Rust port of `legacy/share/core/model_families.hpp`:
//! single source of truth for model family grouping.
//!
//! Rules:
//!   1. Known families: members are the exact prefix name or `prefix-*`;
//!      a lone member gains nothing from a header, so it is emitted ungrouped.
//!   2. Everything else groups by its stem (name minus a trailing `-xN…`),
//!      in first-appearance order after an alphabetical sort; a one-member
//!      stem stays ungrouped for the same reason.
//!   3. Short names drop the family/stem prefix and one leading dash.

pub const KNOWN_FAMILIES: [(&str, &str); 4] = [
    ("Real-ESRGANv3", "Real-ESRGAN v3"),
    ("Real-ESRGAN-x4plus", "Real-ESRGAN x4plus"),
    ("Real-CUGAN-pro", "Real-CUGAN pro"),
    ("Real-CUGAN-se", "Real-CUGAN se"),
];

/// One grouped entry: the original model name and its short display name.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FamilyItem {
    pub name: String,
    pub short_name: String,
}

/// A header (label empty = ungrouped run of rows) and its items.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FamilyGroup {
    pub label: String,
    pub items: Vec<FamilyItem>,
}

/// Stem = name minus a case-insensitive trailing `-x<N>…` ("NomosUni-SPAN-x4"
/// -> "NomosUni-SPAN"); names without one are their own stem.
pub fn model_stem(name: &str) -> &str {
    let bytes = name.as_bytes();
    for i in 0..bytes.len().saturating_sub(2) {
        let at_x = bytes[i] == b'-'
            && bytes[i + 1].eq_ignore_ascii_case(&b'x')
            && bytes[i + 2].is_ascii_digit();
        if at_x {
            return &name[..i];
        }
    }
    name
}

/// Compute the short display name of a model (strips known family or stem prefix).
pub fn short_model_name(name: &str) -> String {
    for (prefix, _) in KNOWN_FAMILIES {
        if let Some(rest) = name.strip_prefix(prefix).and_then(|r| r.strip_prefix('-'))
            && !rest.is_empty()
        {
            return rest.to_string();
        }
    }
    name.to_string()
}

pub fn groups<S: AsRef<str>>(names: &[S]) -> Vec<FamilyGroup> {
    let mut sorted: Vec<String> = names.iter().map(|s| s.as_ref().to_string()).collect();
    sorted.sort();

    let mut out = Vec::new();
    let mut used = vec![false; sorted.len()];

    for (prefix, label) in KNOWN_FAMILIES {
        let mut members = Vec::new();
        for (i, n) in sorted.iter().enumerate() {
            if n == prefix
                || (n.starts_with(prefix)
                    && n.len() > prefix.len()
                    && n.as_bytes()[prefix.len()] == b'-')
            {
                members.push(i);
            }
        }
        if members.is_empty() {
            continue;
        }
        for &i in &members {
            used[i] = true;
        }
        if members.len() == 1 {
            let n = &sorted[members[0]];
            out.push(FamilyGroup {
                label: String::new(),
                items: vec![FamilyItem {
                    name: n.clone(),
                    short_name: n.clone(),
                }],
            });
            continue;
        }
        let items = members
            .into_iter()
            .map(|i| {
                let n = &sorted[i];
                let short_name = if n == prefix {
                    n.clone()
                } else {
                    n[prefix.len() + 1..].to_string()
                };
                FamilyItem {
                    name: n.clone(),
                    short_name,
                }
            })
            .collect();
        out.push(FamilyGroup {
            label: label.to_string(),
            items,
        });
    }

    // Unknown stems, in first-appearance order over the sorted names.
    let mut stems: Vec<String> = Vec::new();
    let mut by_stem: Vec<Vec<usize>> = Vec::new();
    for (i, n) in sorted.iter().enumerate() {
        if used[i] {
            continue;
        }
        let stem = model_stem(n).to_string();
        if let Some(slot) = stems.iter().position(|s| s == &stem) {
            by_stem[slot].push(i);
        } else {
            stems.push(stem);
            by_stem.push(vec![i]);
        }
    }

    for (stem, indices) in stems.into_iter().zip(by_stem.into_iter()) {
        if indices.len() == 1 {
            let n = &sorted[indices[0]];
            out.push(FamilyGroup {
                label: String::new(),
                items: vec![FamilyItem {
                    name: n.clone(),
                    short_name: n.clone(),
                }],
            });
            continue;
        }
        let items = indices
            .into_iter()
            .map(|i| {
                let n = &sorted[i];
                let mut short_name = if n == &stem {
                    n.clone()
                } else {
                    n[stem.len()..].to_string()
                };
                if let Some(stripped) = short_name.strip_prefix('-') {
                    short_name = stripped.to_string();
                }
                FamilyItem {
                    name: n.clone(),
                    short_name,
                }
            })
            .collect();
        out.push(FamilyGroup { label: stem, items });
    }

    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn groups_known_and_unknown_families() {
        let input = vec![
            "Real-ESRGANv3-anime-x4",
            "Real-ESRGANv3-anime-x2",
            "Real-CUGAN-pro-conservative-x2",
            "NomosUni-SPAN-x2",
            "NomosUni-SPAN-x4",
            "AnimeSharp-x4",
        ];
        let g = groups(&input);
        assert_eq!(g.len(), 4);
        assert_eq!(g[0].label, "Real-ESRGAN v3");
        assert_eq!(g[0].items.len(), 2);
        assert_eq!(g[0].items[0].short_name, "anime-x2");
        assert_eq!(g[0].items[1].short_name, "anime-x4");
        // Lone Real-CUGAN-pro is emitted ungrouped
        assert_eq!(g[1].label, "");
        assert_eq!(g[1].items[0].name, "Real-CUGAN-pro-conservative-x2");
        // Lone AnimeSharp-x4 is ungrouped
        assert_eq!(g[2].label, "");
        assert_eq!(g[2].items[0].name, "AnimeSharp-x4");
        // Two NomosUni-SPAN-* group by stem
        assert_eq!(g[3].label, "NomosUni-SPAN");
        assert_eq!(g[3].items[0].short_name, "x2");
        assert_eq!(g[3].items[1].short_name, "x4");
    }
}
