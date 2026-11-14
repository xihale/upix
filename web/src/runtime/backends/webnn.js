// Experimental W3C WebNN (`navigator.ml` + `MLGraphBuilder`) backend (`WebNnBackend`).
// Lowers Upix ncnn graphs into native hardware graphs (`gpu` / `npu` / `cpu` MLContext)
// using strictly <=4D intermediate shapes for maximum DirectML / CoreML / XNNPACK / LiteRT compatibility.

import { resolveCropBounds } from './webgpu.js';
import { t } from '../../i18n/index.js';

const DEVICE_LABELS = { gpu: 'GPU', npu: 'NPU', cpu: 'CPU' };

/**
 * Maps raw `navigator.ml.createContext` failures (browser DOMException text such as
 * "Failed to execute 'createContext' on 'ML': WebNN is not supported on this platform.")
 * to localized user-facing hints. The raw browser text never reaches the UI directly.
 */
function friendlyCreateContextError(err, deviceType = 'gpu') {
  const raw = err instanceof Error ? err.message : String(err ?? '');
  const lower = raw.toLowerCase();
  if (lower.includes('not supported') || lower.includes('unsupported') || lower.includes('not available')) {
    return t('runtime.webnnErrNotSupported');
  }
  if (lower.includes('permission') || lower.includes('denied') || lower.includes('blocked')) {
    return t('runtime.webnnErrBlocked');
  }
  if (lower.includes('out of memory') || lower.includes('allocation') || lower.includes('resource')) {
    return t('runtime.webnnErrOom');
  }
  const label = DEVICE_LABELS[deviceType] ?? String(deviceType).toUpperCase();
  if (lower.includes('device') || lower.includes('adapter')) {
    return t('runtime.webnnErrDevice', { device: label });
  }
  return raw ? t('runtime.webnnErrInitDetail', { detail: raw }) : t('runtime.webnnErrInit');
}

function tensorDesc(dims, dataType = 'float32') {
  return { dataType, shape: dims, dimensions: dims };
}

function resolveConvPads(inW, inH, kextW, kextH, sw, sh, pl, pr, pt, pb) {
  let outPl = Math.max(0, pl);
  let outPr = Math.max(0, pr);
  if (pl === -233 || pl === -234) {
    const wpad = Math.max(0, kextW + Math.floor((inW - 1) / sw) * sw - inW);
    outPl = pl === -233 ? Math.floor(wpad / 2) : wpad - Math.floor(wpad / 2);
    outPr = wpad - outPl;
  }
  let outPt = Math.max(0, pt);
  let outPb = Math.max(0, pb);
  if (pt === -233 || pt === -234) {
    const hpad = Math.max(0, kextH + Math.floor((inH - 1) / sh) * sh - inH);
    outPt = pt === -233 ? Math.floor(hpad / 2) : hpad - Math.floor(hpad / 2);
    outPb = hpad - outPt;
  }
  return [outPl, outPr, outPt, outPb];
}

function transposeDeconvWeightsOihwToIohw(src, numOutput, numInput, kh, kw, group) {
  const inG = numInput / group;
  const outG = numOutput / group;
  const maxk = kh * kw;
  const dst = new Float32Array(src.length);
  for (let g = 0; g < group; g += 1) {
    for (let oc = 0; oc < outG; oc += 1) {
      for (let ic = 0; ic < inG; ic += 1) {
        const sBase = ((g * outG + oc) * inG + ic) * maxk;
        const dBase = ((g * inG + ic) * outG + oc) * maxk;
        for (let k = 0; k < maxk; k += 1) {
          dst[dBase + k] = src[sBase + k];
        }
      }
    }
  }
  return dst;
}

function buildKeysHalfScaleKernel(channels) {
  // Exact Keys A=-0.75 cubic weights at u = 0.5 (`fx = 2*dx + 0.5`)
  const k1d = [-3 / 32, 19 / 32, 19 / 32, -3 / 32];
  const kernel = new Float32Array(channels * 16);
  for (let c = 0; c < channels; c += 1) {
    const base = c * 16;
    for (let y = 0; y < 4; y += 1) {
      for (let x = 0; x < 4; x += 1) {
        kernel[base + y * 4 + x] = k1d[y] * k1d[x];
      }
    }
  }
  return kernel;
}

function scalarConst(builder, val, shape = [1, 1, 1, 1]) {
  const count = shape.reduce((a, b) => a * b, 1);
  const arr = new Float32Array(count).fill(val);
  return builder.constant(tensorDesc(shape), arr);
}

// NCHW edge (replicate) padding built from border `slice` + `concat`.
// `builder.pad(..., { mode: 'edge' })` is not expressible in the TFLite schema,
// so the LiteRT backend (Chrome Linux/Android) rejects the graph at build time;
// slice/concat lower natively everywhere (DirectML / CoreML / XNNPACK / LiteRT).
function edgePadNchw(builder, x, dims, pt, pb, pl, pr) {
  const [n, c, h, w] = dims;
  let y = x;
  if (pt > 0 || pb > 0) {
    const parts = [];
    if (pt > 0) parts.push(builder.slice(y, [0, 0, 0, 0], [n, c, pt, w]));
    parts.push(y);
    if (pb > 0) parts.push(builder.slice(y, [0, 0, h - pb, 0], [n, c, pb, w]));
    y = builder.concat(parts, 2);
  }
  if (pl > 0 || pr > 0) {
    const h2 = h + pt + pb;
    const parts = [];
    if (pl > 0) parts.push(builder.slice(y, [0, 0, 0, 0], [n, c, h2, pl]));
    parts.push(y);
    if (pr > 0) parts.push(builder.slice(y, [0, 0, 0, w - pr], [n, c, h2, pr]));
    y = builder.concat(parts, 3);
  }
  return y;
}

function applyWebNnActivation(builder, x, actType, actParams) {
  switch (actType) {
    case 1:
      return builder.relu(x);
    case 2: {
      const alpha = actParams?.[0] ?? 0.1;
      if (typeof builder.leakyRelu === 'function') {
        return builder.leakyRelu(x, { alpha });
      }
      return builder.max(x, builder.mul(x, scalarConst(builder, alpha)));
    }
    case 3: {
      const minValue = actParams?.[0] ?? 0.0;
      const maxValue = actParams?.[1] ?? 1.0;
      return builder.clamp(x, { minValue, maxValue });
    }
    case 4:
      return builder.sigmoid(x);
    case 5: {
      // Mish: x * tanh(softplus(x))
      const clamped = builder.clamp(x, { minValue: -20.0, maxValue: 20.0 });
      const sp = typeof builder.softplus === 'function'
        ? builder.softplus(clamped)
        : builder.log(builder.add(scalarConst(builder, 1.0), builder.exp(clamped)));
      const hiMask = typeof builder.greater === 'function' && typeof builder.where === 'function'
        ? builder.where(builder.greater(x, scalarConst(builder, 20.0)), x, sp)
        : sp;
      return builder.mul(x, builder.tanh(hiMask));
    }
    case 6: {
      if (typeof builder.hardSwish === 'function' && (!actParams || actParams.length === 0)) {
        return builder.hardSwish(x);
      }
      const alpha = actParams?.[0] ?? (1 / 6);
      const beta = actParams?.[1] ?? 0.5;
      const gate = builder.clamp(
        builder.add(builder.mul(x, scalarConst(builder, alpha)), scalarConst(builder, beta)),
        { minValue: 0.0, maxValue: 1.0 },
      );
      return builder.mul(x, gate);
    }
    default:
      return x;
  }
}

function hwcToPaddedChw(inputHwc, w, h, c, feedW, feedH, prepad, scratchBuf = null) {
  const total = c * feedH * feedW;
  const out = scratchBuf && scratchBuf.length === total ? scratchBuf : new Float32Array(total);
  const plane = feedW * feedH;
  if (prepad === 0 && feedW === w && feedH === h && c === 3) {
    const rBase = 0;
    const gBase = plane;
    const bBase = plane * 2;
    for (let i = 0; i < plane; i += 1) {
      const s = i * 3;
      out[rBase + i] = inputHwc[s];
      out[gBase + i] = inputHwc[s + 1];
      out[bBase + i] = inputHwc[s + 2];
    }
    return out;
  }
  for (let ch = 0; ch < c; ch += 1) {
    const chBase = ch * plane;
    for (let fy = 0; fy < feedH; fy += 1) {
      const sy = Math.max(0, Math.min(h - 1, fy - prepad));
      const sRow = sy * w * c;
      const dRow = chBase + fy * feedW;
      for (let fx = 0; fx < feedW; fx += 1) {
        const sx = Math.max(0, Math.min(w - 1, fx - prepad));
        out[dRow + fx] = inputHwc[sRow + sx * c + ch];
      }
    }
  }
  return out;
}

function chwToCroppedHwc(srcChw, srcW, srcH, outW, outH, c, dstBuf = null) {
  const total = outW * outH * c;
  const out = dstBuf && dstBuf.length === total ? dstBuf : new Float32Array(total);
  const srcPlane = srcW * srcH;
  if (c === 3 && srcW === outW && srcH === outH) {
    const gBase = srcPlane;
    const bBase = srcPlane * 2;
    for (let i = 0; i < srcPlane; i += 1) {
      const d = i * 3;
      out[d] = srcChw[i];
      out[d + 1] = srcChw[gBase + i];
      out[d + 2] = srcChw[bBase + i];
    }
    return out;
  }
  for (let ch = 0; ch < c; ch += 1) {
    const cBase = ch * srcPlane;
    for (let oy = 0; oy < outH; oy += 1) {
      const sRow = cBase + oy * srcW;
      const dRow = oy * outW * c;
      for (let ox = 0; ox < outW; ox += 1) {
        out[dRow + ox * c + ch] = srcChw[sRow + ox];
      }
    }
  }
  return out;
}

// HWC replicate padding for the in-graph-transpose (HWC I/O) graph path:
// feed(fx, fy) = tile(clamp(fx - prepad, 0, w - 1), clamp(fy - prepad, 0, h - 1)),
// matching `hwcToPaddedChw`'s source mapping. With prepad = 0 only the
// right/bottom edges are replicate-padded (canonical edge tiles); the exact-fit
// case returns `inputHwc` untouched (zero-copy).
function padHwcReplicate(inputHwc, w, h, c, feedW, feedH, prepad, scratchBuf = null) {
  if (prepad === 0 && feedW === w && feedH === h) {
    return inputHwc;
  }
  const total = c * feedH * feedW;
  const out = scratchBuf && scratchBuf.length === total ? scratchBuf : new Float32Array(total);
  for (let fy = 0; fy < feedH; fy += 1) {
    const sy = Math.max(0, Math.min(h - 1, fy - prepad));
    const sRow = sy * w * c;
    const dRow = fy * feedW * c;
    for (let fx = 0; fx < feedW; fx += 1) {
      const sx = Math.max(0, Math.min(w - 1, fx - prepad));
      const s = sRow + sx * c;
      const d = dRow + fx * c;
      if (c === 3) {
        out[d] = inputHwc[s];
        out[d + 1] = inputHwc[s + 1];
        out[d + 2] = inputHwc[s + 2];
      } else {
        for (let ch = 0; ch < c; ch += 1) {
          out[d + ch] = inputHwc[s + ch];
        }
      }
    }
  }
  return out;
}

// Crop an (outW x outH) HWC sub-rectangle starting at (offX, offY); returns
// `srcHwc` itself when the extents already match (zero-copy fast path).
function cropHwc(srcHwc, srcW, srcH, c, outW, outH, offX = 0, offY = 0) {
  if (offX === 0 && offY === 0 && srcW === outW && srcH === outH) {
    return srcHwc;
  }
  const out = new Float32Array(outW * outH * c);
  const rowLen = outW * c;
  for (let oy = 0; oy < outH; oy += 1) {
    const sOff = ((offY + oy) * srcW + offX) * c;
    out.set(srcHwc.subarray(sOff, sOff + rowLen), oy * rowLen);
  }
  return out;
}

function destroyMlTensor(tensor) {
  if (tensor && typeof tensor.destroy === 'function') {
    try { tensor.destroy(); } catch { /* ignore */ }
  }
}

function destroyCompiledWebNn(compiled) {
  if (!compiled) return;
  const slots = compiled.slots;
  if (slots) {
    for (const slot of slots) {
      destroyMlTensor(slot.inTensor);
      destroyMlTensor(slot.outTensor);
      slot.inTensor = null;
      slot.outTensor = null;
    }
    compiled.slots = null;
  }
  // Slot-0 aliases (read by external cache eviction); already destroyed above.
  compiled.inTensor = null;
  compiled.outTensor = null;
}

export class WebNnBackend {
  constructor() {
    this.contexts = new Map(); // `${deviceType}:${powerPreference}` -> MLContext
    this.graphCache = new Map(); // `${modelName}:${ctxKey}:${feedW}x${feedH}x${c}` -> { graph, inTensor, outTensor, ... }
    this.parsedCache = null; // { name, meta, weights: Float32Array }
  }

  static async probeDevice(deviceType = 'gpu', powerPreference = 'default') {
    if (typeof navigator === 'undefined' || !navigator.ml || typeof MLGraphBuilder === 'undefined') {
      return {
        available: false,
        reason: t('runtime.webnnNotEnabled'),
      };
    }
    try {
      const ctx = await navigator.ml.createContext({ deviceType, powerPreference });
      if (ctx && typeof ctx.destroy === 'function') {
        ctx.destroy();
      }
      return { available: true };
    } catch (err) {
      return {
        available: false,
        reason: friendlyCreateContextError(err, deviceType),
      };
    }
  }

  static async probe() {
    if (typeof navigator === 'undefined' || !navigator.ml || typeof MLGraphBuilder === 'undefined') {
      return {
        id: 'webnn',
        available: false,
        experimental: true,
        label: 'WebNN',
        deviceName: t('common.disabled'),
        reason: t('runtime.webnnNotEnabled'),
        features: { webnnDevices: { gpu: false, npu: false, cpu: false } },
      };
    }

    const [gpu, npu, cpu] = await Promise.all([
      WebNnBackend.probeDevice('gpu'),
      WebNnBackend.probeDevice('npu'),
      WebNnBackend.probeDevice('cpu'),
    ]);
    const anyAvailable = gpu.available || npu.available || cpu.available;
    const supportedList = [
      gpu.available ? 'GPU' : null,
      npu.available ? 'NPU' : null,
      cpu.available ? 'CPU' : null,
    ].filter(Boolean);

    return {
      id: 'webnn',
      available: anyAvailable,
      experimental: true,
      label: 'WebNN',
      deviceName: anyAvailable ? `WebNN (${supportedList.join(' / ')})` : t('runtime.webnnNoDevices'),
      reason: anyAvailable ? undefined : (gpu.reason || cpu.reason || t('runtime.webnnNoDevicesReason')),
      features: {
        webnnDevices: {
          gpu: gpu.available,
          npu: npu.available,
          cpu: cpu.available,
        },
      },
    };
  }

  async getContext(deviceType = 'gpu', powerPreference = 'high-performance') {
    if (typeof navigator === 'undefined' || !navigator.ml || typeof MLGraphBuilder === 'undefined') {
      throw new Error(`${t('runtime.webnnNotEnabled')} · ${t('runtime.webnnFlagHint')}`);
    }
    const key = `${deviceType}:${powerPreference}`;
    if (this.contexts.has(key)) {
      return { context: this.contexts.get(key), key };
    }
    let ctx;
    try {
      ctx = await navigator.ml.createContext({ deviceType, powerPreference });
    } catch (err) {
      throw new Error(`${friendlyCreateContextError(err, deviceType)} · ${t('runtime.webnnFlagHint')}`, { cause: err });
    }
    this.contexts.set(key, ctx);
    return { context: ctx, key };
  }

  ensureModelParsed(wasmBridge, modelPayload) {
    if (this.parsedCache && this.parsedCache.name === modelPayload.name) {
      return this.parsedCache;
    }
    const loaded = wasmBridge.loadModel(
      modelPayload.name,
      modelPayload.paramText,
      modelPayload.binBytes,
    );
    try {
      for (const compiled of this.graphCache.values()) {
        destroyCompiledWebNn(compiled);
      }
      this.graphCache.clear();
      this.parsedCache = {
        name: modelPayload.name,
        meta: loaded.meta,
        weights: loaded.getWeightsView().slice(),
      };
      return this.parsedCache;
    } finally {
      loaded.dispose();
    }
  }

  async compileGraphForShape(context, ctxKey, parsedModel, feedW, feedH, channels) {
    const cacheKey = `${parsedModel.name}:${ctxKey}:${feedW}x${feedH}x${channels}`;
    if (this.graphCache.has(cacheKey)) {
      return this.graphCache.get(cacheKey);
    }

    const { meta, weights } = parsedModel;
    const builder = new MLGraphBuilder(context);
    const blobs = new Array(meta.blob_count).fill(null);

    const sliceWeights = (span) => weights.subarray(span.offset, span.offset + span.len).slice();

    // In-graph HWC I/O (`useHwcIo`): the graph consumes [1, feedH, feedW, C] and
    // produces [1, rawOutH, rawOutW, C] directly. LiteRT's compiler
    // (`preferredInputLayout === 'nhwc'`) folds the [0,3,1,2] input transpose
    // with its own internal NCHW->NHWC conversion into an identity no-op, which
    // removes both JS-side re-layout loops (`hwcToPaddedChw` / `chwToCroppedHwc`)
    // for full-size tiles. Kept as a flag: backends without `builder.transpose`
    // fall back to the original NCHW pipeline below.
    const useHwcIo = typeof builder.transpose === 'function';
    const inputOp = useHwcIo
      ? builder.input('input', tensorDesc([1, feedH, feedW, channels]))
      : builder.input('input', tensorDesc([1, channels, feedH, feedW]));
    blobs[meta.input_blob] = {
      op: useHwcIo
        ? builder.transpose(inputOp, { permutation: [0, 3, 1, 2] })
        : inputOp,
      c: channels,
      h: feedH,
      w: feedW,
    };

    for (let lIdx = 0; lIdx < meta.layers.length; lIdx += 1) {
      const layer = meta.layers[lIdx];
      const op = layer.op;

      if (op.op === 'input') {
        // Already bound
      } else if (op.op === 'memory_data') {
        let tw = 1; let th = 1; let tc = Math.max(1, op.w || 1);
        if (op.d > 0) { tw = Math.max(1, op.w); th = Math.max(1, op.h) * op.d; tc = Math.max(1, op.c); }
        else if (op.c > 0) { tw = Math.max(1, op.w); th = Math.max(1, op.h); tc = op.c; }
        else if (op.h > 0) { tw = Math.max(1, op.w); th = op.h; tc = 1; }
        const cOp = builder.constant(tensorDesc([1, tc, th, tw]), sliceWeights(op.data));
        blobs[layer.tops[0]] = { op: cOp, c: tc, h: th, w: tw };
      } else if (op.op === 'split') {
        const src = blobs[layer.bottoms[0]];
        for (const top of layer.tops) {
          blobs[top] = src;
        }
      } else if (op.op === 'convolution') {
        const inp = blobs[layer.bottoms[0]];
        const kextW = op.dilation_w * (op.kernel_w - 1) + 1;
        const kextH = op.dilation_h * (op.kernel_h - 1) + 1;
        const [pl, pr, pt, pb] = resolveConvPads(
          inp.w, inp.h, kextW, kextH, op.stride_w, op.stride_h,
          op.pad_left, op.pad_right, op.pad_top, op.pad_bottom,
        );
        const outW = Math.floor((inp.w + pl + pr - kextW) / op.stride_w) + 1;
        const outH = Math.floor((inp.h + pt + pb - kextH) / op.stride_h) + 1;

        let x = inp.op;
        let convPads = [pt, pb, pl, pr];
        if ((op.pad_value || 0) !== 0 && (pt > 0 || pb > 0 || pl > 0 || pr > 0)) {
          x = builder.pad(x, [0, 0, pt, pl], [0, 0, pb, pr], {
            mode: 'constant',
            value: op.pad_value,
          });
          convPads = [0, 0, 0, 0];
        }

        const inG = op.num_input / op.group;
        const wConst = builder.constant(
          tensorDesc([op.num_output, inG, op.kernel_h, op.kernel_w]),
          sliceWeights(op.weight),
        );
        const convOpts = {
          padding: convPads,
          strides: [op.stride_h, op.stride_w],
          dilations: [op.dilation_h, op.dilation_w],
          groups: op.group,
          filterLayout: 'oihw',
        };
        if (op.bias_term) {
          convOpts.bias = builder.constant(tensorDesc([op.num_output]), sliceWeights(op.bias));
        }
        let y = builder.conv2d(x, wConst, convOpts);
        y = applyWebNnActivation(builder, y, op.activation_type, op.activation_params);
        blobs[layer.tops[0]] = { op: y, c: op.num_output, h: outH, w: outW };
      } else if (op.op === 'deconvolution') {
        const inp = blobs[layer.bottoms[0]];
        const kextW = op.dilation_w * (op.kernel_w - 1) + 1;
        const kextH = op.dilation_h * (op.kernel_h - 1) + 1;
        const opr = Math.max(0, op.output_pad_right || 0);
        const opb = Math.max(0, op.output_pad_bottom || 0);
        const fullW = (inp.w - 1) * op.stride_w + kextW + opr;
        const fullH = (inp.h - 1) * op.stride_h + kextH + opb;
        const pl = Math.max(0, op.pad_left || 0);
        const pr = Math.max(0, op.pad_right || 0);
        const pt = Math.max(0, op.pad_top || 0);
        const pb = Math.max(0, op.pad_bottom || 0);
        const outW = op.output_w > 0 ? op.output_w : fullW - pl - pr;
        const outH = op.output_h > 0 ? op.output_h : fullH - pt - pb;

        const outG = op.num_output / op.group;
        const iohwWeights = transposeDeconvWeightsOihwToIohw(
          sliceWeights(op.weight),
          op.num_output,
          op.num_input,
          op.kernel_h,
          op.kernel_w,
          op.group,
        );
        const wConst = builder.constant(
          tensorDesc([op.num_input, outG, op.kernel_h, op.kernel_w]),
          iohwWeights,
        );
        const deconvOpts = {
          padding: [0, 0, 0, 0],
          strides: [op.stride_h, op.stride_w],
          dilations: [op.dilation_h, op.dilation_w],
          outputPadding: [opb, opr],
          groups: op.group,
          filterLayout: 'iohw',
        };
        if (op.bias_term) {
          deconvOpts.bias = builder.constant(tensorDesc([op.num_output]), sliceWeights(op.bias));
        }
        let y = builder.convTranspose2d(inp.op, wConst, deconvOpts);
        if (pl > 0 || pt > 0 || outW !== fullW || outH !== fullH) {
          y = builder.slice(y, [0, 0, pt, pl], [1, op.num_output, outH, outW]);
        }
        y = applyWebNnActivation(builder, y, op.activation_type, op.activation_params);
        blobs[layer.tops[0]] = { op: y, c: op.num_output, h: outH, w: outW };
      } else if (op.op === 'inner_product') {
        const inp = blobs[layer.bottoms[0]];
        let x = inp.op;
        if (inp.h !== 1 || inp.w !== 1 || inp.c !== op.num_input) {
          x = builder.reshape(x, [1, op.num_input, 1, 1]);
        }
        const wConst = builder.constant(
          tensorDesc([op.num_output, op.num_input, 1, 1]),
          sliceWeights(op.weight),
        );
        const convOpts = { filterLayout: 'oihw' };
        if (op.bias_term) {
          convOpts.bias = builder.constant(tensorDesc([op.num_output]), sliceWeights(op.bias));
        }
        let y = builder.conv2d(x, wConst, convOpts);
        y = applyWebNnActivation(builder, y, op.activation_type, op.activation_params);
        blobs[layer.tops[0]] = { op: y, c: op.num_output, h: 1, w: 1 };
      } else if (op.op === 'pooling') {
        const inp = blobs[layer.bottoms[0]];
        if (op.global_pooling) {
          const y = op.pooling_type === 0
            ? builder.reduceMax(inp.op, { axes: [2, 3], keepDimensions: true })
            : builder.reduceMean(inp.op, { axes: [2, 3], keepDimensions: true });
          blobs[layer.tops[0]] = { op: y, c: inp.c, h: 1, w: 1 };
        } else {
          const pl = Math.max(0, op.pad_left || 0);
          const pr = Math.max(0, op.pad_right || 0);
          const pt = Math.max(0, op.pad_top || 0);
          const pb = Math.max(0, op.pad_bottom || 0);
          const outW = Math.floor((inp.w + pl + pr - op.kernel_w) / op.stride_w) + 1;
          const outH = Math.floor((inp.h + pt + pb - op.kernel_h) / op.stride_h) + 1;
          const poolOpts = {
            windowDimensions: [op.kernel_h, op.kernel_w],
            strides: [op.stride_h, op.stride_w],
            padding: [pt, pb, pl, pr],
          };
          const y = op.pooling_type === 0
            ? builder.maxPool2d(inp.op, poolOpts)
            : builder.averagePool2d(inp.op, poolOpts);
          blobs[layer.tops[0]] = { op: y, c: inp.c, h: outH, w: outW };
        }
      } else if (op.op === 'p_re_lu') {
        const inp = blobs[layer.bottoms[0]];
        const slopeShape = op.num_slope > 1 ? [1, op.num_slope, 1, 1] : [1, 1, 1, 1];
        const sConst = builder.constant(tensorDesc(slopeShape), sliceWeights(op.slope));
        const isTfliteGpu = ctxKey.startsWith('gpu:')
          && typeof context.opSupportLimits === 'function'
          && context.opSupportLimits()?.preferredInputLayout === 'nhwc';
        const y = typeof builder.prelu === 'function' && !isTfliteGpu
          ? builder.prelu(inp.op, sConst)
          : builder.add(
              builder.relu(inp.op),
              builder.mul(sConst, builder.min(inp.op, scalarConst(builder, 0.0))),
            );
        blobs[layer.tops[0]] = { op: y, c: inp.c, h: inp.h, w: inp.w };
      } else if (op.op === 're_lu') {
        const inp = blobs[layer.bottoms[0]];
        const y = op.slope
          ? applyWebNnActivation(builder, inp.op, 2, [op.slope])
          : builder.relu(inp.op);
        blobs[layer.tops[0]] = { op: y, c: inp.c, h: inp.h, w: inp.w };
      } else if (op.op === 'clip') {
        const inp = blobs[layer.bottoms[0]];
        const y = builder.clamp(inp.op, { minValue: op.min, maxValue: op.max });
        blobs[layer.tops[0]] = { op: y, c: inp.c, h: inp.h, w: inp.w };
      } else if (op.op === 'swish') {
        const inp = blobs[layer.bottoms[0]];
        const y = builder.mul(inp.op, builder.sigmoid(inp.op));
        blobs[layer.tops[0]] = { op: y, c: inp.c, h: inp.h, w: inp.w };
      } else if (op.op === 'sigmoid') {
        const inp = blobs[layer.bottoms[0]];
        const y = builder.sigmoid(inp.op);
        blobs[layer.tops[0]] = { op: y, c: inp.c, h: inp.h, w: inp.w };
      } else if (op.op === 'binary_op') {
        const a = blobs[layer.bottoms[0]];
        const bTensor = op.with_scalar
          ? { op: scalarConst(builder, op.scalar), c: 1, h: 1, w: 1 }
          : blobs[layer.bottoms[1]];

        const evalBin = (lhs, rhs) => {
          switch (op.op_type) {
            case 0: return builder.add(lhs, rhs);
            case 1: return builder.sub(lhs, rhs);
            case 2: return builder.mul(lhs, rhs);
            case 3: return builder.div(lhs, rhs);
            case 4: return builder.max(lhs, rhs);
            case 5: return builder.min(lhs, rhs);
            case 6: return builder.pow(lhs, rhs);
            case 7: return builder.sub(rhs, lhs);
            case 8: return builder.div(rhs, lhs);
            case 9: return builder.pow(rhs, lhs);
            default: return builder.add(lhs, rhs);
          }
        };

        const y = evalBin(a.op, bTensor.op);
        blobs[layer.tops[0]] = {
          op: y,
          c: Math.max(a.c, bTensor.c),
          h: Math.max(a.h, bTensor.h),
          w: Math.max(a.w, bTensor.w),
        };
      } else if (op.op === 'concat') {
        const inputs = layer.bottoms.map((bi) => blobs[bi]);
        const webnnAxis = (op.axis || 0) + 1; // NCHW shifts ncnn CHW axis by +1
        const y = builder.concat(inputs.map((t) => t.op), webnnAxis);
        blobs[layer.tops[0]] = {
          op: y,
          c: webnnAxis === 1 ? inputs.reduce((s, t) => s + t.c, 0) : inputs[0].c,
          h: webnnAxis === 2 ? inputs.reduce((s, t) => s + t.h, 0) : inputs[0].h,
          w: webnnAxis === 3 ? inputs.reduce((s, t) => s + t.w, 0) : inputs[0].w,
        };
      } else if (op.op === 'slice') {
        const inp = blobs[layer.bottoms[0]];
        const nTops = layer.tops.length;
        const sizes = [];
        if (op.indices && op.indices.length > 0) {
          let prev = 0;
          for (const rawIdx of op.indices) {
            const pos = rawIdx < 0 ? Math.max(0, inp.c + rawIdx) : Math.min(inp.c, rawIdx);
            sizes.push(Math.max(0, pos - prev));
            prev = pos;
          }
          sizes.push(Math.max(0, inp.c - prev));
        } else if (op.slices && op.slices.length > 0) {
          let known = 0; let autoCnt = 0;
          for (const s of op.slices) {
            if (s > 0) known += s; else autoCnt += 1;
          }
          const autoSz = Math.floor(Math.max(0, inp.c - known) / Math.max(1, autoCnt));
          for (let i = 0; i < nTops; i += 1) {
            sizes.push(op.slices[i] > 0 ? op.slices[i] : autoSz);
          }
        } else {
          const each = Math.floor(inp.c / Math.max(1, nTops));
          for (let i = 0; i < nTops; i += 1) sizes.push(each);
        }

        if (typeof builder.split === 'function') {
          const parts = builder.split(inp.op, sizes, { axis: 1 });
          for (let i = 0; i < nTops; i += 1) {
            blobs[layer.tops[i]] = { op: parts[i], c: sizes[i], h: inp.h, w: inp.w };
          }
        } else {
          let chOff = 0;
          for (let i = 0; i < nTops; i += 1) {
            const part = builder.slice(inp.op, [0, chOff, 0, 0], [1, sizes[i], inp.h, inp.w]);
            blobs[layer.tops[i]] = { op: part, c: sizes[i], h: inp.h, w: inp.w };
            chOff += sizes[i];
          }
        }
      } else if (op.op === 'group_norm') {
        const inp = blobs[layer.bottoms[0]];
        const G = Math.max(1, op.group);
        const C = inp.c;
        const groupSpan = Math.floor(C / G) * inp.h * inp.w;

        const flat = builder.reshape(inp.op, [1, G, groupSpan]);
        const mean = builder.reduceMean(flat, { axes: [2], keepDimensions: true });
        const centered = builder.sub(flat, mean);
        const variance = builder.reduceMean(builder.mul(centered, centered), {
          axes: [2],
          keepDimensions: true,
        });
        const denom = builder.sqrt(
          builder.add(variance, scalarConst(builder, op.eps, [1, 1, 1])),
        );
        let norm = builder.reshape(builder.div(centered, denom), [1, C, inp.h, inp.w]);
        if (op.affine) {
          const gamma = builder.constant(tensorDesc([1, C, 1, 1]), sliceWeights(op.gamma));
          const beta = builder.constant(tensorDesc([1, C, 1, 1]), sliceWeights(op.beta));
          norm = builder.add(builder.mul(norm, gamma), beta);
        }
        blobs[layer.tops[0]] = { op: norm, c: C, h: inp.h, w: inp.w };
      } else if (op.op === 'interp') {
        const inp = blobs[layer.bottoms[0]];
        const outW = op.output_width > 0
          ? op.output_width
          : Math.max(1, Math.floor(inp.w * op.width_scale));
        const outH = op.output_height > 0
          ? op.output_height
          : Math.max(1, Math.floor(inp.h * op.height_scale));

        let y;
        if (outW === inp.w && outH === inp.h) {
          y = inp.op;
        } else if (
          op.resize_type === 3
          && !op.align_corner
          && inp.w === outW * 2
          && inp.h === outH * 2
        ) {
          // Exact Keys A=-0.75 bicubic 2x downsample via edge-pad + 4x4 depthwise conv2d!
          const padded = edgePadNchw(builder, inp.op, [1, inp.c, inp.h, inp.w], 1, 1, 1, 1);
          const kConst = builder.constant(
            tensorDesc([inp.c, 1, 4, 4]),
            buildKeysHalfScaleKernel(inp.c),
          );
          y = builder.conv2d(padded, kConst, {
            strides: [2, 2],
            padding: [0, 0, 0, 0],
            groups: inp.c,
            filterLayout: 'oihw',
          });
        } else {
          const mode = op.resize_type === 1 ? 'nearest-neighbor' : 'linear';
          y = builder.resample2d(inp.op, { mode, sizes: [outH, outW], axes: [2, 3] });
        }
        blobs[layer.tops[0]] = { op: y, c: inp.c, h: outH, w: outW };
      } else if (op.op === 'pixel_shuffle') {
        const inp = blobs[layer.bottoms[0]];
        const r = op.upscale_factor;
        if (r <= 1) {
          blobs[layer.tops[0]] = inp;
        } else {
          const outC = Math.floor(inp.c / (r * r));
          const outH = inp.h * r;
          const outW = inp.w * r;
          let x = inp.op;
          if ((op.mode || 0) !== 0) {
            // CRD `[1, r*r, outC, H*W]` -> DCR `[1, outC, r*r, H*W]`
            x = builder.reshape(x, [1, r * r, outC, inp.h * inp.w]);
            x = builder.transpose(x, { permutation: [0, 2, 1, 3] });
          }
          // Strictly 4D two-step depth-to-space (compatible with DirectML / CoreML / XNNPACK):
          x = builder.reshape(x, [outC * r, r, inp.h, inp.w]);
          x = builder.transpose(x, { permutation: [0, 2, 3, 1] });
          x = builder.reshape(x, [outC, r, inp.h, outW]);
          x = builder.transpose(x, { permutation: [0, 2, 1, 3] });
          x = builder.reshape(x, [1, outC, outH, outW]);
          blobs[layer.tops[0]] = { op: x, c: outC, h: outH, w: outW };
        }
      } else if (op.op === 'reorg') {
        const inp = blobs[layer.bottoms[0]];
        const s = op.stride;
        const outH = Math.floor(inp.h / s);
        const outW = Math.floor(inp.w / s);
        const outC = inp.c * s * s;
        // Strictly 4D two-step space-to-depth:
        let x = builder.reshape(inp.op, [inp.c, outH, s, inp.w]);
        x = builder.transpose(x, { permutation: [0, 2, 1, 3] });
        x = builder.reshape(x, [inp.c * s, outH, outW, s]);
        x = builder.transpose(x, { permutation: [0, 3, 1, 2] });
        if ((op.mode || 0) !== 0) {
          x = builder.reshape(x, [1, inp.c, s * s, outH * outW]);
          x = builder.transpose(x, { permutation: [0, 2, 1, 3] });
        }
        x = builder.reshape(x, [1, outC, outH, outW]);
        blobs[layer.tops[0]] = { op: x, c: outC, h: outH, w: outW };
      } else if (op.op === 'crop') {
        const inp = blobs[layer.bottoms[0]];
        const [cs, ce, hs, he, ws, we] = resolveCropBounds(inp.c, inp.h, inp.w, op);
        const oc = ce - cs;
        const oh = he - hs;
        const ow = we - ws;
        const y = builder.slice(inp.op, [0, cs, hs, ws], [1, oc, oh, ow]);
        blobs[layer.tops[0]] = { op: y, c: oc, h: oh, w: ow };
      } else if (op.op === 'eltwise') {
        const inputs = layer.bottoms.map((bi) => blobs[bi]);
        const coeffs = op.coeffs || [];
        let acc = inputs[0].op;
        if ((op.op_type ?? 1) === 1 && coeffs[0] !== undefined && coeffs[0] !== 1.0) {
          acc = builder.mul(acc, scalarConst(builder, coeffs[0]));
        }
        for (let i = 1; i < inputs.length; i += 1) {
          let rhs = inputs[i].op;
          if ((op.op_type ?? 1) === 0) {
            acc = builder.mul(acc, rhs);
          } else if (op.op_type === 2) {
            acc = builder.max(acc, rhs);
          } else {
            const c = coeffs[i] ?? 1.0;
            if (c !== 1.0) {
              rhs = builder.mul(rhs, scalarConst(builder, c));
            }
            acc = builder.add(acc, rhs);
          }
        }
        blobs[layer.tops[0]] = { op: acc, c: inputs[0].c, h: inputs[0].h, w: inputs[0].w };
      } else if (op.op === 'padding') {
        const inp = blobs[layer.bottoms[0]];
        let y;
        if (op.pad_type === 1) {
          y = edgePadNchw(
            builder,
            inp.op,
            [1, inp.c, inp.h, inp.w],
            op.top,
            op.bottom,
            op.left,
            op.right,
          );
        } else {
          const mode = op.pad_type === 2 ? 'reflection' : 'constant';
          y = builder.pad(
            inp.op,
            [0, 0, op.top, op.left],
            [0, 0, op.bottom, op.right],
            { mode, value: op.value || 0 },
          );
        }
        blobs[layer.tops[0]] = {
          op: y,
          c: inp.c,
          h: inp.h + op.top + op.bottom,
          w: inp.w + op.left + op.right,
        };
      } else {
        throw new Error(t('runtime.webnnUnsupportedOp', { op: op.op }));
      }
    }

    const finalOut = blobs[meta.output_blob];
    if (!finalOut) {
      throw new Error(t('runtime.webnnMissingOutput'));
    }

    // Graph output transposed back to HWC [1, rawOutH, rawOutW, outC]; on
    // LiteRT this is its native NHWC output layout (identity fold on GPU).
    const outOp = useHwcIo
      ? builder.transpose(finalOut.op, { permutation: [0, 2, 3, 1] })
      : finalOut.op;
    const graph = await builder.build({ output: outOp });
    // Logical NCHW dims drive tile crop math; physical dims shape the tensors.
    const inDims = [1, channels, feedH, feedW];
    const outDims = [1, finalOut.c, finalOut.h, finalOut.w];
    const inTensorDims = useHwcIo ? [1, feedH, feedW, channels] : inDims;
    const outTensorDims = useHwcIo ? [1, finalOut.h, finalOut.w, finalOut.c] : outDims;
    let slots = null;
    if (typeof context.createTensor === 'function' && typeof context.dispatch === 'function') {
      const makeOpts = (dims, writable, readable) => {
        const base = {
          dataType: 'float32',
          shape: dims,
          dimensions: dims,
          writable,
          readable,
        };
        if (typeof MLTensorUsage !== 'undefined') {
          base.usage = (writable ? MLTensorUsage.WRITE : 0) | (readable ? MLTensorUsage.READ : 0);
        }
        return base;
      };
      const allocSlot = async () => {
        const slot = { inTensor: null, outTensor: null };
        try {
          slot.inTensor = await context.createTensor(makeOpts(inTensorDims, true, false));
          slot.outTensor = await context.createTensor(makeOpts(outTensorDims, false, true));
          return slot;
        } catch (err) {
          destroyMlTensor(slot.inTensor);
          destroyMlTensor(slot.outTensor);
          throw err;
        }
      };
      try {
        // Ping-pong pair: tile i+1 is written + dispatched on slot (i+1)&1
        // while tile i still executes on slot i&1, so runTiles can overlap
        // Chromium GPU IPC + kernel execution with the next tile's prep.
        slots = [];
        slots.push(await allocSlot());
        slots.push(await allocSlot());
      } catch {
        for (const slot of slots || []) {
          destroyMlTensor(slot.inTensor);
          destroyMlTensor(slot.outTensor);
        }
        try {
          // Degrade to a single serialized slot pair before giving up.
          slots = [await allocSlot()];
        } catch {
          slots = null; // no persistent MLTensors -> legacy context.compute path
        }
      }
    }

    const compiled = {
      graph,
      useHwcIo,
      inDims,
      outDims,
      inTensorDims,
      outTensorDims,
      slots,
      // Slot-0 aliases kept for external cache eviction (`bench-webnn.mjs`).
      inTensor: slots ? slots[0].inTensor : null,
      outTensor: slots ? slots[0].outTensor : null,
      inChwBuf: useHwcIo ? null : new Float32Array(channels * feedH * feedW),
      inHwcBuf: useHwcIo ? new Float32Array(channels * feedH * feedW) : null,
      outHwcBuffers: [null, null],
      bufToggle: 0,
    };
    this.graphCache.set(cacheKey, compiled);
    return compiled;
  }

  async executeGraph(context, compiled, inputData, slotIndex = 0) {
    // Modern W3C WebNN dispatch + persistent ping-pong MLTensor slots (Chromium 128+).
    // `inputData` must already match the graph's physical input layout: HWC
    // [1, feedH, feedW, C] when `useHwcIo`, else NCHW [1, C, feedH, feedW].
    const slots = compiled.slots;
    if (slots && slots.length > 0) {
      const slot = slots[((slotIndex % slots.length) + slots.length) % slots.length];
      context.writeTensor(slot.inTensor, inputData);
      context.dispatch(compiled.graph, { input: slot.inTensor }, { output: slot.outTensor });
      const outBuffer = await context.readTensor(slot.outTensor);
      return new Float32Array(outBuffer);
    }

    // Legacy `context.compute` fallback
    if (typeof context.compute === 'function') {
      const outLen = compiled.outTensorDims.reduce((a, b) => a * b, 1);
      const outArr = new Float32Array(outLen);
      const res = await context.compute(
        compiled.graph,
        { input: inputData },
        { output: outArr },
      );
      return res.outputs?.output || outArr;
    }

    throw new Error(t('runtime.webnnDispatchUnsupported'));
  }

  computeFeedShape(meta, width, height) {
    const scale = Math.max(1, meta.scale_factor || 2);
    const outW = width * scale;
    const outH = height * scale;
    const prepad = Math.max(0, meta.prepadding || 0);
    const align = Math.max(1, meta.input_align || 1);
    const baseW = width + 2 * prepad;
    const baseH = height + 2 * prepad;
    const feedW = baseW + ((align - (baseW % align)) % align);
    const feedH = baseH + ((align - (baseH % align)) % align);
    return { outW, outH, prepad, feedW, feedH };
  }

  async inferTileWebNn(
    context,
    ctxKey,
    parsedModel,
    inputF32,
    width,
    height,
    channels,
    slotIndex = 0,
  ) {
    const { outW, outH, prepad, feedW, feedH } = this.computeFeedShape(parsedModel.meta, width, height);
    const compiled = await this.compileGraphForShape(
      context,
      ctxKey,
      parsedModel,
      feedW,
      feedH,
      channels,
    );

    if (compiled.useHwcIo) {
      // HWC graph: full-size, zero-prepad tiles skip every JS re-layout loop —
      // `inputF32` goes straight into writeTensor and the readTensor view is
      // returned as-is when no output crop is needed (zero-copy fast path).
      let feedData = inputF32;
      if (prepad !== 0 || width !== feedW || height !== feedH) {
        feedData = padHwcReplicate(inputF32, width, height, channels, feedW, feedH, prepad, compiled.inHwcBuf);
      }
      const rawHwc = await this.executeGraph(context, compiled, feedData, slotIndex);
      return cropHwc(rawHwc, compiled.outDims[3], compiled.outDims[2], channels, outW, outH);
    }

    // NCHW fallback graph (no `builder.transpose`): original CPU re-layout path.
    const inChw = hwcToPaddedChw(
      inputF32,
      width,
      height,
      channels,
      feedW,
      feedH,
      prepad,
      compiled.inChwBuf,
    );
    const outChw = await this.executeGraph(context, compiled, inChw, slotIndex);
    const outTotal = outW * outH * channels;
    const slot = compiled.bufToggle;
    compiled.bufToggle ^= 1;
    if (!compiled.outHwcBuffers[slot] || compiled.outHwcBuffers[slot].length !== outTotal) {
      compiled.outHwcBuffers[slot] = new Float32Array(outTotal);
    }
    return chwToCroppedHwc(
      outChw,
      compiled.outDims[3],
      compiled.outDims[2],
      outW,
      outH,
      channels,
      compiled.outHwcBuffers[slot],
    );
  }

  async runTiles({
    wasmBridge,
    modelPayload,
    regions,
    extractTile,
    channels,
    deviceType = 'gpu',
    powerPreference = 'high-performance',
    signal,
    onPhase,
    onTileDone,
  }) {
    const parsed = this.ensureModelParsed(wasmBridge, modelPayload);
    const { context, key: ctxKey } = await this.getContext(deviceType, powerPreference);
    if (onPhase) onPhase('compiling');

    // ---- Single-compilation canonical tile shape --------------------------------
    // Edge/corner tiles would otherwise each trigger their own
    // MLGraphBuilder.build() (a 1024x768 @256px image yields 4 distinct feed
    // shapes => 4 cold compiles). Instead compile ONE canonical
    // (canonFeedW, canonFeedH) graph per image and replicate-pad smaller
    // tiles' right/bottom edges up to (canonW, canonH) before inference.
    //
    // Border-correctness (also covers Real-CUGAN with prepad > 0): let
    // P(x, y) = tile(clamp(x - prepad, 0, w - 1), clamp(y - prepad, 0, h - 1))
    // be the per-shape feed and Q(x, y) the canonical feed built from the
    // top-left-aligned canon-padded tile T(x, y) = tile(min(x, w - 1), min(y, h - 1)):
    //   Q(x, y) = T(clamp(x - prepad, 0, canonW - 1), clamp(y - prepad, 0, canonH - 1)).
    // Because min(clamp(x - prepad, 0, canonW - 1), w - 1) === clamp(x - prepad, 0, w - 1)
    // for every x (same for y), Q === P over P's whole extent and extends it
    // with replicate border exactly like P's own alignment padding. All Upix
    // operators are spatially local, and Real-CUGAN "se" nets carry internal
    // Crop layers pinning output (0,0) to tile (0,0), so cropping
    // (region.width * scale, region.height * scale) returns identical pixels;
    // interior tiles feed a bit-identical tensor and skip all padding work.
    let canonW = 0;
    let canonH = 0;
    for (const region of regions) {
      if (region.width > canonW) canonW = region.width;
      if (region.height > canonH) canonH = region.height;
    }
    const scale = Math.max(1, parsed.meta.scale_factor || 2);
    // Canonical single-shape compilation targets GPU/NPU (where building 3 extra
    // static edge shapes costs 600–2100ms while extra tile pixels cost <2ms).
    // On CPU (XNNPACK), graph build is ~20–40ms while expanding edge tiles adds
    // ~35% redundant FLOPs, so CPU runs exact per-region shapes serialized.
    const useCanonicalTile = deviceType !== 'cpu' && regions.length > 0;

    let canonCompiled = null;
    if (regions.length > 0) {
      const initW = useCanonicalTile ? canonW : regions[0].width;
      const initH = useCanonicalTile ? canonH : regions[0].height;
      const initFeed = this.computeFeedShape(parsed.meta, initW, initH);
      canonCompiled = await this.compileGraphForShape(
        context,
        ctxKey,
        parsed,
        initFeed.feedW,
        initFeed.feedH,
        channels,
      );
    }

    // Ping-pong double buffering overlaps GPU IPC + kernel execution across two
    // independent MLTensor pairs; keep CPU serialized to avoid thread contention.
    const doubleBuffered = !!(
      deviceType !== 'cpu'
      && canonCompiled
      && canonCompiled.slots
      && canonCompiled.slots.length >= 2
    );

    let canonPadBuf = null;
    const padToCanon = (w, h, tileIn) => {
      if (w === canonW && h === canonH) {
        return tileIn;
      }
      const total = canonW * canonH * channels;
      if (!canonPadBuf || canonPadBuf.length !== total) {
        canonPadBuf = new Float32Array(total);
      }
      return padHwcReplicate(tileIn, w, h, channels, canonW, canonH, 0, canonPadBuf);
    };

    // Prep + submit one tile and resolve to the cropped (region.width * scale,
    // region.height * scale) HWC tile output.
    // On GPU/NPU edge/corner tiles, when the image covers at least (canonW, canonH),
    // shift the extraction window inward (`shiftX = region.x + region.width - canonW`)
    // instead of synthetic replicate padding: this feeds 100% real image pixels,
    // keeps the true outer image boundary aligned for 3x3 zero-padding, and avoids
    // skewing SE global-pooling / GroupNorm statistics on narrow edge slivers.
    const launch = (idx) => {
      const region = regions[idx];
      const t0 = performance.now();
      if (!useCanonicalTile) {
        const tileIn = extractTile(region, channels);
        const promise = this.inferTileWebNn(
          context,
          ctxKey,
          parsed,
          tileIn,
          region.width,
          region.height,
          channels,
          0,
        );
        return { idx, region, t0, promise };
      }

      if (region.width === canonW && region.height === canonH) {
        const tileIn = extractTile(region, channels);
        const promise = this.inferTileWebNn(
          context,
          ctxKey,
          parsed,
          tileIn,
          canonW,
          canonH,
          channels,
          idx & 1,
        );
        return { idx, region, t0, promise };
      }

      const canShiftX = region.x + region.width >= canonW;
      const canShiftY = region.y + region.height >= canonH;
      if (canShiftX && canShiftY) {
        const shiftX = region.x + region.width - canonW;
        const shiftY = region.y + region.height - canonH;
        const shiftedRegion = {
          x: shiftX,
          y: shiftY,
          width: canonW,
          height: canonH,
          overlap: region.overlap,
        };
        const offX = (region.x - shiftX) * scale;
        const offY = (region.y - shiftY) * scale;
        const tileIn = extractTile(shiftedRegion, channels);
        const promise = this.inferTileWebNn(
          context,
          ctxKey,
          parsed,
          tileIn,
          canonW,
          canonH,
          channels,
          idx & 1,
        ).then((canonOut) => cropHwc(
          canonOut,
          canonW * scale,
          canonH * scale,
          channels,
          region.width * scale,
          region.height * scale,
          offX,
          offY,
        ));
        return { idx, region, t0, promise };
      }

      const tileIn = padToCanon(region.width, region.height, extractTile(region, channels));
      const promise = this.inferTileWebNn(context, ctxKey, parsed, tileIn, canonW, canonH, channels, idx & 1)
        .then((canonOut) => cropHwc(
          canonOut,
          canonW * scale,
          canonH * scale,
          channels,
          region.width * scale,
          region.height * scale,
          0,
          0,
        ));
      return { idx, region, t0, promise };
    };

    let inFlight = null;
    let pendingDone = null;
    if (regions.length > 0 && !(signal && signal.aborted)) {
      inFlight = launch(0);
    }

    for (let i = 0; i < regions.length; i += 1) {
      if (signal && signal.aborted) {
        if (inFlight) inFlight.promise.catch(() => {});
        throw new DOMException(t('common.cancelled'), 'AbortError');
      }
      if (pendingDone) {
        onTileDone(pendingDone.idx, pendingDone.region, pendingDone.tileOut, pendingDone.elapsedMs);
      }
      let launched = null;
      // Submit tile i+1 (extractTile + pad + writeTensor + dispatch) BEFORE
      // awaiting tile i's readTensor — the whole point of ping-pong slots.
      if (doubleBuffered && i + 1 < regions.length) {
        launched = launch(i + 1);
      }
      const tileOut = await inFlight.promise;
      pendingDone = {
        idx: inFlight.idx,
        region: inFlight.region,
        tileOut,
        elapsedMs: performance.now() - inFlight.t0,
      };
      if (!doubleBuffered && i + 1 < regions.length) {
        // Serialized slot path: only start tile i+1 after tile i completed.
        launched = launch(i + 1);
      }
      inFlight = launched;
    }

    if (pendingDone) {
      onTileDone(pendingDone.idx, pendingDone.region, pendingDone.tileOut, pendingDone.elapsedMs);
    }

    return {
      deviceName: `WebNN (${deviceType.toUpperCase()})`,
    };
  }

  async benchmarkTile({
    wasmBridge,
    modelPayload,
    tileSize = 128,
    channels = 3,
    deviceType = 'gpu',
    powerPreference = 'high-performance',
  }) {
    const parsed = this.ensureModelParsed(wasmBridge, modelPayload);
    const { context, key: ctxKey } = await this.getContext(deviceType, powerPreference);
    const dummy = new Float32Array(tileSize * tileSize * channels).fill(0.5);

    // Warmup compiles the MLGraph + persistent MLTensors and boosts GPU out of idle P8 state.
    const wStart = performance.now();
    for (let w = 0; w < 6; w += 1) {
      await this.inferTileWebNn(context, ctxKey, parsed, dummy, tileSize, tileSize, channels);
      if (performance.now() - wStart >= 75) break;
    }

    let totalMs = 0;
    const runs = 3;
    for (let i = 0; i < runs; i += 1) {
      const t0 = performance.now();
      await this.inferTileWebNn(context, ctxKey, parsed, dummy, tileSize, tileSize, channels);
      totalMs += performance.now() - t0;
    }
    return {
      avgTileMs: totalMs / runs,
      deviceName: `WebNN (${deviceType.toUpperCase()})`,
    };
  }

  dispose() {
    for (const compiled of this.graphCache.values()) {
      destroyCompiledWebNn(compiled);
    }
    this.graphCache.clear();
    for (const ctx of this.contexts.values()) {
      if (ctx && typeof ctx.destroy === 'function') {
        try { ctx.destroy(); } catch { /* ignore */ }
      }
    }
    this.contexts.clear();
    this.parsedCache = null;
  }
}
