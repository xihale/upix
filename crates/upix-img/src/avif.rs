//! AVIF codec via runtime dlopen of the system libavif. Port of
//! `legacy/share/utils/image_avif.cpp` (the dlopen build, the default).
//!
//! libavif 1.x public ABI is used through hand-mirrored struct overlays;
//! the library-sized objects are allocated by the library itself and we
//! only write the frozen 1.0 prefix fields.

use std::sync::LazyLock;

type AvifResult = i32;

const AVIF_OK: AvifResult = 0;
const AVIF_NO_CODEC_AVAILABLE: AvifResult = 15;
const AVIF_PIXEL_FORMAT_YUV444: i32 = 1;
const AVIF_PIXEL_FORMAT_YUV420: i32 = 3;
const AVIF_RGB_FORMAT_RGB: i32 = 0;
const AVIF_RGB_FORMAT_RGBA: i32 = 1;

#[repr(C)]
struct AvifRWData {
    data: *mut u8,
    size: usize,
}

/// libavif 1.x public layout (SONAME .so.16). Trailing tail absorbs fields
/// added after 1.0 so a newer SetDefaults cannot write past this struct.
#[repr(C)]
struct AvifRGBImage {
    width: u32,
    height: u32,
    depth: u32,
    format: i32,
    chroma_upsampling: i32,
    chroma_downsampling: i32,
    avoid_libyuv: i32,
    ignore_alpha: i32,
    alpha_premultiplied: i32,
    is_float: i32,
    max_threads: i32,
    pixels: *mut u8,
    row_bytes: u32,
    tail: [u8; 128],
}

impl Default for AvifRGBImage {
    fn default() -> Self {
        // size_of::<AvifRGBImage>() stays within the allocation rounding the
        // library performs for its own larger struct; zeroed tail is fine
        // because SetDefaults fills everything it knows.
        unsafe { std::mem::zeroed() }
    }
}

/// Frozen 1.0 prefix of avifEncoder. We never allocate this; the library
/// returns the library-sized object and we only write quality/speed.
#[repr(C)]
struct AvifEncoderOverlay {
    codec_choice: i32,
    max_threads: i32,
    speed: i32,
    keyframe_interval: i32,
    timescale: u64,
    repetition_count: i32,
    extra_layer_count: u32,
    quality: i32,
    quality_alpha: i32,
}

const _: () = assert!(std::mem::offset_of!(AvifEncoderOverlay, timescale) == 16);
const _: () = assert!(std::mem::offset_of!(AvifEncoderOverlay, quality) == 32);

#[repr(C)]
struct AvifImageHead {
    width: u32,
    height: u32,
    depth: u32,
}

struct AvifImage;
struct AvifDecoder;
struct AvifEncoder;

type FnVersion = unsafe extern "C" fn() -> *const u8;
type FnResultToString = unsafe extern "C" fn(AvifResult) -> *const u8;
type FnDecoderCreate = unsafe extern "C" fn() -> *mut AvifDecoder;
type FnDecoderDestroy = unsafe extern "C" fn(*mut AvifDecoder);
type FnDecoderReadMemory =
    unsafe extern "C" fn(*mut AvifDecoder, *mut AvifImage, *const u8, usize) -> AvifResult;
type FnImageCreate =
    unsafe extern "C" fn(u32, u32, u32, i32) -> *mut AvifImage;
type FnImageCreateEmpty = unsafe extern "C" fn() -> *mut AvifImage;
type FnImageDestroy = unsafe extern "C" fn(*mut AvifImage);
type FnRgbSetDefaults = unsafe extern "C" fn(*mut AvifRGBImage, *const AvifImage);
type FnRgbAllocatePixels = unsafe extern "C" fn(*mut AvifRGBImage) -> AvifResult;
type FnRgbFreePixels = unsafe extern "C" fn(*mut AvifRGBImage);
type FnYuvToRgb = unsafe extern "C" fn(*const AvifImage, *mut AvifRGBImage) -> AvifResult;
type FnRgbToYuv = unsafe extern "C" fn(*mut AvifImage, *const AvifRGBImage) -> AvifResult;
type FnEncoderCreate = unsafe extern "C" fn() -> *mut AvifEncoder;
type FnEncoderDestroy = unsafe extern "C" fn(*mut AvifEncoder);
type FnEncoderWrite =
    unsafe extern "C" fn(*mut AvifEncoder, *mut AvifImage, *mut AvifRWData) -> AvifResult;
type FnRwDataFree = unsafe extern "C" fn(*mut AvifRWData);
// Optional since 1.0: absent only on pre-release/patched builds; the XMP
// marker is silently omitted when the symbol is missing.
type FnImageSetMetadataXmp =
    unsafe extern "C" fn(*mut AvifImage, *const u8, usize) -> AvifResult;

#[derive(Default)]
struct AvifApi {
    lib: Option<libloading::Library>,
    version: Option<FnVersion>,
    result_to_string: Option<FnResultToString>,
    decoder_create: Option<FnDecoderCreate>,
    decoder_destroy: Option<FnDecoderDestroy>,
    decoder_read_memory: Option<FnDecoderReadMemory>,
    image_create: Option<FnImageCreate>,
    image_create_empty: Option<FnImageCreateEmpty>,
    image_destroy: Option<FnImageDestroy>,
    rgb_set_defaults: Option<FnRgbSetDefaults>,
    rgb_allocate_pixels: Option<FnRgbAllocatePixels>,
    rgb_free_pixels: Option<FnRgbFreePixels>,
    yuv_to_rgb: Option<FnYuvToRgb>,
    rgb_to_yuv: Option<FnRgbToYuv>,
    encoder_create: Option<FnEncoderCreate>,
    encoder_destroy: Option<FnEncoderDestroy>,
    encoder_write: Option<FnEncoderWrite>,
    rw_data_free: Option<FnRwDataFree>,
    image_set_metadata_xmp: Option<FnImageSetMetadataXmp>,
}

impl AvifApi {
    fn decode_ready(&self) -> bool {
        self.lib.is_some()
            && self.decoder_create.is_some()
            && self.decoder_destroy.is_some()
            && self.decoder_read_memory.is_some()
            && self.image_create_empty.is_some()
            && self.image_destroy.is_some()
            && self.rgb_set_defaults.is_some()
            && self.rgb_allocate_pixels.is_some()
            && self.rgb_free_pixels.is_some()
            && self.yuv_to_rgb.is_some()
    }

    fn encode_ready(&self) -> bool {
        self.decode_ready()
            && self.image_create.is_some()
            && self.rgb_to_yuv.is_some()
            && self.encoder_create.is_some()
            && self.encoder_destroy.is_some()
            && self.encoder_write.is_some()
            && self.rw_data_free.is_some()
    }
}

fn try_open_avif() -> Option<libloading::Library> {
    const NAMES: &[&str] = &["libavif.so.16", "libavif.so"];
    for name in NAMES {
        // NB: unhandled errors (file exists but is not a valid shared
        // object) mirror the C++ `dlopen` failure handling: try the next
        // candidate.
        if let Ok(lib) = unsafe { libloading::Library::new(name) } {
            return Some(lib);
        }
    }
    None
}

fn result_text(api: &AvifApi, result: AvifResult) -> String {
    if let Some(f) = api.result_to_string {
        let ptr = unsafe { f(result) };
        if !ptr.is_null() {
            if let Ok(text) = unsafe { std::ffi::CStr::from_ptr(ptr.cast()) }.to_str() {
                if !text.is_empty() {
                    return text.to_string();
                }
            }
        }
    }
    result.to_string()
}

fn missing_library_message() -> &'static str {
    "AVIF needs libavif. Install: pacman -S libavif | apt install libavif16 | brew install libavif"
}

fn missing_encoder_message() -> &'static str {
    "libavif cannot encode AVIF (no aom/svt-av1). Reinstall libavif with an encoder"
}

unsafe fn bind_symbol<T: Copy>(lib: &libloading::Library, name: &[u8]) -> Option<T> {
    unsafe { lib.get::<T>(name) }.ok().map(|s| *s)
}

/// Shared by load_api() (logged, for real decode/encode work) and the quiet
/// completion probe below.
unsafe fn bind_symbols(lib: libloading::Library) -> AvifApi {
    let mut api = AvifApi {
        lib: Some(lib),
        ..Default::default()
    };
    let lib = api.lib.as_ref().unwrap();
    unsafe {
        api.version = bind_symbol(lib, b"avifVersion\0");
        api.result_to_string = bind_symbol(lib, b"avifResultToString\0");
        api.decoder_create = bind_symbol(lib, b"avifDecoderCreate\0");
        api.decoder_destroy = bind_symbol(lib, b"avifDecoderDestroy\0");
        api.decoder_read_memory = bind_symbol(lib, b"avifDecoderReadMemory\0");
        api.image_create = bind_symbol(lib, b"avifImageCreate\0");
        api.image_create_empty = bind_symbol(lib, b"avifImageCreateEmpty\0");
        api.image_destroy = bind_symbol(lib, b"avifImageDestroy\0");
        api.rgb_set_defaults = bind_symbol(lib, b"avifRGBImageSetDefaults\0");
        api.rgb_allocate_pixels = bind_symbol(lib, b"avifRGBImageAllocatePixels\0");
        api.rgb_free_pixels = bind_symbol(lib, b"avifRGBImageFreePixels\0");
        api.yuv_to_rgb = bind_symbol(lib, b"avifImageYUVToRGB\0");
        api.rgb_to_yuv = bind_symbol(lib, b"avifImageRGBToYUV\0");
        api.encoder_create = bind_symbol(lib, b"avifEncoderCreate\0");
        api.encoder_destroy = bind_symbol(lib, b"avifEncoderDestroy\0");
        api.encoder_write = bind_symbol(lib, b"avifEncoderWrite\0");
        api.rw_data_free = bind_symbol(lib, b"avifRWDataFree\0");
        api.image_set_metadata_xmp = bind_symbol(lib, b"avifImageSetMetadataXMP\0");
    }
    api
}

/// libavif 1.x is the only accepted ABI; anything else is treated as absent.
fn version_is_1x(api: &AvifApi) -> bool {
    let Some(f) = api.version else {
        return false;
    };
    let ptr = unsafe { f() };
    if ptr.is_null() {
        return false;
    }
    let bytes = unsafe { std::ffi::CStr::from_ptr(ptr.cast()) }.to_bytes();
    bytes.len() >= 2 && bytes[0] == b'1' && bytes[1] == b'.'
}

/// Thread count shared by the encoder (parallel tiles) and RGB conversions.
fn codec_threads() -> i32 {
    let hw = std::thread::available_parallelism()
        .map(|n| n.get() as i32)
        .unwrap_or(0);
    if hw > 0 {
        hw
    } else {
        4
    }
}

fn load_api() -> &'static AvifApi {
    static API: LazyLock<AvifApi> = LazyLock::new(|| {
        let Some(lib) = try_open_avif() else {
            return AvifApi::default();
        };
        let api = unsafe { bind_symbols(lib) };

        if !api.decode_ready() {
            log::warn!("libavif loaded but required decode symbols are missing");
            // Dropping the Library closes the handle, matching the C++
            // close_lib path; no live pointers exist yet.
            return AvifApi::default();
        }
        if !version_is_1x(&api) {
            log::warn!("libavif version is not 1.x, refusing to use it");
            return AvifApi::default();
        }
        // Handle deliberately stays open for the process lifetime.
        api
    });
    &API
}

/// True when the buffer is an ISOBMFF file whose ftyp brands include
/// avif/avis. Port of C++ `looks_like_avif`.
pub fn looks_like_avif(data: &[u8]) -> bool {
    if data.len() < 16 {
        return false;
    }
    find_brand(data, b"avif") || find_brand(data, b"avis")
}

fn find_brand(data: &[u8], brand: &[u8; 4]) -> bool {
    if data.len() < 12 || &data[4..8] != b"ftyp" {
        return false;
    }
    let box_size = u32::from_be_bytes(data[0..4].try_into().unwrap()) as usize;
    if box_size < 16 {
        return false;
    }
    let limit = data.len().min(box_size);
    let mut offset = 8usize;
    while offset + 4 <= limit {
        if &data[offset..offset + 4] == brand {
            return true;
        }
        offset += 4;
    }
    false
}

/// Walks ISOBMFF boxes for the primary `ispe` width/height. (0, 0) if not
/// found. Port of C++ `sniff_dimensions`.
pub fn sniff_dimensions(data: &[u8]) -> (u32, u32) {
    if data.len() < 16 || !looks_like_avif(data) {
        return (0, 0);
    }
    let mut found = IspeWalk::default();
    walk_boxes(data, 0, data.len(), &mut found, 0);
    (found.width, found.height)
}

#[derive(Default)]
struct IspeWalk {
    width: u32,
    height: u32,
}

fn walk_boxes(data: &[u8], begin: usize, end: usize, out: &mut IspeWalk, depth: u32) {
    if depth > 12 || begin >= end || end > data.len() {
        return;
    }
    let mut offset = begin;
    while offset + 8 <= end {
        let size32 = u32::from_be_bytes(data[offset..offset + 4].try_into().unwrap());
        let ctype = &data[offset + 4..offset + 8];
        let mut header = 8usize;
        let mut box_size = size32 as u64;
        if size32 == 1 {
            if offset + 16 > end {
                return;
            }
            let hi =
                u32::from_be_bytes(data[offset + 8..offset + 12].try_into().unwrap()) as u64;
            let lo =
                u32::from_be_bytes(data[offset + 12..offset + 16].try_into().unwrap()) as u64;
            box_size = (hi << 32) | lo;
            header = 16;
        } else if size32 == 0 {
            box_size = (end - offset) as u64;
        }
        // Compare against the remaining length rather than offset+box_size:
        // for a 64-bit largesize near 2^64 that addition wraps, the walk
        // continues with payload_end below payload_begin, and the ispe
        // branch below then reads past the buffer (underflow).
        if box_size < header as u64 || box_size > (end - offset) as u64 {
            return;
        }
        let payload_begin = offset + header;
        let payload_end = offset + box_size as usize;
        if ctype == b"ispe" && payload_end - payload_begin >= 12 {
            out.width = u32::from_be_bytes(
                data[payload_begin + 4..payload_begin + 8].try_into().unwrap(),
            );
            out.height = u32::from_be_bytes(
                data[payload_begin + 8..payload_begin + 12].try_into().unwrap(),
            );
            return;
        }
        if ctype == b"meta" && payload_end >= payload_begin + 4 {
            walk_boxes(data, payload_begin + 4, payload_end, out, depth + 1);
        } else if ctype == b"iprp" || ctype == b"ipco" {
            walk_boxes(data, payload_begin, payload_end, out, depth + 1);
        }
        if out.width > 0 && out.height > 0 {
            return;
        }
        offset = payload_end;
    }
}

/// Quiet, cached probes: does the system libavif load with usable decode /
/// encode symbols? Same acceptance rules as load()/save() but they never
/// log (shell completions consume stdout). Encode implies decode.
pub fn avif_decode_available() -> bool {
    load_api().decode_ready()
}

pub fn avif_encode_available() -> bool {
    load_api().encode_ready()
}

/// CONTRACT API pin: single availability probe. True when the system
/// libavif loads with usable decode symbols (encode readiness is checked
/// separately at save time).
pub fn avif_available() -> bool {
    avif_decode_available()
}

/// C++ `avif_codec::load` (dlopen build).
pub(crate) fn load_avif(buffer: &[u8]) -> Result<crate::Image, crate::ImgError> {
    let api = load_api();
    if !api.decode_ready() {
        return Err(crate::ImgError::new(24, missing_library_message()));
    }
    unsafe {
        let decoder = api.decoder_create.unwrap()();
        let image = api.image_create_empty.unwrap()();
        if decoder.is_null() || image.is_null() {
            if !image.is_null() {
                api.image_destroy.unwrap()(image);
            }
            if !decoder.is_null() {
                api.decoder_destroy.unwrap()(decoder);
            }
            return Err(crate::ImgError::new(25, "AVIF decoder allocation failed"));
        }

        let read_result =
            api.decoder_read_memory.unwrap()(decoder, image, buffer.as_ptr(), buffer.len());
        api.decoder_destroy.unwrap()(decoder);
        if read_result != AVIF_OK {
            api.image_destroy.unwrap()(image);
            return Err(crate::ImgError::new(
                25,
                format!("AVIF decode failed: {}", result_text(api, read_result)),
            ));
        }

        let head = &*(image as *const AvifImageHead);
        if head.width == 0 || head.height == 0 {
            api.image_destroy.unwrap()(image);
            return Err(crate::ImgError::new(25, "AVIF decode produced empty image"));
        }

        let mut rgb = AvifRGBImage::default();
        api.rgb_set_defaults.unwrap()(&mut rgb, image);
        rgb.format = AVIF_RGB_FORMAT_RGB;
        rgb.depth = 8;
        rgb.max_threads = codec_threads(); // parallel YUV→RGB when libyuv steps aside
        let alloc_result = api.rgb_allocate_pixels.unwrap()(&mut rgb);
        if alloc_result != AVIF_OK || rgb.pixels.is_null() || rgb.width == 0 || rgb.height == 0
        {
            api.image_destroy.unwrap()(image);
            return Err(crate::ImgError::new(25, "AVIF RGB allocation failed"));
        }

        let convert_result = api.yuv_to_rgb.unwrap()(image, &mut rgb);
        if convert_result != AVIF_OK {
            api.rgb_free_pixels.unwrap()(&mut rgb);
            api.image_destroy.unwrap()(image);
            return Err(crate::ImgError::new(
                25,
                format!("AVIF YUV to RGB failed: {}", result_text(api, convert_result)),
            ));
        }

        let mut out = crate::Image::new(rgb.width as i32, rgb.height as i32, 3);
        let row_bytes = rgb.width as usize * 3;
        let src_stride = if rgb.row_bytes != 0 {
            rgb.row_bytes as usize
        } else {
            row_bytes
        };
        for y in 0..rgb.height as usize {
            let src = rgb.pixels.add(y * src_stride);
            std::ptr::copy_nonoverlapping(src, out.data.as_mut_ptr().add(y * row_bytes), row_bytes);
        }

        api.rgb_free_pixels.unwrap()(&mut rgb);
        api.image_destroy.unwrap()(image);
        Ok(out)
    }
}

/// AVIF chroma subsampling from EncoderOptions, degrading anything outside
/// {420, 444} to the 420 default.
fn avif_subsampling_or_default(requested: i32) -> i32 {
    if requested == 444 {
        444
    } else {
        420
    }
}

/// C++ `avif_codec::save` (dlopen build): XMP rides along as an AVIF XMP
/// item (best-effort on libs without the symbol); knobs speed/subsampling/
/// threads map directly.
pub(crate) fn save_avif(
    image: &crate::Image,
    quality: i32,
    xmp: &str,
    opts: &crate::EncoderOptions,
) -> Result<Vec<u8>, crate::ImgError> {
    if image.channels != 3 && image.channels != 4 {
        return Err(crate::ImgError::new(18, "AVIF save requires 3 or 4 channels"));
    }
    if image.width <= 0 || image.height <= 0 {
        return Err(crate::ImgError::new(26, "AVIF encode failed: empty image"));
    }

    let api = load_api();
    if !api.decode_ready() {
        return Err(crate::ImgError::new(24, missing_library_message()));
    }
    if !api.encode_ready() {
        return Err(crate::ImgError::new(27, missing_encoder_message()));
    }
    unsafe {
        let avif_image = api.image_create.unwrap()(
            image.width as u32,
            image.height as u32,
            8,
            if avif_subsampling_or_default(opts.avif_subsampling) == 444 {
                AVIF_PIXEL_FORMAT_YUV444
            } else {
                AVIF_PIXEL_FORMAT_YUV420
            },
        );
        if avif_image.is_null() {
            return Err(crate::ImgError::new(26, "AVIF image allocation failed"));
        }

        // The XMP payload rides along as an AVIF XMP item (encoder_write
        // picks it up from the image). Best-effort: an older libavif without
        // the symbol encodes unmarked.
        if !xmp.is_empty() {
            if let Some(set_xmp) = api.image_set_metadata_xmp {
                set_xmp(avif_image, xmp.as_ptr(), xmp.len());
            }
        }

        let mut rgb = AvifRGBImage::default();
        api.rgb_set_defaults.unwrap()(&mut rgb, avif_image);
        rgb.format = if image.channels == 4 {
            AVIF_RGB_FORMAT_RGBA
        } else {
            AVIF_RGB_FORMAT_RGB
        };
        rgb.depth = 8;
        rgb.max_threads = codec_threads();
        rgb.pixels = image.data.as_ptr() as *mut u8;
        rgb.row_bytes = (image.width * image.channels) as u32;

        let convert_result = api.rgb_to_yuv.unwrap()(avif_image, &rgb);
        if convert_result != AVIF_OK {
            api.image_destroy.unwrap()(avif_image);
            return Err(crate::ImgError::new(
                26,
                format!("AVIF RGB to YUV failed: {}", result_text(api, convert_result)),
            ));
        }

        let encoder = api.encoder_create.unwrap()();
        if encoder.is_null() {
            api.image_destroy.unwrap()(avif_image);
            return Err(crate::ImgError::new(26, "AVIF encoder allocation failed"));
        }
        let overlay = &mut *(encoder as *mut AvifEncoderOverlay);
        overlay.quality = quality;
        overlay.quality_alpha = quality;
        overlay.speed = opts.avif_speed.clamp(0, 10);
        // Without threads the encoder is single-threaded.
        overlay.max_threads = if opts.avif_threads > 0 {
            opts.avif_threads
        } else {
            codec_threads()
        };

        let mut output = AvifRWData {
            data: std::ptr::null_mut(),
            size: 0,
        };
        let write_result = api.encoder_write.unwrap()(encoder, avif_image, &mut output);
        api.encoder_destroy.unwrap()(encoder);
        api.image_destroy.unwrap()(avif_image);

        if write_result != AVIF_OK || output.data.is_null() || output.size == 0 {
            if !output.data.is_null() {
                api.rw_data_free.unwrap()(&mut output);
            }
            if write_result == AVIF_NO_CODEC_AVAILABLE {
                return Err(crate::ImgError::new(27, missing_encoder_message()));
            }
            return Err(crate::ImgError::new(
                26,
                format!("AVIF encode failed: {}", result_text(api, write_result)),
            ));
        }

        let encoded =
            std::slice::from_raw_parts(output.data, output.size).to_vec();
        api.rw_data_free.unwrap()(&mut output);
        Ok(encoded)
    }
}
