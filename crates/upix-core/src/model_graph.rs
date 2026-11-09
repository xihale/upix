//! model_graph — Rust port of `engine/core/model_graph.{hpp,cpp}`.
//!
//! Pure `.param` file analysis: filename xN convention, and the graph-derived
//! scale via the upsampler ladder (Interp product; image-facing deconv
//! stride; PixelShuffle trunk — trunk-deconv or pure backward BFS; divided by
//! a head-Reorg stride). No net load, no GPU.

use std::collections::{HashMap, HashSet};
use std::path::Path;

/// The xN scale convention shared by every surface that names a model
/// ("Real-ESRGANv3-anime-x2", nested "x2.param" stems, "Real-ESRGAN-x4plus"):
/// the LAST "x<digits>" run whose value lands in 1..8 wins; None = no scale
/// is encoded.
pub fn scale_from_filename(name: &str) -> Option<i32> {
    let bytes = name.as_bytes();
    let mut from_name = 0;
    for i in 0..bytes.len().saturating_sub(1) {
        if bytes[i] != b'x' && bytes[i] != b'X' {
            continue;
        }
        if !bytes[i + 1].is_ascii_digit() {
            continue;
        }
        let mut n = 0i32;
        let mut j = i + 1;
        // Stop accumulating past the accepted range: an unbounded digit run
        // ("x99999999999") would overflow `n` and could wrap into a value the
        // 1..8 filter accepts.
        while j < bytes.len() && n <= 8 && bytes[j].is_ascii_digit() {
            n = n * 10 + (bytes[j] - b'0') as i32;
            j += 1;
        }
        if (1..=8).contains(&n) {
            from_name = n;
        }
    }
    if from_name >= 1 {
        Some(from_name)
    } else {
        None
    }
}

/// Largest net scale accepted from the param graph. Every caller multiplies
/// image dimensions by the scale in `i32`, so a value outside 1..this bound is
/// reported as "not detected" (scale_detected = false) and the
/// filename-derived convention is used instead.
pub const MAX_GRAPH_SCALE_FACTOR: i32 = 16;

// Upper bound for a head Reorg (space-to-depth) stride read out of the param
// file: real values are 2/4, and the bound keeps stride² free of overflow.
const MAX_REORG_STRIDE: i32 = 16;

// Upper bound for the blob counts a param line declares. The counts are text
// from the file; sizing a vector from a claimed 2000000000 blobs would blow
// up this optional-returning parser.
const MAX_BLOB_COUNT: i32 = 1024;

/// What the param graph alone says about a model — derived from the layer
/// walk, never from the filename.
#[derive(Debug, Clone, PartialEq)]
pub struct ModelGraphInfo {
    /// graph-derived scale (default 2 when the graph carries no recognized
    /// upsampler)
    pub scale_factor: i32,
    /// true only when derived from graph ops
    pub scale_detected: bool,
    pub input_width: i32,
    pub input_height: i32,
    /// 0 = Input layer carries no dims
    pub input_channels: i32,
    /// Crop layers (Real-CUGAN "se" family)
    pub has_border_crop: bool,
}

impl Default for ModelGraphInfo {
    fn default() -> Self {
        Self {
            scale_factor: 2,
            scale_detected: false,
            input_width: 0,
            input_height: 0,
            input_channels: 0,
            has_border_crop: false,
        }
    }
}

/// `std::stoi` prefix semantics: skip leading whitespace, optional sign, then
/// digits; fails (like the C++ throwing) when no digits / value out of i32
/// range. Trailing junk is ignored.
fn stoi_prefix(s: &str) -> Option<i32> {
    let t = s.trim_start();
    let (sign_len, sign) = match t.as_bytes().first() {
        Some(b'+') => (1, 1i64),
        Some(b'-') => (1, -1i64),
        _ => (0, 1i64),
    };
    let digits = &t[sign_len..];
    let end = digits.bytes().position(|b| !b.is_ascii_digit()).unwrap_or(digits.len());
    if end == 0 {
        return None;
    }
    // i64 accumulation with early bail guards the C++ out_of_range throw.
    let mut value: i64 = 0;
    for b in digits[..end].bytes() {
        value = value * 10 + (b - b'0') as i64;
        if value > i32::MAX as i64 + 1 {
            return None;
        }
    }
    value *= sign;
    if value > i32::MAX as i64 || value < i32::MIN as i64 {
        return None;
    }
    Some(value as i32)
}

/// `std::stod` prefix semantics (leading whitespace + partial float; None when
/// no number starts here).
fn stod_prefix(s: &str) -> Option<f64> {
    let t = s.trim_start();
    let bytes = t.as_bytes();
    let mut idx = 0usize;
    if idx < bytes.len() && (bytes[idx] == b'+' || bytes[idx] == b'-') {
        idx += 1;
    }
    let mut seen_digit = false;
    let mut seen_dot = false;
    let mantissa_end;
    loop {
        if idx >= bytes.len() {
            mantissa_end = idx;
            break;
        }
        let b = bytes[idx];
        if b.is_ascii_digit() {
            seen_digit = true;
            idx += 1;
        } else if b == b'.' && !seen_dot {
            seen_dot = true;
            idx += 1;
        } else {
            mantissa_end = idx;
            break;
        }
    }
    if !seen_digit {
        return None;
    }
    // Optional exponent, as std::stod accepts ("1=2.000000e+00").
    let end = mantissa_end;
    if end < bytes.len() && (bytes[end] == b'e' || bytes[end] == b'E') {
        let mut e = end + 1;
        if e < bytes.len() && (bytes[e] == b'+' || bytes[e] == b'-') {
            e += 1;
        }
        let exp_start = e;
        while e < bytes.len() && bytes[e].is_ascii_digit() {
            e += 1;
        }
        if e > exp_start {
            if let Ok(v) = t[..e].parse::<f64>() {
                return Some(v);
            }
        }
    }
    t[..end].parse::<f64>().ok()
}

/// `line.find(key)` + stoi, mirroring the C++ lambda (find-based, so a blob
/// name containing the key text could still match — same lenient behavior).
fn param_i(text: &str, key: &str) -> Option<i32> {
    let pos = text.find(key)?;
    stoi_prefix(&text[pos + key.len()..])
}

fn param_d(text: &str, key: &str, default: f64) -> f64 {
    let pos = match text.find(key) {
        Some(p) => p,
        None => return default,
    };
    stod_prefix(&text[pos + key.len()..]).unwrap_or(default)
}

/// One graph layer line, tokenized: `type name n_bottom n_top [bottoms...] [tops...]`.
struct LayerLine {
    line: String,
    type_: String,
    bottoms: Vec<String>,
    tops: Vec<String>,
}

fn parse_layer_line(line: &str) -> Option<LayerLine> {
    let mut tokens = line.split_whitespace();
    let type_ = tokens.next()?;
    let _name = tokens.next()?;
    let bottom_count: i32 = tokens.next()?.parse().ok()?;
    let top_count: i32 = tokens.next()?.parse().ok()?;
    if !(0..=MAX_BLOB_COUNT).contains(&bottom_count) || !(0..=MAX_BLOB_COUNT).contains(&top_count) {
        return None;
    }
    let mut bottoms = Vec::with_capacity(bottom_count as usize);
    for _ in 0..bottom_count {
        bottoms.push(tokens.next()?.to_string());
    }
    let mut tops = Vec::with_capacity(top_count as usize);
    for _ in 0..top_count {
        tops.push(tokens.next()?.to_string());
    }
    Some(LayerLine { line: line.to_string(), type_: type_.to_string(), bottoms, tops })
}

/// Analyze an ncnn .param file: Input layer dims + net scale via the upsampler
/// ladder. `None` when the file cannot be read.
pub fn detect_from_param(param_path: &Path) -> Option<ModelGraphInfo> {
    let content = std::fs::read_to_string(param_path).ok()?;

    let mut info = ModelGraphInfo::default();

    // Input layer dims: "Input  <name>  <in> <out>  <blob>  0=W 1=H 2=C".
    // Prefix-matched and every parse guarded: a truncated or hand-edited param
    // must leave the dims at their defaults, not blow up this
    // optional-returning API. The leading space in " 0=" keeps a blob name
    // that ends in a digit from being read as a param.
    for line in content.lines() {
        if line.starts_with("Input") {
            if let Some(w) = param_i(line, " 0=") {
                info.input_width = w;
            }
            if let Some(h) = param_i(line, " 1=") {
                info.input_height = h;
            }
            if let Some(c) = param_i(line, " 2=") {
                info.input_channels = c;
            }
        }
    }

    // Infer the net scale factor from Interp layers. SR nets compose
    // upsampling, so the net scale is the product of every Interp layer's
    // per-axis scale (param 1 = height scale, 2 = width scale). Only
    // scale-based resize types (param 0 in {1,3,4,5}) carry a scale;
    // type 2 resizes to fixed output dims instead and is skipped.
    // e.g. realesrgan-x4plus: two 2x Interps -> 4;
    //      Real-ESRGANv3-anime x2: 4x Interp then 0.5x downsample -> 2.
    let mut interp_product = 1.0f64;
    let mut found_interp = false;
    // A space-to-depth Reorg head (pnnx's lowering of torch.pixel_unshuffle,
    // e.g. Real-ESRGAN x2plus) shrinks the net's working resolution by its
    // stride before the upsamplers run; the image-facing scale divides the
    // upsampler product by that stride. Only a Reorg before any upsampler op
    // counts — later Reorgs are internal reshapes.
    let mut head_reorg_stride = 0;
    let mut upsampler_seen = false;
    // Deconvolution-upsampler nets (Real-CUGAN): the final deconv that
    // restores image channels (0=3) carries the net scale in its stride.
    let mut deconv_scale = 0;
    // Real-CUGAN "se" nets carry internal Crop layers trimming the
    // receptive-field border; they need a fixed replicate prepadding so
    // the out == in*scale contract holds (margins from upstream
    // realcugan-ncnn-vulkan: x2=18, x3=14, x4=19).
    // PixelShuffle-trunk bookkeeping (Real-CUGAN x4): trunk deconvs feed a
    // Split; auxiliary deconvs feed residual adds. The net output is the
    // last layer's top blob.
    let mut deconvs: Vec<(String, i32)> = Vec::new(); // (top blob, stride)
    let mut pixelshuffles: Vec<(String, i32)> = Vec::new(); // (top blob, ratio)
    let mut net_output_blob = String::new();
    // bit 1: consumed, bit 2: consumed by Split
    let mut blob_consumers: HashMap<String, u8> = HashMap::new();
    for line in content.lines() {
        if let Some(l) = parse_layer_line(line) {
            net_output_blob = l.tops.last().cloned().unwrap_or(net_output_blob);
            for b in &l.bottoms {
                *blob_consumers.entry(b.clone()).or_insert(0) |= 1;
            }
            if l.type_ == "Split" {
                for b in &l.bottoms {
                    *blob_consumers.entry(b.clone()).or_insert(0) |= 2;
                }
            }
            if l.type_ == "Reorg" {
                let stride = param_i(&l.line, " 0=").unwrap_or(-1);
                let mode = param_i(&l.line, " 1=").unwrap_or(-1);
                if !upsampler_seen && stride > 1 && stride <= MAX_REORG_STRIDE && mode <= 0 {
                    head_reorg_stride = stride;
                }
            } else if l.type_ == "Deconvolution" {
                let stride = param_i(&l.line, " 3=").unwrap_or(-1);
                if (1..=MAX_GRAPH_SCALE_FACTOR).contains(&stride) {
                    let top = l.tops.last().cloned().unwrap_or_default();
                    deconvs.push((top, stride));
                    upsampler_seen = upsampler_seen || stride > 1;
                }
            } else if l.type_ == "PixelShuffle" {
                let ratio = param_i(&l.line, " 0=").unwrap_or(-1);
                if (1..=MAX_GRAPH_SCALE_FACTOR).contains(&ratio) {
                    let top = l.tops.last().cloned().unwrap_or_default();
                    pixelshuffles.push((top, ratio));
                    upsampler_seen = upsampler_seen || ratio > 1;
                }
            }
        }
        if line.starts_with("Interp") {
            if param_d(line, " 0=", 1.0) == 2.0 {
                continue; // fixed output size, not a scale
            }
            // A scale-based Interp always carries 1=<height scale>. Without it
            // the layer is not a scale witness at all: treating it as one (the
            // default 1.0) pinned the net at x1 and suppressed the filename
            // fallback the engine relies on.
            if !line.contains(" 1=") {
                continue;
            }
            let h_scale = param_d(line, " 1=", 1.0);
            if !(h_scale > 0.0) {
                continue;
            }
            found_interp = true;
            interp_product *= h_scale;
            if h_scale > 1.0 {
                upsampler_seen = true;
            }
        }
        if line.starts_with("Crop") {
            info.has_border_crop = true;
        }
        if deconv_scale == 0 && line.starts_with("Deconvolution") {
            if param_i(line, " 0=") == Some(3) {
                // Bounded: an unclamped stride token propagates into every
                // width*scale multiply downstream (int overflow).
                if let Some(stride) = param_i(line, " 3=") {
                    if (1..=MAX_GRAPH_SCALE_FACTOR).contains(&stride) {
                        deconv_scale = stride;
                    }
                }
            }
        }
    }

    // A graph-derived scale is accepted only inside the supported range: every
    // caller multiplies image dimensions by it in `i32`, so a param file whose
    // upsampler arithmetic lands outside 1..MAX_GRAPH_SCALE_FACTOR must be
    // reported as "not detected" (callers then trust the filename convention)
    // instead of exported as a bogus scale.
    let mut interp_scale = 0;
    if found_interp {
        let detected = interp_product + 0.5;
        if (1.0..=MAX_GRAPH_SCALE_FACTOR as f64).contains(&detected) {
            interp_scale = detected as i32;
        }
    }

    let mut ps_trunk_detected = false;
    if interp_scale >= 1 {
        info.scale_factor = interp_scale;
    } else if deconv_scale >= 1 {
        info.scale_factor = deconv_scale;
    } else if !pixelshuffles.is_empty() {
        // PixelShuffle-trunk nets (Real-CUGAN x4): the trunk upsampler is
        // the Deconvolution feeding a Split (auxiliary deconvs feed
        // residual adds), and the RGB merge happens in a trailing
        // PixelShuffle. Net scale = trunk deconv strides x PS ratio.
        let mut trunk = 1i64;
        let mut found_trunk = false;
        for (top, ratio) in &deconvs {
            let c = blob_consumers.get(top).copied().unwrap_or(0);
            if c & 1 != 0 && c & 2 != 0 {
                trunk *= *ratio as i64;
                found_trunk = true;
            }
        }
        if found_trunk {
            for (top, ratio) in &pixelshuffles {
                if *top == net_output_blob {
                    trunk *= *ratio as i64;
                    if !(1..=MAX_GRAPH_SCALE_FACTOR as i64).contains(&trunk) {
                        // Implausible product: report no graph scale rather
                        // than an out-of-range one that overflows callers'
                        // int math.
                        break;
                    }
                    info.scale_factor = trunk as i32;
                    ps_trunk_detected = true;
                    // Real-CUGAN se/pro x4 nets are known to execute to
                    // garbage on the pinned ncnn (both CPU and Vulkan;
                    // upstream ncnn 2022 runs them fine) — x2/x3 from the
                    // same families are unaffected. Warn so a garbage result
                    // from a custom-imported model is explainable.
                    log::warn!(target: "Engine",
                        "PixelShuffle-trunk model (Real-CUGAN x4 family): known broken on this ncnn build; output may be garbage");
                    break;
                }
            }
        } else {
            // Pure PixelShuffle-trunk nets (SPAN family): no Interp and no
            // deconv trunk — the upsampling is a PixelShuffle chain feeding
            // the output, possibly through trailing pointwise ops. Net
            // scale = product of the ratios of the PixelShuffles reachable
            // backwards from the output blob.
            let mut producer_type: HashMap<String, String> = HashMap::new();
            let mut producer_ratio: HashMap<String, i32> = HashMap::new();
            let mut producer_bottoms: HashMap<String, Vec<String>> = HashMap::new();
            for line in content.lines() {
                let Some(l) = parse_layer_line(line) else { continue };
                let mut ratio = 0;
                if l.type_ == "PixelShuffle" {
                    // Bounded here as well as in the first pass: this map
                    // drives the BFS product below, where an unclamped token
                    // would overflow the accumulator.
                    ratio = param_i(&l.line, " 0=").unwrap_or(0);
                    if !(1..=MAX_GRAPH_SCALE_FACTOR).contains(&ratio) {
                        ratio = 0;
                    }
                }
                for t in &l.tops {
                    producer_type.insert(t.clone(), l.type_.clone());
                    producer_ratio.insert(t.clone(), ratio);
                    producer_bottoms.insert(t.clone(), l.bottoms.clone());
                }
            }
            let mut frontier = vec![net_output_blob.clone()];
            let mut seen_blob: HashSet<String> = HashSet::new();
            let mut ps_scale = 1i64;
            let mut any_trunk_ps = false;
            while let Some(blob) = frontier.pop() {
                if !seen_blob.insert(blob.clone()) {
                    continue;
                }
                let Some(ty) = producer_type.get(&blob) else { continue };
                let ratio = producer_ratio.get(&blob).copied().unwrap_or(0);
                if ty == "PixelShuffle" && ratio >= 1 {
                    ps_scale *= ratio as i64;
                    any_trunk_ps = true;
                    if ps_scale > MAX_GRAPH_SCALE_FACTOR as i64 {
                        // Already outside the accepted range: the range check
                        // below rejects it either way, and stopping keeps a
                        // long chain from overflowing the accumulator first.
                        break;
                    }
                }
                if let Some(bottoms) = producer_bottoms.get(&blob) {
                    for b in bottoms {
                        frontier.push(b.clone());
                    }
                }
            }
            if any_trunk_ps && (1..=16).contains(&ps_scale) {
                info.scale_factor = ps_scale as i32;
                ps_trunk_detected = true;
            }
        }
    }
    if head_reorg_stride > 1 && info.scale_factor % head_reorg_stride == 0 {
        info.scale_factor /= head_reorg_stride;
    }
    info.scale_detected = interp_scale >= 1 || deconv_scale >= 1 || ps_trunk_detected;

    Some(info)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    fn write_param(name: &str, body: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("upix-core-graph-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join(name);
        std::fs::write(&path, body).unwrap();
        path
    }

    #[test]
    fn filename_scale_convention() {
        assert_eq!(scale_from_filename("Real-ESRGANv3-anime-x2"), Some(2));
        assert_eq!(scale_from_filename("Real-ESRGAN-x4plus"), Some(4));
        assert_eq!(scale_from_filename("x4"), Some(4));
        assert_eq!(scale_from_filename("X3net"), Some(3));
        // Last valid run wins.
        assert_eq!(scale_from_filename("x2-then-x4"), Some(4));
        assert_eq!(scale_from_filename("net-x2plus-x1"), Some(1));
        // Out of range → not a scale witness.
        assert_eq!(scale_from_filename("x9"), None);
        assert_eq!(scale_from_filename("x16"), None);
        assert_eq!(scale_from_filename("x99999999999"), None);
        assert_eq!(scale_from_filename("plain"), None);
        assert_eq!(scale_from_filename("x"), None);
    }

    #[test]
    fn interp_product_scale() {
        // realesrgan-x4plus style: two 2x Interps.
        let p = write_param("interp4.param", concat!(
            "776 1\n",
            "Input data 0 1 data 0=64 1=64 2=3\n",
            "Convolution conv1 1 1 data out1 0=64 1=3 11=3 6=1728 7=1\n",
            "Interp up1 1 1 out1 up1out 0=1 1=2.000000e+00 2=2.000000e+00\n",
            "Interp up2 1 1 up1out up2out 0=1 1=2.000000e+00 2=2.000000e+00\n",
            "Convolution convout 1 1 up2out out 0=3 1=3 11=3 6=81 7=1\n"));
        let info = detect_from_param(&p).unwrap();
        assert_eq!(info.scale_factor, 4);
        assert!(info.scale_detected);
        assert_eq!((info.input_width, info.input_height, info.input_channels), (64, 64, 3));
    }

    #[test]
    fn interp_with_downsample_step() {
        // Real-ESRGANv3-anime-x2 style: 4x Interp then 0.5x downsample → 2.
        let p = write_param("interp_down.param", concat!(
            "10 1\n",
            "Input data 0 1 data 0=16 1=16 2=3\n",
            "Interp up 1 1 data upout 0=1 1=4.000000e+00 2=4.000000e+00\n",
            "Interp down 1 1 upout downout 0=1 1=5.000000e-01 2=5.000000e-01\n",
            "Convolution out 1 1 downout final 0=3 1=3 11=3 6=81 7=1\n"));
        let info = detect_from_param(&p).unwrap();
        assert_eq!(info.scale_factor, 2);
        assert!(info.scale_detected);
    }

    #[test]
    fn interp_without_scale_param_is_no_witness() {
        // No " 1=" on the Interp → not a witness; no other upsampler →
        // default 2, not detected.
        let p = write_param("interp_noscale.param", concat!(
            "10 1\n",
            "Input data 0 1 data\n",
            "Interp up 1 1 data out 0=2\n"));
        let info = detect_from_param(&p).unwrap();
        assert_eq!(info.scale_factor, 2);
        assert!(!info.scale_detected);
    }

    #[test]
    fn fixed_size_interp_is_skipped() {
        let p = write_param("interp_fixed.param", concat!(
            "10 1\n",
            "Input data 0 1 data\n",
            "Interp up 1 1 data out 0=2 1=2.000000e+00 2=2.000000e+00\n"));
        let info = detect_from_param(&p).unwrap();
        assert_eq!(info.scale_factor, 2);
        assert!(!info.scale_detected);
    }

    #[test]
    fn deconv_image_facing_scale() {
        // Real-CUGAN style: final deconv with 0=3 (image channels), stride 2.
        let p = write_param("deconv.param", concat!(
            "10 1\n",
            "Input data 0 1 data\n",
            "Convolution c1 1 1 data c1out 0=16 1=3 11=3 6=432 7=1\n",
            "Deconvolution up 1 1 c1out upout 0=3 1=4 11=4 3=2 6=768 7=1\n"));
        let info = detect_from_param(&p).unwrap();
        assert_eq!(info.scale_factor, 2);
        assert!(info.scale_detected);
    }

    #[test]
    fn pure_pixelshuffle_trunk_bfs() {
        // SPAN family: PS(4) chain feeding the output blob.
        let p = write_param("span.param", concat!(
            "10 1\n",
            "Input data 0 1 data\n",
            "Convolution body 1 1 data bodyout 0=64 1=3 11=3 6=1728 7=1\n",
            "Convolution psmain 1 1 bodyout psinput 0=48 1=3 11=3 6=82944 7=1\n",
            "PixelShuffle ps 1 1 psinput psout 0=4\n",
            "Convolution tail 1 1 psout imgout 0=3 1=3 11=3 6=81 7=1\n"));
        let info = detect_from_param(&p).unwrap();
        assert_eq!(info.scale_factor, 4);
        assert!(info.scale_detected);
    }

    #[test]
    fn head_reorg_divides_scale() {
        // Real-ESRGAN x2plus style: pixel_unshuffle head (Reorg stride 2) +
        // Interp 2x → image-facing scale 1? No: 2/2 = 1... but the convention
        // here matches the C++ arithmetic exactly.
        let p = write_param("reorg.param", concat!(
            "10 1\n",
            "Input data 0 1 data\n",
            "Reorg shuf 1 1 data shufout 0=2 1=0\n",
            "Convolution c1 1 1 shufout c1out 0=12 1=3 11=3 6=324 7=1\n",
            "Interp up 1 1 c1out upout 0=1 1=2.000000e+00 2=2.000000e+00\n",
            "Convolution out 1 1 upout final 0=3 1=3 11=3 6=81 7=1\n"));
        let info = detect_from_param(&p).unwrap();
        assert_eq!(info.scale_factor, 1);
        assert!(info.scale_detected);
    }

    #[test]
    fn crop_flag_and_missing_file() {
        let p = write_param("crop.param", concat!(
            "10 1\n",
            "Input data 0 1 data\n",
            "Crop trim 1 1 data trimout \n"));
        let info = detect_from_param(&p).unwrap();
        assert!(info.has_border_crop);

        assert!(detect_from_param(Path::new("/nonexistent/nope.param")).is_none());
    }

    #[test]
    fn hostile_blob_counts_are_rejected() {
        let p = write_param("hostile.param", concat!(
            "10 1\n",
            "Split boom 1 2000000000 data out\n"));
        // Must not allocate/panic; defaults returned.
        let info = detect_from_param(&p).unwrap();
        assert_eq!(info.scale_factor, 2);
    }
}
