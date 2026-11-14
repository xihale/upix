// Formatting + vocabulary shared by every view.

export function formatBytes(bytes) {
  if (!Number.isFinite(bytes) || bytes < 0) return '—';
  if (bytes < 1024) return `${bytes} B`;
  const units = ['KB', 'MB', 'GB', 'TB'];
  let value = bytes / 1024;
  let i = 0;
  while (value >= 1024 && i < units.length - 1) { value /= 1024; i++; }
  return `${value >= 100 || i === 0 ? value.toFixed(0) : value.toFixed(value >= 10 ? 1 : 2).replace(/\.?0+$/, '')} ${units[i]}`;
}

export function formatDims(w, h) {
  return `${w}×${h}`;
}

export function formatMs(ms) {
  if (!Number.isFinite(ms)) return '—';
  if (ms < 1000) return `${ms < 10 ? ms.toFixed(1) : Math.round(ms)} ms`;
  return `${(ms / 1000).toFixed(ms < 10_000 ? 2 : 1)} s`;
}

export function formatNum(value, digits = 1) {
  return Number.isFinite(value) ? value.toFixed(digits) : '—';
}

export function stripExt(name) {
  return name.replace(/\.[^./\\]+$/, '');
}

export const BACKENDS = {
  webgpu: { id: 'webgpu', label: 'WebGPU', short: 'WebGPU', shortFail: '浏览器不支持' },
  wasm: { id: 'wasm', label: 'WASM CPU', short: 'WASM', shortFail: '浏览器不支持' },
  webnn: { id: 'webnn', label: 'WebNN', short: 'WebNN', experimental: true, shortFail: '环境不支持' },
};
export const BACKEND_IDS = ['webgpu', 'wasm', 'webnn'];

/**
 * Benchmark row plan shared by the Engine view and `runBenchmark`: one row per
 * backend plus one row per WebNN device (`gpu` / `npu` / `cpu`), so all three
 * WebNN endpoints get measured instead of just the workbench-selected one.
 * `lab.webnn` auto-probe results (fresher, with per-device reasons) take
 * precedence over `caps.webnn.features.webnnDevices` when present.
 */
export function benchRows(caps, lab = {}) {
  const rows = [
    { id: 'webgpu', label: BACKENDS.webgpu.label, available: !!caps?.webgpu?.available, reason: caps?.webgpu?.reason },
    { id: 'wasm', label: BACKENDS.wasm.label, available: !!caps?.wasm?.available, reason: caps?.wasm?.reason },
  ];
  const devices = caps?.webnn?.features?.webnnDevices ?? {};
  const nnCap = caps?.webnn;
  for (const d of ['gpu', 'npu', 'cpu']) {
    const probe = lab?.webnn?.[d];
    rows.push({
      id: `webnn:${d}`,
      label: `WebNN ${d.toUpperCase()}`,
      available: probe ? probe.status === 'ok' : (nnCap?.available && devices[d] === true),
      probing: probe?.status === 'probing',
      reason: probe?.reason
        ?? (nnCap?.available && devices[d] === false ? `${d.toUpperCase()} 不可用` : nnCap?.reason),
    });
  }
  return rows;
}

const SCENES = {
  anime: '动漫', 动漫: '动漫',
  photo: '照片', 照片: '照片',
  general: '综合', 综合: '综合',
  restore: '修复', restoration: '修复', 修复: '修复',
};
const SCENE_KEYS = { 动漫: 'anime', 照片: 'photo', 综合: 'general', 修复: 'restore' };

export function sceneLabel(scene) {
  return SCENES[String(scene ?? '').trim().toLowerCase()] ?? (scene || '综合');
}
export function sceneKey(scene) {
  return SCENE_KEYS[sceneLabel(scene)] ?? 'general';
}

export function matchesModelQuery(model, query) {
  if (!query) return true;
  const hay = `${model.name} ${model.shortName ?? ''} ${model.family} ${sceneLabel(model.scene)} ${model.scene}`.toLowerCase();
  return hay.includes(query);
}

export function groupModelsByFamily(models) {
  const families = new Map();
  for (const m of models) {
    if (!families.has(m.family)) families.set(m.family, []);
    families.get(m.family).push(m);
  }
  const nodes = [];
  for (const [family, items] of families) {
    nodes.push({ key: `g:${family}`, kind: 'group', family, count: items.length });
    for (const model of items) nodes.push({ key: `m:${model.name}`, kind: 'model', model });
  }
  return nodes;
}

export function downloadProgress(dl) {
  const busy = !!dl && !dl.error;
  const pct = busy && dl.total ? (dl.loaded / dl.total) * 100 : 0;
  return {
    busy,
    pct,
    label: busy ? (dl.phase === 'verify' ? '校验中' : `${Math.round(pct)}%`) : '',
    error: dl?.error ?? '',
  };
}

export const OUTPUT_FORMATS = [
  { value: 'follow', label: '跟随' },
  { value: 'png', label: 'PNG' },
  { value: 'jpeg', label: 'JPEG' },
  { value: 'webp', label: 'WebP' },
];

export const OUTPUT_FORMAT_LABELS = {
  png: 'PNG',
  jpeg: 'JPEG',
  webp: 'WebP',
};

export function isFollowOutputFormat(format) {
  const f = String(format ?? '').toLowerCase().trim();
  return !f || f === 'follow' || f === 'auto';
}

export function normalizeOutputFormat(format) {
  const fmt = String(format ?? '').toLowerCase().replace(/^\./, '').trim();
  if (fmt === 'jpg' || fmt === 'jpeg') return 'jpeg';
  if (fmt === 'png' || fmt === 'webp') return fmt;
  if (fmt === 'avif') return 'webp';
  return 'png';
}

export function inferInputFormat(item) {
  const probed = item?.probe?.format;
  if (probed) return String(probed).toLowerCase();
  const type = (item?.file?.type || '').toLowerCase();
  if (type.includes('jpeg') || type.includes('jpg')) return 'jpeg';
  if (type.includes('webp')) return 'webp';
  if (type.includes('avif')) return 'avif';
  if (type.includes('png')) return 'png';
  const ext = (item?.name?.split('.').pop() || '').toLowerCase();
  if (ext === 'jpg' || ext === 'jpeg') return 'jpeg';
  if (ext === 'png' || ext === 'webp' || ext === 'avif') return ext;
  return '';
}

export const IMAGE_EXT = /\.(png|jpe?g|webp|avif)$/i;
export const MODEL_EXT = /\.(param|bin|zip)$/i;

export function isImageFile(file) {
  return file.type.startsWith('image/') || IMAGE_EXT.test(file.name);
}
export function isModelFile(file) {
  return MODEL_EXT.test(file.name);
}

export function errorMessage(error) {
  return error?.message || String(error) || '未知错误';
}
