/**
 * Inference orchestrator — the web counterpart of the engine's tiled
 * process_image (engine/core/tile_processor.cpp): decode → tile → infer →
 * blend → normalize, with progress + cancellation. Same semantics, single
 * thread (ORT owns the GPU); the engine's proc/blend thread split is replaced
 * by cooperative yielding so the UI thread stays responsive.
 */
import type { InferenceSession } from 'onnxruntime-web/webgpu';
import { computeTileLayout, type TileConfig } from './pipeline/layout';
import { TileBlender } from './pipeline/blend';
import { rgbaCropToChw } from './pipeline/tensors';
import type { ExecutionProvider } from './pipeline/session';
import { Tensor } from 'onnxruntime-web/webgpu';

export interface ModelInfo {
  name: string;
  scale: number;
  license: string;
  size_bytes: number;
  sha256: string;
  url: string;
}

export interface RunOptions {
  tileSize: number;
  overlap: number;
  ep: ExecutionProvider;
  signal?: AbortSignal;
  onProgress?: (done: number, total: number, message: string) => void;
}

export interface UpscaleResult {
  width: number;
  height: number;
  rgba: Uint8ClampedArray;
  tiles: number;
  ep: 'webgpu' | 'wasm' | 'webnn';
  notes: string[];
  ms: number;
}

function yieldToUi(): Promise<void> {
  return new Promise((r) => setTimeout(r, 0));
}

/** Decode a blob to an RGBA pixel buffer + dimensions. */
export async function decodeToRgba(source: Blob | ImageBitmap): Promise<{ rgba: Uint8ClampedArray; width: number; height: number }> {
  const bmp = source instanceof ImageBitmap ? source : await createImageBitmap(source);
  const canvas = new OffscreenCanvas(bmp.width, bmp.height);
  const ctx = canvas.getContext('2d', { willReadFrequently: true })!;
  ctx.drawImage(bmp, 0, 0);
  const data = ctx.getImageData(0, 0, bmp.width, bmp.height);
  bmp.close?.();
  return { rgba: data.data, width: bmp.width, height: bmp.height };
}

export async function upscaleImage(
  session: InferenceSession,
  modelScale: number,
  source: { rgba: Uint8ClampedArray; width: number; height: number },
  opts: RunOptions,
): Promise<UpscaleResult> {
  const t0 = performance.now();
  const config: TileConfig = { tileSize: opts.tileSize, overlap: opts.overlap };
  const regions = computeTileLayout(source.width, source.height, config);
  const outW = source.width * modelScale;
  const outH = source.height * modelScale;
  const blender = new TileBlender(outW, outH, 3);
  const inputName = session.inputNames[0]!;

  for (let i = 0; i < regions.length; i++) {
    if (opts.signal?.aborted) throw new DOMException('aborted', 'AbortError');
    const region = regions[i]!;
    const chw = rgbaCropToChw(source.rgba, source.width, region.x, region.y, region.width, region.height);
    const feed = new Tensor('float32', chw, [1, 3, region.height, region.width]);
    const results = await session.run({ [inputName]: feed });
    const out = results[session.outputNames[0]!]!;
    blender.addTile(out.data as Float32Array, region, modelScale, config.overlap);
    opts.onProgress?.(i + 1, regions.length, `tile ${i + 1}/${regions.length}`);
    // cooperative yield: keep the page responsive without a worker for now
    if (i % 2 === 1) await yieldToUi();
  }

  const rgba = new Uint8ClampedArray(outW * outH * 4);
  blender.normalizeIntoRgba8(rgba);
  return {
    width: outW,
    height: outH,
    rgba,
    tiles: regions.length,
    ep: (opts.ep === 'auto' ? 'webgpu' : opts.ep) as UpscaleResult['ep'],
    notes: [],
    ms: performance.now() - t0,
  };
}
