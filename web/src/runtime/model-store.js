// IndexedDB model store + GitHub Release `manifest.json` client + local `.param`/`.bin`/`.zip` importer.
// Verifies SHA-256 digests (`crypto.subtle` with WASM `upix_sha256_hex` fallback for LAN HTTP contexts).

import { extractZipEntries } from './zip-utils.js';

const DB_NAME = 'upix-models-v1';
const DB_VERSION = 1;
const STORE_MODELS = 'models';
const STORE_META = 'meta';

export const HF_MIRROR_BASE = 'https://hf-mirror.com/xihale/upix-models/resolve/main';
export const HF_RELEASE_BASE = 'https://huggingface.co/xihale/upix-models/resolve/main';
export const DEFAULT_RELEASE_BASE = 'https://github.com/xihale/upix/releases/download/models';

export function isMainlandChinaLocale() {
  try {
    const tz = Intl.DateTimeFormat().resolvedOptions().timeZone || '';
    if (tz === 'Asia/Shanghai' || tz === 'Asia/Chongqing' || tz === 'Asia/Harbin' || tz === 'Asia/Urumqi') {
      return true;
    }
  } catch { /* ignore */ }
  try {
    const langs = navigator.languages?.length ? navigator.languages : [navigator.language];
    return langs.some((l) => typeof l === 'string' && /^zh(?:-(?:cn|hans|hans-cn))?$/i.test(l));
  } catch {
    return false;
  }
}

export function getDefaultModelSource() {
  return isMainlandChinaLocale() ? 'hf-mirror' : 'hf';
}

export function resolveWebModelSource(pref) {
  const v = (pref || '').trim().toLowerCase();
  if (v === 'hf-mirror' || v === HF_MIRROR_BASE.toLowerCase()) return 'hf-mirror';
  if (v === 'hf' || v === 'huggingface' || v === HF_RELEASE_BASE.toLowerCase()) return 'hf';
  // GitHub Release lacks CORS headers on S3 redirects and is locked on Web.
  return getDefaultModelSource();
}

export function getSourceBaseUrl(sourceId) {
  return resolveWebModelSource(sourceId) === 'hf-mirror' ? HF_MIRROR_BASE : HF_RELEASE_BASE;
}

function openDb() {
  return new Promise((resolve, reject) => {
    if (typeof indexedDB === 'undefined') {
      reject(new Error('浏览器不支持 IndexedDB'));
      return;
    }
    const req = indexedDB.open(DB_NAME, DB_VERSION);
    req.onupgradeneeded = () => {
      const db = req.result;
      if (!db.objectStoreNames.contains(STORE_MODELS)) {
        db.createObjectStore(STORE_MODELS, { keyPath: 'name' });
      }
      if (!db.objectStoreNames.contains(STORE_META)) {
        db.createObjectStore(STORE_META, { keyPath: 'key' });
      }
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error || new Error('打开 IndexedDB 失败'));
  });
}

function idbGet(db, storeName, key) {
  return new Promise((resolve, reject) => {
    const tx = db.transaction(storeName, 'readonly');
    const req = tx.objectStore(storeName).get(key);
    req.onsuccess = () => resolve(req.result || null);
    req.onerror = () => reject(req.error);
  });
}

function idbGetAll(db, storeName) {
  return new Promise((resolve, reject) => {
    const tx = db.transaction(storeName, 'readonly');
    const req = tx.objectStore(storeName).getAll();
    req.onsuccess = () => resolve(req.result || []);
    req.onerror = () => reject(req.error);
  });
}

function idbPut(db, storeName, value) {
  return new Promise((resolve, reject) => {
    const tx = db.transaction(storeName, 'readwrite');
    tx.objectStore(storeName).put(value);
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error);
  });
}

function idbDelete(db, storeName, key) {
  return new Promise((resolve, reject) => {
    const tx = db.transaction(storeName, 'readwrite');
    tx.objectStore(storeName).delete(key);
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error);
  });
}

function idbClear(db, storeName) {
  return new Promise((resolve, reject) => {
    const tx = db.transaction(storeName, 'readwrite');
    tx.objectStore(storeName).clear();
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error);
  });
}

async function computeSha256Hex(bytes, wasmBridge) {
  if (typeof crypto !== 'undefined' && crypto.subtle && typeof crypto.subtle.digest === 'function') {
    const buf = bytes.byteOffset === 0 && bytes.byteLength === bytes.buffer.byteLength
      ? bytes.buffer
      : bytes.slice().buffer;
    const digest = await crypto.subtle.digest('SHA-256', buf);
    const arr = new Uint8Array(digest);
    let hex = '';
    for (let i = 0; i < arr.length; i += 1) {
      hex += arr[i].toString(16).padStart(2, '0');
    }
    return hex;
  }
  return wasmBridge.sha256Hex(bytes);
}

function buildCandidateUrls(filename, mirrorPref) {
  const candidates = [];
  const trimmed = (mirrorPref || '').trim().replace(/\/+$/, '');
  const primaryId = resolveWebModelSource(trimmed);
  // Allow optional non-GitHub custom base URL for legacy/power users, otherwise use chosen route.
  if (/^https?:\/\//i.test(trimmed) && !/github\.com|hf-mirror\.com|huggingface\.co/i.test(trimmed)) {
    candidates.push(`${trimmed}/${filename}`);
  }
  if (import.meta.env.DEV && !trimmed) {
    candidates.push(`/api/upix-models/${filename}`);
  }
  if (primaryId === 'hf-mirror') {
    candidates.push(`${HF_MIRROR_BASE}/${filename}`);
    candidates.push(`${HF_RELEASE_BASE}/${filename}`);
  } else {
    candidates.push(`${HF_RELEASE_BASE}/${filename}`);
  }
  return [...new Set(candidates)];
}

async function fetchStreamWithProgress(url, { signal, onProgress, expectedTotal = 0 } = {}) {
  const resp = await fetch(url, { signal, credentials: 'omit' });
  if (!resp.ok) {
    const friendly = resp.status === 404 ? '下载地址不存在（文件可能已下架）'
      : resp.status === 403 ? '下载被拒绝（可能触发了访问限制）'
        : resp.status >= 500 ? '下载服务器暂时不可用，请稍后重试'
          : `下载失败（网络错误 ${resp.status}）`;
    throw new Error(friendly);
  }
  const contentType = resp.headers.get('content-type') || '';
  if (contentType.includes('text/html')) {
    throw new Error('下载地址返回了无效数据（可能被网络拦截），可尝试配置镜像或导入本地模型文件');
  }
  const totalHeader = Number(resp.headers.get('content-length') || 0);
  const total = totalHeader > 0 ? totalHeader : expectedTotal;

  if (!resp.body || typeof resp.body.getReader !== 'function') {
    const ab = await resp.arrayBuffer();
    const out = new Uint8Array(ab);
    if (onProgress) onProgress({ loaded: out.byteLength, total: out.byteLength, phase: 'download' });
    return out;
  }

  const reader = resp.body.getReader();
  const chunks = [];
  let loaded = 0;
  while (true) {
    const { value, done } = await reader.read();
    if (done) break;
    chunks.push(value);
    loaded += value.byteLength;
    if (onProgress) {
      onProgress({ loaded, total, phase: 'download' });
    }
  }

  const merged = new Uint8Array(loaded);
  let offset = 0;
  for (const ch of chunks) {
    merged.set(ch, offset);
    offset += ch.byteLength;
  }
  return merged;
}

export class UpixModelStore {
  constructor(wasmBridge) {
    this.wasm = wasmBridge;
    this.dbPromise = null;
    this.localDevModels = null; // Map<name, { sizeBytes }> when Vite dev middleware is present
    this.payloadCache = new Map(); // LRU Map<name, { name, paramText, binBytes }> (max 4)
  }

  rememberPayload(payload) {
    if (this.payloadCache.has(payload.name)) {
      this.payloadCache.delete(payload.name);
    } else if (this.payloadCache.size >= 4) {
      const oldestKey = this.payloadCache.keys().next().value;
      this.payloadCache.delete(oldestKey);
    }
    this.payloadCache.set(payload.name, payload);
    return payload;
  }

  async db() {
    if (!this.dbPromise) {
      this.dbPromise = openDb().catch(() => null);
    }
    return this.dbPromise;
  }

  async probeLocalDevModels() {
    if (this.localDevModels !== null) return this.localDevModels;
    if (!import.meta.env.DEV) {
      this.localDevModels = new Map();
      return this.localDevModels;
    }
    try {
      const resp = await fetch('/api/local-models/index.json', { credentials: 'omit' });
      if (resp.ok && (resp.headers.get('content-type') || '').includes('application/json')) {
        const list = await resp.json();
        const map = new Map();
        if (Array.isArray(list)) {
          for (const item of list) {
            if (item && item.name) map.set(item.name, item);
          }
        }
        this.localDevModels = map;
        return map;
      }
    } catch {
      // Production static hosting has no `/api/local-models` endpoint.
    }
    this.localDevModels = new Map();
    return this.localDevModels;
  }

  async fetchRemoteManifest({ refresh = false, mirrorBaseUrl = '' } = {}) {
    const db = await this.db();
    if (!refresh && db) {
      const cached = await idbGet(db, STORE_META, 'remote_manifest');
      if (cached && Array.isArray(cached.models) && Date.now() - cached.fetchedAt < 30 * 60 * 1000) {
        return { models: cached.models, error: null };
      }
    }

    const primaryId = resolveWebModelSource(mirrorBaseUrl);
    const urls = buildCandidateUrls('manifest.json', mirrorBaseUrl);
    let lastErr = primaryId === 'hf' ? 'Hugging Face 无法连接' : '无法连接模型发布源';
    for (const url of urls) {
      try {
        const resp = await fetch(url, { credentials: 'omit' });
        if (!resp.ok) {
          lastErr = `HTTP ${resp.status}`;
          continue;
        }
        const ct = resp.headers.get('content-type') || '';
        if (ct.includes('text/html')) {
          continue;
        }
        const json = await resp.json();
        if (json && json.version === 1 && Array.isArray(json.models)) {
          if (db) {
            await idbPut(db, STORE_META, {
              key: 'remote_manifest',
              models: json.models,
              fetchedAt: Date.now(),
            }).catch(() => {});
          }
          return { models: json.models, error: null };
        }
      } catch (err) {
        lastErr = err instanceof Error ? err.message : String(err);
      }
    }

    const finalErr = primaryId === 'hf' ? `Hugging Face 无法连接（${lastErr}）` : lastErr;
    // Fallback to stale cached manifest if available.
    if (db) {
      const stale = await idbGet(db, STORE_META, 'remote_manifest').catch(() => null);
      if (stale && Array.isArray(stale.models)) {
        return { models: stale.models, error: finalErr };
      }
    }
    return { models: [], error: finalErr };
  }

  async listModels({ refreshRemote = false, mirrorBaseUrl = '' } = {}) {
    const catalog = this.wasm.getCatalog();
    const [{ models: remoteModels, error: remoteError }, localDevMap, db] = await Promise.all([
      this.fetchRemoteManifest({ refresh: refreshRemote, mirrorBaseUrl }),
      this.probeLocalDevModels(),
      this.db(),
    ]);

    const installedRecords = db ? await idbGetAll(db, STORE_MODELS).catch(() => []) : [];
    const installedByName = new Map();
    let storageUsedBytes = 0;
    for (const rec of installedRecords) {
      // Do not keep heavy ArrayBuffers in the list response!
      const bytes = (rec.binBytes ? rec.binBytes.byteLength : 0) + (rec.paramText ? rec.paramText.length : 0);
      storageUsedBytes += bytes;
      installedByName.set(rec.name, { ...rec, byteSize: bytes });
    }

    const remoteByName = new Map();
    for (const rm of remoteModels) {
      remoteByName.set(rm.name, rm);
    }

    const seen = new Set();
    const out = [];

    for (const c of catalog) {
      seen.add(c.name);
      const rm = remoteByName.get(c.name);
      const inst = installedByName.get(c.name);
      const localDev = localDevMap.get(c.name);
      out.push({
        name: c.name,
        shortName: c.short_name || c.name,
        family: c.family || '独立模型',
        scale: (inst && inst.scale) || (rm && rm.scale) || c.scale || 2,
        scene: c.scene || '',
        recommended: Boolean(c.recommended),
        sizeBytes: (inst && inst.byteSize)
          || (rm && rm.size_bytes)
          || (localDev && localDev.sizeBytes)
          || 0,
        sha256: (rm && rm.sha256) || (inst && inst.sha256) || '',
        url: `${getSourceBaseUrl(mirrorBaseUrl)}/${c.name}.zip`,
        installed: Boolean(inst || localDev),
        cachedInIdb: Boolean(inst),
        source: inst ? (inst.source || 'remote') : (localDev ? 'builtin' : 'remote'),
      });
    }

    // Include any custom user-imported models not in the published catalog.
    for (const rec of installedRecords) {
      if (seen.has(rec.name)) continue;
      const classified = this.wasm.classifyModel(rec.name) || {};
      const bytes = (rec.binBytes ? rec.binBytes.byteLength : 0) + (rec.paramText ? rec.paramText.length : 0);
      out.push({
        name: rec.name,
        shortName: rec.shortName || classified.short_name || rec.name,
        family: rec.family || '本地导入',
        scale: rec.scale || classified.scale || 2,
        scene: rec.scene || classified.scene || '自定义',
        recommended: false,
        sizeBytes: bytes,
        sha256: rec.sha256 || '',
        url: '',
        installed: true,
        cachedInIdb: true,
        source: 'imported',
      });
    }

    return {
      models: out,
      remoteError: remoteError || undefined,
      storageUsedBytes,
    };
  }

  /**
   * Retrieve `{ name, paramText, binBytes: Uint8Array }` from memory LRU, IndexedDB, or dev local-models.
   */
  async getModelPayload(name) {
    if (this.payloadCache.has(name)) {
      return this.rememberPayload(this.payloadCache.get(name));
    }

    const db = await this.db();
    if (db) {
      const rec = await idbGet(db, STORE_MODELS, name);
      if (rec && rec.paramText && rec.binBytes) {
        return this.rememberPayload({
          name: rec.name,
          paramText: rec.paramText,
          binBytes: rec.binBytes instanceof Uint8Array ? rec.binBytes : new Uint8Array(rec.binBytes),
        });
      }
    }

    const localDevMap = await this.probeLocalDevModels();
    if (localDevMap.has(name)) {
      const [pResp, bResp] = await Promise.all([
        fetch(`/api/local-models/${encodeURIComponent(name)}.param`),
        fetch(`/api/local-models/${encodeURIComponent(name)}.bin`),
      ]);
      if (pResp.ok && bResp.ok) {
        const [paramText, binAb] = await Promise.all([pResp.text(), bResp.arrayBuffer()]);
        return this.rememberPayload({
          name,
          paramText,
          binBytes: new Uint8Array(binAb),
        });
      }
    }

    throw new Error(`模型 "${name}" 未下载，需先下载或导入`);
  }

  async saveVerifiedPair(name, paramText, binBytes, { source = 'remote', sha256 = '' } = {}) {
    // Validate via Rust ncnn parser before persisting!
    const probe = this.wasm.loadModel(name, paramText, binBytes);
    const scale = probe.meta.scale_factor || 2;
    probe.dispose();

    const classified = this.wasm.classifyModel(name) || {};
    const binBuffer = binBytes.byteOffset === 0 && binBytes.byteLength === binBytes.buffer.byteLength
      ? binBytes.buffer
      : binBytes.slice().buffer;

    this.rememberPayload({
      name,
      paramText,
      binBytes: new Uint8Array(binBuffer),
    });

    const record = {
      name,
      shortName: classified.short_name || name,
      family: source === 'imported' && !classified.scene ? '本地导入' : (classified.family || ''),
      scale,
      scene: classified.scene || (source === 'imported' ? '自定义' : ''),
      recommended: Boolean(classified.recommended),
      paramText,
      binBytes: binBuffer,
      sizeBytes: binBuffer.byteLength + paramText.length,
      sha256,
      source,
      installedAt: Date.now(),
    };

    const db = await this.db();
    if (!db) {
      throw new Error('IndexedDB 不可用，无法保存模型缓存');
    }
    await idbPut(db, STORE_MODELS, record);

    return {
      name: record.name,
      shortName: record.shortName,
      family: record.family,
      scale: record.scale,
      scene: record.scene,
      recommended: record.recommended,
      sizeBytes: record.sizeBytes,
      sha256: record.sha256,
      url: `${HF_RELEASE_BASE}/${name}.zip`,
      installed: true,
      cachedInIdb: true,
      source: record.source,
    };
  }

  async downloadModel(name, { mirrorBaseUrl = '', signal, onProgress } = {}) {
    if (!name || /[/\\]/.test(name)) {
      throw new Error('模型名称无效');
    }

    // Fast path in local dev if `~/.local/share/upix/models` already has the model!
    const localDevMap = await this.probeLocalDevModels();
    if (localDevMap.has(name) && !mirrorBaseUrl) {
      if (onProgress) onProgress({ loaded: 0, total: localDevMap.get(name).sizeBytes || 0, phase: 'download' });
      const payload = await this.getModelPayload(name);
      if (onProgress) onProgress({ loaded: payload.binBytes.byteLength, total: payload.binBytes.byteLength, phase: 'verify' });
      return this.saveVerifiedPair(name, payload.paramText, payload.binBytes, { source: 'remote' });
    }

    const { models: remoteModels } = await this.fetchRemoteManifest({ mirrorBaseUrl });
    const rm = remoteModels.find((m) => m.name === name);
    const expectedSha = rm && rm.sha256 ? rm.sha256.toLowerCase() : '';
    const expectedSize = (rm && rm.size_bytes) || 0;
    const urls = buildCandidateUrls(`${name}.zip`, mirrorBaseUrl);
    let zipBytes = null;
    let lastError = '下载失败';

    for (const url of urls) {
      if (signal && signal.aborted) {
        throw new DOMException('已取消', 'AbortError');
      }
      try {
        zipBytes = await fetchStreamWithProgress(url, {
          signal,
          onProgress,
          expectedTotal: expectedSize,
        });
        break;
      } catch (err) {
        if (err && err.name === 'AbortError') throw err;
        lastError = err instanceof Error ? err.message : String(err);
      }
    }

    if (!zipBytes) {
      const sourcePrefix = resolveWebModelSource(mirrorBaseUrl) === 'hf' ? 'Hugging Face 无法连接：' : '';
      throw new Error(`模型 ${name} 下载失败：${sourcePrefix}${lastError}（可切换镜像源或导入 .zip/.param+.bin）`);
    }

    if (onProgress) {
      onProgress({ loaded: zipBytes.byteLength, total: zipBytes.byteLength, phase: 'verify' });
    }

    const actualSha = await computeSha256Hex(zipBytes, this.wasm);
    if (expectedSha && actualSha.toLowerCase() !== expectedSha) {
      throw new Error(`模型包 SHA-256 校验失败：预期 ${expectedSha.slice(0, 12)}…，实际 ${actualSha.slice(0, 12)}…`);
    }

    if (onProgress) {
      onProgress({ loaded: zipBytes.byteLength, total: zipBytes.byteLength, phase: 'verify' });
    }

    const entries = await extractZipEntries(zipBytes);
    const paramBytes = entries.get(`${name}.param`)
      || [...entries.entries()].find(([k]) => k.endsWith('.param'))?.[1];
    const binBytes = entries.get(`${name}.bin`)
      || [...entries.entries()].find(([k]) => k.endsWith('.bin'))?.[1];

    if (!paramBytes || !binBytes) {
      throw new Error(`模型压缩包缺少 ${name}.param 或 ${name}.bin`);
    }

    const paramText = new TextDecoder('utf-8').decode(paramBytes);
    return this.saveVerifiedPair(name, paramText, binBytes, {
      source: 'remote',
      sha256: actualSha,
    });
  }

  /**
   * Import user-selected `.zip` archives or `.param` + `.bin` file pairs.
   * @param {FileList | File[]} fileListOrArray
   */
  async importModelFiles(fileListOrArray) {
    const files = Array.from(fileListOrArray || []);
    if (files.length === 0) return [];

    const imported = [];
    const paramByStem = new Map();
    const binByStem = new Map();

    for (const file of files) {
      const fname = file.name || '';
      const lower = fname.toLowerCase();
      if (lower.endsWith('.zip')) {
        const ab = await file.arrayBuffer();
        const zipBytes = new Uint8Array(ab);
        const sha = await computeSha256Hex(zipBytes, this.wasm);
        const entries = await extractZipEntries(zipBytes);
        const zipParams = new Map();
        const zipBins = new Map();
        for (const [entryName, data] of entries.entries()) {
          if (entryName.toLowerCase().endsWith('.param')) {
            zipParams.set(entryName.slice(0, -6), data);
          } else if (entryName.toLowerCase().endsWith('.bin')) {
            zipBins.set(entryName.slice(0, -4), data);
          }
        }
        for (const [stem, pBytes] of zipParams.entries()) {
          const bBytes = zipBins.get(stem);
          if (bBytes) {
            const pText = new TextDecoder('utf-8').decode(pBytes);
            const item = await this.saveVerifiedPair(stem, pText, bBytes, {
              source: 'imported',
              sha256: sha,
            });
            imported.push(item);
          }
        }
      } else if (lower.endsWith('.param')) {
        const stem = fname.slice(0, -6);
        paramByStem.set(stem, await file.text());
      } else if (lower.endsWith('.bin')) {
        const stem = fname.slice(0, -4);
        binByStem.set(stem, new Uint8Array(await file.arrayBuffer()));
      }
    }

    for (const [stem, paramText] of paramByStem.entries()) {
      const binBytes = binByStem.get(stem);
      if (!binBytes) {
        throw new Error(`找到 ${stem}.param，但缺少 ${stem}.bin`);
      }
      const sha = await computeSha256Hex(binBytes, this.wasm);
      const item = await this.saveVerifiedPair(stem, paramText, binBytes, {
        source: 'imported',
        sha256: sha,
      });
      imported.push(item);
    }

    if (imported.length === 0) {
      throw new Error('未识别到 ncnn 模型文件（需同名 .param + .bin 或包含两者的 .zip）');
    }
    return imported;
  }

  async deleteModel(name) {
    this.payloadCache.delete(name);
    const db = await this.db();
    if (db) {
      await idbDelete(db, STORE_MODELS, name);
    }
  }

  async clearCache() {
    this.payloadCache.clear();
    const db = await this.db();
    if (db) {
      await idbClear(db, STORE_MODELS);
    }
  }
}
