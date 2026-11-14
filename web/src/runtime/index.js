// Unified browser runtime for Upix Web (`createUpixRuntime`).
// Orchestrates:
// 1. WebGPU compute backend (`WebGpuBackend`)
// 2. WASM SIMD128 multi-Worker CPU backend (`WasmCpuBackend`)
// 3. Experimental W3C WebNN backend (`WebNnBackend` — `gpu` / `npu` / `cpu`)
// Plus `resolveAutoBackend` architecture- & batch-aware WebGPU/WebNN hybrid routing
// (`batchSizeHint`, warm WebNN graph cache, CUGAN/RealPLKSR WebNN-favored families),
// IndexedDB model management, OOM tile-size shrink ladder, quadratic overlap blending,
// RGBA Mitchell-Netravali alpha bypass, `urn:upix:sr:1` XMP metadata, and batch ZIP export.

import { UpixWasmBridge, supportsWasmSimd128 } from './wasm-bridge.js';
import { UpixModelStore } from './model-store.js';
import { WasmCpuBackend, MAX_WASM_WORKERS, defaultThreadCount, systemThreadMax } from './backends/wasm-cpu.js';
import { WebGpuBackend } from './backends/webgpu.js';
import { WebNnBackend } from './backends/webnn.js';
import { decodeInputImage, encodeOutputImage, formatOutputFilename } from './image-io.js';
import { isFollowOutputFormat, normalizeOutputFormat } from '../ui/format.js';
import { createZipBlob } from './zip-utils.js';
import { getSyntheticBenchmarkModel, SYNTH_BENCHMARK_MODEL_NAME } from './synth-model.js';
import { t } from '../i18n/index.js';

const TILE_SHRINK_LADDER = [512, 384, 256, 192, 128, 96, 64];

function chooseAutoTileSize(backendId, modelName = '') {
  const lower = modelName.toLowerCase();
  const isHeavy = lower.includes('rrdb')
    || lower.includes('plksr')
    || lower.includes('hat')
    || lower.includes('dat')
    || lower.includes('swin')
    || lower.includes('esrgan-x')
    || lower.includes('bsrgan')
    || lower.includes('animesharp')
    || lower.includes('ultrasharp')
    || lower.includes('remacri')
    || lower.includes('hfa2k')
    || lower.includes('nomos8k');
  const isMedium = lower.includes('cugan')
    || lower.includes('omnisr')
    || lower.includes('esc');

  if (backendId === 'webgpu') {
    if (isHeavy) return 192;
    if (isMedium) return 256;
    return 384;
  }
  if (backendId === 'webnn') {
    if (isHeavy) return 128;
    if (isMedium) return 192;
    return 256;
  }
  // wasm cpu
  if (isHeavy) return 128;
  if (isMedium) return 160;
  return 192;
}

function isOomLikeError(err) {
  if (!err) return false;
  if (err.isOom) return true;
  const msg = (err.message || String(err)).toLowerCase();
  return msg.includes('out of memory')
    || msg.includes('oom')
    || msg.includes('超限')
    || msg.includes('分配失败')
    || msg.includes('buffer size')
    || msg.includes('allocation');
}

function nextSmallerTileSize(current) {
  for (const step of TILE_SHRINK_LADDER) {
    if (step < current) return step;
  }
  return 0;
}

function normalizePngLevel(pngLevel) {
  if (typeof pngLevel === 'number' && Number.isFinite(pngLevel)) {
    return pngLevel;
  }
  const key = String(pngLevel ?? 'auto').toLowerCase();
  if (key === 'fast') return 1;
  if (key === 'best') return 9;
  return -1;
}

function normalizePositiveInt(value) {
  if (value === undefined || value === null || value === 'auto') return 0;
  const n = Number(value);
  return Number.isFinite(n) && n > 0 ? Math.floor(n) : 0;
}

// Model families where WebNN (LiteRT) warm GPU throughput beats WebGPU by 2.5x-4.1x
// (`docs/webnn-benchmark.md`): Real-CUGAN (Deconvolution) and RealPLKSR (17x17 large kernel).
const WEBNN_WARM_MODEL_KEYS = ['cugan', 'plksr'];

/**
 * WebNN GPU graph is "warm" for a model when `WebNnBackend.graphCache` still holds a compiled
 * graph keyed `${modelName}:gpu:<powerPreference>:<W>x<H>x<C>` — i.e. this exact model was
 * already built and dispatched on the GPU context earlier in this session.
 */
function hasWarmWebNnGpuGraph(webnnBackend, modelName) {
  if (!webnnBackend?.graphCache || typeof webnnBackend.graphCache.keys !== 'function' || !modelName) {
    return false;
  }
  const prefix = `${modelName}:gpu:`;
  for (const key of webnnBackend.graphCache.keys()) {
    if (typeof key === 'string' && key.startsWith(prefix)) {
      return true;
    }
  }
  return false;
}

/**
 * Architecture- & batch-aware `backend: 'auto'` routing.
 * Benchmark facts (`docs/webnn-benchmark.md`):
 * - WebGPU compiles pipelines in ~4-50 ms and already wins on 3x3 / 1x1 families
 *   (SPAN, Compact, ESRGANv3), so it stays the default single-image choice;
 * - WebNN beats WebGPU by 2.5x-4.1x warm on Real-CUGAN and RealPLKSR graphs, once its
 *   slower cold compile is amortized (warm graph cache or a >= 2 image batch);
 * - without WebGPU, even WebNN CPU (XNNPACK) beats a 12-worker WASM SIMD pool by ~1.8-2.5x,
 *   so WebNN (gpu/npu/cpu) routes ahead of the WASM fallback. Callers keep the
 *   `webnn -> webgpu -> wasm` failure chain in `processImage` as the safety net.
 * @param {{
 *   caps: object,
 *   modelName?: string,
 *   batchSizeHint?: number,
 *   webnnBackend?: object,
 * }} opts
 * @returns {{ backend: 'webgpu' | 'webnn' | 'wasm', webnnDevice: 'gpu' | 'npu' | 'cpu' }}
 */
export function resolveAutoBackend({ caps, modelName = '', batchSizeHint = 1, webnnBackend = null } = {}) {
  const webgpuOk = Boolean(caps?.webgpu?.available);
  const webnnOk = Boolean(caps?.webnn?.available);
  const devices = caps?.webnn?.features?.webnnDevices || {};
  const batch = Math.max(1, Math.floor(Number(batchSizeHint) || 1));
  const lower = String(modelName || '').toLowerCase();

  // No WebGPU at all: WebNN is the only hardware accelerator here.
  if (!webgpuOk && webnnOk) {
    const webnnDevice = devices.gpu ? 'gpu' : (devices.npu ? 'npu' : 'cpu');
    return { backend: 'webnn', webnnDevice };
  }

  if (webgpuOk && webnnOk && devices.gpu) {
    const warmGraph = hasWarmWebNnGpuGraph(webnnBackend, modelName);
    const webnnFavoured = WEBNN_WARM_MODEL_KEYS.some((k) => lower.includes(k));
    if (warmGraph || (batch >= 2 && webnnFavoured)) {
      return { backend: 'webnn', webnnDevice: 'gpu' };
    }
  }

  return { backend: webgpuOk ? 'webgpu' : 'wasm', webnnDevice: 'gpu' };
}

export class UpixRuntime {
  constructor(config = {}) {
    this.config = config;
    this.initPromise = null;
    this.wasm = null;
    this.modelStore = null;
    this.cpuBackend = null;
    this.webgpuBackend = new WebGpuBackend();
    this.webnnBackend = new WebNnBackend();
    this.webnnAutoDisabled = false; // latched when auto-routed WebNN fails; degrades to WebGPU/WASM
    this.cachedCapabilities = null;
    this.modelMetaCache = new Map(); // modelName -> { scale, modelChannels, binLen }
  }

  async init() {
    if (this.wasm) return this;
    if (!this.initPromise) {
      this.initPromise = (async () => {
        this.wasm = await UpixWasmBridge.create(this.config.wasmUrl);
        this.modelStore = new UpixModelStore(this.wasm);
        this.cpuBackend = new WasmCpuBackend(this.wasm);
        this.cachedCapabilities = null;
        return this;
      })().catch((err) => {
        this.initPromise = null;
        throw err;
      });
    }
    return this.initPromise;
  }

  getModelBriefMeta(modelPayload) {
    const binLen = modelPayload.binBytes ? modelPayload.binBytes.byteLength : 0;
    const cached = this.modelMetaCache.get(modelPayload.name);
    if (cached && cached.binLen === binLen) {
      return cached;
    }
    const probeLoaded = this.wasm.loadModel(
      modelPayload.name,
      modelPayload.paramText,
      modelPayload.binBytes,
    );
    try {
      const entry = {
        scale: Math.max(1, probeLoaded.meta.scale_factor || 2),
        modelChannels: probeLoaded.meta.input_channels === 1 ? 1 : 3,
        binLen,
      };
      this.modelMetaCache.set(modelPayload.name, entry);
      return entry;
    } finally {
      probeLoaded.dispose();
    }
  }

  async getCapabilities({ refresh = false } = {}) {
    if (this.cachedCapabilities && !refresh) {
      return this.cachedCapabilities;
    }

    let wasmAvailable = false;
    let wasmReason;
    try {
      await this.init();
      wasmAvailable = true;
    } catch (err) {
      wasmReason = err instanceof Error ? err.message : String(err);
    }

    const [webgpu, webnn] = await Promise.all([
      WebGpuBackend.probe(),
      WebNnBackend.probe(),
    ]);

    const hwThreads = (typeof navigator !== 'undefined' && navigator.hardwareConcurrency) || 4;
    const threads = Math.max(1, Math.min(hwThreads, MAX_WASM_WORKERS));
    const simd128 = supportsWasmSimd128();
    const isolated = globalThis.crossOriginIsolated === true;
    const wasm = {
      id: 'wasm',
      available: wasmAvailable,
      label: 'WASM (CPU)',
      deviceName: wasmAvailable
        ? this.cpuBackend.getDeviceName(defaultThreadCount())
        : t('common.unavailable'),
      reason: wasmReason,
      features: {
        simd128,
        threads,
        // What '自动' resolves to for the next run — surfaced in the UI.
        autoThreads: defaultThreadCount(),
        crossOriginIsolated: isolated,
      },
    };

    this.cachedCapabilities = { webgpu, wasm, webnn };
    return this.cachedCapabilities;
  }

  async probeWebNnDevice(deviceType = 'gpu', powerPreference = 'default') {
    return WebNnBackend.probeDevice(deviceType, powerPreference);
  }

  async listModels(options = {}) {
    await this.init();
    return this.modelStore.listModels(options);
  }

  async downloadModel(name, options = {}) {
    await this.init();
    this.modelMetaCache.delete(name);
    return this.modelStore.downloadModel(name, options);
  }

  async importModelFiles(fileListOrArray) {
    await this.init();
    this.modelMetaCache.clear();
    return this.modelStore.importModelFiles(fileListOrArray);
  }

  async deleteModel(name) {
    await this.init();
    this.modelMetaCache.delete(name);
    return this.modelStore.deleteModel(name);
  }

  async clearModelCache() {
    await this.init();
    this.modelMetaCache.clear();
    return this.modelStore.clearCache();
  }

  async probeImage(fileOrBlob) {
    await this.init();
    return decodeInputImage(fileOrBlob, this.wasm, { metadataOnly: true });
  }

  async resolveModelPayload(modelName) {
    if (!modelName || modelName === SYNTH_BENCHMARK_MODEL_NAME) {
      return getSyntheticBenchmarkModel();
    }
    try {
      return await this.modelStore.getModelPayload(modelName);
    } catch (err) {
      const catalog = this.wasm.getCatalog();
      const aliasMatch = catalog.find(
        (c) => c.short_name === modelName || c.name.toLowerCase() === modelName.toLowerCase(),
      );
      if (aliasMatch && aliasMatch.name !== modelName) {
        return this.modelStore.getModelPayload(aliasMatch.name);
      }
      throw err;
    }
  }

  async resolveBackend(requestedBackend = 'auto') {
    const req = (requestedBackend || 'auto').toLowerCase();
    if (req === 'webgpu' || req === 'wasm' || req === 'webnn') {
      return req;
    }
    const caps = await this.getCapabilities();
    if (caps.webgpu.available) return 'webgpu';
    return 'wasm';
  }

  async processImage(fileOrBlob, options = {}) {
    await this.init();
    const tStart = performance.now();

    const {
      modelName = 'AnimeJaNai-v2-Compact-x2',
      backend = 'auto',
      webnnDevice: rawWebnnDevice = 'gpu',
      webnnPower,
      webnnPowerPreference,
      tileSize: rawTileSize,
      tileOverlap = 16,
      preferFp16 = true,
      wasmThreads: rawWasmThreads,
      batchSizeHint = 1,
      outputFormat = 'follow',
      quality = 92,
      jpegSubsampling: rawJpegSubsampling = 420,
      pngLevel: rawPngLevel = 'auto',
      webpLossless = false,
      force = false,
      signal,
      onProgress,
    } = options;

    const powerPref = webnnPower || webnnPowerPreference || 'high-performance';
    // Batch callers (e.g. `upscaleBatch`) pass `batchSizeHint = files.length` so the auto
    // router can amortize WebNN's cold compile across the batch; default is a single image.
    const hintBatchSize = Math.max(1, Math.floor(Number(batchSizeHint) || 1));
    let webnnDevice = rawWebnnDevice;
    const tileSize = normalizePositiveInt(rawTileSize);
    const wasmThreads = normalizePositiveInt(rawWasmThreads);
    const jpegSubsampling = Number(rawJpegSubsampling) || 420;
    const pngLevel = normalizePngLevel(rawPngLevel);

    const emitProgress = (patch) => {
      if (typeof onProgress === 'function') {
        onProgress(patch);
      }
    };

    if (signal && signal.aborted) {
      throw new DOMException(t('common.cancelled'), 'AbortError');
    }

    const reqBackend = (backend || 'auto').toLowerCase();
    const explicitBackend = reqBackend === 'webgpu' || reqBackend === 'webnn' || reqBackend === 'wasm';
    const caps = await this.getCapabilities();
    let backendUsed;
    if (explicitBackend) {
      // Explicit `backend: 'webgpu' | 'webnn' | 'wasm'` (e.g. bench-webnn.mjs): never rerouted.
      backendUsed = reqBackend;
    } else {
      const auto = resolveAutoBackend({
        caps,
        modelName,
        batchSizeHint: hintBatchSize,
        webnnBackend: this.webnnBackend,
      });
      if (auto.backend === 'webnn' && this.webnnAutoDisabled) {
        backendUsed = caps.webgpu?.available ? 'webgpu' : 'wasm';
      } else {
        backendUsed = auto.backend;
        if (auto.backend === 'webnn' && options.webnnDevice == null && auto.webnnDevice) {
          webnnDevice = auto.webnnDevice;
        }
      }
    }

    emitProgress({
      phase: 'decode',
      completedTiles: 0,
      totalTiles: 0,
      percentage: 2,
      tileMs: 0,
      tilesPerSec: 0,
      etaMs: 0,
      backendUsed,
      tileSizeUsed: tileSize || 0,
    });

    const decoded = await decodeInputImage(fileOrBlob, this.wasm, { metadataOnly: false });
    const { width: inW, height: inH, channels: srcChannels, srMarker, format: inputFormat } = decoded;

    if (!force && srMarker) {
      return {
        skipped: true,
        skipReason: t('runtime.skipSrMarker', {
          model: srMarker.model || 'unknown',
          scale: srMarker.scale || '?',
        }),
        existingMarker: srMarker,
        inputWidth: inW,
        inputHeight: inH,
        outputWidth: inW,
        outputHeight: inH,
        scale: srMarker.scale || 1,
        backendUsed,
        deviceName: caps[backendUsed]?.deviceName || '',
        elapsedMs: performance.now() - tStart,
        tilesTotal: 0,
        tileSizeUsed: 0,
      };
    }

    emitProgress({
      phase: 'load-model',
      completedTiles: 0,
      totalTiles: 0,
      percentage: 4,
      tileMs: 0,
      tilesPerSec: 0,
      etaMs: 0,
      backendUsed,
      tileSizeUsed: tileSize || 0,
    });

    const modelPayload = await this.resolveModelPayload(modelName);
    const { scale, modelChannels } = this.getModelBriefMeta(modelPayload);

    const outW = inW * scale;
    const outH = inH * scale;
    const resolvedFormat = isFollowOutputFormat(outputFormat)
      ? normalizeOutputFormat(inputFormat)
      : normalizeOutputFormat(outputFormat);
    const supportsAlphaOut = resolvedFormat !== 'jpeg';
    const outChannels = srcChannels === 4 && supportsAlphaOut ? 4 : modelChannels;

    let alphaScaled = null;
    if (srcChannels === 4 && outChannels === 4) {
      const pxCount = inW * inH;
      const alphaIn = new Uint8Array(pxCount);
      for (let i = 0; i < pxCount; i += 1) {
        alphaIn[i] = decoded.pixels[i * 4 + 3];
      }
      alphaScaled = this.wasm.bicubicScaleU8(alphaIn, inW, inH, outW, outH);
    }

    const pinnedInput = this.wasm.pinImageU8(decoded.pixels, inW, inH, srcChannels);
    let currentTileSize = tileSize > 0 ? tileSize : chooseAutoTileSize(backendUsed, modelPayload.name);
    let finalPixelsU8;
    let deviceName;
    let tilesTotal = 0;

    try {
      while (true) {
        if (signal && signal.aborted) {
          throw new DOMException(t('common.cancelled'), 'AbortError');
        }

        const effectiveOverlap = Math.max(0, Math.min(tileOverlap, Math.floor(currentTileSize / 4)));
        const regions = this.wasm.computeTileLayout(inW, inH, currentTileSize, effectiveOverlap);
        tilesTotal = regions.length;

        const blendSession = this.wasm.createBlendSession(outW, outH, modelChannels, outChannels);
        let completedCount = 0;
        let sumTileMs = 0;

        emitProgress({
          phase: backendUsed === 'wasm' ? 'infer' : 'compile',
          completedTiles: 0,
          totalTiles: tilesTotal,
          percentage: 6,
          tileMs: 0,
          tilesPerSec: 0,
          etaMs: 0,
          backendUsed,
          tileSizeUsed: currentTileSize,
        });

        const handleTileDone = (_idx, region, tileOutF32, tileMs) => {
          blendSession.blendTile(tileOutF32, region, scale, effectiveOverlap);
          completedCount += 1;
          sumTileMs += tileMs;
          const avgMs = sumTileMs / completedCount;
          const remaining = Math.max(0, tilesTotal - completedCount);
          const concurrency = backendUsed === 'wasm'
            ? Math.max(1, Math.min(wasmThreads || defaultThreadCount(), tilesTotal, systemThreadMax()))
            : 1;
          const effectiveTileMs = avgMs / concurrency;
          const etaMs = remaining * effectiveTileMs;
          const percentage = Math.min(94, Math.round(6 + (completedCount / Math.max(1, tilesTotal)) * 88));

          emitProgress({
            phase: 'infer',
            completedTiles: completedCount,
            totalTiles: tilesTotal,
            percentage,
            tileMs: effectiveTileMs,
            tilesPerSec: effectiveTileMs > 0 ? 1000 / effectiveTileMs : 0,
            etaMs,
            backendUsed,
            tileSizeUsed: currentTileSize,
          });
        };

        try {
          if (backendUsed === 'webgpu') {
            const res = await this.webgpuBackend.runTiles({
              wasmBridge: this.wasm,
              modelPayload,
              regions,
              extractTile: (r, ch) => pinnedInput.extractTile(r, ch),
              channels: modelChannels,
              preferFp16,
              signal,
              onCompiled: () => emitProgress({
                phase: 'infer',
                completedTiles: 0,
                totalTiles: tilesTotal,
                percentage: 6,
                tileMs: 0,
                tilesPerSec: 0,
                etaMs: 0,
                backendUsed,
                tileSizeUsed: currentTileSize,
              }),
              onTileDone: handleTileDone,
            });
            deviceName = res.deviceName;
          } else if (backendUsed === 'webnn') {
            const res = await this.webnnBackend.runTiles({
              wasmBridge: this.wasm,
              modelPayload,
              regions,
              extractTile: (r, ch) => pinnedInput.extractTile(r, ch),
              channels: modelChannels,
              deviceType: webnnDevice,
              powerPreference: powerPref,
              signal,
              onPhase: () => emitProgress({
                phase: 'compile',
                completedTiles: completedCount,
                totalTiles: tilesTotal,
                percentage: 6,
                tileMs: 0,
                tilesPerSec: 0,
                etaMs: 0,
                backendUsed,
                tileSizeUsed: currentTileSize,
              }),
              onTileDone: handleTileDone,
            });
            deviceName = res.deviceName;
          } else {
            const res = await this.cpuBackend.runTiles({
              modelPayload,
              regions,
              extractTile: (r, ch) => pinnedInput.extractTile(r, ch),
              channels: modelChannels,
              scale,
              threads: wasmThreads,
              signal,
              onTileDone: handleTileDone,
            });
            deviceName = res.deviceName;
          }

          emitProgress({
            phase: 'blend',
            completedTiles: tilesTotal,
            totalTiles: tilesTotal,
            percentage: 95,
            tileMs: completedCount > 0 ? sumTileMs / completedCount : 0,
            tilesPerSec: 0,
            etaMs: 0,
            backendUsed,
            tileSizeUsed: currentTileSize,
          });

          finalPixelsU8 = blendSession.finalizeToU8(alphaScaled);
          blendSession.dispose();
          break;
        } catch (err) {
          blendSession.dispose();
          if (err && err.name === 'AbortError') {
            throw err;
          }
          if (isOomLikeError(err)) {
            const smaller = nextSmallerTileSize(currentTileSize);
            if (smaller > 0) {
              currentTileSize = smaller;
              continue;
            }
          }
          if (reqBackend === 'auto' && backendUsed === 'webnn') {
            // Auto only: cleanly degrade `webnn -> webgpu -> wasm` when WebNN fails.
            this.webnnAutoDisabled = true;
            backendUsed = caps.webgpu?.available ? 'webgpu' : 'wasm';
            currentTileSize = tileSize > 0 ? tileSize : chooseAutoTileSize(backendUsed, modelPayload.name);
            continue;
          }
          if (reqBackend === 'auto' && backendUsed === 'webgpu') {
            backendUsed = 'wasm';
            currentTileSize = tileSize > 0 ? tileSize : chooseAutoTileSize('wasm', modelPayload.name);
            continue;
          }
          throw err;
        }
      }
    } finally {
      pinnedInput.dispose();
    }

    emitProgress({
      phase: 'encode',
      completedTiles: tilesTotal,
      totalTiles: tilesTotal,
      percentage: 98,
      tileMs: 0,
      tilesPerSec: 0,
      etaMs: 0,
      backendUsed,
      tileSizeUsed: currentTileSize,
    });

    const xmp = this.wasm.buildSrXmp(modelPayload.name, scale);
    const encoded = await encodeOutputImage(
      this.wasm,
      finalPixelsU8,
      outW,
      outH,
      outChannels,
      { outputFormat: resolvedFormat, quality, jpegSubsampling, pngLevel, webpLossless },
      xmp,
    );

    const filename = formatOutputFilename(
      fileOrBlob.name || 'image',
      modelPayload.name,
      scale,
      encoded.ext,
    );
    const elapsedMs = performance.now() - tStart;

    emitProgress({
      phase: 'encode',
      completedTiles: tilesTotal,
      totalTiles: tilesTotal,
      percentage: 100,
      tileMs: 0,
      tilesPerSec: 0,
      etaMs: 0,
      backendUsed,
      tileSizeUsed: currentTileSize,
    });

    return {
      skipped: false,
      blob: encoded.blob,
      bytes: encoded.bytes,
      mimeType: encoded.mimeType,
      filename,
      inputWidth: inW,
      inputHeight: inH,
      outputWidth: outW,
      outputHeight: outH,
      scale,
      backendUsed,
      deviceName,
      elapsedMs,
      tilesTotal,
      tileSizeUsed: currentTileSize,
    };
  }

  async benchmark({
    backend = 'webgpu',
    modelName = '',
    tileSize = 128,
    preferFp16 = true,
    webnnDevice = 'gpu',
    webnnPower,
    webnnPowerPreference,
    wasmThreads = 1,
  } = {}) {
    await this.init();
    const resolvedBackend = await this.resolveBackend(backend);
    const powerPref = webnnPower || webnnPowerPreference || 'high-performance';

    let modelPayload = null;
    if (modelName && modelName !== SYNTH_BENCHMARK_MODEL_NAME) {
      try {
        modelPayload = await this.resolveModelPayload(modelName);
      } catch {
        modelPayload = null;
      }
    }
    if (!modelPayload) {
      const { models } = await this.modelStore.listModels().catch(() => ({ models: [] }));
      const firstInstalled = models.find((m) => m.installed);
      if (firstInstalled) {
        modelPayload = await this.modelStore.getModelPayload(firstInstalled.name).catch(() => null);
      }
    }
    if (!modelPayload) {
      modelPayload = getSyntheticBenchmarkModel();
    }

    const { scale, modelChannels: channels } = this.getModelBriefMeta(modelPayload);

    let res;
    if (resolvedBackend === 'webgpu') {
      res = await this.webgpuBackend.benchmarkTile({
        wasmBridge: this.wasm,
        modelPayload,
        tileSize,
        channels,
        preferFp16,
      });
    } else if (resolvedBackend === 'webnn') {
      res = await this.webnnBackend.benchmarkTile({
        wasmBridge: this.wasm,
        modelPayload,
        tileSize,
        channels,
        deviceType: webnnDevice,
        powerPreference: powerPref,
      });
    } else {
      const threads = wasmThreads === 'auto' || wasmThreads === 0
        ? 0
        : Math.min(normalizePositiveInt(wasmThreads) || 1, systemThreadMax());
      res = await this.cpuBackend.benchmarkTile({
        modelPayload,
        tileSize,
        channels,
        scale,
        threads,
      });
    }

    const avgTileMs = Math.max(0.05, res.avgTileMs);
    const tilesPerSec = 1000 / avgTileMs;
    const mpixPerSec = ((tileSize * tileSize) / 1_000_000) * tilesPerSec;

    return {
      backend: resolvedBackend,
      deviceName: res.deviceName,
      modelName: modelPayload.name,
      tileSize,
      avgTileMs,
      tilesPerSec,
      mpixPerSec,
    };
  }

  async buildZipArchive(entries) {
    return createZipBlob(entries);
  }

  dispose() {
    if (this.cpuBackend) this.cpuBackend.dispose();
    if (this.webgpuBackend) this.webgpuBackend.dispose();
    if (this.webnnBackend) this.webnnBackend.dispose();
  }
}

export function createUpixRuntime(config = {}) {
  return new UpixRuntime(config);
}

export { SYNTH_BENCHMARK_MODEL_NAME };
