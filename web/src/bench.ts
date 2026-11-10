/**
 * Path-C bench (issue #2): same input, same tile semantics, one run per
 * execution provider. WebNN numbers require a real device (Windows NPU /
 * Android LiteRT backend); on unsupported browsers the row reports why.
 *
 * Trigger with ?bench=1 — results land in the notes area as a table.
 */
import { fetchManifest, fetchModel } from './manifest';
import { createSession, hasWebGpu, hasWebNn, type ExecutionProvider } from './pipeline/session';
import { upscaleImage, type ModelInfo } from './engine';

const BENCH_SIZE = 128; // single tile, 2× → 256×256 output
const ITERATIONS = 3;

function syntheticSource(size = BENCH_SIZE): { rgba: Uint8ClampedArray; width: number; height: number } {
  const rgba = new Uint8ClampedArray(size * size * 4);
  for (let y = 0; y < size; y++) {
    for (let x = 0; x < size; x++) {
      const i = (y * size + x) * 4;
      rgba[i] = (x * 255) / (size - 1);
      rgba[i + 1] = (y * 255) / (size - 1);
      rgba[i + 2] = 128;
      rgba[i + 3] = 255;
    }
  }
  return { rgba, width: size, height: size };
}

function pickModel(models: ModelInfo[], decomposed: boolean): ModelInfo | undefined {
  return models.find((m) => (m as ModelInfo & { pixelshuffle?: string }).pixelshuffle === (decomposed ? 'decomposed' : 'native'));
}

export async function runBench(log: (line: string) => void): Promise<void> {
  const manifest = await fetchManifest();
  const source = syntheticSource();
  const eps: ExecutionProvider[] = ['wasm', 'webgpu', 'webnn'];

  log('bench: 128×128 → 256×256, single tile, 3 timed runs (median)');
  for (const ep of eps) {
    if (ep === 'webgpu' && !hasWebGpu()) {
      log(`webgpu   | unavailable`);
      continue;
    }
    if (ep === 'webnn' && !hasWebNn()) {
      log(`webnn    | unavailable (navigator.ml missing — Chromium-only)`);
      continue;
    }
    // WebNN needs the single-partition (decomposed PixelShuffle) graph
    const model = pickModel(manifest.models, ep === 'webnn');
    if (!model) {
      log(`${ep.padEnd(8)} | no suitable model in manifest`);
      continue;
    }
    try {
      const t0 = performance.now();
      const bytes = await fetchModel(model);
      const { session, notes } = await createSession(bytes, ep);
      const loadMs = performance.now() - t0;
      const times: number[] = [];
      for (let i = 0; i < ITERATIONS + 1; i++) {
        const r = await upscaleImage(session, model.scale, source, { tileSize: BENCH_SIZE, overlap: 16, ep });
        if (i > 0) times.push(r.ms);
      }
      session.release();
      times.sort((a, b) => a - b);
      const median = times[Math.floor(times.length / 2)]!;
      const extra = ep === 'webnn' ? notes.join('; ') : '';
      log(`${ep.padEnd(8)} | ${median.toFixed(0).padStart(6)} ms (load ${loadMs.toFixed(0)} ms) ${extra}`);
    } catch (err) {
      log(`${ep.padEnd(8)} | FAILED: ${err instanceof Error ? err.message : err}`);
    }
  }
  log('bench done — WebNN rows need real NPU hardware to be meaningful (issue #2 path C)');
}
