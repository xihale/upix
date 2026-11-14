// Dedicated Web Worker executing WASM SIMD128 ncnn tile inference (`crates/upix-wasm`).

import { UpixWasmBridge } from './wasm-bridge.js';
import { setActiveLocale, t } from '../i18n/index.js';

let bridge = null;
let loadedModel = null; // { name, handle, meta }

self.onmessage = async (event) => {
  const msg = event.data;
  if (!msg || typeof msg !== 'object') return;

  try {
    if (msg.locale) {
      setActiveLocale(msg.locale);
    }
    if (msg.type === 'init') {
      bridge = await UpixWasmBridge.create(msg.wasmModule);
      self.postMessage({ type: 'ready', reqId: msg.reqId });
      return;
    }

    if (msg.type === 'load_model') {
      if (!bridge) {
        bridge = await UpixWasmBridge.create();
      }
      if (loadedModel && loadedModel.name === msg.modelName) {
        self.postMessage({ type: 'model_loaded', reqId: msg.reqId, scale: loadedModel.meta.scale_factor });
        return;
      }
      if (loadedModel) {
        bridge.freeModel(loadedModel.handle);
        loadedModel = null;
      }
      const res = bridge.loadModel(msg.modelName, msg.paramText, new Uint8Array(msg.binBuffer));
      loadedModel = { name: msg.modelName, handle: res.handle, meta: res.meta };
      self.postMessage({ type: 'model_loaded', reqId: msg.reqId, scale: res.meta.scale_factor });
      return;
    }

    if (msg.type === 'infer_tile') {
      if (!bridge || !loadedModel) {
        throw new Error(t('runtime.workerModelNotLoaded'));
      }
      const inputF32 = new Float32Array(msg.tileBuffer);
      const t0 = performance.now();
      const outF32 = bridge.inferTileCpu(
        loadedModel.handle,
        inputF32,
        msg.width,
        msg.height,
        msg.channels,
        msg.scale,
      );
      const elapsedMs = performance.now() - t0;
      self.postMessage(
        {
          type: 'tile_done',
          reqId: msg.reqId,
          tileIndex: msg.tileIndex,
          outBuffer: outF32.buffer,
          elapsedMs,
        },
        [outF32.buffer],
      );
    }
  } catch (err) {
    self.postMessage({
      type: 'error',
      reqId: msg.reqId,
      message: err instanceof Error ? err.message : String(err),
    });
  }
};
