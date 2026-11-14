// Browser-native image decode (`createImageBitmap` + EXIF `from-image`) and
// Rust WASM container encode (`PNG` / `JPEG` with `420/422/444` / `WebP` + `urn:upix:sr:1` XMP).

import { t } from '../i18n/index.js';

export function detectImageFormat(bytes, fallbackMime = '', fileName = '') {
  if (bytes && bytes.byteLength >= 12) {
    if (
      bytes[0] === 0x89 && bytes[1] === 0x50 && bytes[2] === 0x4e && bytes[3] === 0x47
    ) {
      return 'png';
    }
    if (bytes[0] === 0xff && bytes[1] === 0xd8) {
      return 'jpeg';
    }
    if (
      bytes[0] === 0x52 && bytes[1] === 0x49 && bytes[2] === 0x46 && bytes[3] === 0x46
      && bytes[8] === 0x57 && bytes[9] === 0x45 && bytes[10] === 0x42 && bytes[11] === 0x50
    ) {
      return 'webp';
    }
    if (
      bytes[4] === 0x66 && bytes[5] === 0x74 && bytes[6] === 0x79 && bytes[7] === 0x70
    ) {
      const brand = String.fromCharCode(bytes[8], bytes[9], bytes[10], bytes[11]);
      if (brand === 'avif' || brand === 'avis' || brand === 'mif1') {
        return 'avif';
      }
    }
  }
  const lowerMime = (fallbackMime || '').toLowerCase();
  if (lowerMime.includes('png')) return 'png';
  if (lowerMime.includes('jpeg') || lowerMime.includes('jpg')) return 'jpeg';
  if (lowerMime.includes('webp')) return 'webp';
  if (lowerMime.includes('avif')) return 'avif';

  const ext = (fileName.split('.').pop() || '').toLowerCase();
  if (ext === 'jpg') return 'jpeg';
  if (['png', 'jpeg', 'webp', 'avif'].includes(ext)) return ext;
  return 'png';
}

const LITTLE_ENDIAN = (() => {
  const u8 = new Uint8Array([1, 0, 0, 0]);
  return new Uint32Array(u8.buffer)[0] === 1;
})();
const RGBA_ALPHA_MASK = LITTLE_ENDIAN ? 0xff000000 : 0x000000ff;

// ASCII bytes for `urn:upix:sr:1`
const UPIX_NS_BYTES = [117, 114, 110, 58, 117, 112, 105, 120, 58, 115, 114, 58, 49];

function hasUpixSrNamespace(bytes) {
  const nLen = UPIX_NS_BYTES.length;
  const limit = bytes.byteLength - nLen;
  if (limit < 0) return false;
  let pos = 0;
  while (pos <= limit) {
    const idx = bytes.indexOf(117, pos); // 'u'
    if (idx === -1 || idx > limit) return false;
    let match = true;
    for (let j = 1; j < nLen; j += 1) {
      if (bytes[idx + j] !== UPIX_NS_BYTES[j]) {
        match = false;
        break;
      }
    }
    if (match) return true;
    pos = idx + 1;
  }
  return false;
}

function parseExifOrientation(bytes, view, tiffStart, maxEnd) {
  if (tiffStart + 8 > maxEnd) return 1;
  const b0 = bytes[tiffStart];
  const b1 = bytes[tiffStart + 1];
  let little = false;
  if (b0 === 0x49 && b1 === 0x49) little = true;
  else if (b0 !== 0x4d || b1 !== 0x4d) return 1;
  if (view.getUint16(tiffStart + 2, little) !== 0x002a) return 1;
  const ifdOffset = view.getUint32(tiffStart + 4, little);
  const ifd0 = tiffStart + ifdOffset;
  if (ifd0 + 2 > maxEnd) return 1;
  const entries = view.getUint16(ifd0, little);
  for (let i = 0; i < entries; i += 1) {
    const entry = ifd0 + 2 + i * 12;
    if (entry + 12 > maxEnd) break;
    if (view.getUint16(entry, little) === 0x0112) {
      return view.getUint16(entry + 8, little);
    }
  }
  return 1;
}

function probeBinaryHeader(bytes, rawFormat) {
  const len = bytes.byteLength;
  if (len < 16) return null;
  const view = new DataView(bytes.buffer, bytes.byteOffset, len);

  if (rawFormat === 'png') {
    if (
      len >= 29
      && bytes[0] === 0x89 && bytes[1] === 0x50 && bytes[2] === 0x4e && bytes[3] === 0x47
      && bytes[12] === 0x49 && bytes[13] === 0x48 && bytes[14] === 0x44 && bytes[15] === 0x52
    ) {
      const width = view.getUint32(16, false);
      const height = view.getUint32(20, false);
      if (width <= 0 || height <= 0) return null;
      const colorType = bytes[25];
      if (colorType === 4 || colorType === 6) {
        return { width, height, channels: 4 };
      }
      let hasTrns = false;
      let off = 8;
      while (off + 8 <= len) {
        const chunkLen = view.getUint32(off, false);
        const c0 = bytes[off + 4];
        const c1 = bytes[off + 5];
        const c2 = bytes[off + 6];
        const c3 = bytes[off + 7];
        if (c0 === 0x74 && c1 === 0x52 && c2 === 0x4e && c3 === 0x53) { // tRNS
          hasTrns = true;
          break;
        }
        if (
          (c0 === 0x49 && c1 === 0x44 && c2 === 0x41 && c3 === 0x54) // IDAT
          || (c0 === 0x49 && c1 === 0x45 && c2 === 0x4e && c3 === 0x44) // IEND
        ) {
          break;
        }
        const next = off + 12 + chunkLen;
        if (next <= off) break;
        off = next;
      }
      return { width, height, channels: hasTrns ? 4 : 3 };
    }
    return null;
  }

  if (rawFormat === 'jpeg') {
    if (bytes[0] !== 0xff || bytes[1] !== 0xd8) return null;
    let orientation = 1;
    let pos = 2;
    while (pos + 4 <= len) {
      if (bytes[pos] !== 0xff) {
        pos += 1;
        continue;
      }
      while (pos < len && bytes[pos] === 0xff) pos += 1;
      if (pos >= len) break;
      const marker = bytes[pos];
      pos += 1;
      if (marker === 0xd9 || marker === 0xda) break;
      if (marker === 0x00 || marker === 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
      if (pos + 2 > len) break;
      const segLen = view.getUint16(pos, false);
      if (segLen < 2 || pos + segLen > len) break;

      if (
        marker === 0xe1
        && segLen >= 14
        && bytes[pos + 2] === 0x45 && bytes[pos + 3] === 0x78
        && bytes[pos + 4] === 0x69 && bytes[pos + 5] === 0x66
        && bytes[pos + 6] === 0x00 && bytes[pos + 7] === 0x00
      ) {
        orientation = parseExifOrientation(bytes, view, pos + 8, pos + segLen);
      } else if (
        (marker >= 0xc0 && marker <= 0xc3)
        || (marker >= 0xc5 && marker <= 0xc7)
        || (marker >= 0xc9 && marker <= 0xcb)
        || (marker >= 0xcd && marker <= 0xcf)
      ) {
        if (segLen >= 7) {
          let height = view.getUint16(pos + 3, false);
          let width = view.getUint16(pos + 5, false);
          if (width > 0 && height > 0) {
            if (orientation >= 5 && orientation <= 8) {
              const tmp = width;
              width = height;
              height = tmp;
            }
            return { width, height, channels: 3 };
          }
        }
      }
      pos += segLen;
    }
    return null;
  }

  if (rawFormat === 'webp') {
    if (len < 25) return null;
    const fourcc = String.fromCharCode(bytes[12], bytes[13], bytes[14], bytes[15]);
    if (fourcc === 'VP8X' && len >= 30) {
      const hasAlpha = (bytes[20] & 0x10) !== 0;
      const width = 1 + (bytes[24] | (bytes[25] << 8) | (bytes[26] << 16));
      const height = 1 + (bytes[27] | (bytes[28] << 8) | (bytes[29] << 16));
      if (width > 0 && height > 0) return { width, height, channels: hasAlpha ? 4 : 3 };
    } else if (fourcc === 'VP8 ' && len >= 30) {
      if (bytes[23] === 0x9d && bytes[24] === 0x01 && bytes[25] === 0x2a) {
        const width = view.getUint16(26, true) & 0x3fff;
        const height = view.getUint16(28, true) & 0x3fff;
        if (width > 0 && height > 0) return { width, height, channels: 3 };
      }
    } else if (fourcc === 'VP8L' && len >= 25 && bytes[20] === 0x2f) {
      const bits = view.getUint32(21, true);
      const width = (bits & 0x3fff) + 1;
      const height = ((bits >>> 14) & 0x3fff) + 1;
      const hasAlpha = ((bits >>> 28) & 1) !== 0;
      if (width > 0 && height > 0) return { width, height, channels: hasAlpha ? 4 : 3 };
    }
    return null;
  }

  return null;
}

let sharedDecodeSurface = null;

function getSharedDecodeCtx(width, height) {
  if (sharedDecodeSurface) {
    sharedDecodeSurface.canvas.width = width;
    sharedDecodeSurface.canvas.height = height;
    return sharedDecodeSurface.ctx;
  }
  const surface = makeCanvas2d(width, height);
  sharedDecodeSurface = surface;
  return surface.ctx;
}

function makeCanvas2d(width, height) {
  if (typeof OffscreenCanvas !== 'undefined') {
    const canvas = new OffscreenCanvas(width, height);
    const ctx = canvas.getContext('2d', { willReadFrequently: true });
    if (ctx) return { canvas, ctx, isOffscreen: true };
  }
  if (typeof document !== 'undefined') {
    const canvas = document.createElement('canvas');
    canvas.width = width;
    canvas.height = height;
    const ctx = canvas.getContext('2d', { willReadFrequently: true });
    if (ctx) return { canvas, ctx, isOffscreen: false };
  }
  throw new Error(t('runtime.canvas2dUnsupported'));
}

async function decodeBitmap(blob) {
  if (typeof createImageBitmap === 'function') {
    try {
      return await createImageBitmap(blob, {
        imageOrientation: 'from-image',
        premultiplyAlpha: 'none',
        colorSpaceConversion: 'none',
      });
    } catch {
      return createImageBitmap(blob);
    }
  }

  if (typeof Image !== 'undefined' && typeof URL !== 'undefined') {
    const url = URL.createObjectURL(blob);
    try {
      const img = await new Promise((resolve, reject) => {
        const el = new Image();
        el.onload = () => resolve(el);
        el.onerror = () => reject(new Error(t('runtime.decodeFailed')));
        el.src = url;
      });
      return img;
    } finally {
      URL.revokeObjectURL(url);
    }
  }

  throw new Error(t('runtime.createImageBitmapUnsupported'));
}

/**
 * Read raw bytes, probe dimensions & channels, and detect `urn:upix:sr:1` XMP marker.
 */
export async function decodeInputImage(fileOrBlob, wasmBridge, { metadataOnly = false } = {}) {
  const ab = await fileOrBlob.arrayBuffer();
  const rawBytes = new Uint8Array(ab);
  const srMarker = hasUpixSrNamespace(rawBytes) ? wasmBridge.probeSrMarker(rawBytes) : null;
  const rawFormat = detectImageFormat(rawBytes, fileOrBlob.type, fileOrBlob.name || '');
  const format = rawFormat.toUpperCase();

  if (metadataOnly) {
    const fastMeta = probeBinaryHeader(rawBytes, rawFormat);
    if (fastMeta) {
      return {
        width: fastMeta.width,
        height: fastMeta.height,
        channels: fastMeta.channels,
        format,
        srMarker,
      };
    }
  }

  const bitmap = await decodeBitmap(
    fileOrBlob instanceof Blob ? fileOrBlob : new Blob([rawBytes]),
  );
  const width = bitmap.width || bitmap.naturalWidth || 0;
  const height = bitmap.height || bitmap.naturalHeight || 0;

  if (width <= 0 || height <= 0) {
    if (typeof bitmap.close === 'function') bitmap.close();
    throw new Error(t('runtime.invalidImageDims'));
  }

  if (metadataOnly && rawFormat === 'jpeg') {
    if (typeof bitmap.close === 'function') bitmap.close();
    return { width, height, channels: 3, format, srMarker };
  }

  const ctx = getSharedDecodeCtx(width, height);
  ctx.drawImage(bitmap, 0, 0);
  if (typeof bitmap.close === 'function') bitmap.close();

  const rgba = ctx.getImageData(0, 0, width, height).data;
  const pxCount = width * height;

  let hasTransparency = false;
  if (rawFormat !== 'jpeg') {
    const u32 = new Uint32Array(rgba.buffer, rgba.byteOffset, pxCount);
    for (let i = 0; i < pxCount; i += 1) {
      if (((u32[i] & RGBA_ALPHA_MASK) >>> 0) !== RGBA_ALPHA_MASK) {
        hasTransparency = true;
        break;
      }
    }
  }

  if (metadataOnly) {
    return {
      width,
      height,
      channels: hasTransparency ? 4 : 3,
      format,
      srMarker,
    };
  }

  if (hasTransparency) {
    return {
      width,
      height,
      channels: 4,
      format,
      srMarker,
      pixels: new Uint8Array(rgba.buffer, rgba.byteOffset, rgba.byteLength),
    };
  }

  const rgb = new Uint8Array(pxCount * 3);
  for (let p = 0; p < pxCount; p += 1) {
    const s = p * 4;
    const d = p * 3;
    rgb[d] = rgba[s];
    rgb[d + 1] = rgba[s + 1];
    rgb[d + 2] = rgba[s + 2];
  }
  return {
    width,
    height,
    channels: 3,
    format,
    srMarker,
    pixels: rgb,
  };
}

async function canvasEncodeBlob(canvas, isOffscreen, mimeType, qualityFloat) {
  if (isOffscreen && typeof canvas.convertToBlob === 'function') {
    return canvas.convertToBlob({ type: mimeType, quality: qualityFloat });
  }
  if (typeof canvas.toBlob === 'function') {
    return new Promise((resolve, reject) => {
      canvas.toBlob(
        (blob) => (blob ? resolve(blob) : reject(new Error(t('runtime.canvasWebpFailed')))),
        mimeType,
        qualityFloat,
      );
    });
  }
  throw new Error(t('runtime.canvasBlobUnsupported'));
}

/**
 * Encode processed `u8` pixels (`RGB` or `RGBA`) to PNG / JPEG / WebP with `urn:upix:sr:1` XMP marker.
 */
export async function encodeOutputImage(
  wasmBridge,
  pixelsU8,
  width,
  height,
  channels,
  {
    outputFormat = 'png',
    quality = 92,
    jpegSubsampling = 420,
    pngLevel = -1,
    webpLossless = false,
  } = {},
  xmp = '',
) {
  const fmt = (outputFormat || 'png').toLowerCase().replace(/^\./, '');

  if (fmt === 'jpg' || fmt === 'jpeg') {
    const bytes = wasmBridge.encodeJpeg(
      pixelsU8,
      width,
      height,
      channels,
      quality,
      jpegSubsampling,
      xmp,
    );
    return {
      bytes,
      blob: new Blob([bytes], { type: 'image/jpeg' }),
      mimeType: 'image/jpeg',
      ext: 'jpg',
    };
  }

  if (fmt === 'webp') {
    const { canvas, ctx, isOffscreen } = makeCanvas2d(width, height);
    const px = width * height;
    const rgba = new Uint8ClampedArray(px * 4);
    if (channels === 4) {
      rgba.set(pixelsU8.subarray(0, px * 4));
    } else {
      for (let i = 0; i < px; i += 1) {
        rgba[i * 4] = pixelsU8[i * 3];
        rgba[i * 4 + 1] = pixelsU8[i * 3 + 1];
        rgba[i * 4 + 2] = pixelsU8[i * 3 + 2];
        rgba[i * 4 + 3] = 255;
      }
    }
    ctx.putImageData(new ImageData(rgba, width, height), 0, 0);
    const q = webpLossless ? 1.0 : Math.max(0.01, Math.min(1.0, quality / 100));
    const rawBlob = await canvasEncodeBlob(canvas, isOffscreen, 'image/webp', q);

    // Some browsers silently fall back to `image/png` if WebP encoding is absent.
    if (rawBlob.type === 'image/webp') {
      const rawWebp = new Uint8Array(await rawBlob.arrayBuffer());
      const muxed = wasmBridge.webpAttachXmp(rawWebp, xmp);
      return {
        bytes: muxed,
        blob: new Blob([muxed], { type: 'image/webp' }),
        mimeType: 'image/webp',
        ext: 'webp',
      };
    }
  }

  // Default / PNG
  const bytes = wasmBridge.encodePng(
    pixelsU8,
    width,
    height,
    channels,
    quality,
    pngLevel,
    xmp,
  );
  return {
    bytes,
    blob: new Blob([bytes], { type: 'image/png' }),
    mimeType: 'image/png',
    ext: 'png',
  };
}

export function formatOutputFilename(originalName, modelName, scale, ext) {
  const base = (originalName || 'image')
    .replace(/\.[^.]+$/, '')
    .replace(/[^a-zA-Z0-9_\u4e00-\u9fa5.-]+/g, '_') || 'image';
  const safeModel = (modelName || 'upix').replace(/[^a-zA-Z0-9_-]+/g, '-');
  return `${base}_${safeModel}_x${scale}.${ext}`;
}
