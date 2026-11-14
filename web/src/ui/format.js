// Formatting + vocabulary shared by every view.

import { formatNumber, formatPercent, t } from '../i18n/index.js';

export function formatBytes(bytes) {
  if (!Number.isFinite(bytes) || bytes < 0) return '—';
  if (bytes < 1024) return `${formatNumber(bytes, { maximumFractionDigits: 0 })} B`;
  const units = ['KB', 'MB', 'GB', 'TB'];
  let value = bytes / 1024;
  let i = 0;
  while (value >= 1024 && i < units.length - 1) { value /= 1024; i++; }
  const maxDigits = value >= 100 || i === 0 ? 0 : (value >= 10 ? 1 : 2);
  return `${formatNumber(value, { maximumFractionDigits: maxDigits })} ${units[i]}`;
}

export function formatDims(w, h) {
  return `${w}×${h}`;
}

export function formatMs(ms) {
  if (!Number.isFinite(ms)) return '—';
  if (ms < 1000) {
    const digits = ms < 10 ? 1 : 0;
    return `${formatNumber(ms, { minimumFractionDigits: digits, maximumFractionDigits: digits })} ms`;
  }
  const digits = ms < 10_000 ? 2 : 1;
  return `${formatNumber(ms / 1000, { minimumFractionDigits: digits, maximumFractionDigits: digits })} s`;
}

export function formatNum(value, digits = 1) {
  return Number.isFinite(value)
    ? formatNumber(value, { minimumFractionDigits: digits, maximumFractionDigits: digits })
    : '—';
}

export function stripExt(name) {
  return name.replace(/\.[^./\\]+$/, '');
}

export const BACKEND_IDS = ['webgpu', 'wasm', 'webnn'];

export function getBackends() {
  return {
    webgpu: {
      id: 'webgpu',
      label: t('backends.webgpu.label'),
      short: t('backends.webgpu.short'),
      shortFail: t('backends.webgpu.shortFail'),
    },
    wasm: {
      id: 'wasm',
      label: t('backends.wasm.label'),
      short: t('backends.wasm.short'),
      shortFail: t('backends.wasm.shortFail'),
    },
    webnn: {
      id: 'webnn',
      label: t('backends.webnn.label'),
      short: t('backends.webnn.short'),
      experimental: true,
      shortFail: t('backends.webnn.shortFail'),
    },
  };
}

/**
 * Benchmark row plan shared by the Engine view and `runBenchmark`: one row per
 * backend plus one row per WebNN device (`gpu` / `npu` / `cpu`), so all three
 * WebNN endpoints get measured instead of just the workbench-selected one.
 * `lab.webnn` auto-probe results (fresher, with per-device reasons) take
 * precedence over `caps.webnn.features.webnnDevices` when present.
 */
export function benchRows(caps, lab = {}) {
  const backends = getBackends();
  const rows = [
    { id: 'webgpu', label: backends.webgpu.label, available: !!caps?.webgpu?.available, reason: caps?.webgpu?.reason },
    { id: 'wasm', label: backends.wasm.label, available: !!caps?.wasm?.available, reason: caps?.wasm?.reason },
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
        ?? (nnCap?.available && devices[d] === false
          ? t('sheet.deviceUnavailable', { device: d.toUpperCase() })
          : nnCap?.reason),
    });
  }
  return rows;
}

const SCENE_ALIASES = {
  anime: 'anime', 动漫: 'anime',
  photo: 'photo', 照片: 'photo',
  general: 'general', 综合: 'general',
  restore: 'restore', restoration: 'restore', 修复: 'restore',
  custom: 'custom', 自定义: 'custom',
};

export function sceneKey(scene) {
  return SCENE_ALIASES[String(scene ?? '').trim().toLowerCase()] ?? 'general';
}

export function sceneLabel(scene) {
  const raw = String(scene ?? '').trim();
  const key = SCENE_ALIASES[raw.toLowerCase()];
  if (key) return t(`scenes.${key}`);
  return raw || t('scenes.general');
}

const FAMILY_ALIASES = {
  standalone: 'standalone',
  独立模型: 'standalone',
  imported: 'imported',
  本地导入: 'imported',
};

export function familyLabel(family) {
  const raw = String(family ?? '').trim();
  const key = FAMILY_ALIASES[raw.toLowerCase()] ?? FAMILY_ALIASES[raw];
  return key ? t(`families.${key}`) : raw;
}

export function matchesModelQuery(model, query) {
  if (!query) return true;
  const hay = [
    model.name,
    model.shortName ?? '',
    model.family,
    familyLabel(model.family),
    sceneLabel(model.scene),
    sceneKey(model.scene),
    model.scene,
  ].join(' ').toLowerCase();
  return hay.includes(query);
}

export function groupModelsByFamily(models) {
  const families = new Map();
  for (const m of models) {
    const fam = familyLabel(m.family);
    if (!families.has(fam)) families.set(fam, []);
    families.get(fam).push(m);
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
    label: busy ? (dl.phase === 'verify' ? t('inspector.verifying') : formatPercent(pct)) : '',
    error: dl?.error ?? '',
  };
}

export function getOutputFormats() {
  return [
    { value: 'follow', label: t('inspector.formatFollow') },
    { value: 'png', label: 'PNG' },
    { value: 'jpeg', label: 'JPEG' },
    { value: 'webp', label: 'WebP' },
  ];
}

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
  return error?.message || String(error) || t('common.unknownError');
}
