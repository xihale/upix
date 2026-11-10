/**
 * Tile blending — port of the engine's Vulkan blend_tile + normalize_by_weight
 * semantics (engine/core/vulkan_shaders.hpp kBlendTileComp / kNormalizeByWeightComp
 * and the dispatch constants in ncnn_gpu_pipeline.cpp):
 *
 *   - weights live in OUTPUT space: tile dims and overlap are scaled by the model
 *     scale factor (blend_constants[7] = config.overlap * scale_factor)
 *   - blend_weight is a squared ramp: w = t^2 where t goes 0→1 across the
 *     effective overlap at each interior edge (1.0 at image borders and on
 *     non-overlapped sides — conditions use the tile's output start vs full size)
 *   - per-pixel weight is accumulated on channel 0's plane and the accumulated
 *     color is divided by it at the end (guard: weight <= 1e-6 → passthrough 0)
 *
 * Accumulators are HWC float (like state.out_gpu_hwc); tile input is CHW float
 * (ncnn's native layout, which ORT NCHW matches).
 */

export function blendWeight(
  pos: number,
  size: number,
  overlap: number,
  tileStart: number,
  fullSize: number,
): number {
  const effectiveOverlap = Math.min(overlap, size);
  if (effectiveOverlap <= 0) return 1.0;
  let w = 1.0;
  if (tileStart > 0 && pos < effectiveOverlap) {
    w = (pos + 1) / effectiveOverlap;
  } else if (tileStart + size < fullSize && pos >= size - effectiveOverlap) {
    w = (size - pos) / effectiveOverlap;
  }
  return w * w;
}

export class TileBlender {
  readonly outWidth: number;
  readonly outHeight: number;
  readonly channels: number;
  /** HWC float accumulator */
  readonly accum: Float32Array;
  /** per-pixel weight plane */
  readonly weights: Float32Array;

  constructor(outWidth: number, outHeight: number, channels: number) {
    this.outWidth = outWidth;
    this.outHeight = outHeight;
    this.channels = channels;
    this.accum = new Float32Array(outWidth * outHeight * channels);
    this.weights = new Float32Array(outWidth * outHeight);
  }

  /**
   * @param tileData CHW float, dims = region.width*scale × region.height*scale
   * @param region   input-space tile region (x/y/width/height)
   * @param scale    model scale factor
   * @param overlap  input-space overlap (scaled internally, per engine)
   */
  addTile(tileData: Float32Array, region: { x: number; y: number; width: number; height: number }, scale: number, overlap: number): void {
    const tileW = region.width * scale;
    const tileH = region.height * scale;
    const outX = region.x * scale;
    const outY = region.y * scale;
    const scaledOverlap = overlap * scale;
    const fullW = this.outWidth;
    const fullH = this.outHeight;
    const { channels } = this;
    const { accum, weights } = this;

    for (let gy = 0; gy < tileH; gy++) {
      const outPy = outY + gy;
      if (outPy < 0 || outPy >= fullH) continue;
      const wy = blendWeight(gy, tileH, scaledOverlap, outY, fullH);
      const srcRow = gy * tileW;
      const dstRow = outPy * fullW;
      for (let gx = 0; gx < tileW; gx++) {
        const outPx = outX + gx;
        if (outPx < 0 || outPx >= fullW) continue;
        const wx = blendWeight(gx, tileW, scaledOverlap, outX, fullW);
        const w = wx * wy;
        const srcXY = srcRow + gx;
        const dstIdx = (dstRow + outPx) * channels;
        for (let c = 0; c < channels; c++) {
          accum[dstIdx + c] = (accum[dstIdx + c] as number) + tileData[c * tileW * tileH + srcXY]! * w;
        }
        weights[dstRow + outPx] = (weights[dstRow + outPx] as number) + w;
      }
    }
  }

  /** Divide accumulated color by the weight plane into an RGBA8 bitmap buffer. */
  normalizeIntoRgba8(dst: Uint8ClampedArray): void {
    this.forEachNormalized((v, i) => {
      dst[i * 4] = Math.round(Math.min(255, Math.max(0, v[0] * 255)));
      dst[i * 4 + 1] = Math.round(Math.min(255, Math.max(0, v[1] * 255)));
      dst[i * 4 + 2] = Math.round(Math.min(255, Math.max(0, v[2] * 255)));
      dst[i * 4 + 3] = 255;
    });
  }

  /**
   * Normalized HWC float [0,1] — the domain the engine's tiled==whole gate
   * (CONVERTING.md §5, ≥99 dB) is measured in.
   */
  normalizeIntoF32(dst: Float32Array): void {
    this.forEachNormalized((v, i) => {
      dst[i * 3] = v[0];
      dst[i * 3 + 1] = v[1];
      dst[i * 3 + 2] = v[2];
    });
  }

  private forEachNormalized(fn: (v: [number, number, number], i: number) => void): void {
    const { accum, weights, channels } = this;
    const px = this.outWidth * this.outHeight;
    for (let i = 0; i < px; i++) {
      const w = weights[i]!;
      if (w <= 1e-6) {
        fn([0, 0, 0], i);
        continue;
      }
      fn([
        accum[i * channels]! / w,
        accum[i * channels + 1]! / w,
        accum[i * channels + 2]! / w,
      ], i);
    }
  }
}
