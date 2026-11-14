// WebGPU WGSL compute backend (`WebGpuBackend`) for Upix ncnn graphs.
// Features:
// - Register-blocked 64x64 (`4x4` per thread, `@workgroup_size(16, 16, 1)`) shared-memory
//   `conv_1x1` GEMM and implicit-im2col `conv_tiled` kernels (+ 16x16 `conv_small` for narrow heads)
// - Dedicated workgroup halo-cached `conv_large_halo` kernel for 13x13 / 17x17-style dense
//   convs (stride 1, dilation 1, `kw,kh` in `7..19`, e.g. RealPLKSR), and a residue-stepped
//   4-channel-per-thread `deconv_gather` for Real-CUGAN ConvTranspose layers
// - Native `shader-f16` (`preferFp16`) storage pipeline with full `f32` FMA accumulation & reductions
// - Graph-level operator fusion (`Convolution` / `Deconvolution` / `InnerProduct` + `PReLU` / `ReLU` /
//   `Clip` / `Sigmoid` / `Swish`) eliminating ~40-50% of memory-bound intermediate dispatches
// - Compiled per-shape tile execution plan cache (`GPUBuffer` pool + pre-built `GPUBindGroup`s +
//   single-pass command-buffer coalescing + persistent `MAP_READ` staging + CPU/GPU tile overlap)

import { formatNumber, t } from '../../i18n/index.js';

const UNIFORM_STRIDE = 256;
const UNIFORM_SIZE = 64; // 16 x u32/f32
const MAX_CACHED_PLANS = 4;

const WGSL_ACTIVATION_FN = /* wgsl */ `
fn apply_act(v: f32, act_type: u32, p0: f32, p1: f32) -> f32 {
  switch (act_type) {
    case 1u: { return max(v, 0.0); }
    case 2u: { return select(v * p0, v, v > 0.0); }
    case 3u: { return clamp(v, p0, p1); }
    case 4u: { return 1.0 / (1.0 + exp(-v)); }
    case 5u: {
      var sp: f32;
      if (v > 20.0) {
        sp = v;
      } else if (v < -20.0) {
        sp = exp(v);
      } else {
        sp = log(1.0 + exp(v));
      }
      return v * tanh(sp);
    }
    case 6u: { return v * clamp(v * p0 + p1, 0.0, 1.0); }
    case 8u: { return v / (1.0 + exp(-v)); }
    default: { return v; }
  }
}

fn finish_channel_val(
  raw: f32,
  oc: u32,
  has_bias: u32,
  act_type: u32,
  p0: f32,
  p1: f32,
  bias_val: f32,
  slope_val: f32
) -> f32 {
  if (act_type == 7u) {
    let b = raw + bias_val;
    return select(b * slope_val, b, b >= 0.0);
  }
  var v = raw;
  if (has_bias != 0u) {
    v = v + bias_val;
  }
  return apply_act(v, act_type, p0, p1);
}
`;

const SHADER_BODIES = {
  // u = [in_w, in_h, feed_w, feed_h, channels, prepad, total_elems, 0...]
  preprocess: /* wgsl */ `
struct Params { u: array<u32, 16> };
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> src: array<f32>;
@group(0) @binding(2) var<storage, read_write> dst: array<Elem>;

@compute @workgroup_size(256)
fn main(
  @builtin(workgroup_id) wid: vec3<u32>,
  @builtin(num_workgroups) nwg: vec3<u32>,
  @builtin(local_invocation_index) lid: u32
) {
  let idx = (wid.y * nwg.x + wid.x) * 256u + lid;
  let total = params.u[6];
  if (idx >= total) { return; }

  let in_w = params.u[0];
  let in_h = params.u[1];
  let feed_w = params.u[2];
  let feed_h = params.u[3];
  let ch_cnt = params.u[4];
  let prepad = i32(params.u[5]);

  let plane = feed_w * feed_h;
  let c = idx / plane;
  let rem = idx - c * plane;
  let fy = i32(rem / feed_w);
  let fx = i32(rem - u32(fy) * feed_w);

  let sy = u32(clamp(fy - prepad, 0, i32(in_h) - 1));
  let sx = u32(clamp(fx - prepad, 0, i32(in_w) - 1));
  dst[idx] = Elem(src[(sy * in_w + sx) * ch_cnt + c]);
}
`,

  // u = [src_w, src_h, out_w, out_h, channels, total_elems, 0...]
  postprocess: /* wgsl */ `
struct Params { u: array<u32, 16> };
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> src: array<Elem>;
@group(0) @binding(2) var<storage, read_write> dst: array<f32>;

@compute @workgroup_size(256)
fn main(
  @builtin(workgroup_id) wid: vec3<u32>,
  @builtin(num_workgroups) nwg: vec3<u32>,
  @builtin(local_invocation_index) lid: u32
) {
  let idx = (wid.y * nwg.x + wid.x) * 256u + lid;
  let total = params.u[5];
  if (idx >= total) { return; }

  let src_w = params.u[0];
  let src_h = params.u[1];
  let out_w = params.u[2];
  let ch_cnt = params.u[4];

  let px = idx / ch_cnt;
  let c = idx - px * ch_cnt;
  let oy = px / out_w;
  let ox = px - oy * out_w;
  dst[idx] = f32(src[(c * src_h + oy) * src_w + ox]);
}
`,

  // Dedicated 64x64 (`4x4` per thread) 1x1 Pointwise Convolution (pure tiled GEMM, zero `/` or `%`)
  conv_1x1: /* wgsl */ `
struct Params { u: array<u32, 16> };
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> inp: array<Elem>;
@group(0) @binding(2) var<storage, read> weight: array<Elem>;
@group(0) @binding(3) var<storage, read> bias: array<f32>;
@group(0) @binding(4) var<storage, read_write> out: array<Elem>;

var<workgroup> tileA: array<f32, 1040>;
var<workgroup> tileB: array<f32, 1040>;

${WGSL_ACTIVATION_FN}

@compute @workgroup_size(16, 16, 1)
fn main(
  @builtin(workgroup_id) wid: vec3<u32>,
  @builtin(num_workgroups) nwg: vec3<u32>,
  @builtin(local_invocation_id) lid: vec3<u32>
) {
  let out_w = params.u[2];
  let out_h = params.u[3];
  let in_g = params.u[4];
  let out_g = params.u[5];
  let flags = params.u[13];
  let has_bias = flags & 0xffu;
  let act_type = (flags >> 8u) & 0xffu;
  let m_tiles = flags >> 16u;

  let g = wid.z;
  let wg_m = wid.y % m_tiles;
  let wg_n = wid.x + (wid.y / m_tiles) * nwg.x;

  let lx = lid.x;
  let ly = lid.y;
  let tid = ly * 16u + lx;

  let out_plane = out_w * out_h;
  let m_wg_base = wg_m * 64u;
  let n_wg_base = wg_n * 64u;
  let g_out_base = g * out_g;
  let g_in_base = g * in_g;

  // Thread mapping for coalesced global loads:
  // A (64 x 16): ka_local = tid & 15, row_base = tid >> 4 (step +16)
  // B (16 x 64): col_local = tid & 63, kb_base = tid >> 6 (step +4)
  let ka_local = tid & 15u;
  let a_row_base = tid >> 4u;
  let b_col_local = tid & 63u;
  let kb_base = tid >> 6u;
  let b_col = n_wg_base + b_col_local;
  let b_col_valid = b_col < out_plane;

  var acc0 = vec4<f32>(0.0);
  var acc1 = vec4<f32>(0.0);
  var acc2 = vec4<f32>(0.0);
  var acc3 = vec4<f32>(0.0);

  var t: u32 = 0u;
  loop {
    if (t >= in_g) { break; }

    let ka = t + ka_local;
    let ka_valid = ka < in_g;
    for (var s = 0u; s < 4u; s = s + 1u) {
      let r_loc = a_row_base + s * 16u;
      let m_idx = m_wg_base + r_loc;
      var wv: f32 = 0.0;
      if (ka_valid && m_idx < out_g) {
        wv = f32(weight[(g_out_base + m_idx) * in_g + ka]);
      }
      tileA[ka_local * 65u + r_loc] = wv;
    }

    for (var s = 0u; s < 4u; s = s + 1u) {
      let kb_loc = kb_base + s * 4u;
      let kb = t + kb_loc;
      var iv: f32 = 0.0;
      if (b_col_valid && kb < in_g) {
        iv = f32(inp[(g_in_base + kb) * out_plane + b_col]);
      }
      tileB[kb_loc * 65u + b_col_local] = iv;
    }

    workgroupBarrier();

    for (var k = 0u; k < 16u; k = k + 1u) {
      let a_off = k * 65u + ly * 4u;
      let b_off = k * 65u + lx;
      let bv = vec4<f32>(
        tileB[b_off],
        tileB[b_off + 16u],
        tileB[b_off + 32u],
        tileB[b_off + 48u]
      );
      acc0 = fma(vec4<f32>(tileA[a_off]), bv, acc0);
      acc1 = fma(vec4<f32>(tileA[a_off + 1u]), bv, acc1);
      acc2 = fma(vec4<f32>(tileA[a_off + 2u]), bv, acc2);
      acc3 = fma(vec4<f32>(tileA[a_off + 3u]), bv, acc3);
    }

    workgroupBarrier();
    t = t + 16u;
  }

  let p0 = bitcast<f32>(params.u[14]);
  let p1 = bitcast<f32>(params.u[15]);
  let m_thread_base = m_wg_base + ly * 4u;

  for (var r = 0u; r < 4u; r = r + 1u) {
    let m_idx = m_thread_base + r;
    if (m_idx < out_g) {
      let oc = g_out_base + m_idx;
      var b_val: f32 = 0.0;
      var s_val: f32 = 0.0;
      if (act_type == 7u) {
        b_val = bias[oc * 2u];
        s_val = bias[oc * 2u + 1u];
      } else if (has_bias != 0u) {
        b_val = bias[oc];
      }
      var row_vec: vec4<f32>;
      switch (r) {
        case 0u: { row_vec = acc0; }
        case 1u: { row_vec = acc1; }
        case 2u: { row_vec = acc2; }
        default: { row_vec = acc3; }
      }
      let out_row_base = oc * out_plane;
      for (var c = 0u; c < 4u; c = c + 1u) {
        let n_idx = n_wg_base + lx + c * 16u;
        if (n_idx < out_plane) {
          let val = finish_channel_val(row_vec[c], oc, has_bias, act_type, p0, p1, b_val, s_val);
          out[out_row_base + n_idx] = Elem(val);
        }
      }
    }
  }
}
`,

  // Register-blocked 64x64 (`4x4` per thread) Tiled Implicit-im2col Convolution (`out_g > 16`)
  conv_tiled: /* wgsl */ `
struct Params { u: array<u32, 16> };
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> inp: array<Elem>;
@group(0) @binding(2) var<storage, read> weight: array<Elem>;
@group(0) @binding(3) var<storage, read> bias: array<f32>;
@group(0) @binding(4) var<storage, read_write> out: array<Elem>;

var<workgroup> tileA: array<f32, 1040>;
var<workgroup> tileB: array<f32, 1040>;

${WGSL_ACTIVATION_FN}

@compute @workgroup_size(16, 16, 1)
fn main(
  @builtin(workgroup_id) wid: vec3<u32>,
  @builtin(num_workgroups) nwg: vec3<u32>,
  @builtin(local_invocation_id) lid: vec3<u32>
) {
  let in_w = params.u[0];
  let in_h = params.u[1];
  let out_w = params.u[2];
  let out_h = params.u[3];
  let in_g = params.u[4];
  let out_g = params.u[5];
  let kw = params.u[6];
  let kh = params.u[7];
  let stride_w = params.u[8] & 0xffffu;
  let stride_h = params.u[8] >> 16u;
  let dil_w = params.u[9] & 0xffffu;
  let dil_h = params.u[9] >> 16u;
  let pad_left = bitcast<i32>(params.u[10]);
  let pad_top = bitcast<i32>(params.u[11]);
  let pad_value = bitcast<f32>(params.u[12]);
  let flags = params.u[13];
  let has_bias = flags & 0xffu;
  let act_type = (flags >> 8u) & 0xffu;
  let m_tiles = flags >> 16u;

  let g = wid.z;
  let wg_m = wid.y % m_tiles;
  let wg_n = wid.x + (wid.y / m_tiles) * nwg.x;

  let lx = lid.x;
  let ly = lid.y;
  let tid = ly * 16u + lx;

  let out_plane = out_w * out_h;
  let maxk = kw * kh;
  let k_dim = in_g * maxk;
  let m_wg_base = wg_m * 64u;
  let n_wg_base = wg_n * 64u;
  let g_out_base = g * out_g;
  let g_in_base = g * in_g;

  let ka_local = tid & 15u;
  let a_row_base = tid >> 4u;
  let b_col_local = tid & 63u;
  let kb_base = tid >> 6u;
  let b_col = n_wg_base + b_col_local;
  let b_col_valid = b_col < out_plane;
  let b_oy = b_col / out_w;
  let b_ox = b_col - b_oy * out_w;
  let b_sy_base = i32(b_oy * stride_h) - pad_top;
  let b_sx_base = i32(b_ox * stride_w) - pad_left;

  var acc0 = vec4<f32>(0.0);
  var acc1 = vec4<f32>(0.0);
  var acc2 = vec4<f32>(0.0);
  var acc3 = vec4<f32>(0.0);

  var t: u32 = 0u;
  loop {
    if (t >= k_dim) { break; }

    let ka = t + ka_local;
    let ka_valid = ka < k_dim;
    for (var s = 0u; s < 4u; s = s + 1u) {
      let r_loc = a_row_base + s * 16u;
      let m_idx = m_wg_base + r_loc;
      var wv: f32 = 0.0;
      if (ka_valid && m_idx < out_g) {
        wv = f32(weight[(g_out_base + m_idx) * k_dim + ka]);
      }
      tileA[ka_local * 65u + r_loc] = wv;
    }

    for (var s = 0u; s < 4u; s = s + 1u) {
      let kb_loc = kb_base + s * 4u;
      let kb = t + kb_loc;
      var iv: f32 = 0.0;
      if (b_col_valid && kb < k_dim) {
        let ic = kb / maxk;
        let rem = kb - ic * maxk;
        let ky = rem / kw;
        let kx = rem - ky * kw;
        let sy = b_sy_base + i32(ky * dil_h);
        let sx = b_sx_base + i32(kx * dil_w);
        if (sy >= 0 && sy < i32(in_h) && sx >= 0 && sx < i32(in_w)) {
          iv = f32(inp[((g_in_base + ic) * in_h + u32(sy)) * in_w + u32(sx)]);
        } else {
          iv = pad_value;
        }
      }
      tileB[kb_loc * 65u + b_col_local] = iv;
    }

    workgroupBarrier();

    for (var k = 0u; k < 16u; k = k + 1u) {
      let a_off = k * 65u + ly * 4u;
      let b_off = k * 65u + lx;
      let bv = vec4<f32>(
        tileB[b_off],
        tileB[b_off + 16u],
        tileB[b_off + 32u],
        tileB[b_off + 48u]
      );
      acc0 = fma(vec4<f32>(tileA[a_off]), bv, acc0);
      acc1 = fma(vec4<f32>(tileA[a_off + 1u]), bv, acc1);
      acc2 = fma(vec4<f32>(tileA[a_off + 2u]), bv, acc2);
      acc3 = fma(vec4<f32>(tileA[a_off + 3u]), bv, acc3);
    }

    workgroupBarrier();
    t = t + 16u;
  }

  let p0 = bitcast<f32>(params.u[14]);
  let p1 = bitcast<f32>(params.u[15]);
  let m_thread_base = m_wg_base + ly * 4u;

  for (var r = 0u; r < 4u; r = r + 1u) {
    let m_idx = m_thread_base + r;
    if (m_idx < out_g) {
      let oc = g_out_base + m_idx;
      var b_val: f32 = 0.0;
      var s_val: f32 = 0.0;
      if (act_type == 7u) {
        b_val = bias[oc * 2u];
        s_val = bias[oc * 2u + 1u];
      } else if (has_bias != 0u) {
        b_val = bias[oc];
      }
      var row_vec: vec4<f32>;
      switch (r) {
        case 0u: { row_vec = acc0; }
        case 1u: { row_vec = acc1; }
        case 2u: { row_vec = acc2; }
        default: { row_vec = acc3; }
      }
      let out_row_base = oc * out_plane;
      for (var c = 0u; c < 4u; c = c + 1u) {
        let n_idx = n_wg_base + lx + c * 16u;
        if (n_idx < out_plane) {
          let val = finish_channel_val(row_vec[c], oc, has_bias, act_type, p0, p1, b_val, s_val);
          out[out_row_base + n_idx] = Elem(val);
        }
      }
    }
  }
}
`,

  // Compact 16x16 tiled convolution for narrow output heads (`out_g <= 16`, e.g. 3ch RGB / 12ch PixelShuffle)
  conv_small: /* wgsl */ `
struct Params { u: array<u32, 16> };
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> inp: array<Elem>;
@group(0) @binding(2) var<storage, read> weight: array<Elem>;
@group(0) @binding(3) var<storage, read> bias: array<f32>;
@group(0) @binding(4) var<storage, read_write> out: array<Elem>;

var<workgroup> tileA: array<f32, 272>;
var<workgroup> tileB: array<f32, 272>;

${WGSL_ACTIVATION_FN}

@compute @workgroup_size(16, 16, 1)
fn main(
  @builtin(workgroup_id) wid: vec3<u32>,
  @builtin(num_workgroups) nwg: vec3<u32>,
  @builtin(local_invocation_id) lid: vec3<u32>
) {
  let in_w = params.u[0];
  let in_h = params.u[1];
  let out_w = params.u[2];
  let out_h = params.u[3];
  let in_g = params.u[4];
  let out_g = params.u[5];
  let kw = params.u[6];
  let kh = params.u[7];
  let stride_w = params.u[8] & 0xffffu;
  let stride_h = params.u[8] >> 16u;
  let dil_w = params.u[9] & 0xffffu;
  let dil_h = params.u[9] >> 16u;
  let pad_left = bitcast<i32>(params.u[10]);
  let pad_top = bitcast<i32>(params.u[11]);
  let pad_value = bitcast<f32>(params.u[12]);
  let flags = params.u[13];
  let has_bias = flags & 0xffu;
  let act_type = (flags >> 8u) & 0xffu;
  let m_tiles = flags >> 16u;

  let g = wid.z;
  let wg_m = wid.y % m_tiles;
  let wg_n = wid.x + (wid.y / m_tiles) * nwg.x;

  let ly = lid.y;
  let lx = lid.x;
  let m = wg_m * 16u + ly;
  let n = wg_n * 16u + lx;

  let out_plane = out_w * out_h;
  let maxk = kw * kh;
  let k_dim = in_g * maxk;

  let oy = n / out_w;
  let ox = n - oy * out_w;
  let sy_base = i32(oy * stride_h) - pad_top;
  let sx_base = i32(ox * stride_w) - pad_left;
  let w_row_base = (g * out_g + m) * k_dim;
  let in_g_base = g * in_g;

  var acc: f32 = 0.0;
  var t: u32 = 0u;
  loop {
    if (t >= k_dim) { break; }

    let ka = t + lx;
    if (m < out_g && ka < k_dim) {
      tileA[ly * 17u + lx] = f32(weight[w_row_base + ka]);
    } else {
      tileA[ly * 17u + lx] = 0.0;
    }

    let kb = t + ly;
    if (n < out_plane && kb < k_dim) {
      let ic = kb / maxk;
      let rem = kb - ic * maxk;
      let ky = rem / kw;
      let kx = rem - ky * kw;
      let sy = sy_base + i32(ky * dil_h);
      let sx = sx_base + i32(kx * dil_w);
      if (sy >= 0 && sy < i32(in_h) && sx >= 0 && sx < i32(in_w)) {
        tileB[ly * 17u + lx] = f32(inp[((in_g_base + ic) * in_h + u32(sy)) * in_w + u32(sx)]);
      } else {
        tileB[ly * 17u + lx] = pad_value;
      }
    } else {
      tileB[ly * 17u + lx] = 0.0;
    }

    workgroupBarrier();

    for (var k = 0u; k < 16u; k = k + 1u) {
      acc = fma(tileA[ly * 17u + k], tileB[k * 17u + lx], acc);
    }

    workgroupBarrier();
    t = t + 16u;
  }

  if (m < out_g && n < out_plane) {
    let oc = g * out_g + m;
    var b_val: f32 = 0.0;
    var s_val: f32 = 0.0;
    if (act_type == 7u) {
      b_val = bias[oc * 2u];
      s_val = bias[oc * 2u + 1u];
    } else if (has_bias != 0u) {
      b_val = bias[oc];
    }
    let p0 = bitcast<f32>(params.u[14]);
    let p1 = bitcast<f32>(params.u[15]);
    out[oc * out_plane + n] = Elem(finish_channel_val(acc, oc, has_bias, act_type, p0, p1, b_val, s_val));
  }
}
`,

  // Dedicated workgroup halo-cached large-kernel convolution (stride 1, dilation 1, `7 <= kw,kh <= 19`,
  // e.g. the 13x13 / 17x17 partial dense convs in RealPLKSR). Each 16x16 workgroup computes a
  // 16x16 spatial tile of 4 output channels: 256 threads cooperatively stage the
  // `(16 + kw - 1) x (16 + kh - 1)` input halo plus a `4 x kh x kw` weight slice into
  // `var<workgroup>` (`1156 + 1444 = 2600 f32 = 10.4 KB < 16 KB` default limit), then every
  // thread streams `in_g * kh * kw` FMA per channel purely from shared memory instead of
  // re-reading global memory per tap (~30x less global traffic than `conv_small` / `conv_tiled`).
  conv_large_halo: /* wgsl */ `
struct Params { u: array<u32, 16> };
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> inp: array<Elem>;
@group(0) @binding(2) var<storage, read> weight: array<Elem>;
@group(0) @binding(3) var<storage, read> bias: array<f32>;
@group(0) @binding(4) var<storage, read_write> out: array<Elem>;

var<workgroup> inHalo: array<f32, 1156>; // 34 x 34: 16px tile + <=19px kernel halo, fixed 34 stride
var<workgroup> wCache: array<f32, 1444>; // 4 output channels x 19 x 19 weights (maxk stride)

${WGSL_ACTIVATION_FN}

@compute @workgroup_size(16, 16, 1)
fn main(
  @builtin(workgroup_id) wid: vec3<u32>,
  @builtin(num_workgroups) nwg: vec3<u32>,
  @builtin(local_invocation_id) lid: vec3<u32>
) {
  let in_w = params.u[0];
  let in_h = params.u[1];
  let out_w = params.u[2];
  let out_h = params.u[3];
  let in_g = params.u[4];
  let out_g = params.u[5];
  let kw = params.u[6];
  let kh = params.u[7];
  let pad_left = bitcast<i32>(params.u[10]);
  let pad_top = bitcast<i32>(params.u[11]);
  let pad_value = bitcast<f32>(params.u[12]);
  let flags = params.u[13];
  let has_bias = flags & 0xffu;
  let act_type = (flags >> 8u) & 0xffu;
  let m_tiles = flags >> 16u; // output-channel blocks (4 channels each)

  let g = wid.z;
  let wg_m = wid.y % m_tiles;
  let tile_chunk = wid.y / m_tiles;
  let tile_idx = wid.x + tile_chunk * nwg.x;

  let tiles_x = (out_w + 15u) / 16u;
  let ox0 = (tile_idx % tiles_x) * 16u;
  let oy0 = (tile_idx / tiles_x) * 16u;

  let lx = lid.x;
  let ly = lid.y;
  let tid = ly * 16u + lx;

  let out_plane = out_w * out_h;
  let in_plane = in_w * in_h;
  let maxk = kw * kh;
  let g_out_base = g * out_g;
  let g_in_base = g * in_g;
  let oc_base = wg_m * 4u;

  // Halo window top-left in input coordinates (stride 1 + dilation 1 conv)
  let ix0 = i32(ox0) - pad_left;
  let iy0 = i32(oy0) - pad_top;

  // Weight cache lane strides (hoisted so the inner loop is pure fma + shared reads)
  let w_lane1 = maxk;
  let w_lane2 = maxk * 2u;
  let w_lane3 = maxk * 3u;

  var acc0: f32 = 0.0;
  var acc1: f32 = 0.0;
  var acc2: f32 = 0.0;
  var acc3: f32 = 0.0;

  var ic: u32 = 0u;
  loop {
    if (ic >= in_g) { break; }

    // Cooperatively stage the input halo patch for channel ic
    let ic_base = (g_in_base + ic) * in_plane;
    for (var i = tid; i < 1156u; i = i + 256u) {
      let hy = i / 34u;
      let hx = i - hy * 34u;
      let sx = ix0 + i32(hx);
      let sy = iy0 + i32(hy);
      var v: f32 = pad_value;
      if (sx >= 0 && sx < i32(in_w) && sy >= 0 && sy < i32(in_h)) {
        v = f32(inp[ic_base + u32(sy) * in_w + u32(sx)]);
      }
      inHalo[hy * 34u + hx] = v;
    }

    // Cooperatively stage the 4-channel weight slice for channel ic
    let w_ic_base = (g_out_base + oc_base) * in_g * maxk + ic * maxk;
    for (var i = tid; i < 1444u; i = i + 256u) {
      let oc_local = i / maxk;
      let k_off = i - oc_local * maxk;
      var wv: f32 = 0.0;
      if (oc_base + oc_local < out_g) {
        wv = f32(weight[w_ic_base + oc_local * in_g * maxk + k_off]);
      }
      wCache[i] = wv;
    }

    workgroupBarrier();

    // Every thread: 1 output pixel x 4 channels, all taps served from shared memory.
    // Halo reads stay in [0, 15 + kh - 1] x [0, 15 + kw - 1] — exactly the staged patch.
    let h_base = ly * 34u + lx;
    for (var ky = 0u; ky < kh; ky = ky + 1u) {
      let h_row = h_base + ky * 34u;
      let w_row = ky * kw;
      for (var kx = 0u; kx < kw; kx = kx + 1u) {
        let iv = inHalo[h_row + kx];
        let k_off = w_row + kx;
        acc0 = fma(wCache[k_off], iv, acc0);
        acc1 = fma(wCache[w_lane1 + k_off], iv, acc1);
        acc2 = fma(wCache[w_lane2 + k_off], iv, acc2);
        acc3 = fma(wCache[w_lane3 + k_off], iv, acc3);
      }
    }

    workgroupBarrier();
    ic = ic + 1u;
  }

  let ox = ox0 + lx;
  let oy = oy0 + ly;
  if (ox < out_w && oy < out_h) {
    let p0 = bitcast<f32>(params.u[14]);
    let p1 = bitcast<f32>(params.u[15]);
    let n_idx = oy * out_w + ox;
    var accs: vec4<f32>;
    accs[0] = acc0;
    accs[1] = acc1;
    accs[2] = acc2;
    accs[3] = acc3;
    let oc_end = g_out_base + out_g;
    for (var j = 0u; j < 4u; j = j + 1u) {
      let oc = g_out_base + oc_base + j;
      if (oc < oc_end) {
        var b_val: f32 = 0.0;
        var s_val: f32 = 0.0;
        if (act_type == 7u) {
          b_val = bias[oc * 2u];
          s_val = bias[oc * 2u + 1u];
        } else if (has_bias != 0u) {
          b_val = bias[oc];
        }
        out[oc * out_plane + n_idx] = Elem(finish_channel_val(accs[j], oc, has_bias, act_type, p0, p1, b_val, s_val));
      }
    }
  }
}
`,

  // Depthwise Convolution (`group == num_input == num_output`)
  conv_depthwise: /* wgsl */ `
struct Params { u: array<u32, 16> };
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> inp: array<Elem>;
@group(0) @binding(2) var<storage, read> weight: array<Elem>;
@group(0) @binding(3) var<storage, read> bias: array<f32>;
@group(0) @binding(4) var<storage, read_write> out: array<Elem>;

${WGSL_ACTIVATION_FN}

@compute @workgroup_size(256)
fn main(
  @builtin(workgroup_id) wid: vec3<u32>,
  @builtin(num_workgroups) nwg: vec3<u32>,
  @builtin(local_invocation_index) lid: u32
) {
  let idx = (wid.y * nwg.x + wid.x) * 256u + lid;
  let out_w = params.u[2];
  let out_h = params.u[3];
  let channels = params.u[5];
  let total = out_w * out_h * channels;
  if (idx >= total) { return; }

  let in_w = params.u[0];
  let in_h = params.u[1];
  let kw = params.u[6];
  let kh = params.u[7];
  let stride_w = params.u[8] & 0xffffu;
  let stride_h = params.u[8] >> 16u;
  let dil_w = params.u[9] & 0xffffu;
  let dil_h = params.u[9] >> 16u;
  let pad_left = bitcast<i32>(params.u[10]);
  let pad_top = bitcast<i32>(params.u[11]);
  let pad_value = bitcast<f32>(params.u[12]);
  let flags = params.u[13];
  let has_bias = flags & 0xffu;
  let act_type = (flags >> 8u) & 0xffu;

  let out_plane = out_w * out_h;
  let c = idx / out_plane;
  let rem = idx - c * out_plane;
  let oy = rem / out_w;
  let ox = rem - oy * out_w;

  let maxk = kw * kh;
  let w_base = c * maxk;
  let in_base = c * in_w * in_h;

  var sum: f32 = 0.0;
  for (var ky = 0u; ky < kh; ky = ky + 1u) {
    let sy = i32(oy * stride_h + ky * dil_h) - pad_top;
    for (var kx = 0u; kx < kw; kx = kx + 1u) {
      let sx = i32(ox * stride_w + kx * dil_w) - pad_left;
      var v: f32 = pad_value;
      if (sy >= 0 && sy < i32(in_h) && sx >= 0 && sx < i32(in_w)) {
        v = f32(inp[in_base + u32(sy) * in_w + u32(sx)]);
      }
      sum = fma(f32(weight[w_base + ky * kw + kx]), v, sum);
    }
  }

  var b_val: f32 = 0.0;
  var s_val: f32 = 0.0;
  if (act_type == 7u) {
    b_val = bias[c * 2u];
    s_val = bias[c * 2u + 1u];
  } else if (has_bias != 0u) {
    b_val = bias[c];
  }
  let res = finish_channel_val(sum, c, has_bias, act_type, bitcast<f32>(params.u[14]), bitcast<f32>(params.u[15]), b_val, s_val);
  out[idx] = Elem(res);
}
`,

  // Gather Deconvolution (exact ncnn `[g, out_g, in_g, kh, kw]` weight layout, zero atomics).
  // Each thread computes 4 output channels of one pixel: for every valid `(ky, kx)` tap one
  // gathered input element feeds all 4 accumulators (4x less input traffic, identical
  // `ky -> kx -> ic` fma order, so results stay bit-exact vs the 1-channel/thread version).
  // For undilated kernels the valid taps form arithmetic progressions
  // `ky = max(fy % stride_h, fy - (in_h - 1) * stride_h) .. min(kh - 1, fy)` step `stride_h`
  // (same for `kx`), so the loops run with zero `%` re-checks and zero skipped iterations.
  deconv_gather: /* wgsl */ `
struct Params { u: array<u32, 16> };
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> inp: array<Elem>;
@group(0) @binding(2) var<storage, read> weight: array<Elem>;
@group(0) @binding(3) var<storage, read> bias: array<f32>;
@group(0) @binding(4) var<storage, read_write> out: array<Elem>;

${WGSL_ACTIVATION_FN}

@compute @workgroup_size(256)
fn main(
  @builtin(workgroup_id) wid: vec3<u32>,
  @builtin(num_workgroups) nwg: vec3<u32>,
  @builtin(local_invocation_index) lid: u32
) {
  let idx = (wid.y * nwg.x + wid.x) * 256u + lid;
  let out_w = params.u[2];
  let out_h = params.u[3];
  let in_g = params.u[4];
  let out_g = params.u[5];
  let group_cnt = params.u[13] >> 16u;
  let oc_blocks_g = (out_g + 3u) / 4u;
  let out_plane = out_w * out_h;
  let total = group_cnt * oc_blocks_g * out_plane;
  if (idx >= total) { return; }

  let in_w = params.u[0];
  let in_h = params.u[1];
  let kw = params.u[6];
  let kh = params.u[7];
  let stride_w = i32(params.u[8] & 0xffffu);
  let stride_h = i32(params.u[8] >> 16u);
  let dil_w = i32(params.u[9] & 0xffffu);
  let dil_h = i32(params.u[9] >> 16u);
  let pad_left = bitcast<i32>(params.u[10]);
  let pad_top = bitcast<i32>(params.u[11]);
  let has_bias = params.u[13] & 0xffu;
  let act_type = (params.u[13] >> 8u) & 0xffu;

  let oc_blk_all = idx / out_plane;
  let px = idx - oc_blk_all * out_plane;
  let g = oc_blk_all / oc_blocks_g;
  let oc_blk = oc_blk_all - g * oc_blocks_g;
  let oy = i32(px / out_w);
  let ox = i32(px - u32(oy) * out_w);

  let oc_base = g * out_g + oc_blk * 4u;
  let fy = oy + pad_top;
  let fx = ox + pad_left;

  let maxk = kw * kh;
  let in_plane = in_w * in_h;
  let g_in_base = g * in_g;
  let w_stride = in_g * maxk;
  let w_oc0 = oc_base * w_stride;
  let w_oc1 = w_oc0 + w_stride;
  let w_oc2 = w_oc1 + w_stride;
  let w_oc3 = w_oc2 + w_stride;

  var sum0: f32 = 0.0;
  var sum1: f32 = 0.0;
  var sum2: f32 = 0.0;
  var sum3: f32 = 0.0;

  if (dil_h == 1 && dil_w == 1) {
    // Undilated fast path: solve the valid tap progressions up-front (pads are >= 0 so
    // fy / fx are non-negative). Every visited tap satisfies dy % stride == 0,
    // 0 <= iy < in_h and 0 <= ix < in_w by construction — no per-iteration checks.
    let ky_start = max(fy % stride_h, fy - (i32(in_h) - 1) * stride_h);
    let ky_end = min(i32(kh) - 1, fy);
    let kx_start = max(fx % stride_w, fx - (i32(in_w) - 1) * stride_w);
    let kx_end = min(i32(kw) - 1, fx);
    for (var ky = ky_start; ky <= ky_end; ky = ky + stride_h) {
      let row_base = u32((fy - ky) / stride_h) * in_w;
      let w_ky = u32(ky) * kw;
      for (var kx = kx_start; kx <= kx_end; kx = kx + stride_w) {
        let k_off = w_ky + u32(kx);
        let pix_off = row_base + u32((fx - kx) / stride_w);
        for (var ic = 0u; ic < in_g; ic = ic + 1u) {
          let iv = f32(inp[(g_in_base + ic) * in_plane + pix_off]);
          sum0 = fma(f32(weight[w_oc0 + ic * maxk + k_off]), iv, sum0);
          sum1 = fma(f32(weight[w_oc1 + ic * maxk + k_off]), iv, sum1);
          sum2 = fma(f32(weight[w_oc2 + ic * maxk + k_off]), iv, sum2);
          sum3 = fma(f32(weight[w_oc3 + ic * maxk + k_off]), iv, sum3);
        }
      }
    }
  } else {
    // Dilated fallback: original per-tap divisibility checks, 4 channels per thread.
    for (var ky = 0u; ky < kh; ky = ky + 1u) {
      let dy = fy - i32(ky) * dil_h;
      if (dy < 0 || (dy % stride_h) != 0) { continue; }
      let iy = dy / stride_h;
      if (iy >= i32(in_h)) { continue; }
      let row_base = u32(iy) * in_w;
      let w_ky = ky * kw;
      for (var kx = 0u; kx < kw; kx = kx + 1u) {
        let dx = fx - i32(kx) * dil_w;
        if (dx < 0 || (dx % stride_w) != 0) { continue; }
        let ix = dx / stride_w;
        if (ix >= i32(in_w)) { continue; }
        let k_off = w_ky + kx;
        let pix_off = row_base + u32(ix);
        for (var ic = 0u; ic < in_g; ic = ic + 1u) {
          let iv = f32(inp[(g_in_base + ic) * in_plane + pix_off]);
          sum0 = fma(f32(weight[w_oc0 + ic * maxk + k_off]), iv, sum0);
          sum1 = fma(f32(weight[w_oc1 + ic * maxk + k_off]), iv, sum1);
          sum2 = fma(f32(weight[w_oc2 + ic * maxk + k_off]), iv, sum2);
          sum3 = fma(f32(weight[w_oc3 + ic * maxk + k_off]), iv, sum3);
        }
      }
    }
  }

  let p0 = bitcast<f32>(params.u[14]);
  let p1 = bitcast<f32>(params.u[15]);
  let grp_end = (g + 1u) * out_g;
  var oc_all: vec4<u32>;
  oc_all[0] = oc_base;
  oc_all[1] = oc_base + 1u;
  oc_all[2] = oc_base + 2u;
  oc_all[3] = oc_base + 3u;
  var sums: vec4<f32>;
  sums[0] = sum0;
  sums[1] = sum1;
  sums[2] = sum2;
  sums[3] = sum3;
  for (var j = 0u; j < 4u; j = j + 1u) {
    let oc_all_j = oc_all[j];
    if (oc_all_j < grp_end) {
      var b_val: f32 = 0.0;
      var s_val: f32 = 0.0;
      if (act_type == 7u) {
        b_val = bias[oc_all_j * 2u];
        s_val = bias[oc_all_j * 2u + 1u];
      } else if (has_bias != 0u) {
        b_val = bias[oc_all_j];
      }
      out[oc_all_j * out_plane + px] = Elem(finish_channel_val(sums[j], oc_all_j, has_bias, act_type, p0, p1, b_val, s_val));
    }
  }
}
`,

  // InnerProduct: u = [num_input, num_output, has_bias, act_type, p0, p1]
  inner_product: /* wgsl */ `
struct Params { u: array<u32, 16> };
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> inp: array<Elem>;
@group(0) @binding(2) var<storage, read> weight: array<Elem>;
@group(0) @binding(3) var<storage, read> bias: array<f32>;
@group(0) @binding(4) var<storage, read_write> out: array<Elem>;

${WGSL_ACTIVATION_FN}

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let oc = gid.x;
  let num_in = params.u[0];
  let num_out = params.u[1];
  if (oc >= num_out) { return; }

  let has_bias = params.u[2];
  let act_type = params.u[3];
  var sum: f32 = 0.0;
  let base = oc * num_in;
  for (var i = 0u; i < num_in; i = i + 1u) {
    sum = fma(f32(weight[base + i]), f32(inp[i]), sum);
  }
  var b_val: f32 = 0.0;
  var s_val: f32 = 0.0;
  if (act_type == 7u) {
    b_val = bias[oc * 2u];
    s_val = bias[oc * 2u + 1u];
  } else if (has_bias != 0u) {
    b_val = bias[oc];
  }
  let res = finish_channel_val(sum, oc, has_bias, act_type, bitcast<f32>(params.u[4]), bitcast<f32>(params.u[5]), b_val, s_val);
  out[oc] = Elem(res);
}
`,

  // Global Pooling (1 workgroup of 256 threads per channel!)
  // u = [plane_size, channels, pooling_type]
  pooling_global: /* wgsl */ `
struct Params { u: array<u32, 16> };
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> inp: array<Elem>;
@group(0) @binding(2) var<storage, read_write> out: array<Elem>;

var<workgroup> sdata: array<f32, 256>;

@compute @workgroup_size(256)
fn main(
  @builtin(workgroup_id) wid: vec3<u32>,
  @builtin(local_invocation_index) lid: u32
) {
  let c = wid.x;
  let plane = params.u[0];
  let ptype = params.u[2];
  let base = c * plane;

  if (ptype == 0u) {
    var mx: f32 = -3.402823e+38;
    var i = lid;
    loop {
      if (i >= plane) { break; }
      mx = max(mx, f32(inp[base + i]));
      i = i + 256u;
    }
    sdata[lid] = mx;
    workgroupBarrier();
    var step = 128u;
    loop {
      if (step == 0u) { break; }
      if (lid < step) { sdata[lid] = max(sdata[lid], sdata[lid + step]); }
      workgroupBarrier();
      step = step >> 1u;
    }
    if (lid == 0u) { out[c] = Elem(sdata[0]); }
  } else {
    var sum: f32 = 0.0;
    var i = lid;
    loop {
      if (i >= plane) { break; }
      sum = sum + f32(inp[base + i]);
      i = i + 256u;
    }
    sdata[lid] = sum;
    workgroupBarrier();
    var step = 128u;
    loop {
      if (step == 0u) { break; }
      if (lid < step) { sdata[lid] = sdata[lid] + sdata[lid + step]; }
      workgroupBarrier();
      step = step >> 1u;
    }
    if (lid == 0u) { out[c] = Elem(sdata[0] / f32(max(plane, 1u))); }
  }
}
`,

  // Unary & PReLU:
  // u = [total, plane_size, mode (0=relu, 1=clip, 2=swish, 3=sigmoid, 4=prelu), num_slope, p0, p1]
  unary_op: /* wgsl */ `
struct Params { u: array<u32, 16> };
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> inp: array<Elem>;
@group(0) @binding(2) var<storage, read> slope_buf: array<f32>;
@group(0) @binding(3) var<storage, read_write> out: array<Elem>;

@compute @workgroup_size(256)
fn main(
  @builtin(workgroup_id) wid: vec3<u32>,
  @builtin(num_workgroups) nwg: vec3<u32>,
  @builtin(local_invocation_index) lid: u32
) {
  let idx = (wid.y * nwg.x + wid.x) * 256u + lid;
  if (idx >= params.u[0]) { return; }

  let v = f32(inp[idx]);
  let mode = params.u[2];
  let p0 = bitcast<f32>(params.u[4]);
  let p1 = bitcast<f32>(params.u[5]);

  var res: f32 = v;
  switch (mode) {
    case 0u: { res = select(v * p0, v, v >= 0.0); }
    case 1u: { res = clamp(v, p0, p1); }
    case 2u: { res = v / (1.0 + exp(-v)); }
    case 3u: { res = 1.0 / (1.0 + exp(-v)); }
    case 4u: {
      let c = idx / max(params.u[1], 1u);
      let s = slope_buf[select(0u, c, params.u[3] > 1u)];
      res = select(v * s, v, v >= 0.0);
    }
    default: {}
  }
  out[idx] = Elem(res);
}
`,

  // BinaryOp (supports scalar AND full 3D `[c, h, w]` broadcast)
  // u = [aw, ah, ac, bw, bh, bc, ow, oh, oc, op_type, with_scalar, bitcast(scalar), total]
  binary_op: /* wgsl */ `
struct Params { u: array<u32, 16> };
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> a_buf: array<Elem>;
@group(0) @binding(2) var<storage, read> b_buf: array<Elem>;
@group(0) @binding(3) var<storage, read_write> out: array<Elem>;

fn eval_bin(op: u32, a: f32, b: f32) -> f32 {
  switch (op) {
    case 0u: { return a + b; }
    case 1u: { return a - b; }
    case 2u: { return a * b; }
    case 3u: { return a / b; }
    case 4u: { return max(a, b); }
    case 5u: { return min(a, b); }
    case 6u: { return pow(a, b); }
    case 7u: { return b - a; }
    case 8u: { return b / a; }
    case 9u: { return pow(b, a); }
    default: { return a + b; }
  }
}

@compute @workgroup_size(256)
fn main(
  @builtin(workgroup_id) wid: vec3<u32>,
  @builtin(num_workgroups) nwg: vec3<u32>,
  @builtin(local_invocation_index) lid: u32
) {
  let idx = (wid.y * nwg.x + wid.x) * 256u + lid;
  let total = params.u[12];
  if (idx >= total) { return; }

  let op = params.u[9];
  if (params.u[10] != 0u) {
    out[idx] = Elem(eval_bin(op, f32(a_buf[idx]), bitcast<f32>(params.u[11])));
    return;
  }

  let aw = params.u[0]; let ah = params.u[1]; let ac = params.u[2];
  let bw = params.u[3]; let bh = params.u[4]; let bc = params.u[5];
  let ow = params.u[6]; let oh = params.u[7];

  if (aw == ow && ah == oh && bw == ow && bh == oh && ac == bc) {
    out[idx] = Elem(eval_bin(op, f32(a_buf[idx]), f32(b_buf[idx])));
    return;
  }

  let o_plane = ow * oh;
  let c = idx / o_plane;
  let rem = idx - c * o_plane;
  let y = rem / ow;
  let x = rem - y * ow;

  let ca = select(min(c, ac - 1u), 0u, ac == 1u);
  let ya = select(min(y, ah - 1u), 0u, ah == 1u);
  let xa = select(min(x, aw - 1u), 0u, aw == 1u);

  let cb = select(min(c, bc - 1u), 0u, bc == 1u);
  let yb = select(min(y, bh - 1u), 0u, bh == 1u);
  let xb = select(min(x, bw - 1u), 0u, bw == 1u);

  let va = f32(a_buf[(ca * ah + ya) * aw + xa]);
  let vb = f32(b_buf[(cb * bh + yb) * bw + xb]);
  out[idx] = Elem(eval_bin(op, va, vb));
}
`,

  // GroupNorm: 1 workgroup of 256 threads per group `g in 0..groups`!
  // u = [plane_size, ch_per_group, affine, bitcast(eps)]
  groupnorm: /* wgsl */ `
struct Params { u: array<u32, 16> };
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> inp: array<Elem>;
@group(0) @binding(2) var<storage, read> gamma: array<f32>;
@group(0) @binding(3) var<storage, read> beta: array<f32>;
@group(0) @binding(4) var<storage, read_write> out: array<Elem>;

var<workgroup> sdata: array<f32, 256>;

@compute @workgroup_size(256)
fn main(
  @builtin(workgroup_id) wid: vec3<u32>,
  @builtin(local_invocation_index) lid: u32
) {
  let g = wid.x;
  let plane = params.u[0];
  let ch_g = params.u[1];
  let affine = params.u[2];
  let eps = bitcast<f32>(params.u[3]);

  let group_elems = plane * ch_g;
  let g_base = g * group_elems;

  var sum: f32 = 0.0;
  var i = lid;
  loop {
    if (i >= group_elems) { break; }
    sum = sum + f32(inp[g_base + i]);
    i = i + 256u;
  }
  sdata[lid] = sum;
  workgroupBarrier();

  var step = 128u;
  loop {
    if (step == 0u) { break; }
    if (lid < step) { sdata[lid] = sdata[lid] + sdata[lid + step]; }
    workgroupBarrier();
    step = step >> 1u;
  }
  let mean = sdata[0] / f32(max(group_elems, 1u));
  workgroupBarrier();

  var sqsum: f32 = 0.0;
  i = lid;
  loop {
    if (i >= group_elems) { break; }
    let d = f32(inp[g_base + i]) - mean;
    sqsum = fma(d, d, sqsum);
    i = i + 256u;
  }
  sdata[lid] = sqsum;
  workgroupBarrier();

  step = 128u;
  loop {
    if (step == 0u) { break; }
    if (lid < step) { sdata[lid] = sdata[lid] + sdata[lid + step]; }
    workgroupBarrier();
    step = step >> 1u;
  }
  let inv_std = inverseSqrt(sdata[0] / f32(max(group_elems, 1u)) + eps);
  let neg_mean_inv = -mean * inv_std;

  i = lid;
  loop {
    if (i >= group_elems) { break; }
    let v = fma(f32(inp[g_base + i]), inv_std, neg_mean_inv);
    if (affine != 0u) {
      let c_abs = g * ch_g + (i / plane);
      out[g_base + i] = Elem(fma(v, gamma[c_abs], beta[c_abs]));
    } else {
      out[g_base + i] = Elem(v);
    }
    i = i + 256u;
  }
}
`,

  // Interp (nearest = 1, bilinear = 2, Keys A=-0.75 bicubic = 3)
  // u = [in_w, in_h, out_w, out_h, channels, resize_type, align_corner, total]
  interp: /* wgsl */ `
struct Params { u: array<u32, 16> };
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> inp: array<Elem>;
@group(0) @binding(2) var<storage, read_write> out: array<Elem>;

fn cubic_weights(fx_in: f32, dim: u32, idx_out: ptr<function, vec4<u32>>) -> vec4<f32> {
  var sx = i32(floor(fx_in));
  var u = fx_in - f32(sx);
  if (sx < -1) { sx = -1; u = 0.0; }
  if (sx >= i32(dim)) { sx = i32(dim) - 1; u = 0.0; }
  let A: f32 = -0.75;
  let u_p1 = u + 1.0;
  let u_1m = 1.0 - u;
  let c0 = ((A * u_p1 - 5.0 * A) * u_p1 + 8.0 * A) * u_p1 - 4.0 * A;
  let c1 = ((A + 2.0) * u - (A + 3.0)) * u * u + 1.0;
  let c2 = ((A + 2.0) * u_1m - (A + 3.0)) * u_1m * u_1m + 1.0;
  let c3 = 1.0 - c0 - c1 - c2;
  let mx = max(i32(dim) - 1, 0);
  (*idx_out) = vec4<u32>(
    u32(clamp(sx - 1, 0, mx)),
    u32(clamp(sx, 0, mx)),
    u32(clamp(sx + 1, 0, mx)),
    u32(clamp(sx + 2, 0, mx))
  );
  return vec4<f32>(c0, c1, c2, c3);
}

@compute @workgroup_size(256)
fn main(
  @builtin(workgroup_id) wid: vec3<u32>,
  @builtin(num_workgroups) nwg: vec3<u32>,
  @builtin(local_invocation_index) lid: u32
) {
  let idx = (wid.y * nwg.x + wid.x) * 256u + lid;
  let total = params.u[7];
  if (idx >= total) { return; }

  let in_w = params.u[0];
  let in_h = params.u[1];
  let out_w = params.u[2];
  let out_h = params.u[3];
  let rtype = params.u[5];
  let align = params.u[6];

  let out_plane = out_w * out_h;
  let c = idx / out_plane;
  let rem = idx - c * out_plane;
  let dy = rem / out_w;
  let dx = rem - dy * out_w;
  let base = c * in_w * in_h;

  if (rtype == 1u) {
    let sx = min(u32(f32(dx) * (f32(in_w) / f32(out_w))), in_w - 1u);
    let sy = min(u32(f32(dy) * (f32(in_h) / f32(out_h))), in_h - 1u);
    out[idx] = inp[base + sy * in_w + sx];
    return;
  }

  var scale_x: f32 = f32(in_w) / f32(out_w);
  var scale_y: f32 = f32(in_h) / f32(out_h);
  if (align != 0u) {
    scale_x = select(0.0, f32(in_w - 1u) / f32(out_w - 1u), out_w > 1u);
    scale_y = select(0.0, f32(in_h - 1u) / f32(out_h - 1u), out_h > 1u);
  }
  let fx_raw = select((f32(dx) + 0.5) * scale_x - 0.5, f32(dx) * scale_x, align != 0u);
  let fy_raw = select((f32(dy) + 0.5) * scale_y - 0.5, f32(dy) * scale_y, align != 0u);

  if (rtype == 2u) {
    var fx = max(fx_raw, 0.0);
    var sx = i32(floor(fx));
    fx = fx - f32(sx);
    if (in_w == 1u) { sx = 0; fx = 0.0; }
    else if (sx >= i32(in_w) - 1) { sx = i32(in_w) - 2; fx = 1.0; }

    var fy = max(fy_raw, 0.0);
    var sy = i32(floor(fy));
    fy = fy - f32(sy);
    if (in_h == 1u) { sy = 0; fy = 0.0; }
    else if (sy >= i32(in_h) - 1) { sy = i32(in_h) - 2; fy = 1.0; }

    let sx0 = u32(sx); let sx1 = min(sx0 + 1u, in_w - 1u);
    let sy0 = u32(sy); let sy1 = min(sy0 + 1u, in_h - 1u);
    let v0 = mix(f32(inp[base + sy0 * in_w + sx0]), f32(inp[base + sy0 * in_w + sx1]), fx);
    let v1 = mix(f32(inp[base + sy1 * in_w + sx0]), f32(inp[base + sy1 * in_w + sx1]), fx);
    out[idx] = Elem(mix(v0, v1, fy));
    return;
  }

  var x_idx: vec4<u32>;
  var y_idx: vec4<u32>;
  let wx = cubic_weights(fx_raw, in_w, &x_idx);
  let wy = cubic_weights(fy_raw, in_h, &y_idx);

  var rows: vec4<f32>;
  for (var r = 0u; r < 4u; r = r + 1u) {
    let rbase = base + y_idx[r] * in_w;
    let p = vec4<f32>(
      f32(inp[rbase + x_idx[0]]),
      f32(inp[rbase + x_idx[1]]),
      f32(inp[rbase + x_idx[2]]),
      f32(inp[rbase + x_idx[3]])
    );
    rows[r] = dot(p, wx);
  }
  out[idx] = Elem(dot(rows, wy));
}
`,

  // PixelShuffle & Reorg
  // u = [in_w, in_h, in_c, out_w, out_h, out_c, factor, mode, is_reorg, total]
  space_depth: /* wgsl */ `
struct Params { u: array<u32, 16> };
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> inp: array<Elem>;
@group(0) @binding(2) var<storage, read_write> out: array<Elem>;

@compute @workgroup_size(256)
fn main(
  @builtin(workgroup_id) wid: vec3<u32>,
  @builtin(num_workgroups) nwg: vec3<u32>,
  @builtin(local_invocation_index) lid: u32
) {
  let idx = (wid.y * nwg.x + wid.x) * 256u + lid;
  let total = params.u[9];
  if (idx >= total) { return; }

  let in_w = params.u[0];
  let in_h = params.u[1];
  let in_c = params.u[2];
  let out_w = params.u[3];
  let out_h = params.u[4];
  let out_c = params.u[5];
  let r = params.u[6];
  let mode = params.u[7];
  let is_reorg = params.u[8];

  let out_plane = out_w * out_h;
  let oc = idx / out_plane;
  let rem = idx - oc * out_plane;
  let oy = rem / out_w;
  let ox = rem - oy * out_w;
  let r2 = r * r;

  if (is_reorg == 0u) {
    let iy = oy / r;
    let sh = oy - iy * r;
    let ix = ox / r;
    let sw = ox - ix * r;
    let ic = select((sh * r + sw) * out_c + oc, oc * r2 + sh * r + sw, mode == 0u);
    out[idx] = inp[(ic * in_h + iy) * in_w + ix];
  } else {
    let ic = select(oc % in_c, oc / r2, mode == 0u);
    let sub = select(oc / in_c, oc % r2, mode == 0u);
    let sh = sub / r;
    let sw = sub - sh * r;
    let iy = oy * r + sh;
    let ix = ox * r + sw;
    out[idx] = inp[(ic * in_h + iy) * in_w + ix];
  }
}
`,

  // Crop / Slice: copy sub-box `[cs..ce, hs..he, ws..we]` from `[in_c, in_h, in_w]`
  // u = [in_w, in_h, out_w, out_h, cs, hs, ws, total]
  sub_box: /* wgsl */ `
struct Params { u: array<u32, 16> };
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> inp: array<Elem>;
@group(0) @binding(2) var<storage, read_write> out: array<Elem>;

@compute @workgroup_size(256)
fn main(
  @builtin(workgroup_id) wid: vec3<u32>,
  @builtin(num_workgroups) nwg: vec3<u32>,
  @builtin(local_invocation_index) lid: u32
) {
  let idx = (wid.y * nwg.x + wid.x) * 256u + lid;
  if (idx >= params.u[7]) { return; }

  let in_w = params.u[0];
  let in_h = params.u[1];
  let out_w = params.u[2];
  let out_h = params.u[3];
  let cs = params.u[4];
  let hs = params.u[5];
  let ws = params.u[6];

  let out_plane = out_w * out_h;
  let c = idx / out_plane;
  let rem = idx - c * out_plane;
  let y = rem / out_w;
  let x = rem - y * out_w;

  out[idx] = inp[((cs + c) * in_h + (hs + y)) * in_w + (ws + x)];
}
`,

  // Element-aligned channel concat fallback when `f16` byte offsets are not 4-byte aligned
  // u = [dst_elem_offset, total_elems]
  concat_elems: /* wgsl */ `
struct Params { u: array<u32, 16> };
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> inp: array<Elem>;
@group(0) @binding(2) var<storage, read_write> out: array<Elem>;

@compute @workgroup_size(256)
fn main(
  @builtin(workgroup_id) wid: vec3<u32>,
  @builtin(num_workgroups) nwg: vec3<u32>,
  @builtin(local_invocation_index) lid: u32
) {
  let idx = (wid.y * nwg.x + wid.x) * 256u + lid;
  if (idx >= params.u[1]) { return; }
  out[params.u[0] + idx] = inp[idx];
}
`,

  // u = [total, op_type (0=prod, 1=sum, 2=max), bitcast<u32>(c0), bitcast<u32>(c1)]
  eltwise_pair: /* wgsl */ `
struct Params { u: array<u32, 16> };
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> a: array<Elem>;
@group(0) @binding(2) var<storage, read> b: array<Elem>;
@group(0) @binding(3) var<storage, read_write> out: array<Elem>;

@compute @workgroup_size(256)
fn main(
  @builtin(workgroup_id) wid: vec3<u32>,
  @builtin(num_workgroups) nwg: vec3<u32>,
  @builtin(local_invocation_index) lid: u32
) {
  let idx = (wid.y * nwg.x + wid.x) * 256u + lid;
  if (idx >= params.u[0]) { return; }

  let op_type = params.u[1];
  let c0 = bitcast<f32>(params.u[2]);
  let c1 = bitcast<f32>(params.u[3]);
  let va = f32(a[idx]);
  let vb = f32(b[idx]);
  var res: f32;
  if (op_type == 0u) {
    res = va * vb;
  } else if (op_type == 2u) {
    res = max(va, vb);
  } else {
    res = fma(va, c0, vb * c1);
  }
  out[idx] = Elem(res);
}
`,

  // u = [in_w, in_h, out_w, out_h, top, left, pad_type, bitcast<u32>(value), total]
  pad_2d: /* wgsl */ `
struct Params { u: array<u32, 16> };
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> inp: array<Elem>;
@group(0) @binding(2) var<storage, read_write> out: array<Elem>;

fn reflect_idx(v_in: i32, len: i32) -> u32 {
  if (len <= 1) { return 0u; }
  var v = v_in;
  loop {
    if (v >= 0 && v < len) { break; }
    if (v < 0) { v = -v; }
    if (v >= len) { v = 2 * len - 2 - v; }
  }
  return u32(v);
}

@compute @workgroup_size(256)
fn main(
  @builtin(workgroup_id) wid: vec3<u32>,
  @builtin(num_workgroups) nwg: vec3<u32>,
  @builtin(local_invocation_index) lid: u32
) {
  let idx = (wid.y * nwg.x + wid.x) * 256u + lid;
  if (idx >= params.u[8]) { return; }

  let in_w = params.u[0];
  let in_h = params.u[1];
  let out_w = params.u[2];
  let out_h = params.u[3];
  let pad_top = i32(params.u[4]);
  let pad_left = i32(params.u[5]);
  let pad_type = params.u[6];
  let pad_val = Elem(bitcast<f32>(params.u[7]));

  let out_plane = out_w * out_h;
  let c = idx / out_plane;
  let rem = idx - c * out_plane;
  let dy = i32(rem / out_w);
  let dx = i32(rem - u32(dy) * out_w);
  let sy = dy - pad_top;
  let sx = dx - pad_left;

  if (pad_type == 1u) {
    let cy = u32(clamp(sy, 0, i32(in_h) - 1));
    let cx = u32(clamp(sx, 0, i32(in_w) - 1));
    out[idx] = inp[(c * in_h + cy) * in_w + cx];
  } else if (pad_type == 2u) {
    let ry = reflect_idx(sy, i32(in_h));
    let rx = reflect_idx(sx, i32(in_w));
    out[idx] = inp[(c * in_h + ry) * in_w + rx];
  } else {
    if (sy >= 0 && sy < i32(in_h) && sx >= 0 && sx < i32(in_w)) {
      out[idx] = inp[(c * in_h + u32(sy)) * in_w + u32(sx)];
    } else {
      out[idx] = pad_val;
    }
  }
}
`,
};

function buildShaderSource(key, useFp16) {
  const header = useFp16 ? 'enable f16;\nalias Elem = f16;\n' : 'alias Elem = f32;\n';
  return header + SHADER_BODIES[key];
}

function formatGpuDeviceName(info = {}) {
  const desc = (info.description || info.device || '').trim();
  if (desc && !/^0x[0-9a-f]+$/i.test(desc)) {
    return desc;
  }
  const rawVendor = (info.vendor || '').trim();
  const rawArch = (info.architecture || '').trim();
  const vendorMap = {
    nvidia: 'NVIDIA',
    amd: 'AMD',
    intel: 'Intel',
    apple: 'Apple',
    qualcomm: 'Qualcomm',
    arm: 'ARM',
    google: 'Google',
    mesa: 'Mesa',
  };
  const vendor = vendorMap[rawVendor.toLowerCase()]
    || (rawVendor ? rawVendor.charAt(0).toUpperCase() + rawVendor.slice(1) : '');
  if (vendor && rawArch) return `${vendor} (${rawArch})`;
  return vendor || rawArch || 'WebGPU GPU';
}

const F32_SCRATCH = new Float32Array(1);
const U32_SCRATCH = new Uint32Array(F32_SCRATCH.buffer);

function f32Bits(val) {
  F32_SCRATCH[0] = val;
  return U32_SCRATCH[0];
}

function i32Bits(val) {
  return (val | 0) >>> 0;
}

function alignBytes(n, align = 16) {
  return Math.max(align, Math.ceil(n / align) * align);
}

function f32ToF16BitsScalar(val) {
  F32_SCRATCH[0] = val;
  const x = U32_SCRATCH[0];
  const sign = (x >>> 16) & 0x8000;
  let exp = ((x >>> 23) & 0xff) - 127 + 15;
  let mant = x & 0x7fffff;
  if (exp <= 0) {
    if (exp < -10) return sign;
    mant = (mant | 0x800000) >>> (1 - exp);
    return sign | ((mant + 0x1000) >>> 13);
  }
  if (exp >= 31) {
    return sign | 0x7c00;
  }
  return sign | (exp << 10) | ((mant + 0x1000) >>> 13);
}

function convertFloat32ToFloat16Bytes(floatSlice) {
  const NativeF16 = globalThis.Float16Array;
  if (typeof NativeF16 === 'function') {
    const f16 = new NativeF16(floatSlice);
    return new Uint8Array(f16.buffer, f16.byteOffset, f16.byteLength);
  }
  const out = new Uint16Array(floatSlice.length);
  for (let i = 0; i < floatSlice.length; i += 1) {
    out[i] = f32ToF16BitsScalar(floatSlice[i]);
  }
  return new Uint8Array(out.buffer, out.byteOffset, out.byteLength);
}

function canRepresentInFp16(weights) {
  for (let i = 0; i < weights.length; i += 1) {
    const v = weights[i];
    if (v > 60000 || v < -60000) return false;
  }
  return true;
}

function grid2dForCount(totalThreads, wgSize = 256) {
  const groups = Math.max(1, Math.ceil(totalThreads / wgSize));
  if (groups <= 65535) {
    return [groups, 1, 1];
  }
  const gx = 32768;
  const gy = Math.ceil(groups / gx);
  return [gx, gy, 1];
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

export function resolveCropBounds(inC, inH, inW, op) {
  const { starts = [], ends = [], axes = [] } = op;
  if (starts.length > 0 && starts.length === ends.length) {
    let cs = 0; let ce = inC;
    let hs = 0; let he = inH;
    let ws = 0; let we = inW;
    const resolve = (val, dim, isEnd) => {
      if (isEnd && (val === -233 || val >= 0x7fffffff)) return dim;
      const r = val < 0 ? dim + val : val;
      return Math.max(0, Math.min(dim, r));
    };
    for (let i = 0; i < starts.length; i += 1) {
      const rawAxis = axes[i] !== undefined ? axes[i] : i;
      const norm = rawAxis < 0 ? 3 + rawAxis : rawAxis;
      if (norm === 0) { cs = resolve(starts[i], inC, false); ce = resolve(ends[i], inC, true); }
      else if (norm === 1) { hs = resolve(starts[i], inH, false); he = resolve(ends[i], inH, true); }
      else if (norm === 2) { ws = resolve(starts[i], inW, false); we = resolve(ends[i], inW, true); }
    }
    return [cs, Math.max(cs, ce), hs, Math.max(hs, he), ws, Math.max(ws, we)];
  }
  const cs = Math.max(0, op.coffset || 0);
  const ce = (op.outc === -233 || op.outc <= 0)
    ? Math.max(cs, inC - Math.max(0, op.coffset2 || 0))
    : Math.min(inC, cs + op.outc);
  const hs = Math.max(0, op.hoffset || 0);
  const he = (op.outh === -233 || op.outh <= 0)
    ? Math.max(hs, inH - Math.max(0, op.hoffset2 || 0))
    : Math.min(inH, hs + op.outh);
  const ws = Math.max(0, op.woffset || 0);
  const we = (op.outw === -233 || op.outw <= 0)
    ? Math.max(ws, inW - Math.max(0, op.woffset2 || 0))
    : Math.min(inW, ws + op.outw);
  return [cs, ce, hs, he, ws, we];
}

/**
 * Fuse single-consumer `Convolution` / `Deconvolution` / `InnerProduct` + `ReLU` / `Clip` /
 * `Sigmoid` / `Swish` / `PReLU` pairs into single fused layers before GPU buffer upload.
 */
function buildFusedGraph(meta) {
  const fusedLayers = [];
  const consumers = meta.blob_consumers.slice();
  const srcLayers = meta.layers;

  let i = 0;
  while (i < srcLayers.length) {
    const cur = srcLayers[i];
    const op = cur.op;
    const canFuseBase = (
      (op.op === 'convolution' || op.op === 'deconvolution' || op.op === 'inner_product')
      && (op.activation_type || 0) === 0
      && cur.tops.length === 1
      && i + 1 < srcLayers.length
    );

    if (canFuseBase) {
      const midBlob = cur.tops[0];
      const next = srcLayers[i + 1];
      const nextOp = next.op;
      if (
        midBlob !== meta.output_blob
        && consumers[midBlob] === 1
        && next.bottoms.length === 1
        && next.bottoms[0] === midBlob
        && next.tops.length === 1
      ) {
        let fusedActType = 0;
        let fusedActParams = [];
        let fusedPreluSlope = null;
        let fusedPreluNumSlope = 0;

        if (nextOp.op === 're_lu') {
          const s = nextOp.slope || 0;
          fusedActType = s === 0 ? 1 : 2;
          fusedActParams = [s, 0];
        } else if (nextOp.op === 'clip') {
          fusedActType = 3;
          fusedActParams = [nextOp.min, nextOp.max];
        } else if (nextOp.op === 'sigmoid') {
          fusedActType = 4;
          fusedActParams = [0, 0];
        } else if (nextOp.op === 'swish') {
          fusedActType = 8;
          fusedActParams = [0, 0];
        } else if (nextOp.op === 'p_re_lu') {
          fusedActType = 7;
          fusedPreluSlope = nextOp.slope;
          fusedPreluNumSlope = nextOp.num_slope || 1;
        }

        if (fusedActType !== 0) {
          consumers[midBlob] = 0;
          fusedLayers.push({
            name: cur.name,
            bottoms: cur.bottoms,
            tops: next.tops,
            op: {
              ...op,
              activation_type: fusedActType,
              activation_params: fusedActParams,
              fused_prelu_slope: fusedPreluSlope,
              fused_prelu_num_slope: fusedPreluNumSlope,
            },
          });
          i += 2;
          continue;
        }
      }
    }

    fusedLayers.push(cur);
    i += 1;
  }

  return {
    ...meta,
    layers: fusedLayers,
    blob_consumers: consumers,
  };
}

function destroyPlan(plan) {
  if (!plan) return;
  for (const buf of plan.ownedBuffers) {
    try { buf.destroy(); } catch { /* ignore */ }
  }
  plan.ownedBuffers.length = 0;
}

export class WebGpuBackend {
  constructor() {
    this.adapter = null;
    this.device = null;
    this.deviceName = 'WebGPU';
    this.features = { fp16: false, maxBufferSize: 0, maxStorageBufferBindingSize: 0 };
    this.pipelines = new Map(); // `${key}:${numRead}:${useFp16 ? 1 : 0}` -> { pipeline, bgl }
    this.dummyStorageBuffer = null;
    this.loadedModel = null; // { name, useFp16, meta, layerWeights, ownedBuffers, planCache }
  }

  static async probe() {
    if (typeof navigator === 'undefined' || !navigator.gpu) {
      return {
        id: 'webgpu',
        available: false,
        label: 'WebGPU',
        deviceName: t('common.unavailable'),
        reason: t('runtime.webgpuUnsupportedProbe'),
      };
    }
    try {
      const adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
      if (!adapter) {
        return {
          id: 'webgpu',
          available: false,
          label: 'WebGPU',
          deviceName: t('runtime.noGpuAdapter'),
          reason: t('runtime.noGpuAdapterReason'),
        };
      }
      const info = adapter.info || (typeof adapter.requestAdapterInfo === 'function' ? await adapter.requestAdapterInfo().catch(() => ({})) : {});
      const deviceName = formatGpuDeviceName(info);
      const fp16 = adapter.features ? adapter.features.has('shader-f16') : false;
      const limits = adapter.limits || {};
      return {
        id: 'webgpu',
        available: true,
        label: 'WebGPU',
        deviceName,
        features: {
          fp16,
          maxBufferSize: limits.maxBufferSize || 268435456,
          maxStorageBufferBindingSize: limits.maxStorageBufferBindingSize || 134217728,
        },
      };
    } catch (err) {
      return {
        id: 'webgpu',
        available: false,
        label: 'WebGPU',
        deviceName: t('runtime.initFailed'),
        reason: err instanceof Error ? err.message : String(err),
      };
    }
  }

  async init() {
    if (this.device) return;
    if (typeof navigator === 'undefined' || !navigator.gpu) {
      throw new Error(t('runtime.webgpuUnsupportedUseWasm'));
    }
    this.adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
    if (!this.adapter) {
      throw new Error(t('runtime.webgpuNoAdapterUseWasm'));
    }

    const info = this.adapter.info || {};
    this.deviceName = formatGpuDeviceName(info);

    const limits = this.adapter.limits || {};
    const requiredLimits = {};
    if (limits.maxStorageBufferBindingSize) {
      requiredLimits.maxStorageBufferBindingSize = Math.min(limits.maxStorageBufferBindingSize, 2 * 1024 * 1024 * 1024);
    }
    if (limits.maxBufferSize) {
      requiredLimits.maxBufferSize = Math.min(limits.maxBufferSize, 2 * 1024 * 1024 * 1024);
    }

    const requiredFeatures = [];
    if (this.adapter.features && this.adapter.features.has('shader-f16')) {
      requiredFeatures.push('shader-f16');
    }

    this.device = await this.adapter.requestDevice({
      requiredFeatures,
      requiredLimits,
    });
    this.device.lost.then(() => {
      this.device = null;
      this.pipelines.clear();
      this.loadedModel = null;
    });

    this.features = {
      fp16: this.device.features.has('shader-f16'),
      maxBufferSize: this.device.limits.maxBufferSize,
      maxStorageBufferBindingSize: this.device.limits.maxStorageBufferBindingSize,
    };

    this.dummyStorageBuffer = this.device.createBuffer({
      size: 16,
      usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST,
    });
  }

  getPipeline(key, numReadBuffers, useFp16) {
    const cacheKey = `${key}:${numReadBuffers}:${useFp16 ? 1 : 0}`;
    if (this.pipelines.has(cacheKey)) {
      return this.pipelines.get(cacheKey);
    }
    const entries = [
      {
        binding: 0,
        visibility: GPUShaderStage.COMPUTE,
        buffer: { type: 'uniform', hasDynamicOffset: true, minBindingSize: UNIFORM_SIZE },
      },
    ];
    for (let i = 0; i < numReadBuffers; i += 1) {
      entries.push({
        binding: 1 + i,
        visibility: GPUShaderStage.COMPUTE,
        buffer: { type: 'read-only-storage' },
      });
    }
    entries.push({
      binding: 1 + numReadBuffers,
      visibility: GPUShaderStage.COMPUTE,
      buffer: { type: 'storage' },
    });

    const bgl = this.device.createBindGroupLayout({ entries });
    const module = this.device.createShaderModule({ code: buildShaderSource(key, useFp16) });
    const pipeline = this.device.createComputePipeline({
      layout: this.device.createPipelineLayout({ bindGroupLayouts: [bgl] }),
      compute: { module, entryPoint: 'main' },
    });
    const compiled = { pipeline, bgl };
    this.pipelines.set(cacheKey, compiled);
    return compiled;
  }

  createStaticBufferFromBytes(rawBytes) {
    if (!rawBytes || rawBytes.byteLength === 0) {
      return this.dummyStorageBuffer;
    }
    const byteLength = alignBytes(rawBytes.byteLength, 16);
    const buf = this.device.createBuffer({
      size: byteLength,
      usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST,
    });
    const view = rawBytes instanceof Uint8Array
      ? rawBytes
      : new Uint8Array(rawBytes.buffer, rawBytes.byteOffset, rawBytes.byteLength);
    if (view.byteLength === byteLength) {
      this.device.queue.writeBuffer(buf, 0, view);
    } else {
      const padded = new Uint8Array(byteLength);
      padded.set(view);
      this.device.queue.writeBuffer(buf, 0, padded);
    }
    return buf;
  }

  disposeLoadedModel() {
    if (!this.loadedModel) return;
    if (this.loadedModel.planCache) {
      for (const plan of this.loadedModel.planCache.values()) {
        destroyPlan(plan);
      }
      this.loadedModel.planCache.clear();
    }
    for (const buf of this.loadedModel.ownedBuffers) {
      try { buf.destroy(); } catch { /* ignore */ }
    }
    this.loadedModel = null;
  }

  async prepareModel(wasmBridge, modelPayload, preferFp16 = true) {
    await this.init();
    const wantFp16 = Boolean(preferFp16 !== false && this.features.fp16);
    if (
      this.loadedModel
      && this.loadedModel.name === modelPayload.name
      && this.loadedModel.wantFp16 === wantFp16
    ) {
      return this.loadedModel;
    }
    this.disposeLoadedModel();

    const parsed = wasmBridge.loadModel(
      modelPayload.name,
      modelPayload.paramText,
      modelPayload.binBytes,
    );
    try {
      const fusedMeta = buildFusedGraph(parsed.meta);
      const weights = parsed.getWeightsView();
      const useFp16 = wantFp16 && canRepresentInFp16(weights);
      const ownedBuffers = [];
      const layerWeights = new Map();

      const uploadElemSpan = (span) => {
        if (!span || !span.len) return this.dummyStorageBuffer;
        const sub = weights.subarray(span.offset, span.offset + span.len);
        const bytes = useFp16
          ? convertFloat32ToFloat16Bytes(sub)
          : new Uint8Array(sub.buffer, sub.byteOffset, sub.byteLength);
        const gpuBuf = this.createStaticBufferFromBytes(bytes);
        ownedBuffers.push(gpuBuf);
        return gpuBuf;
      };

      const uploadF32Span = (span) => {
        if (!span || !span.len) return this.dummyStorageBuffer;
        const sub = weights.subarray(span.offset, span.offset + span.len);
        const gpuBuf = this.createStaticBufferFromBytes(sub);
        ownedBuffers.push(gpuBuf);
        return gpuBuf;
      };

      const uploadPackedBiasSlope = (op) => {
        const outC = op.num_output;
        const packed = new Float32Array(outC * 2);
        const bSlice = (op.bias_term && op.bias && op.bias.len)
          ? weights.subarray(op.bias.offset, op.bias.offset + op.bias.len)
          : null;
        const sSpan = op.fused_prelu_slope;
        const sSlice = (sSpan && sSpan.len)
          ? weights.subarray(sSpan.offset, sSpan.offset + sSpan.len)
          : null;
        const multiSlope = (op.fused_prelu_num_slope || 1) > 1 && sSlice && sSlice.length >= outC;

        for (let c = 0; c < outC; c += 1) {
          packed[c * 2] = bSlice ? bSlice[c] : 0.0;
          packed[c * 2 + 1] = sSlice ? (multiSlope ? sSlice[c] : sSlice[0]) : 0.0;
        }
        const gpuBuf = this.createStaticBufferFromBytes(packed);
        ownedBuffers.push(gpuBuf);
        return gpuBuf;
      };

      for (let i = 0; i < fusedMeta.layers.length; i += 1) {
        const op = fusedMeta.layers[i].op;
        if (op.op === 'memory_data') {
          layerWeights.set(i, { data: uploadElemSpan(op.data) });
        } else if (op.op === 'convolution' || op.op === 'deconvolution' || op.op === 'inner_product') {
          const biasBuf = op.activation_type === 7
            ? uploadPackedBiasSlope(op)
            : (op.bias_term ? uploadF32Span(op.bias) : this.dummyStorageBuffer);
          layerWeights.set(i, {
            weight: uploadElemSpan(op.weight),
            bias: biasBuf,
          });
        } else if (op.op === 'p_re_lu') {
          layerWeights.set(i, { slope: uploadF32Span(op.slope) });
        } else if (op.op === 'group_norm') {
          layerWeights.set(i, {
            gamma: op.affine ? uploadF32Span(op.gamma) : this.dummyStorageBuffer,
            beta: op.affine ? uploadF32Span(op.beta) : this.dummyStorageBuffer,
          });
        }
      }

      this.loadedModel = {
        name: modelPayload.name,
        wantFp16,
        useFp16,
        meta: fusedMeta,
        layerWeights,
        ownedBuffers,
        planCache: new Map(),
      };
      return this.loadedModel;
    } finally {
      parsed.dispose();
    }
  }

  /**
   * Build and cache a reusable GPU execution plan for a fixed `(width, height, channels)` tile shape:
   * - Pre-allocates intermediate `GPUBuffer`s (reused within the graph via refcounted pooling)
   * - Uploads static per-layer uniform parameters once
   * - Pre-builds all `GPUBindGroup`s once
   * - Coalesces consecutive compute dispatches into single `GPUComputePassEncoder` segments
   */
  getOrCreateTilePlan(model, width, height, channels) {
    const planKey = `${width}x${height}x${channels}`;
    if (model.planCache.has(planKey)) {
      return model.planCache.get(planKey);
    }
    if (model.planCache.size >= MAX_CACHED_PLANS) {
      const oldestKey = model.planCache.keys().next().value;
      destroyPlan(model.planCache.get(oldestKey));
      model.planCache.delete(oldestKey);
    }

    const { meta, layerWeights, useFp16 } = model;
    const elemBytes = useFp16 ? 2 : 4;
    const scale = Math.max(1, meta.scale_factor || 2);
    const outW = width * scale;
    const outH = height * scale;

    const prepad = Math.max(0, meta.prepadding || 0);
    const align = Math.max(1, meta.input_align || 1);
    const baseW = width + 2 * prepad;
    const baseH = height + 2 * prepad;
    const feedW = baseW + ((align - (baseW % align)) % align);
    const feedH = baseH + ((align - (baseH % align)) % align);

    const ownedBuffers = [];
    const localPool = [];

    const maxBind = this.features.maxStorageBufferBindingSize || 134217728;
    const maxBuf = this.features.maxBufferSize || 268435456;

    const allocPlanStorage = (minBytes) => {
      const size = alignBytes(minBytes, 256);
      if (size > maxBind || size > maxBuf) {
        const mb = formatNumber(size / (1024 * 1024), { minimumFractionDigits: 1, maximumFractionDigits: 1 });
        const err = new Error(t('runtime.gpuBindingOom', { mb }));
        err.isOom = true;
        throw err;
      }
      let bestIdx = -1;
      let bestSize = Infinity;
      for (let i = 0; i < localPool.length; i += 1) {
        const item = localPool[i];
        if (item.size >= size && item.size <= Math.max(size * 2, 65536) && item.size < bestSize) {
          bestSize = item.size;
          bestIdx = i;
        }
      }
      if (bestIdx >= 0) {
        const [reused] = localPool.splice(bestIdx, 1);
        return reused;
      }
      const buf = this.device.createBuffer({
        size,
        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST,
      });
      ownedBuffers.push(buf);
      return { buf, size };
    };

    const recyclePlanStorage = (entry) => {
      if (entry && entry.buf) {
        localPool.push(entry);
      }
    };

    try {
      const maxDispatches = meta.layers.length * 2 + 16;
      const uniformCapacity = Math.ceil((Math.max(32, maxDispatches) * UNIFORM_STRIDE) / 16384) * 16384;
      const uniformBuf = this.device.createBuffer({
        size: uniformCapacity,
        usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
      });
      ownedBuffers.push(uniformBuf);

      const uniformStaging = new Uint32Array(uniformCapacity / 4);
      let dispatchIndex = 0;
      const dispatches = [];

      const recordOp = (shaderKey, readBuffers, writeBuffer, u32Params, workgroups) => {
        if ((dispatchIndex + 1) * UNIFORM_STRIDE > uniformCapacity) {
          throw new Error(t('runtime.gpuBufferLimit'));
        }
        const wordBase = (dispatchIndex * UNIFORM_STRIDE) / 4;
        for (let i = 0; i < Math.min(16, u32Params.length); i += 1) {
          uniformStaging[wordBase + i] = u32Params[i] >>> 0;
        }
        const { pipeline, bgl } = this.getPipeline(shaderKey, readBuffers.length, useFp16);
        const entries = [
          {
            binding: 0,
            resource: { buffer: uniformBuf, offset: 0, size: UNIFORM_SIZE },
          },
        ];
        for (let i = 0; i < readBuffers.length; i += 1) {
          entries.push({ binding: 1 + i, resource: { buffer: readBuffers[i] } });
        }
        entries.push({ binding: 1 + readBuffers.length, resource: { buffer: writeBuffer } });

        const bg = this.device.createBindGroup({ layout: bgl, entries });
        dispatches.push({
          type: 'compute',
          pipeline,
          bg,
          dynamicOffset: dispatchIndex * UNIFORM_STRIDE,
          workgroups,
        });
        dispatchIndex += 1;
      };

      const blobs = new Array(meta.blob_count).fill(null);
      const consumerCounts = meta.blob_consumers.slice();
      consumerCounts[meta.output_blob] = (consumerCounts[meta.output_blob] || 0) + 1;

      const allocTensor = (w, h, c, consumers) => {
        const entry = allocPlanStorage(w * h * c * elemBytes);
        return { w, h, c, entry, refObj: { refs: Math.max(1, consumers), entry } };
      };

      const releaseBlob = (blobIdx) => {
        const b = blobs[blobIdx];
        if (!b || !b.refObj) return;
        b.refObj.refs -= 1;
        if (b.refObj.refs === 0 && b.refObj.entry) {
          recyclePlanStorage(b.refObj.entry);
          b.refObj.entry = null;
        }
      };

      const inFloats = width * height * channels;
      const inByteLen = inFloats * 4;
      const rawInEntry = allocPlanStorage(inByteLen);

      const feedTotal = feedW * feedH * channels;
      const feedTensor = allocTensor(feedW, feedH, channels, consumerCounts[meta.input_blob] || 1);
      recordOp(
        'preprocess',
        [rawInEntry.buf],
        feedTensor.entry.buf,
        [width, height, feedW, feedH, channels, prepad, feedTotal],
        grid2dForCount(feedTotal),
      );
      blobs[meta.input_blob] = feedTensor;

      for (let lIdx = 0; lIdx < meta.layers.length; lIdx += 1) {
        const layer = meta.layers[lIdx];
        const op = layer.op;
        const lw = layerWeights.get(lIdx);

        if (op.op === 'input') {
          // Already bound
        } else if (op.op === 'memory_data') {
          let tw = 1; let th = 1; let tc = Math.max(1, op.w || 1);
          if (op.d > 0) { tw = Math.max(1, op.w); th = Math.max(1, op.h) * op.d; tc = Math.max(1, op.c); }
          else if (op.c > 0) { tw = Math.max(1, op.w); th = Math.max(1, op.h); tc = op.c; }
          else if (op.h > 0) { tw = Math.max(1, op.w); th = op.h; tc = 1; }
          blobs[layer.tops[0]] = { w: tw, h: th, c: tc, entry: { buf: lw.data }, refObj: null };
        } else if (op.op === 'split') {
          const src = blobs[layer.bottoms[0]];
          let addedRefs = 0;
          for (const top of layer.tops) {
            addedRefs += Math.max(1, consumerCounts[top] || 1);
          }
          if (src.refObj) {
            src.refObj.refs += addedRefs;
          }
          for (const top of layer.tops) {
            blobs[top] = { w: src.w, h: src.h, c: src.c, entry: src.entry, refObj: src.refObj };
          }
        } else if (op.op === 'convolution') {
          const inp = blobs[layer.bottoms[0]];
          const kextW = op.dilation_w * (op.kernel_w - 1) + 1;
          const kextH = op.dilation_h * (op.kernel_h - 1) + 1;
          const [pl, pr, pt, pb] = resolveConvPads(
            inp.w, inp.h, kextW, kextH, op.stride_w, op.stride_h,
            op.pad_left, op.pad_right, op.pad_top, op.pad_bottom,
          );
          const cw = Math.floor((inp.w + pl + pr - kextW) / op.stride_w) + 1;
          const ch = Math.floor((inp.h + pt + pb - kextH) / op.stride_h) + 1;
          const top = layer.tops[0];
          const outT = allocTensor(cw, ch, op.num_output, consumerCounts[top]);

          const actP0 = op.activation_params?.[0] ?? (op.activation_type === 6 ? 1 / 6 : 0.1);
          const actP1 = op.activation_params?.[1] ?? (op.activation_type === 6 ? 0.5 : 1.0);

          if (op.group === op.num_input && op.group === op.num_output) {
            const total = cw * ch * op.num_output;
            const flags = (op.bias_term ? 1 : 0) | ((op.activation_type & 0xff) << 8);
            recordOp(
              'conv_depthwise',
              [inp.entry.buf, lw.weight, lw.bias],
              outT.entry.buf,
              [
                inp.w, inp.h, cw, ch, 1, op.num_output, op.kernel_w, op.kernel_h,
                (op.stride_w & 0xffff) | (op.stride_h << 16),
                (op.dilation_w & 0xffff) | (op.dilation_h << 16),
                i32Bits(pl), i32Bits(pt), f32Bits(op.pad_value || 0),
                flags, f32Bits(actP0), f32Bits(actP1),
              ],
              grid2dForCount(total),
            );
          } else {
            const inG = op.num_input / op.group;
            const outG = op.num_output / op.group;
            const outPlane = cw * ch;

            // RealPLKSR-style partial large-kernel convs (13x13 / 17x17 dense, stride 1,
            // dilation 1): the workgroup halo cache beats both `conv_small` (which would
            // issue `in_g * maxk` uncached global reads per thread) and `conv_tiled`
            // (whose 64-wide im2col tiles cannot stage the large halo).
            const isLargeKernel = (
              op.stride_w === 1 && op.stride_h === 1
              && op.dilation_w === 1 && op.dilation_h === 1
              && op.kernel_w >= 7 && op.kernel_w <= 19
              && op.kernel_h >= 7 && op.kernel_h <= 19
            );

            if (isLargeKernel) {
              const mTiles = Math.ceil(outG / 4);
              const totalTiles = Math.ceil(cw / 16) * Math.ceil(ch / 16);
              const nWgX = Math.min(totalTiles, 4096);
              const nChunks = Math.ceil(totalTiles / nWgX);
              const flags = (op.bias_term ? 1 : 0) | ((op.activation_type & 0xff) << 8) | (mTiles << 16);
              recordOp(
                'conv_large_halo',
                [inp.entry.buf, lw.weight, lw.bias],
                outT.entry.buf,
                [
                  inp.w, inp.h, cw, ch, inG, outG, op.kernel_w, op.kernel_h,
                  (op.stride_w & 0xffff) | (op.stride_h << 16),
                  (op.dilation_w & 0xffff) | (op.dilation_h << 16),
                  i32Bits(pl), i32Bits(pt), f32Bits(op.pad_value || 0),
                  flags, f32Bits(actP0), f32Bits(actP1),
                ],
                [nWgX, mTiles * nChunks, op.group],
              );
            } else if (outG <= 16) {
              const mTiles = Math.ceil(outG / 16);
              const nTilesTotal = Math.ceil(outPlane / 16);
              const nWgX = Math.min(nTilesTotal, 4096);
              const nChunks = Math.ceil(nTilesTotal / nWgX);
              const flags = (op.bias_term ? 1 : 0) | ((op.activation_type & 0xff) << 8) | (mTiles << 16);
              recordOp(
                'conv_small',
                [inp.entry.buf, lw.weight, lw.bias],
                outT.entry.buf,
                [
                  inp.w, inp.h, cw, ch, inG, outG, op.kernel_w, op.kernel_h,
                  (op.stride_w & 0xffff) | (op.stride_h << 16),
                  (op.dilation_w & 0xffff) | (op.dilation_h << 16),
                  i32Bits(pl), i32Bits(pt), f32Bits(op.pad_value || 0),
                  flags, f32Bits(actP0), f32Bits(actP1),
                ],
                [nWgX, mTiles * nChunks, op.group],
              );
            } else {
              const mTiles = Math.ceil(outG / 64);
              const nTilesTotal = Math.ceil(outPlane / 64);
              const nWgX = Math.min(nTilesTotal, 4096);
              const nChunks = Math.ceil(nTilesTotal / nWgX);
              const flags = (op.bias_term ? 1 : 0) | ((op.activation_type & 0xff) << 8) | (mTiles << 16);
              const is1x1 = (
                op.kernel_w === 1 && op.kernel_h === 1
                && op.stride_w === 1 && op.stride_h === 1
                && pl === 0 && pr === 0 && pt === 0 && pb === 0
              );
              recordOp(
                is1x1 ? 'conv_1x1' : 'conv_tiled',
                [inp.entry.buf, lw.weight, lw.bias],
                outT.entry.buf,
                [
                  inp.w, inp.h, cw, ch, inG, outG, op.kernel_w, op.kernel_h,
                  (op.stride_w & 0xffff) | (op.stride_h << 16),
                  (op.dilation_w & 0xffff) | (op.dilation_h << 16),
                  i32Bits(pl), i32Bits(pt), f32Bits(op.pad_value || 0),
                  flags, f32Bits(actP0), f32Bits(actP1),
                ],
                [nWgX, mTiles * nChunks, op.group],
              );
            }
          }
          blobs[top] = outT;
        } else if (op.op === 'deconvolution') {
          const inp = blobs[layer.bottoms[0]];
          const kextW = op.dilation_w * (op.kernel_w - 1) + 1;
          const kextH = op.dilation_h * (op.kernel_h - 1) + 1;
          const fullW = (inp.w - 1) * op.stride_w + kextW + Math.max(0, op.output_pad_right || 0);
          const fullH = (inp.h - 1) * op.stride_h + kextH + Math.max(0, op.output_pad_bottom || 0);
          const pl = Math.max(0, op.pad_left || 0);
          const pr = Math.max(0, op.pad_right || 0);
          const pt = Math.max(0, op.pad_top || 0);
          const pb = Math.max(0, op.pad_bottom || 0);
          const dw = op.output_w > 0 ? op.output_w : fullW - pl - pr;
          const dh = op.output_h > 0 ? op.output_h : fullH - pt - pb;
          const top = layer.tops[0];
          const outT = allocTensor(dw, dh, op.num_output, consumerCounts[top]);

          const inG = op.num_input / op.group;
          const outG = op.num_output / op.group;
          const actP0 = op.activation_params?.[0] ?? 0.1;
          const actP1 = op.activation_params?.[1] ?? 1.0;
          const flags = (op.bias_term ? 1 : 0) | ((op.activation_type & 0xff) << 8) | (op.group << 16);
          // Each `deconv_gather` thread computes 4 output channels of one pixel
          const total = dw * dh * op.group * Math.ceil(outG / 4);

          recordOp(
            'deconv_gather',
            [inp.entry.buf, lw.weight, lw.bias],
            outT.entry.buf,
            [
              inp.w, inp.h, dw, dh, inG, outG, op.kernel_w, op.kernel_h,
              (op.stride_w & 0xffff) | (op.stride_h << 16),
              (op.dilation_w & 0xffff) | (op.dilation_h << 16),
              i32Bits(pl), i32Bits(pt), 0,
              flags, f32Bits(actP0), f32Bits(actP1),
            ],
            grid2dForCount(total),
          );
          blobs[top] = outT;
        } else if (op.op === 'inner_product') {
          const inp = blobs[layer.bottoms[0]];
          const top = layer.tops[0];
          const outT = allocTensor(1, 1, op.num_output, consumerCounts[top]);
          const actP0 = op.activation_params?.[0] ?? 0.1;
          const actP1 = op.activation_params?.[1] ?? 1.0;
          recordOp(
            'inner_product',
            [inp.entry.buf, lw.weight, lw.bias],
            outT.entry.buf,
            [
              op.num_input,
              op.num_output,
              op.bias_term ? 1 : 0,
              op.activation_type,
              f32Bits(actP0),
              f32Bits(actP1),
            ],
            [Math.ceil(op.num_output / 64), 1, 1],
          );
          blobs[top] = outT;
        } else if (op.op === 'pooling') {
          const inp = blobs[layer.bottoms[0]];
          const top = layer.tops[0];
          if (!op.global_pooling) {
            throw new Error(t('runtime.webgpuIncompatible'));
          }
          const outT = allocTensor(1, 1, inp.c, consumerCounts[top]);
          recordOp(
            'pooling_global',
            [inp.entry.buf],
            outT.entry.buf,
            [inp.w * inp.h, inp.c, op.pooling_type],
            [inp.c, 1, 1],
          );
          blobs[top] = outT;
        } else if (
          op.op === 'p_re_lu' || op.op === 're_lu' || op.op === 'clip'
          || op.op === 'swish' || op.op === 'sigmoid'
        ) {
          const inp = blobs[layer.bottoms[0]];
          const top = layer.tops[0];
          const outT = allocTensor(inp.w, inp.h, inp.c, consumerCounts[top]);
          const total = inp.w * inp.h * inp.c;
          let mode = 0; let p0 = 0; let p1 = 0; let numSlope = 0;
          let slopeBuf = this.dummyStorageBuffer;
          if (op.op === 're_lu') { mode = 0; p0 = op.slope || 0; }
          else if (op.op === 'clip') { mode = 1; p0 = op.min; p1 = op.max; }
          else if (op.op === 'swish') { mode = 2; }
          else if (op.op === 'sigmoid') { mode = 3; }
          else if (op.op === 'p_re_lu') { mode = 4; numSlope = op.num_slope; slopeBuf = lw.slope; }
          recordOp(
            'unary_op',
            [inp.entry.buf, slopeBuf],
            outT.entry.buf,
            [total, inp.w * inp.h, mode, numSlope, f32Bits(p0), f32Bits(p1)],
            grid2dForCount(total),
          );
          blobs[top] = outT;
        } else if (op.op === 'binary_op') {
          const a = blobs[layer.bottoms[0]];
          const top = layer.tops[0];
          if (op.with_scalar) {
            const total = a.w * a.h * a.c;
            const outT = allocTensor(a.w, a.h, a.c, consumerCounts[top]);
            recordOp(
              'binary_op',
              [a.entry.buf, this.dummyStorageBuffer],
              outT.entry.buf,
              [a.w, a.h, a.c, 1, 1, 1, a.w, a.h, a.c, op.op_type, 1, f32Bits(op.scalar), total],
              grid2dForCount(total),
            );
            blobs[top] = outT;
          } else {
            const b = blobs[layer.bottoms[1]];
            const ow = Math.max(a.w, b.w);
            const oh = Math.max(a.h, b.h);
            const oc = Math.max(a.c, b.c);
            const total = ow * oh * oc;
            const outT = allocTensor(ow, oh, oc, consumerCounts[top]);
            recordOp(
              'binary_op',
              [a.entry.buf, b.entry.buf],
              outT.entry.buf,
              [a.w, a.h, a.c, b.w, b.h, b.c, ow, oh, oc, op.op_type, 0, 0, total],
              grid2dForCount(total),
            );
            blobs[top] = outT;
          }
        } else if (op.op === 'concat') {
          if (op.axis !== 0) {
            throw new Error(t('runtime.webgpuIncompatible'));
          }
          const first = blobs[layer.bottoms[0]];
          let totalC = 0;
          for (const bi of layer.bottoms) totalC += blobs[bi].c;
          const top = layer.tops[0];
          const outT = allocTensor(first.w, first.h, totalC, consumerCounts[top]);
          let elemOffset = 0;
          for (const bi of layer.bottoms) {
            const bt = blobs[bi];
            const elemCount = bt.w * bt.h * bt.c;
            const byteOffset = elemOffset * elemBytes;
            const byteSize = elemCount * elemBytes;
            if ((byteOffset & 3) === 0 && (byteSize & 3) === 0) {
              dispatches.push({
                type: 'copy',
                src: bt.entry.buf,
                srcOffset: 0,
                dst: outT.entry.buf,
                dstOffset: byteOffset,
                byteSize,
              });
            } else {
              recordOp(
                'concat_elems',
                [bt.entry.buf],
                outT.entry.buf,
                [elemOffset, elemCount],
                grid2dForCount(elemCount),
              );
            }
            elemOffset += elemCount;
          }
          blobs[top] = outT;
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

          let chOff = 0;
          const planeElems = inp.w * inp.h;
          const planeBytes = planeElems * elemBytes;
          for (let i = 0; i < nTops; i += 1) {
            const sz = sizes[i];
            const top = layer.tops[i];
            const outT = allocTensor(inp.w, inp.h, sz, consumerCounts[top]);
            const srcOffset = chOff * planeBytes;
            const byteSize = sz * planeBytes;
            if ((srcOffset & 3) === 0 && (byteSize & 3) === 0) {
              dispatches.push({
                type: 'copy',
                src: inp.entry.buf,
                srcOffset,
                dst: outT.entry.buf,
                dstOffset: 0,
                byteSize,
              });
            } else {
              const total = planeElems * sz;
              recordOp(
                'sub_box',
                [inp.entry.buf],
                outT.entry.buf,
                [inp.w, inp.h, inp.w, inp.h, chOff, 0, 0, total],
                grid2dForCount(total),
              );
            }
            blobs[top] = outT;
            chOff += sz;
          }
        } else if (op.op === 'group_norm') {
          const inp = blobs[layer.bottoms[0]];
          const top = layer.tops[0];
          const outT = allocTensor(inp.w, inp.h, inp.c, consumerCounts[top]);
          const chTotal = op.channels > 0 ? op.channels : inp.c;
          const chG = Math.floor(chTotal / Math.max(1, op.group));
          recordOp(
            'groupnorm',
            [inp.entry.buf, lw.gamma, lw.beta],
            outT.entry.buf,
            [inp.w * inp.h, chG, op.affine ? 1 : 0, f32Bits(op.eps)],
            [op.group, 1, 1],
          );
          blobs[top] = outT;
        } else if (op.op === 'interp') {
          const inp = blobs[layer.bottoms[0]];
          const iw = op.output_width > 0
            ? op.output_width
            : Math.max(1, Math.floor(inp.w * op.width_scale));
          const ih = op.output_height > 0
            ? op.output_height
            : Math.max(1, Math.floor(inp.h * op.height_scale));
          const top = layer.tops[0];
          const outT = allocTensor(iw, ih, inp.c, consumerCounts[top]);
          const total = iw * ih * inp.c;
          recordOp(
            'interp',
            [inp.entry.buf],
            outT.entry.buf,
            [inp.w, inp.h, iw, ih, inp.c, op.resize_type, op.align_corner ? 1 : 0, total],
            grid2dForCount(total),
          );
          blobs[top] = outT;
        } else if (op.op === 'pixel_shuffle') {
          const inp = blobs[layer.bottoms[0]];
          const r = op.upscale_factor;
          const top = layer.tops[0];
          if (r <= 1) {
            if (inp.refObj) inp.refObj.refs += Math.max(1, consumerCounts[top]);
            blobs[top] = { w: inp.w, h: inp.h, c: inp.c, entry: inp.entry, refObj: inp.refObj };
          } else {
            const oc = Math.floor(inp.c / (r * r));
            const ow = inp.w * r;
            const oh = inp.h * r;
            const total = ow * oh * oc;
            const outT = allocTensor(ow, oh, oc, consumerCounts[top]);
            recordOp(
              'space_depth',
              [inp.entry.buf],
              outT.entry.buf,
              [inp.w, inp.h, inp.c, ow, oh, oc, r, op.mode || 0, 0, total],
              grid2dForCount(total),
            );
            blobs[top] = outT;
          }
        } else if (op.op === 'reorg') {
          const inp = blobs[layer.bottoms[0]];
          const s = op.stride;
          const ow = Math.floor(inp.w / s);
          const oh = Math.floor(inp.h / s);
          const oc = inp.c * s * s;
          const total = ow * oh * oc;
          const top = layer.tops[0];
          const outT = allocTensor(ow, oh, oc, consumerCounts[top]);
          recordOp(
            'space_depth',
            [inp.entry.buf],
            outT.entry.buf,
            [inp.w, inp.h, inp.c, ow, oh, oc, s, op.mode || 0, 1, total],
            grid2dForCount(total),
          );
          blobs[top] = outT;
        } else if (op.op === 'crop') {
          const inp = blobs[layer.bottoms[0]];
          const [cs, ce, hs, he, ws, we] = resolveCropBounds(inp.c, inp.h, inp.w, op);
          const oc = ce - cs;
          const oh = he - hs;
          const ow = we - ws;
          const total = ow * oh * oc;
          const top = layer.tops[0];
          const outT = allocTensor(ow, oh, oc, consumerCounts[top]);
          recordOp(
            'sub_box',
            [inp.entry.buf],
            outT.entry.buf,
            [inp.w, inp.h, ow, oh, cs, hs, ws, total],
            grid2dForCount(total),
          );
          blobs[top] = outT;
        } else if (op.op === 'eltwise') {
          const first = blobs[layer.bottoms[0]];
          const total = first.w * first.h * first.c;
          const top = layer.tops[0];
          const coeffs = op.coeffs || [];
          let curEntry = first.entry;
          let curOwnedTemp = false;

          for (let i = 1; i < layer.bottoms.length; i += 1) {
            const rhs = blobs[layer.bottoms[i]];
            const isLast = i === layer.bottoms.length - 1;
            const dstTensor = isLast
              ? allocTensor(first.w, first.h, first.c, consumerCounts[top])
              : { entry: allocPlanStorage(total * elemBytes) };
            const c0 = i === 1 ? (coeffs[0] ?? 1.0) : 1.0;
            const c1 = coeffs[i] ?? 1.0;
            recordOp(
              'eltwise_pair',
              [curEntry.buf, rhs.entry.buf],
              dstTensor.entry.buf,
              [total, op.op_type ?? 1, f32Bits(c0), f32Bits(c1)],
              grid2dForCount(total),
            );
            if (curOwnedTemp) {
              recyclePlanStorage(curEntry);
            }
            if (isLast) {
              blobs[top] = dstTensor;
            } else {
              curEntry = dstTensor.entry;
              curOwnedTemp = true;
            }
          }
        } else if (op.op === 'padding') {
          const inp = blobs[layer.bottoms[0]];
          const ow = inp.w + op.left + op.right;
          const oh = inp.h + op.top + op.bottom;
          const total = ow * oh * inp.c;
          const top = layer.tops[0];
          const outT = allocTensor(ow, oh, inp.c, consumerCounts[top]);
          recordOp(
            'pad_2d',
            [inp.entry.buf],
            outT.entry.buf,
            [inp.w, inp.h, ow, oh, op.top, op.left, op.pad_type || 0, f32Bits(op.value || 0), total],
            grid2dForCount(total),
          );
          blobs[top] = outT;
        } else {
          throw new Error(t('runtime.webgpuUnsupportedOp', { op: op.op }));
        }

        for (const bIdx of layer.bottoms) {
          releaseBlob(bIdx);
        }
      }

      const finalBlob = blobs[meta.output_blob];
      if (!finalBlob) {
        throw new Error(t('runtime.webgpuEmptyOutput'));
      }

      const outTotalFloats = outW * outH * channels;
      const outByteLen = outTotalFloats * 4;
      const rawOutEntry = allocPlanStorage(outByteLen);
      recordOp(
        'postprocess',
        [finalBlob.entry.buf],
        rawOutEntry.buf,
        [finalBlob.w, finalBlob.h, outW, outH, channels, outTotalFloats],
        grid2dForCount(outTotalFloats),
      );
      releaseBlob(meta.output_blob);

      // Upload all uniforms for this tile shape ONCE!
      this.device.queue.writeBuffer(
        uniformBuf,
        0,
        uniformStaging.subarray(0, (dispatchIndex * UNIFORM_STRIDE) / 4),
      );

      const readbackBuf = this.device.createBuffer({
        size: outByteLen,
        usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
      });
      ownedBuffers.push(readbackBuf);

      // Group dispatches into command-buffer chunks (up to 192 steps/chunk for TDR safety on heavy RRDBs)
      // and coalesce consecutive `compute` steps into single `beginComputePass` scopes!
      const CHUNK = 192;
      const chunks = [];
      for (let start = 0; start < dispatches.length; start += CHUNK) {
        const end = Math.min(dispatches.length, start + CHUNK);
        const segments = [];
        let curCompute = null;
        for (let i = start; i < end; i += 1) {
          const step = dispatches[i];
          if (step.type === 'copy') {
            curCompute = null;
            segments.push(step);
          } else {
            if (!curCompute) {
              curCompute = { type: 'compute_pass', steps: [] };
              segments.push(curCompute);
            }
            curCompute.steps.push(step);
          }
        }
        chunks.push({ segments, isLast: end === dispatches.length });
      }

      const plan = {
        inByteLen,
        outTotalFloats,
        outByteLen,
        rawInBuf: rawInEntry.buf,
        rawOutBuf: rawOutEntry.buf,
        readbackBuf,
        chunks,
        ownedBuffers,
        outBuffers: [
          new Float32Array(outTotalFloats),
          new Float32Array(outTotalFloats),
        ],
        bufToggle: 0,
      };
      model.planCache.set(planKey, plan);
      return plan;
    } catch (err) {
      for (const buf of ownedBuffers) {
        try { buf.destroy(); } catch { /* ignore */ }
      }
      throw err;
    }
  }

  /**
   * Execute a cached GPU tile plan and read back the scaled HWC `Float32Array`.
   */
  async inferTileGpu(model, inputF32, width, height, channels) {
    const plan = this.getOrCreateTilePlan(model, width, height, channels);
    this.device.queue.writeBuffer(plan.rawInBuf, 0, inputF32);

    for (let cIdx = 0; cIdx < plan.chunks.length; cIdx += 1) {
      const chunk = plan.chunks[cIdx];
      const encoder = this.device.createCommandEncoder();
      for (let sIdx = 0; sIdx < chunk.segments.length; sIdx += 1) {
        const seg = chunk.segments[sIdx];
        if (seg.type === 'copy') {
          encoder.copyBufferToBuffer(seg.src, seg.srcOffset, seg.dst, seg.dstOffset, seg.byteSize);
        } else {
          const pass = encoder.beginComputePass();
          let lastPipeline = null;
          for (let i = 0; i < seg.steps.length; i += 1) {
            const step = seg.steps[i];
            if (step.pipeline !== lastPipeline) {
              pass.setPipeline(step.pipeline);
              lastPipeline = step.pipeline;
            }
            pass.setBindGroup(0, step.bg, [step.dynamicOffset]);
            pass.dispatchWorkgroups(step.workgroups[0], step.workgroups[1], step.workgroups[2]);
          }
          pass.end();
        }
      }
      if (chunk.isLast) {
        encoder.copyBufferToBuffer(plan.rawOutBuf, 0, plan.readbackBuf, 0, plan.outByteLen);
      }
      this.device.queue.submit([encoder.finish()]);
    }

    await plan.readbackBuf.mapAsync(GPUMapMode.READ);
    const dst = plan.outBuffers[plan.bufToggle];
    plan.bufToggle ^= 1;
    dst.set(new Float32Array(plan.readbackBuf.getMappedRange(0, plan.outByteLen)));
    plan.readbackBuf.unmap();
    return dst;
  }

  async runTiles({
    wasmBridge,
    modelPayload,
    regions,
    extractTile,
    channels,
    preferFp16 = true,
    signal,
    onCompiled,
    onTileDone,
  }) {
    const model = await this.prepareModel(wasmBridge, modelPayload, preferFp16);
    if (regions.length > 0) {
      this.getOrCreateTilePlan(model, regions[0].width, regions[0].height, channels);
    }
    if (onCompiled) onCompiled();

    let nextTileIn = regions.length > 0 ? extractTile(regions[0], channels) : null;
    let pendingDone = null;

    for (let i = 0; i < regions.length; i += 1) {
      if (signal && signal.aborted) {
        throw new DOMException(t('common.cancelled'), 'AbortError');
      }
      const region = regions[i];
      const tileIn = nextTileIn;
      const t0 = performance.now();

      // `inferTileGpu` synchronously submits GPU command buffers before its `await mapAsync`,
      // so the GPU executes tile `i` while CPU blends tile `i-1` and extracts tile `i+1`!
      const gpuPromise = this.inferTileGpu(model, tileIn, region.width, region.height, channels);

      if (pendingDone) {
        onTileDone(pendingDone.idx, pendingDone.region, pendingDone.tileOut, pendingDone.elapsedMs);
      }
      nextTileIn = (i + 1 < regions.length) ? extractTile(regions[i + 1], channels) : null;

      const tileOut = await gpuPromise;
      const elapsedMs = performance.now() - t0;
      pendingDone = { idx: i, region, tileOut, elapsedMs };
    }

    if (pendingDone) {
      onTileDone(pendingDone.idx, pendingDone.region, pendingDone.tileOut, pendingDone.elapsedMs);
    }
    return { deviceName: this.deviceName };
  }

  async benchmarkTile({ wasmBridge, modelPayload, tileSize = 128, channels = 3, preferFp16 = true }) {
    const model = await this.prepareModel(wasmBridge, modelPayload, preferFp16);
    const dummy = new Float32Array(tileSize * tileSize * channels).fill(0.5);
    const wStart = performance.now();
    for (let w = 0; w < 6; w += 1) {
      await this.inferTileGpu(model, dummy, tileSize, tileSize, channels);
      if (performance.now() - wStart >= 75) break;
    }
    let totalMs = 0;
    const runs = 3;
    for (let i = 0; i < runs; i += 1) {
      const t0 = performance.now();
      await this.inferTileGpu(model, dummy, tileSize, tileSize, channels);
      totalMs += performance.now() - t0;
    }
    return {
      avgTileMs: totalMs / runs,
      deviceName: this.deviceName,
    };
  }

  dispose() {
    this.disposeLoadedModel();
    if (this.dummyStorageBuffer) {
      try { this.dummyStorageBuffer.destroy(); } catch { /* ignore */ }
      this.dummyStorageBuffer = null;
    }
    if (this.device) {
      try { this.device.destroy(); } catch { /* ignore */ }
      this.device = null;
    }
  }
}
