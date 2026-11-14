// WASM (CPU) SIMD128 multi-Worker tile backend (`WasmCpuBackend`).

import { supportsWasmSimd128 } from '../wasm-bridge.js';
import { getLocale, t } from '../../i18n/index.js';

export const MAX_WASM_WORKERS = 16;

export function defaultThreadCount() {
  const hw = (typeof navigator !== 'undefined' && navigator.hardwareConcurrency) || 4;
  return Math.max(1, Math.min(hw, 12));
}

/** Hard ceiling for worker count on this device: cores, never above 16. */
export function systemThreadMax() {
  const hw = (typeof navigator !== 'undefined' && navigator.hardwareConcurrency) || 4;
  return Math.max(1, Math.min(hw, MAX_WASM_WORKERS));
}

class WorkerSlot {
  constructor(worker) {
    this.worker = worker;
    this.loadedModelName = '';
    this.pending = new Map();
    this.nextReqId = 1;

    this.worker.onmessage = (ev) => {
      const data = ev.data || {};
      const entry = this.pending.get(data.reqId);
      if (!entry) return;
      this.pending.delete(data.reqId);
      if (data.type === 'error') {
        entry.reject(new Error(data.message || t('runtime.workerFailed')));
      } else {
        entry.resolve(data);
      }
    };

    this.worker.onerror = (err) => {
      const msg = err?.message || t('runtime.workerExited');
      for (const entry of this.pending.values()) {
        entry.reject(new Error(msg));
      }
      this.pending.clear();
    };
  }

  request(payload, transfer = []) {
    const reqId = this.nextReqId;
    this.nextReqId += 1;
    return new Promise((resolve, reject) => {
      this.pending.set(reqId, { resolve, reject });
      this.worker.postMessage({ ...payload, reqId }, transfer);
    });
  }

  terminate() {
    for (const entry of this.pending.values()) {
      entry.reject(new DOMException(t('common.cancelled'), 'AbortError'));
    }
    this.pending.clear();
    this.worker.terminate();
  }
}

export class WasmCpuBackend {
  constructor(hostBridge) {
    this.hostBridge = hostBridge;
    this.simd128 = supportsWasmSimd128();
    this.slots = [];
    this.fallbackSession = null;
    this.workersSupported = typeof Worker !== 'undefined';
  }

  getDeviceName(threads = defaultThreadCount()) {
    const simdTag = this.simd128 ? 'SIMD128' : 'Scalar';
    const workerTag = this.workersSupported && threads > 1 ? `${threads} Workers` : '1 Worker';
    return `WASM ${simdTag} · ${workerTag}`;
  }

  async ensurePool(desiredThreads) {
    if (!this.workersSupported) return;
    const target = Math.max(1, Math.min(desiredThreads || defaultThreadCount(), MAX_WASM_WORKERS));
    while (this.slots.length < target) {
      try {
        const w = new Worker(new URL('../cpu.worker.js', import.meta.url), { type: 'module' });
        const slot = new WorkerSlot(w);
        await slot.request({ type: 'init', wasmModule: this.hostBridge.module, locale: getLocale() });
        this.slots.push(slot);
      } catch {
        this.workersSupported = false;
        this.terminateAll();
        break;
      }
    }
  }

  terminateAll() {
    for (const s of this.slots) {
      s.terminate();
    }
    this.slots = [];
  }

  async prepareModel(modelPayload, desiredThreads) {
    await this.ensurePool(desiredThreads);
    const activeCount = Math.min(
      this.slots.length,
      Math.max(1, Math.min(desiredThreads || defaultThreadCount(), MAX_WASM_WORKERS)),
    );

    if (activeCount > 0) {
      const activeSlots = this.slots.slice(0, activeCount);
      await Promise.all(
        activeSlots.map(async (slot) => {
          if (slot.loadedModelName === modelPayload.name) return;
          const binCopy = modelPayload.binBytes.slice().buffer;
          await slot.request(
            {
              type: 'load_model',
              locale: getLocale(),
              modelName: modelPayload.name,
              paramText: modelPayload.paramText,
              binBuffer: binCopy,
            },
            [binCopy],
          );
          slot.loadedModelName = modelPayload.name;
        }),
      );
      return activeSlots;
    }

    // Fallback when Workers are unavailable
    if (!this.fallbackSession || this.fallbackSession.name !== modelPayload.name) {
      if (this.fallbackSession) this.fallbackSession.loaded.dispose();
      const loaded = this.hostBridge.loadModel(
        modelPayload.name,
        modelPayload.paramText,
        modelPayload.binBytes,
      );
      this.fallbackSession = { name: modelPayload.name, loaded };
    }
    return [];
  }

  /**
   * Execute tiled inference across the Worker pool, streaming completed tiles into `onTileDone`.
   */
  async runTiles({
    modelPayload,
    regions,
    extractTile,
    channels,
    scale,
    threads,
    signal,
    onTileDone,
  }) {
    const activeThreads = Math.max(
      1,
      Math.min(threads || defaultThreadCount(), regions.length, MAX_WASM_WORKERS),
    );
    const activeSlots = await this.prepareModel(modelPayload, activeThreads);

    if (activeSlots.length === 0) {
      // Single-thread cooperative fallback
      for (let i = 0; i < regions.length; i += 1) {
        if (signal && signal.aborted) {
          throw new DOMException(t('common.cancelled'), 'AbortError');
        }
        const region = regions[i];
        const tileIn = extractTile(region, channels);
        const t0 = performance.now();
        const tileOut = this.hostBridge.inferTileCpu(
          this.fallbackSession.loaded.handle,
          tileIn,
          region.width,
          region.height,
          channels,
          scale,
        );
        const elapsedMs = performance.now() - t0;
        onTileDone(i, region, tileOut, elapsedMs);
        // Yield so UI can paint progress bar
        await new Promise((r) => setTimeout(r, 0));
      }
      return { deviceName: this.getDeviceName(1) };
    }

    let nextIndex = 0;
    let aborted = false;
    const onAbort = () => {
      aborted = true;
      this.terminateAll();
    };
    if (signal) {
      if (signal.aborted) throw new DOMException(t('common.cancelled'), 'AbortError');
      signal.addEventListener('abort', onAbort, { once: true });
    }

    try {
      const workerLoop = async (slot) => {
        while (!aborted) {
          const idx = nextIndex;
          if (idx >= regions.length) break;
          nextIndex += 1;
          const region = regions[idx];
          const tileIn = extractTile(region, channels);
          const res = await slot.request(
            {
              type: 'infer_tile',
              tileIndex: idx,
              tileBuffer: tileIn.buffer,
              width: region.width,
              height: region.height,
              channels,
              scale,
            },
            [tileIn.buffer],
          );
          const outF32 = new Float32Array(res.outBuffer);
          onTileDone(idx, region, outF32, res.elapsedMs);
        }
      };

      await Promise.all(activeSlots.map((slot) => workerLoop(slot)));
      return { deviceName: this.getDeviceName(activeSlots.length) };
    } finally {
      if (signal) {
        signal.removeEventListener('abort', onAbort);
      }
    }
  }

  async benchmarkTile({ modelPayload, tileSize = 128, channels = 3, scale = 2, threads = 1 }) {
    const requested = threads === 'auto' || threads === 0
      ? defaultThreadCount()
      : Math.max(1, Math.min(Number(threads) || 1, MAX_WASM_WORKERS));
    const activeSlots = await this.prepareModel(modelPayload, requested);
    const dummy = new Float32Array(tileSize * tileSize * channels).fill(0.5);

    if (activeSlots.length > 1) {
      await Promise.all(
        activeSlots.map((slot, idx) => {
          const wBuf = dummy.slice().buffer;
          return slot.request(
            { type: 'infer_tile', tileIndex: idx, tileBuffer: wBuf, width: tileSize, height: tileSize, channels, scale },
            [wBuf],
          );
        }),
      );
      const totalTiles = activeSlots.length * 2;
      let nextTile = 0;
      const t0 = performance.now();
      await Promise.all(
        activeSlots.map(async (slot) => {
          while (true) {
            const idx = nextTile;
            if (idx >= totalTiles) break;
            nextTile += 1;
            const buf = dummy.slice().buffer;
            await slot.request(
              { type: 'infer_tile', tileIndex: idx, tileBuffer: buf, width: tileSize, height: tileSize, channels, scale },
              [buf],
            );
          }
        }),
      );
      const wallMs = performance.now() - t0;
      return {
        avgTileMs: wallMs / totalTiles,
        deviceName: this.getDeviceName(activeSlots.length),
      };
    }

    // Warmup 1 pass + measure 2 passes on single worker
    if (activeSlots.length === 1) {
      const slot = activeSlots[0];
      const wBuf = dummy.slice().buffer;
      await slot.request(
        { type: 'infer_tile', tileIndex: 0, tileBuffer: wBuf, width: tileSize, height: tileSize, channels, scale },
        [wBuf],
      );
      let totalMs = 0;
      const runs = 2;
      for (let i = 0; i < runs; i += 1) {
        const buf = dummy.slice().buffer;
        const res = await slot.request(
          { type: 'infer_tile', tileIndex: i, tileBuffer: buf, width: tileSize, height: tileSize, channels, scale },
          [buf],
        );
        totalMs += res.elapsedMs;
      }
      return {
        avgTileMs: totalMs / runs,
        deviceName: this.getDeviceName(1),
      };
    }

    const h = this.fallbackSession.loaded.handle;
    this.hostBridge.inferTileCpu(h, dummy, tileSize, tileSize, channels, scale);
    const t0 = performance.now();
    this.hostBridge.inferTileCpu(h, dummy, tileSize, tileSize, channels, scale);
    const elapsed = performance.now() - t0;
    return {
      avgTileMs: elapsed,
      deviceName: this.getDeviceName(1),
    };
  }

  dispose() {
    this.terminateAll();
    if (this.fallbackSession) {
      this.fallbackSession.loaded.dispose();
      this.fallbackSession = null;
    }
  }
}
