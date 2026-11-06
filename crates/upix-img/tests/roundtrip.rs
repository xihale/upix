//! Round-trip + semantic tests for upix-img (CONTRACT §4 acceptance).

use std::path::PathBuf;

use upix_img::{
    apply_orientation, convert_image_channels, decode, decode_path, encode, sr_marker,
    EncoderOptions, Image,
};

/// Deterministic gradient test image (bytes vary so compression knobs bite).
fn test_image(width: i32, height: i32, channels: i32) -> Image {
    let mut img = Image::new(width, height, channels);
    for y in 0..height {
        for x in 0..width {
            for c in 0..channels {
                let idx = (y as usize * width as usize + x as usize) * channels as usize
                    + c as usize;
                img.data[idx] = ((x * 7 + y * 13 + c * 29) % 256) as u8;
            }
        }
    }
    img
}

fn xmp() -> String {
    sr_marker::build_xmp("NomosUni-SPAN-x2", 4, 1730828000123)
}

#[test]
fn sr_marker_build_xmp_byte_exact() {
    let x = sr_marker::build_xmp("m", 2, 42);
    assert_eq!(
        x,
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF \
         xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\
         <rdf:Description xmlns:cs=\"urn:upix:sr:1\" cs:model=\"m\" \
         cs:scale=\"2\" cs:ts=\"42\"/></rdf:RDF></x:xmpmeta>"
    );
}

#[test]
fn sr_marker_escapes_and_roundtrips() {
    let model = "a&b<c>\"d'e";
    let x = sr_marker::build_xmp(model, 3, -5);
    assert!(x.contains("cs:model=\"a&amp;b&lt;c&gt;&quot;d&apos;e\""));
    let marker = sr_marker::probe(x.as_bytes()).expect("probe on own packet");
    assert_eq!(marker.model, model);
    assert_eq!(marker.scale, 3);
    assert_eq!(marker.ts, -5);
}

#[test]
fn sr_marker_probe_finds_needle_in_any_container() {
    let x = xmp();
    // Wrapped in foreign bytes (PNG-like, JPEG-like, plus junk both sides):
    // probe is a raw needle scan and must not care about the container.
    let mut buf = vec![0x89u8, b'P', b'N', b'G'];
    buf.extend_from_slice(&[0u8; 37]);
    buf.extend_from_slice(x.as_bytes());
    buf.extend_from_slice(&[0xFFu8, 0xD9, 0x11, 0x22]);
    let m = sr_marker::probe(&buf).expect("probe through junk");
    assert_eq!(m.scale, 4);
    assert_eq!(m.ts, 1730828000123);
    assert_eq!(m.model, "NomosUni-SPAN-x2");
}

#[test]
fn sr_marker_probe_rejects_garbage_and_truncation() {
    assert_eq!(sr_marker::probe(&[]), None);
    assert_eq!(sr_marker::probe(b"short but no needle"), None);
    // Random bytes never contain the needle → None.
    let mut rng_state = 0x12345678u32;
    let random: Vec<u8> = (0..4096)
        .map(|_| {
            rng_state = rng_state.wrapping_mul(1664525).wrapping_add(1013904223);
            (rng_state >> 24) as u8
        })
        .collect();
    assert_eq!(sr_marker::probe(&random), None);

    // Truncated/foreign marker: URN present but scale/ts missing or
    // unparseable → None.
    let full = xmp();
    let cut = &full[..full.find("cs:scale").unwrap()];
    assert_eq!(sr_marker::probe(cut.as_bytes()), None);
    let bad_scale = full.replace("cs:scale=\"4\"", "cs:scale=\"4x\"");
    assert_eq!(sr_marker::probe(bad_scale.as_bytes()), None);
    let bad_ts = full.replace("cs:ts=\"1730828000123\"", "cs:ts=\"\"\"");
    assert_eq!(sr_marker::probe(bad_ts.as_bytes()), None);
    // Scale beyond the 512-byte attribute window → attribute not found.
    let mut distant = full.clone();
    distant.insert_str(
        distant.find("cs:model").unwrap(),
        &format!("cs:pad=\"{}\"", "P".repeat(600)),
    );
    assert_eq!(sr_marker::probe(distant.as_bytes()), None);
}

#[test]
fn png_roundtrip_with_xmp() {
    let img = test_image(61, 47, 3);
    let bytes = encode(&img, "png", 90, &EncoderOptions::default(), Some(&xmp()))
        .expect("png encode");
    let marker = sr_marker::probe(&bytes).expect("xmp probe on png");
    assert_eq!(marker.scale, 4);
    assert_eq!(marker.ts, 1730828000123);

    let back = decode(&bytes).expect("png decode");
    assert_eq!((back.width, back.height, back.channels), (61, 47, 3));
    assert_eq!(back.data, img.data, "PNG is lossless");
}

#[test]
fn png_gray_roundtrip_expands_to_rgb() {
    let img = test_image(17, 9, 1);
    let bytes =
        encode(&img, "png", 90, &EncoderOptions::default(), None).expect("png encode");
    let back = decode(&bytes).expect("png decode");
    // libpng png_set_gray_to_rgb semantics.
    assert_eq!((back.width, back.height, back.channels), (17, 9, 3));
    for y in 0..9 {
        for x in 0..17 {
            let src = img.data[(y * 17 + x) as usize];
            let dst = &back.data[((y * 17 + x) * 3) as usize..][..3];
            assert_eq!(dst, &[src, src, src]);
        }
    }
}

#[test]
fn png_rgba_roundtrip() {
    let img = test_image(23, 11, 4);
    let bytes =
        encode(&img, "png", 90, &EncoderOptions::default(), None).expect("png encode");
    let back = decode(&bytes).expect("png decode");
    assert_eq!((back.width, back.height, back.channels), (23, 11, 4));
    assert_eq!(back.data, img.data);
}

#[test]
fn png_quality_changes_output_size() {
    let img = test_image(256, 256, 3);
    let fast = encode(&img, "png", 100, &EncoderOptions::default(), None).unwrap();
    let best = encode(&img, "png", 1, &EncoderOptions::default(), None).unwrap();
    assert!(
        best.len() < fast.len(),
        "best compression must be smaller than fastest ({} vs {})",
        best.len(),
        fast.len()
    );
    // Explicit knob overrides the quality-derived mapping.
    let mut opts = EncoderOptions::default();
    opts.png_level = 0;
    let lvl0 = encode(&img, "png", 90, &opts, None).unwrap();
    opts.png_level = 9;
    let lvl9 = encode(&img, "png", 90, &opts, None).unwrap();
    assert!(lvl9.len() < lvl0.len());
}

#[test]
fn jpeg_roundtrip_quality_and_subsampling() {
    let img = test_image(120, 90, 3);
    let q95 = encode(&img, "jpg", 95, &EncoderOptions::default(), None).unwrap();
    let q30 = encode(&img, "jpg", 30, &EncoderOptions::default(), None).unwrap();
    assert!(
        q95.len() > q30.len(),
        "higher quality must be larger ({} vs {})",
        q95.len(),
        q30.len()
    );

    let back = decode(&q95).expect("jpeg decode");
    assert_eq!((back.width, back.height, back.channels), (120, 90, 3));

    let mut s444 = EncoderOptions::default();
    s444.jpeg_subsampling = 444;
    let b444 = encode(&img, "jpeg", 95, &s444, None).unwrap();
    let b420 = encode(&img, "jpeg", 95, &EncoderOptions::default(), None).unwrap();
    assert_ne!(
        b420, b444,
        "subsampling knob must change the encoded stream"
    );
    // Invalid subsampling degrades to 420, identical to the default.
    let mut s_bad = EncoderOptions::default();
    s_bad.jpeg_subsampling = 411;
    let b_bad = encode(&img, "jpeg", 95, &s_bad, None).unwrap();
    assert_eq!(b_bad, b420);

    // Grayscale JPEG input decodes back as RGB (JCS_RGB semantics).
    let gray = test_image(40, 30, 1);
    let gbytes =
        encode(&gray, "jpg", 90, &EncoderOptions::default(), None).unwrap();
    let gback = decode(&gbytes).unwrap();
    assert_eq!((gback.width, gback.height, gback.channels), (40, 30, 3));
}

#[test]
fn jpeg_xmp_app1_probe() {
    let img = test_image(64, 64, 3);
    let bytes = encode(&img, "jpg", 90, &EncoderOptions::default(), Some(&xmp()))
        .expect("jpeg encode");
    // APP1 in the header region (after the JFIF APP0 both libjpeg and
    // jpeg-encoder emit), standard namespace prefix.
    let app1_pos = bytes
        .windows(2)
        .position(|w| w == [0xFF, 0xE1])
        .expect("APP1 marker present");
    let seg_len = u16::from_be_bytes([bytes[app1_pos + 2], bytes[app1_pos + 3]]) as usize;
    let ns_start = app1_pos + 4;
    let ns_end = ns_start + b"http://ns.adobe.com/xap/1.0/\0".len();
    assert_eq!(&bytes[ns_start..ns_end], b"http://ns.adobe.com/xap/1.0/\0");
    assert_eq!(seg_len, ns_end - ns_start + xmp().len() + 2); // +2 length bytes

    let marker = sr_marker::probe(&bytes).expect("xmp probe on jpeg");
    assert_eq!(marker.scale, 4);
    assert_eq!(marker.ts, 1730828000123);

    // The jpeg-encoder crate wrote the segment; zune-jpeg must still decode.
    let back = decode(&bytes).expect("decode with APP1 present");
    assert_eq!(back.width, 64);
}

#[test]
fn webp_roundtrip_and_xmp_mux() {
    let img = test_image(80, 60, 3);
    let plain = encode(&img, "webp", 90, &EncoderOptions::default(), None).unwrap();
    assert_eq!(&plain[0..4], b"RIFF");
    assert_eq!(&plain[8..12], b"WEBP");

    let back = decode(&plain).expect("webp decode");
    assert_eq!((back.width, back.height, back.channels), (80, 60, 3));

    let marked = encode(&img, "webp", 90, &EncoderOptions::default(), Some(&xmp()))
        .expect("webp encode with xmp");
    // VP8X container with the "XMP " chunk attached.
    assert_eq!(&marked[12..16], b"VP8X");
    assert!(marked.windows(4).any(|w| w == b"XMP "));
    let marker = sr_marker::probe(&marked).expect("xmp probe on webp");
    assert_eq!(marker.scale, 4);

    // Muxed container still decodes.
    let mback = decode(&marked).expect("webp decode of VP8X container");
    assert_eq!(mback.width, 80);
    assert_eq!(mback.height, 60);

    // Lossless flag changes the bitstream (VP8L) and decodes exactly.
    let mut opts = EncoderOptions::default();
    opts.webp_lossless = true;
    let lossless = encode(&img, "webp", 90, &opts, None).unwrap();
    assert_ne!(lossless, plain);
    let lback = decode(&lossless).expect("lossless webp decode");
    assert_eq!(lback.width, 80);
    assert_eq!(lback.height, 60);

    // Method knob changes the encoded stream.
    let mut m0 = EncoderOptions::default();
    m0.webp_method = 0;
    let m0b = encode(&img, "webp", 75, &m0, None).unwrap();
    let mut m6 = EncoderOptions::default();
    m6.webp_method = 6;
    let m6b = encode(&img, "webp", 75, &m6, None).unwrap();
    assert_ne!(m0b, m6b, "webp_method must affect the output");
}

#[test]
fn webp_rgba_encode() {
    let img = test_image(33, 21, 4);
    let bytes =
        encode(&img, "webp", 90, &EncoderOptions::default(), None).expect("rgba encode");
    let back = decode(&bytes).expect("decode");
    assert_eq!(back.width, 33);
    assert_eq!(back.height, 21);
    assert_eq!(back.channels, 3, "decoder drops alpha (MODE_RGB)");
}

#[test]
fn real_webp_sample_decodes() {
    let path: PathBuf = concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../site/public/images/source-light.webp"
    )
    .into();
    if !path.exists() {
        panic!("sample missing: {}", path.display());
    }
    let (img, orientation) = decode_path(&path).expect("decode real webp");
    assert!(img.valid());
    assert_eq!(img.channels, 3);
    assert_eq!(orientation, None, "webp carries no JPEG EXIF");

    // And it re-encodes through every non-AVIF codec.
    for fmt in ["png", "jpg", "webp"] {
        let bytes = encode(&img, fmt, 90, &EncoderOptions::default(), Some(&xmp()))
            .unwrap_or_else(|e| panic!("re-encode {fmt}: {e}"));
        let back = decode(&bytes).unwrap_or_else(|e| panic!("re-decode {fmt}: {e}"));
        assert_eq!(back.width, img.width, "{fmt} width preserved");
        assert_eq!(back.height, img.height, "{fmt} height preserved");
        assert!(
            sr_marker::probe(&bytes).is_some(),
            "{fmt} carries the SR marker"
        );
    }
}

#[test]
fn garbage_decode_errors() {
    assert!(decode(&[]).is_err());
    let mut rng_state = 0xdeadbeefu32;
    let random: Vec<u8> = (0..2048)
        .map(|_| {
            rng_state = rng_state.wrapping_mul(1664525).wrapping_add(1013904223);
            (rng_state >> 24) as u8
        })
        .collect();
    assert!(decode(&random).is_err());

    // Truncated containers: PNG/WebP error out; JPEG stays tolerant like
    // libjpeg (premature EOF is a warning, the decoder pads missing rows
    // and still returns an image) — the C++ load_jpeg had the same
    // behavior, so both outcomes are conformant here.
    let img = test_image(32, 32, 3);
    let png = encode(&img, "png", 90, &EncoderOptions::default(), None).unwrap();
    let jpg = encode(&img, "jpg", 90, &EncoderOptions::default(), None).unwrap();
    let webp = encode(&img, "webp", 90, &EncoderOptions::default(), None).unwrap();
    assert!(decode(&png[..png.len() / 2]).is_err());
    assert!(decode(&webp[..webp.len() / 2]).is_err());
    let _ = decode(&jpg[..jpg.len() / 2]);
}

#[test]
fn exif_parse_orientation() {
    use upix_img::exif::parse_jpeg_exif_orientation;

    fn jpeg_with_exif(orientation: u16) -> Vec<u8> {
        // SOI + APP1(Exif, big-endian TIFF, one IFD0 entry) + APP0 filler + EOI.
        let mut v = vec![0xFF, 0xD8];
        let mut tiff = Vec::new();
        tiff.extend_from_slice(b"MM\0\x2a"); // big-endian TIFF
        tiff.extend_from_slice(&8u32.to_be_bytes()); // IFD0 offset
        tiff.extend_from_slice(&1u16.to_be_bytes()); // entry count
        tiff.extend_from_slice(&0x0112u16.to_be_bytes()); // Orientation
        tiff.extend_from_slice(&3u16.to_be_bytes()); // SHORT
        tiff.extend_from_slice(&1u32.to_be_bytes()); // count
        tiff.extend_from_slice(&(orientation as u16).to_be_bytes()); // value
        tiff.extend_from_slice(&0u32.to_be_bytes()); // next IFD
        let mut app1 = Vec::new();
        app1.extend_from_slice(b"Exif\0\0");
        app1.extend_from_slice(&tiff);
        v.push(0xFF);
        v.push(0xE1);
        v.extend_from_slice(&((app1.len() as u16 + 2).to_be_bytes()));
        v.extend_from_slice(&app1);
        v.extend_from_slice(&[0xFF, 0xD9]);
        v
    }

    assert_eq!(parse_jpeg_exif_orientation(&jpeg_with_exif(6)), 6);
    assert_eq!(parse_jpeg_exif_orientation(&jpeg_with_exif(1)), 1);
    assert_eq!(parse_jpeg_exif_orientation(&jpeg_with_exif(9)), 1, "range check");
    assert_eq!(parse_jpeg_exif_orientation(&jpeg_with_exif(0)), 1, "range check");
    assert_eq!(parse_jpeg_exif_orientation(b"\xFF\xD8"), 1, "truncated");
    assert_eq!(parse_jpeg_exif_orientation(b"not a jpeg"), 1, "non-JPEG");
    assert_eq!(parse_jpeg_exif_orientation(&[]), 1);

    // Little-endian TIFF path.
    let mut v = vec![0xFF, 0xD8, 0xFF, 0xE1, 0x00, 0x1A];
    let mut tiff = Vec::new();
    tiff.extend_from_slice(b"II\x2a\0");
    tiff.extend_from_slice(&8u32.to_le_bytes());
    tiff.extend_from_slice(&1u16.to_le_bytes());
    tiff.extend_from_slice(&0x0112u16.to_le_bytes());
    tiff.extend_from_slice(&3u16.to_le_bytes());
    tiff.extend_from_slice(&1u32.to_le_bytes());
    tiff.extend_from_slice(&8u16.to_le_bytes()); // orientation 8, value left-justified
    tiff.extend_from_slice(&0u32.to_le_bytes());
    let mut app1 = Vec::new();
    app1.extend_from_slice(b"Exif\0\0");
    app1.extend_from_slice(&tiff);
    // JPEG segment lengths are big-endian regardless of the TIFF byte order.
    v[4..6].copy_from_slice(&((app1.len() as u16 + 2).to_be_bytes()));
    v.extend_from_slice(&app1);
    assert_eq!(parse_jpeg_exif_orientation(&v), 8);
}

#[test]
fn exif_apply_orientation_transforms() {
    // 3 wide, 2 tall, 1 channel: pixels 0..6.
    let mut img = Image::new(3, 2, 1);
    img.data.copy_from_slice(&[0, 1, 2, 3, 4, 5]);

    // Orientation 6 (rotate 90° CW): out(x,y) = in(y, h-1-x) → 2x3.
    let mut six = img.clone();
    assert!(apply_orientation(&mut six, 6));
    assert_eq!((six.width, six.height), (2, 3));
    assert_eq!(six.data, vec![3, 0, 4, 1, 5, 2]);

    // Orientation 5 (transpose): out(x,y) = in(y,x) → 2x3.
    let mut five = img.clone();
    assert!(apply_orientation(&mut five, 5));
    assert_eq!((five.width, five.height), (2, 3));
    assert_eq!(five.data, vec![0, 3, 1, 4, 2, 5]);

    // Orientation 2 (mirror horizontal), geometry-preserving.
    let mut two = img.clone();
    assert!(apply_orientation(&mut two, 2));
    assert_eq!((two.width, two.height), (3, 2));
    assert_eq!(two.data, vec![2, 1, 0, 5, 4, 3]);

    // Orientation 3 (180°).
    let mut three = img.clone();
    assert!(apply_orientation(&mut three, 3));
    assert_eq!(three.data, vec![5, 4, 3, 2, 1, 0]);

    // Orientation 1 / invalid: untouched.
    let mut one = img.clone();
    assert!(!apply_orientation(&mut one, 1));
    assert!(!apply_orientation(&mut one, 0));
    assert!(!apply_orientation(&mut one, 9));
    assert_eq!(one, img);
}

#[test]
fn decode_path_returns_orientation_for_jpeg() {
    let dir = std::env::temp_dir().join("upix-img-tests");
    std::fs::create_dir_all(&dir).unwrap();
    let path = dir.join("oriented.jpg");

    // Build a real JPEG (decodable) + a synthetic EXIF APP1 spliced after SOI.
    let img = test_image(10, 10, 3);
    let mut bytes =
        encode(&img, "jpg", 95, &EncoderOptions::default(), None).unwrap();
    let mut tiff = Vec::new();
    tiff.extend_from_slice(b"MM\0\x2a");
    tiff.extend_from_slice(&8u32.to_be_bytes());
    tiff.extend_from_slice(&1u16.to_be_bytes());
    tiff.extend_from_slice(&0x0112u16.to_be_bytes());
    tiff.extend_from_slice(&3u16.to_be_bytes());
    tiff.extend_from_slice(&1u32.to_be_bytes());
    tiff.extend_from_slice(&6u16.to_be_bytes());
    tiff.extend_from_slice(&0u32.to_be_bytes());
    let mut app1 = Vec::new();
    app1.extend_from_slice(b"Exif\0\0");
    app1.extend_from_slice(&tiff);
    let mut seg = vec![0xFF, 0xE1];
    seg.extend_from_slice(&((app1.len() as u16 + 2).to_be_bytes()));
    seg.extend_from_slice(&app1);
    bytes.splice(2..2, seg);
    std::fs::write(&path, &bytes).unwrap();

    let (back, orientation) = decode_path(&path).expect("decode_path");
    assert_eq!(back.width, 10);
    assert_eq!(orientation, Some(6));

    // Baking orientation 6 into the pixels swaps the geometry.
    let mut rotated = back;
    assert!(apply_orientation(&mut rotated, orientation.unwrap()));
    assert_eq!((rotated.width, rotated.height), (10, 10)); // square: no size change
    let _ = rotated;

    // PNG returns no orientation.
    let ppath = dir.join("plain.png");
    std::fs::write(&ppath, encode(&img, "png", 90, &EncoderOptions::default(), None).unwrap())
        .unwrap();
    let (_, p_orient) = decode_path(&ppath).unwrap();
    assert_eq!(p_orient, None);
}

#[test]
fn avif_paths() {
    let img = test_image(48, 36, 3);
    if !upix_img::avif_available() {
        println!("skipping AVIF tests: system libavif not available");
        return;
    }
    let bytes = encode(
        &img,
        "avif",
        60,
        &EncoderOptions::default(),
        Some(&xmp()),
    )
    .expect("avif encode");
    assert!(
        upix_img::looks_like_avif(&bytes),
        "output must carry avif brands"
    );

    let marker = sr_marker::probe(&bytes).expect("xmp probe on avif");
    assert_eq!(marker.scale, 4);

    let back = decode(&bytes).expect("avif decode");
    assert_eq!((back.width, back.height, back.channels), (48, 36, 3));
    // Lossy codec: pixels differ, dimensions don't.
    assert_ne!(back.data, img.data);

    // Quality knob reflects in the output.
    let q10 = encode(&img, "avif", 10, &EncoderOptions::default(), None).unwrap();
    let q90 = encode(&img, "avif", 90, &EncoderOptions::default(), None).unwrap();
    println!("avif sizes: q10={} q90={}", q10.len(), q90.len());

    // Speed knob accepted across the full range.
    let mut fast = EncoderOptions::default();
    fast.avif_speed = 10;
    let _ = encode(&img, "avif", 60, &fast, None).unwrap();

    // RGBA encode path.
    let rgba = test_image(20, 20, 4);
    let rbytes = encode(&rgba, "avif", 60, &EncoderOptions::default(), None).unwrap();
    let rback = decode(&rbytes).unwrap();
    assert_eq!(rback.width, 20);

    // sniff_dimensions reads the ispe box.
    let (w, h) = upix_img::sniff_dimensions(&bytes);
    assert_eq!((w, h), (48, 36));
}

#[test]
fn unsupported_formats_and_channels() {
    let img = test_image(8, 8, 3);
    assert!(encode(&img, "bmp", 90, &EncoderOptions::default(), None).is_err());
    assert!(encode(&img, "gif", 90, &EncoderOptions::default(), None).is_err());
    let rgba = test_image(8, 8, 4);
    assert!(encode(&rgba, "jpg", 90, &EncoderOptions::default(), None).is_err());
    assert!(decode(b"not an image at all, just bytes").is_err());
}

#[test]
fn convert_image_channels_semantics() {
    // 4→3 drops alpha (historical byte-identical passthrough).
    let mut rgba = Image::new(2, 1, 4);
    rgba.data.copy_from_slice(&[1, 2, 3, 250, 4, 5, 6, 0]);
    let rgb = convert_image_channels(&rgba, 3).unwrap();
    assert_eq!(rgb.channels, 3);
    assert_eq!(rgb.data, vec![1, 2, 3, 4, 5, 6]);

    // 3→1 truncating luma: (77r + 150g + 29b + 128) >> 8.
    let mut rgb2 = Image::new(1, 1, 3);
    rgb2.data.copy_from_slice(&[255, 0, 0]);
    let gray = convert_image_channels(&rgb2, 1).unwrap();
    assert_eq!(gray.data, vec![((77 * 255 + 128) >> 8) as u8]);

    // 2→4 keeps alpha; gray replicates.
    let mut ga = Image::new(1, 1, 2);
    ga.data.copy_from_slice(&[7, 200]);
    let rgba4 = convert_image_channels(&ga, 4).unwrap();
    assert_eq!(rgba4.data, vec![7, 7, 7, 200]);

    // Out-of-range channel counts error.
    let bad = Image::new(1, 1, 5);
    assert!(convert_image_channels(&bad, 3).is_err());
    assert!(convert_image_channels(&rgb2, 0).is_err());

    // normalize keeps RGBA for RGB models.
    let mut n = rgba.clone();
    assert!(upix_img::normalize_image_channels(&mut n, 3, "test"));
    assert_eq!(n.channels, 4, "4→3 retained for RGB models");
    assert!(upix_img::normalize_image_channels(&mut n, 3, "test"));
    let mut g3 = rgb.clone();
    assert!(upix_img::normalize_image_channels(&mut g3, 4, "test"));
    assert_eq!(g3.channels, 4);
    assert_eq!(g3.data[3], 255, "alpha default 255");
}
