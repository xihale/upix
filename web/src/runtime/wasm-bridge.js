// Zero-dependency bridge to `crates/upix-wasm` C ABI exports.
// Works identically on the main thread and inside dedicated Web Workers,
// and safely re-reads `memory.buffer` after every WASM call in case linear
// memory grew (`memory.grow`).

import { t } from '../i18n/index.js';

const WASM_SIMD_PROBE = new Uint8Array([
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7b, 0x03,
  0x02, 0x01, 0x00, 0x0a, 0x0a, 0x01, 0x08, 0x00,
  0x41, 0x00, 0xfd, 0x0f, 0xfd, 0x62, 0x0b,
]);

export function supportsWasmSimd128() {
  try {
    return typeof WebAssembly === 'object' && WebAssembly.validate(WASM_SIMD_PROBE);
  } catch {
    return false;
  }
}

function defaultWasmUrl(simd) {
  const base = (typeof import.meta !== 'undefined' && import.meta.env && import.meta.env.BASE_URL)
    ? import.meta.env.BASE_URL
    : './';
  const normalized = base.endsWith('/') ? base : `${base}/`;
  const file = simd ? 'wasm/upix_wasm.wasm' : 'wasm/upix_wasm_scalar.wasm';
  if (typeof document !== 'undefined' && document.baseURI) {
    return new URL(`${normalized}${file}`, document.baseURI).href;
  }
  if (typeof self !== 'undefined' && self.location) {
    return new URL(`${normalized}${file}`, self.location.href).href;
  }
  return `${normalized}${file}`;
}

let cachedCompiledModulePromise = null;

export async function compileUpixWasmModule(customUrl) {
  if (!customUrl && cachedCompiledModulePromise) {
    return cachedCompiledModulePromise;
  }
  const load = async () => {
    const simd = supportsWasmSimd128();
    const primaryUrl = customUrl || defaultWasmUrl(simd);
    let resp = await fetch(primaryUrl);
    if (!resp.ok && !simd && !customUrl) {
      resp = await fetch(defaultWasmUrl(true));
    }
    if (!resp.ok) {
      throw new Error(t('runtime.wasmLoadFailed', { status: resp.status, url: primaryUrl }));
    }
    if (typeof WebAssembly.compileStreaming === 'function') {
      try {
        return await WebAssembly.compileStreaming(resp.clone());
      } catch {
        // Fallback when server omits `application/wasm` MIME header.
      }
    }
    const bytes = await resp.arrayBuffer();
    return WebAssembly.compile(bytes);
  };

  if (!customUrl) {
    cachedCompiledModulePromise = load().catch((err) => {
      cachedCompiledModulePromise = null;
      throw err;
    });
    return cachedCompiledModulePromise;
  }
  return load();
}

export class UpixWasmBridge {
  constructor(instance, moduleRef) {
    this.instance = instance;
    this.module = moduleRef;
    this.exports = instance.exports;
    this.memory = instance.exports.memory;
    this.encoder = new TextEncoder();
    this.decoder = new TextDecoder('utf-8');
    this.inferInPtr = 0;
    this.inferInCap = 0;
    this.inferOutPtr = 0;
    this.inferOutCap = 0;
  }

  static async create(compiledModuleOrUrl) {
    const mod = compiledModuleOrUrl instanceof WebAssembly.Module
      ? compiledModuleOrUrl
      : await compileUpixWasmModule(compiledModuleOrUrl);
    const instance = await WebAssembly.instantiate(mod, {});
    return new UpixWasmBridge(instance, mod);
  }

  u8() {
    return new Uint8Array(this.memory.buffer);
  }

  ensureScratch(ptrKey, capKey, byteLen) {
    if (this[capKey] >= byteLen && this[ptrKey]) {
      return this[ptrKey];
    }
    if (this[ptrKey] && this[capKey] > 0) {
      this.exports.upix_dealloc(this[ptrKey] >>> 0, this[capKey] >>> 0);
      this[ptrKey] = 0;
      this[capKey] = 0;
    }
    const ptr = this.exports.upix_alloc(byteLen) >>> 0;
    if (!ptr) {
      throw new Error(t('runtime.wasmBufAllocFailed', { bytes: byteLen }));
    }
    this[ptrKey] = ptr;
    this[capKey] = byteLen;
    return ptr;
  }

  allocBytes(bytes) {
    if (!bytes || bytes.byteLength === 0) {
      return { ptr: 0, len: 0 };
    }
    const view = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    const ptr = this.exports.upix_alloc(view.byteLength) >>> 0;
    if (!ptr) {
      throw new Error(t('runtime.wasmMemAllocFailed', { bytes: view.byteLength }));
    }
    this.u8().set(view, ptr);
    return { ptr, len: view.byteLength };
  }

  allocString(str) {
    if (!str) return { ptr: 0, len: 0 };
    return this.allocBytes(this.encoder.encode(str));
  }

  free(ptr, len) {
    if (ptr && len > 0) {
      this.exports.upix_dealloc(ptr >>> 0, len >>> 0);
    }
  }

  readLastError() {
    const ptr = this.exports.upix_last_error_ptr() >>> 0;
    const len = this.exports.upix_last_error_len() >>> 0;
    if (!ptr || !len) return t('runtime.wasmUnknownError');
    return this.decoder.decode(new Uint8Array(this.memory.buffer, ptr, len));
  }

  readResultBytesCopy() {
    const ptr = this.exports.upix_result_ptr() >>> 0;
    const len = this.exports.upix_result_len() >>> 0;
    if (!ptr || !len) return new Uint8Array(0);
    return new Uint8Array(this.memory.buffer, ptr, len).slice();
  }

  readResultText() {
    const ptr = this.exports.upix_result_ptr() >>> 0;
    const len = this.exports.upix_result_len() >>> 0;
    if (!ptr || !len) return '';
    return this.decoder.decode(new Uint8Array(this.memory.buffer, ptr, len));
  }

  readResultJson() {
    const text = this.readResultText();
    return text ? JSON.parse(text) : null;
  }

  getCatalog() {
    const rc = this.exports.upix_catalog_json();
    if (rc !== 0) throw new Error(this.readLastError());
    return this.readResultJson() || [];
  }

  classifyModel(name) {
    const n = this.allocString(name);
    try {
      const rc = this.exports.upix_classify_model_json(n.ptr, n.len);
      if (rc !== 0) throw new Error(this.readLastError());
      return this.readResultJson();
    } finally {
      this.free(n.ptr, n.len);
    }
  }

  sha256Hex(bytes) {
    const b = this.allocBytes(bytes);
    try {
      const rc = this.exports.upix_sha256_hex(b.ptr, b.len);
      if (rc !== 0) throw new Error(this.readLastError());
      return this.readResultText();
    } finally {
      this.free(b.ptr, b.len);
    }
  }

  /**
   * Parse `.param` + `.bin` inside Rust WASM, returning `{ handle, meta, getWeightsView() }`.
   */
  loadModel(name, paramText, binBytes) {
    const n = this.allocString(name);
    const p = this.allocString(paramText);
    const b = this.allocBytes(binBytes);
    let handle = 0;
    try {
      handle = this.exports.upix_model_load(n.ptr, n.len, p.ptr, p.len, b.ptr, b.len) >>> 0;
      if (!handle) {
        throw new Error(this.readLastError());
      }
    } finally {
      this.free(n.ptr, n.len);
      this.free(p.ptr, p.len);
      this.free(b.ptr, b.len);
    }

    const metaRc = this.exports.upix_model_meta_json(handle);
    if (metaRc !== 0) {
      this.exports.upix_model_free(handle);
      throw new Error(this.readLastError());
    }
    const meta = this.readResultJson();

    return {
      handle,
      meta,
      getWeightsView: () => {
        const wPtr = this.exports.upix_model_weights_ptr(handle) >>> 0;
        const wLen = this.exports.upix_model_weights_len(handle) >>> 0;
        if (!wPtr || !wLen) return new Float32Array(0);
        return new Float32Array(this.memory.buffer, wPtr, wLen);
      },
      dispose: () => {
        this.exports.upix_model_free(handle);
      },
    };
  }

  freeModel(handle) {
    if (handle) {
      this.exports.upix_model_free(handle >>> 0);
    }
  }

  /**
   * Run one HWC `Float32Array` tile through the Rust SIMD128 CPU engine.
   */
  inferTileCpu(handle, inputF32, width, height, channels, scale) {
    const inByteLen = inputF32.byteLength;
    const outFloats = (width * scale) * (height * scale) * channels;
    const outByteLen = outFloats * 4;
    const inPtr = this.ensureScratch('inferInPtr', 'inferInCap', inByteLen);
    const outPtr = this.ensureScratch('inferOutPtr', 'inferOutCap', outByteLen);

    new Float32Array(this.memory.buffer, inPtr, inputF32.length).set(inputF32);
    const rc = this.exports.upix_model_infer_f32(
      handle >>> 0,
      inPtr,
      outPtr,
      width,
      height,
      channels,
    );
    if (rc !== 0) {
      throw new Error(this.readLastError());
    }
    return new Float32Array(this.memory.buffer, outPtr, outFloats).slice();
  }

  computeTileLayout(width, height, tileSize, overlap) {
    const rc = this.exports.upix_compute_tile_layout_json(width, height, tileSize, overlap);
    if (rc !== 0) throw new Error(this.readLastError());
    return this.readResultJson() || [];
  }

  /**
   * Pin a source `u8` image inside WASM memory so repeated `extractTileU8` calls
   * across dozens of tiles avoid re-copying the full image!
   */
  pinImageU8(pixelsU8, width, height, srcChannels) {
    const alloc = this.allocBytes(pixelsU8);
    let tilePtr = 0;
    let tileCap = 0;
    let disposed = false;
    return {
      extractTile: (region, dstChannels) => {
        const outLen = region.width * region.height * dstChannels;
        const outBytes = outLen * 4;
        if (tileCap < outBytes || !tilePtr) {
          if (tilePtr && tileCap > 0) {
            this.free(tilePtr, tileCap);
          }
          tilePtr = this.exports.upix_alloc(outBytes) >>> 0;
          if (!tilePtr) {
            tileCap = 0;
            throw new Error(t('runtime.wasmTileAllocFailed'));
          }
          tileCap = outBytes;
        }
        const rc = this.exports.upix_extract_tile_u8(
          alloc.ptr,
          width,
          height,
          srcChannels,
          dstChannels,
          region.x,
          region.y,
          region.width,
          region.height,
          tilePtr,
        );
        if (rc !== 0) throw new Error(this.readLastError());
        return new Float32Array(this.memory.buffer, tilePtr, outLen).slice();
      },
      dispose: () => {
        if (!disposed) {
          disposed = true;
          if (tilePtr && tileCap > 0) {
            this.free(tilePtr, tileCap);
            tilePtr = 0;
            tileCap = 0;
          }
          this.free(alloc.ptr, alloc.len);
        }
      },
    };
  }

  /**
   * Create a persistent WASM float blend accumulator using `upix_core::TileProcessor::blend_tile`
   * (`w * w` quadratic edge fade) + `finalize_accum_to_u8`.
   */
  createBlendSession(outWidth, outHeight, modelChannels, outChannels) {
    const px = outWidth * outHeight;
    const accumBytes = px * modelChannels * 4;
    const weightBytes = px * 4;
    const accumPtr = this.exports.upix_alloc(accumBytes) >>> 0;
    const weightPtr = this.exports.upix_alloc(weightBytes) >>> 0;
    if (!accumPtr || !weightPtr) {
      if (accumPtr) this.free(accumPtr, accumBytes);
      if (weightPtr) this.free(weightPtr, weightBytes);
      throw new Error(t('runtime.wasmBlendOom'));
    }
    this.u8().fill(0, accumPtr, accumPtr + accumBytes);
    this.u8().fill(0, weightPtr, weightPtr + weightBytes);

    let tilePtr = 0;
    let tileCap = 0;
    let disposed = false;
    return {
      blendTile: (tileF32, region, scale, overlap) => {
        const tileBytes = tileF32.byteLength;
        if (tileCap < tileBytes || !tilePtr) {
          if (tilePtr && tileCap > 0) {
            this.free(tilePtr, tileCap);
          }
          tilePtr = this.exports.upix_alloc(tileBytes) >>> 0;
          if (!tilePtr) {
            tileCap = 0;
            throw new Error(t('runtime.wasmBlendTileAllocFailed'));
          }
          tileCap = tileBytes;
        }
        new Float32Array(this.memory.buffer, tilePtr, tileF32.length).set(tileF32);
        this.exports.upix_blend_tile(
          tilePtr,
          accumPtr,
          weightPtr,
          outWidth,
          outHeight,
          modelChannels,
          region.x,
          region.y,
          region.width,
          region.height,
          scale,
          overlap,
        );
      },
      finalizeToU8: (alphaPlaneU8 = null) => {
        const outBytes = px * outChannels;
        const outPtr = this.exports.upix_alloc(outBytes) >>> 0;
        if (!outPtr) throw new Error(t('runtime.wasmFinalBufAllocFailed'));
        let alphaAlloc = { ptr: 0, len: 0 };
        try {
          if (alphaPlaneU8 && alphaPlaneU8.byteLength > 0) {
            alphaAlloc = this.allocBytes(alphaPlaneU8);
          }
          this.exports.upix_finalize_blend_u8(
            accumPtr,
            weightPtr,
            alphaAlloc.ptr,
            outPtr,
            outWidth,
            outHeight,
            modelChannels,
            outChannels,
          );
          return new Uint8Array(this.memory.buffer, outPtr, outBytes).slice();
        } finally {
          this.free(alphaAlloc.ptr, alphaAlloc.len);
          this.free(outPtr, outBytes);
        }
      },
      dispose: () => {
        if (!disposed) {
          disposed = true;
          if (tilePtr && tileCap > 0) {
            this.free(tilePtr, tileCap);
            tilePtr = 0;
            tileCap = 0;
          }
          this.free(accumPtr, accumBytes);
          this.free(weightPtr, weightBytes);
        }
      },
    };
  }

  bicubicScaleU8(srcU8, srcW, srcH, dstW, dstH) {
    if (srcW === dstW && srcH === dstH) {
      return srcU8.slice();
    }
    const srcAlloc = this.allocBytes(srcU8);
    const dstBytes = dstW * dstH;
    const dstPtr = this.exports.upix_alloc(dstBytes) >>> 0;
    if (!dstPtr) {
      this.free(srcAlloc.ptr, srcAlloc.len);
      throw new Error(t('runtime.wasmAlphaScaleAllocFailed'));
    }
    try {
      this.exports.upix_bicubic_scale_u8(srcAlloc.ptr, srcW, srcH, dstPtr, dstW, dstH);
      return new Uint8Array(this.memory.buffer, dstPtr, dstBytes).slice();
    } finally {
      this.free(srcAlloc.ptr, srcAlloc.len);
      this.free(dstPtr, dstBytes);
    }
  }

  probeSrMarker(fileBytes) {
    const b = this.allocBytes(fileBytes);
    try {
      const rc = this.exports.upix_probe_sr_marker_json(b.ptr, b.len);
      if (rc !== 0) return null;
      return this.readResultJson();
    } finally {
      this.free(b.ptr, b.len);
    }
  }

  buildSrXmp(modelName, scale, tsSec = Math.floor(Date.now() / 1000)) {
    const m = this.allocString(modelName);
    try {
      const rc = this.exports.upix_build_sr_xmp(m.ptr, m.len, scale, BigInt(tsSec));
      if (rc !== 0) throw new Error(this.readLastError());
      return this.readResultText();
    } finally {
      this.free(m.ptr, m.len);
    }
  }

  encodePng(pixelsU8, width, height, channels, quality = 90, pngLevel = -1, xmp = '') {
    const px = this.allocBytes(pixelsU8);
    const x = this.allocString(xmp);
    try {
      const rc = this.exports.upix_encode_png(
        px.ptr,
        px.len,
        width,
        height,
        channels,
        quality,
        pngLevel,
        x.ptr,
        x.len,
      );
      if (rc !== 0) throw new Error(this.readLastError());
      return this.readResultBytesCopy();
    } finally {
      this.free(px.ptr, px.len);
      this.free(x.ptr, x.len);
    }
  }

  encodeJpeg(pixelsU8, width, height, channels, quality = 92, subsampling = 420, xmp = '') {
    const px = this.allocBytes(pixelsU8);
    const x = this.allocString(xmp);
    try {
      const rc = this.exports.upix_encode_jpeg(
        px.ptr,
        px.len,
        width,
        height,
        channels,
        quality,
        subsampling,
        x.ptr,
        x.len,
      );
      if (rc !== 0) throw new Error(this.readLastError());
      return this.readResultBytesCopy();
    } finally {
      this.free(px.ptr, px.len);
      this.free(x.ptr, x.len);
    }
  }

  webpAttachXmp(webpBytes, xmp) {
    if (!xmp) return webpBytes instanceof Uint8Array ? webpBytes : new Uint8Array(webpBytes);
    const w = this.allocBytes(webpBytes);
    const x = this.allocString(xmp);
    try {
      const rc = this.exports.upix_webp_attach_xmp(w.ptr, w.len, x.ptr, x.len);
      if (rc !== 0) return webpBytes instanceof Uint8Array ? webpBytes : new Uint8Array(webpBytes);
      return this.readResultBytesCopy();
    } finally {
      this.free(w.ptr, w.len);
      this.free(x.ptr, x.len);
    }
  }
}
