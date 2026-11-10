/**
 * ONNX Runtime session management with execution-provider negotiation.
 *
 * EP order (issue #2 path B1): WebGPU when the browser exposes it, wasm
 * otherwise. `ort-webgpu` failures at session creation (old drivers, EP
 * miscompiles) fall back too — same philosophy as the engine's Vulkan init:
 * probe, fall back, log.
 *
 * WebNN (path C experiment): `executionProviders: ['webnn']` requires the
 * `onnxruntime-web/all` entrypoint; single-partition coverage is model-side
 * (see tools/make_dev_model.py --decompose-pixelshuffle). Exposed as an
 * experimental opt-in, off by default.
 */
import * as ortWebgpu from 'onnxruntime-web/webgpu';

export type ExecutionProvider = 'auto' | 'webgpu' | 'wasm' | 'webnn';

export interface SessionInfo {
  session: ortWebgpu.InferenceSession;
  ep: 'webgpu' | 'wasm' | 'webnn';
  notes: string[];
}

let envConfigured = false;

function configureEnv(): string[] {
  const notes: string[] = [];
  if (envConfigured) return notes;
  envConfigured = true;
  const ort = ortWebgpu.env;
  // Serve the wasm/jsep runtime from our own origin (public/ort/, copied by
  // the vite config) — the app stays fully local, no CDN.
  // Absolute URL required: ORT dynamic-import()s the .mjs glue, and a bare
  // relative specifier ('ort/…') is unresolvable from a page module.
  ort.wasm.wasmPaths = new URL('ort/', document.baseURI).href;
  // SharedArrayBuffer/threads need COOP/COEP; GitHub Pages can't set response
  // headers (see issue #2), so threads are opt-in on crossOriginIsolated only.
  if (typeof crossOriginIsolated !== 'undefined' && crossOriginIsolated) {
    ort.wasm.numThreads = Math.min(4, Math.max(1, (navigator.hardwareConcurrency || 2) - 1));
    notes.push(`wasm threads: ${ort.wasm.numThreads} (crossOriginIsolated)`);
  } else {
    ort.wasm.numThreads = 1;
  }
  ort.wasm.simd = true;
  ort.logLevel = 'warning';
  return notes;
}

let webGpuAdapter: any = null;
let webGpuProbed = false;

/**
 * WebGPU availability = an adapter actually exists, NOT merely that
 * `navigator.gpu` is exposed. Chrome Android exposes the API on versions /
 * devices without a usable adapter (e.g. Android < 12), and reporting ✓ there
 * both misleads the user and sends `auto` down a path that can only fall back.
 */
export async function probeWebGpu(): Promise<boolean> {
  if (webGpuProbed) return webGpuAdapter !== null;
  webGpuProbed = true;
  if (typeof navigator === 'undefined' || !('gpu' in navigator)) return false;
  try {
    webGpuAdapter = await (navigator as any).gpu.requestAdapter();
  } catch {
    webGpuAdapter = null;
  }
  return webGpuAdapter !== null;
}

export function webGpuAdapterInfo(): string {
  const info = webGpuAdapter?.info ?? {};
  return info.description || info.device || info.vendor || 'unknown adapter';
}

export function hasWebNn(): boolean {
  return typeof navigator !== 'undefined' && 'ml' in navigator;
}

export async function createSession(
  modelBytes: Uint8Array,
  choice: ExecutionProvider,
): Promise<SessionInfo> {
  const notes = configureEnv();
  const canGpu = await probeWebGpu();
  const wants = choice === 'auto' ? (canGpu ? 'webgpu' : 'wasm') : choice;

  if (wants === 'webnn') {
    if (!hasWebNn()) throw new Error('WebNN (navigator.ml) not available in this browser');
    notes.push('WebNN is experimental (issue #2 path C): Chromium-only, needs a single-partition graph');
    // ORT's typed WebNN option surface still churns (issue #2 path C);
    // the runtime accepts these fields, the type defs lag.
    const session = await ortWebgpu.InferenceSession.create(modelBytes, {
      executionProviders: [{ name: 'webnn', deviceType: 'gpu', powerPreference: 'low-performance' }],
    } as unknown as Parameters<typeof ortWebgpu.InferenceSession.create>[1]);
    return { session, ep: 'webnn', notes };
  }

  if (wants === 'webgpu') {
    try {
      if (!canGpu) throw new Error('navigator.gpu has no adapter on this device');
      notes.push(`webgpu adapter: ${webGpuAdapterInfo()}`);
      const session = await ortWebgpu.InferenceSession.create(modelBytes, {
        executionProviders: ['webgpu'],
      });
      return { session, ep: 'webgpu', notes };
    } catch (err) {
      notes.push(`webgpu EP failed (${err instanceof Error ? err.message : String(err)}), falling back to wasm`);
    }
  }

  const session = await ortWebgpu.InferenceSession.create(modelBytes, {
    executionProviders: ['wasm'],
  });
  return { session, ep: 'wasm', notes };
}
