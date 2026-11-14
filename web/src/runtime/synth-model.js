// Deterministic in-memory ncnn SRVGG-mini (`x2`) generator (~27 KB).
// Used by `runtime.benchmark()` when no remote/imported model is cached yet so users can
// benchmark WebGPU vs WASM SIMD128 vs WebNN immediately with zero network traffic.

export const SYNTH_BENCHMARK_MODEL_NAME = 'upix-bench-srvgg-x2';

let cachedSynthPayload = null;

export function getSyntheticBenchmarkModel() {
  if (cachedSynthPayload) {
    return cachedSynthPayload;
  }

  const cIn = 3;
  const cMid = 16;
  const scale = 2;
  const cTail = cIn * scale * scale; // 12

  const w0Size = cMid * cIn * 9;   // 432
  const w1Size = cMid * cMid * 9;  // 2304
  const w2Size = cMid * cMid * 9;  // 2304
  const w3Size = cTail * cMid * 9; // 1728

  const paramLines = [
    '7767517',
    '12 13',
    `Input            input            0 1 in0 0=0 1=0 2=${cIn}`,
    'Split            splitncnn_0      1 2 in0 in0_main in0_skip',
    `Convolution      conv_head        1 1 in0_main feat0 0=${cMid} 1=3 2=1 3=1 4=1 5=1 6=${w0Size}`,
    `PReLU            act_head         1 1 feat0 act0 0=${cMid}`,
    `Convolution      conv_body1       1 1 act0 feat1 0=${cMid} 1=3 2=1 3=1 4=1 5=1 6=${w1Size}`,
    `PReLU            act_body1        1 1 feat1 act1 0=${cMid}`,
    `Convolution      conv_body2       1 1 act1 feat2 0=${cMid} 1=3 2=1 3=1 4=1 5=1 6=${w2Size}`,
    `PReLU            act_body2        1 1 feat2 act2 0=${cMid}`,
    `Convolution      conv_tail        1 1 act2 tail 0=${cTail} 1=3 2=1 3=1 4=1 5=1 6=${w3Size}`,
    `PixelShuffle     upsample         1 1 tail sr_res 0=${scale} 1=0`,
    'Interp           skip_up          1 1 in0_skip skip_2x 0=1 1=2.000000 2=2.000000',
    'Eltwise          residual_sum     2 1 sr_res skip_2x out0 0=1',
  ];
  const paramText = `${paramLines.join('\n')}\n`;

  // Calculate exact binary layout:
  // Each Convolution with bias_term=1 reads: [u32 tag=0x00000000] + weight[wSize] + bias[outC] (untagged fp32)
  // Each PReLU with num_slope=cMid reads: slope[cMid] (untagged fp32)
  const totalU32Words = (1 + w0Size + cMid)
    + cMid
    + (1 + w1Size + cMid)
    + cMid
    + (1 + w2Size + cMid)
    + cMid
    + (1 + w3Size + cTail);

  const buf = new ArrayBuffer(totalU32Words * 4);
  const view = new DataView(buf);
  let byteOff = 0;

  let seed = 0x55504958; // "UPIX"
  const nextRand = () => {
    seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
    return (seed / 0xffffffff) * 2.0 - 1.0;
  };

  const writeU32 = (v) => {
    view.setUint32(byteOff, v >>> 0, true);
    byteOff += 4;
  };
  const writeF32 = (v) => {
    view.setFloat32(byteOff, v, true);
    byteOff += 4;
  };

  const writeConv = (wCount, outChannels, scaleStd) => {
    writeU32(0x00000000); // fp32 tag
    for (let i = 0; i < wCount; i += 1) {
      writeF32(nextRand() * scaleStd);
    }
    for (let c = 0; c < outChannels; c += 1) {
      writeF32(0.0);
    }
  };

  const writePrelu = (channels) => {
    for (let c = 0; c < channels; c += 1) {
      writeF32(0.1);
    }
  };

  writeConv(w0Size, cMid, 0.04);
  writePrelu(cMid);
  writeConv(w1Size, cMid, 0.03);
  writePrelu(cMid);
  writeConv(w2Size, cMid, 0.03);
  writePrelu(cMid);
  writeConv(w3Size, cTail, 0.01);

  cachedSynthPayload = {
    name: SYNTH_BENCHMARK_MODEL_NAME,
    paramText,
    binBytes: new Uint8Array(buf),
  };
  return cachedSynthPayload;
}
