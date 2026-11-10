/**
 * Tensor conversion helpers — DOM-free numeric core.
 * The engine works in CHW float planes at [0,1] (ncnn from_pixels / the u8
 * upload shaders do exactly this conversion); ORT NCHW matches, so no layout
 * juggling beyond RGBA packing order.
 */

/** RGBA8 crop → CHW float [0,1] (engine HwcCropU8ToChw semantics). */
export function rgbaCropToChw(
  rgba: Uint8ClampedArray | Uint8Array,
  imgW: number,
  x: number,
  y: number,
  w: number,
  h: number,
): Float32Array {
  const planes = w * h;
  const out = new Float32Array(3 * planes);
  for (let gy = 0; gy < h; gy++) {
    const srcRow = ((y + gy) * imgW + x) * 4;
    for (let gx = 0; gx < w; gx++) {
      const s = srcRow + gx * 4;
      const d = gy * w + gx;
      out[d] = rgba[s]! / 255;
      out[planes + d] = rgba[s + 1]! / 255;
      out[2 * planes + d] = rgba[s + 2]! / 255;
    }
  }
  return out;
}

/** NCHW float [0,1] output → RGBA8 (opaque alpha), clamped. */
export function chwToRgba(
  chw: Float32Array,
  width: number,
  height: number,
): Uint8ClampedArray {
  const planes = width * height;
  const out = new Uint8ClampedArray(planes * 4);
  for (let i = 0; i < planes; i++) {
    const d = i * 4;
    out[d] = Math.round(Math.min(1, Math.max(0, chw[i]!)) * 255);
    out[d + 1] = Math.round(Math.min(1, Math.max(0, chw[planes + i]!)) * 255);
    out[d + 2] = Math.round(Math.min(1, Math.max(0, chw[2 * planes + i]!)) * 255);
    out[d + 3] = 255;
  }
  return out;
}

/** PSNR between two same-sized RGBA8 buffers, [0,1] float domain (CONVERTING.md §5 gate). */
export function psnrRgba(a: Uint8ClampedArray | Uint8Array, b: Uint8ClampedArray | Uint8Array): number {
  let sum = 0;
  for (let i = 0; i < a.length; i++) {
    const diff = a[i]! / 255 - b[i]! / 255;
    sum += diff * diff;
  }
  const mse = sum / a.length;
  if (mse === 0) return Infinity;
  return 10 * Math.log10(1 / mse);
}
