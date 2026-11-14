// Controller: every state transition and every runtime call lives here, so the
// views stay declarative (read store, call an action).

import { downloadBlob, isAbort } from './ui/dom.js';
import { benchRows, errorMessage, getBackends, isImageFile, isModelFile, stripExt } from './ui/format.js';
import { createSampleImage } from './sample.js';
import {
  clearWorkspaceItems, deleteWorkspaceItems, loadWorkspaceItems, putWorkspaceItem,
} from './runtime/workspace-store.js';
import { formatNumber, t } from './i18n/index.js';

export function createActions({ runtime, store, toast }) {
  const downloadAborts = new Map();
  let runAbort = null;
  let nextId = 1;
  let progressFrame = 0;
  let pendingProgress = null;

  const get = () => store.get();
  const setSettings = patch => store.set(s => ({ settings: { ...s.settings, ...patch } }));
  const patchItem = (id, patch) => store.set(s => ({
    queue: s.queue.map(item => (item.id === id ? { ...item, ...patch } : item)),
  }));
  const findItem = id => get().queue.find(item => item.id === id);
  const findModel = name => get().models.find(m => m.name === name);

  // ── workspace persistence (IndexedDB) ────────────────────────────────
  // Queue items survive refreshes; a record is dropped the moment its output is
  // downloaded, so the next refresh destroys exactly the already-saved images.
  // All writes run through one serialized chain to keep put/delete ordering.
  let wsChain = Promise.resolve();
  const wsEnqueue = (op) => { wsChain = wsChain.then(op).catch(() => { /* storage blocked */ }); };

  // Strips objectURL/bytes duplicates; keeps the Blob + result metadata.
  function snapshotResult(result) {
    const rest = { ...result };
    delete rest.url;
    delete rest.bytes;
    return rest;
  }

  function snapshotItem(item) {
    return {
      id: item.id,
      name: item.name,
      file: item.file,
      probe: item.probe ?? null,
      marker: item.marker ?? null,
      force: !!item.force,
      // An interrupted run restores as queued.
      status: item.status === 'processing' ? 'queued' : item.status,
      error: item.error ?? '',
      result: item.result ? snapshotResult(item.result) : null,
    };
  }

  const persistItem = (id) => {
    const item = findItem(id);
    if (item) wsEnqueue(() => putWorkspaceItem(snapshotItem(item)));
  };
  const forgetItems = ids => wsEnqueue(() => deleteWorkspaceItems(ids));

  async function restoreWorkspace() {
    let records;
    try {
      records = await loadWorkspaceItems();
    } catch {
      return; // persistence unavailable: start with a fresh workspace
    }
    if (!records.length) return;
    const items = records.map(rec => ({
      ...rec,
      status: rec.status === 'processing' ? 'queued' : rec.status,
      url: URL.createObjectURL(rec.file),
      result: rec.result
        ? { ...rec.result, url: rec.result.blob ? URL.createObjectURL(rec.result.blob) : undefined }
        : null,
    }));
    nextId = Math.max(1, ...records.map(r => Number(r.id) + 1));
    store.set(s => ({
      queue: [...items, ...s.queue],
      activeId: s.activeId ?? items[0]?.id ?? null,
    }));
    // Records persisted before their probe finished: re-probe now.
    await Promise.all(items.filter(i => !i.probe && i.status === 'queued').map(async (item) => {
      try {
        const probe = await runtime.probeImage(item.file);
        patchItem(item.id, { probe, marker: probe.srMarker ?? null, status: 'queued' });
      } catch (error) {
        // Transient boot failure: patch the UI but keep the record queued on disk.
        patchItem(item.id, { status: 'failed', error: errorMessage(error) });
        return;
      }
      persistItem(item.id);
    }));
  }

  function reconcileBackend() {
    const { caps, settings } = get();
    if (!caps) return;
    const current = caps[settings.backend];
    // Unavailable backend (e.g. persisted WebNN on a machine without it): fall back.
    if (current?.available) return;
    const fallback = ['webgpu', 'wasm'].find(id => caps[id]?.available);
    if (fallback) setSettings({ backend: fallback });
  }

  function reconcileModel() {
    const { models, settings } = get();
    if (!models.length || models.some(m => m.name === settings.modelName)) return;
    const fastDefaults = ['Real-ESRGANv3-anime-x2', 'Real-ESRGANv3-general-x4', 'AnimeJaNai-v2-Compact-x2'];
    const pick = fastDefaults.map(n => models.find(m => m.name === n && m.installed)).find(Boolean)
      ?? models.find(m => m.installed && m.recommended)
      ?? models.find(m => m.installed)
      ?? fastDefaults.map(n => models.find(m => m.name === n)).find(Boolean)
      ?? models.find(m => m.recommended)
      ?? models[0];
    setSettings({ modelName: pick.name });
  }

  async function refreshCaps({ silent = false } = {}) {
    if (!silent) store.set({ capsLoading: true });
    try {
      const caps = await runtime.getCapabilities();
      store.set({ caps, capsLoading: false });
      reconcileBackend();
    } catch (error) {
      store.set({ capsLoading: false, bootError: errorMessage(error) });
    }
  }

  async function refreshModels({ remote = false } = {}) {
    store.set({ modelsLoading: true });
    try {
      const mirrorBaseUrl = get().mirrorBaseUrl || undefined;
      const result = await runtime.listModels({ refreshRemote: remote, mirrorBaseUrl });
      store.set({
        models: result.models,
        storageUsed: result.storageUsedBytes ?? 0,
        remoteError: result.remoteError ?? '',
        remoteErrorKind: result.remoteErrorKind ?? '',
        modelsLoading: false,
      });
      reconcileModel();
    } catch (error) {
      store.set({ modelsLoading: false, remoteError: errorMessage(error), remoteErrorKind: 'remote' });
    }
  }

  async function init() {
    try {
      await runtime.init();
    } catch (error) {
      store.set({ bootError: errorMessage(error) });
      toast(t('actions.initFailed', { error: errorMessage(error) }), 'error');
    }
    await restoreWorkspace();
    await Promise.all([refreshCaps(), refreshModels()]);
  }

  // ── queue ────────────────────────────────────────────────────────────────
  async function addFiles(fileList) {
    const files = [...fileList];
    const images = files.filter(isImageFile);
    const models = files.filter(f => !isImageFile(f) && isModelFile(f));
    const ignored = files.length - images.length - models.length;
    if (ignored > 0) toast(t('actions.ignoredFiles', { count: ignored }), 'warn');
    if (models.length) await importModels(models);
    if (!images.length) return;

    const items = images.map(file => ({
      id: nextId++,
      file,
      name: file.name,
      url: URL.createObjectURL(file),
      probe: null,
      marker: null,
      force: false,
      status: 'probing',
      error: '',
      result: null,
    }));
    store.set(s => ({ queue: [...s.queue, ...items], activeId: items[0].id }));
    items.forEach(persistItem);

    await Promise.all(items.map(async item => {
      try {
        const probe = await runtime.probeImage(item.file);
        patchItem(item.id, { probe, marker: probe.srMarker ?? null, status: 'queued' });
        persistItem(item.id);
      } catch (error) {
        patchItem(item.id, { status: 'failed', error: errorMessage(error) });
        persistItem(item.id);
      }
    }));
  }

  function revoke(item) {
    URL.revokeObjectURL(item.url);
    if (item.result?.url) URL.revokeObjectURL(item.result.url);
  }

  function removeItem(id) {
    const item = findItem(id);
    if (!item) return;
    if (item.status === 'processing') cancelRun();
    revoke(item);
    forgetItems([id]);
    store.set(s => {
      const queue = s.queue.filter(i => i.id !== id);
      const activeId = s.activeId === id ? (queue[0]?.id ?? null) : s.activeId;
      return { queue, activeId };
    });
  }

  function clearQueue() {
    if (get().running) cancelRun();
    get().queue.forEach(revoke);
    wsEnqueue(() => clearWorkspaceItems());
    store.set({ queue: [], activeId: null, progress: null });
  }

  const selectItem = id => store.set({ activeId: id });

  function setItemForce(id, force) {
    const item = findItem(id);
    if (!item) return;
    patchItem(id, { force, ...(force && item.status === 'skipped' ? { status: 'queued', result: null } : {}) });
    persistItem(id);
  }

  // ── processing ───────────────────────────────────────────────────────────
  function pushProgress(p) {
    pendingProgress = p;
    if (progressFrame) return;
    progressFrame = requestAnimationFrame(() => {
      progressFrame = 0;
      store.set(s => ({ progress: { ...s.progress, ...pendingProgress } }));
    });
  }

  function buildOptions(item, signal) {
    const s = get().settings;
    const options = {
      modelName: s.modelName,
      backend: s.backend,
      webnnDevice: s.webnnDevice,
      webnnPower: s.webnnPower,
      tileOverlap: s.tileOverlap,
      preferFp16: s.preferFp16,
      outputFormat: s.outputFormat,
      quality: s.quality,
      jpegSubsampling: s.jpegSubsampling,
      pngLevel: s.pngLevel,
      webpLossless: s.webpLossless,
      force: s.force || item.force,
      signal,
    };
    if (s.tileSize !== 'auto') options.tileSize = Number(s.tileSize);
    if (s.wasmThreads !== 'auto') options.wasmThreads = Number(s.wasmThreads);
    return options;
  }

  async function ensureModel() {
    const { settings } = get();
    const model = findModel(settings.modelName);
    if (!model) { toast(t('actions.noModelSelected'), 'warn'); return false; }
    if (model.installed) return true;
    return downloadModel(model.name);
  }

  async function processItems(items) {
    const controller = new AbortController();
    runAbort = controller;
    store.set({ running: true, progress: null });
    let done = 0;
    let skipped = 0;
    let failed = 0;
    const started = performance.now();
    try {
      for (const [index, item] of items.entries()) {
        if (controller.signal.aborted) break;
        const live = findItem(item.id);
        if (!live) continue;
        if (live.result?.url) URL.revokeObjectURL(live.result.url);
        patchItem(item.id, { status: 'processing', error: '', result: null });
        persistItem(item.id);
        store.set({ activeId: item.id, progress: {
          itemId: item.id, phase: 'decode', completedTiles: 0, totalTiles: 0, percentage: 0,
          queueIndex: index, queueTotal: items.length,
        } });
        try {
          const res = await runtime.processImage(live.file, {
            ...buildOptions(live, controller.signal),
            batchSizeHint: items.length,
            onProgress: p => pushProgress({ ...p, itemId: item.id, queueIndex: index, queueTotal: items.length }),
          });
          if (!findItem(item.id)) continue;
          if (res.skipped) {
            skipped++;
            patchItem(item.id, { status: 'skipped', result: { ...res }, marker: res.existingMarker ?? live.marker });
            persistItem(item.id);
          } else {
            done++;
            const blob = res.blob ?? new Blob([res.bytes], { type: res.mimeType });
            patchItem(item.id, { status: 'done', result: { ...res, blob, url: URL.createObjectURL(blob) } });
            persistItem(item.id);
          }
        } catch (error) {
          if (isAbort(error) || controller.signal.aborted) {
            patchItem(item.id, { status: 'queued', error: '' });
            persistItem(item.id);
            break;
          }
          failed++;
          patchItem(item.id, { status: 'failed', error: errorMessage(error) });
          persistItem(item.id);
        }
      }
    } finally {
      cancelAnimationFrame(progressFrame);
      progressFrame = 0;
      runAbort = null;
      store.set({ running: false, progress: null });
    }
    if (controller.signal.aborted) { toast(t('common.cancelled'), 'info'); return; }
    const seconds = formatNumber((performance.now() - started) / 1000, {
      minimumFractionDigits: 1,
      maximumFractionDigits: 1,
    });
    const parts = [t('actions.batchDone', { count: done })];
    if (skipped) parts.push(t('actions.batchSkipped', { count: skipped }));
    if (failed) parts.push(t('actions.batchFailed', { count: failed }));
    toast(t('actions.batchSummary', { parts: parts.join(' · '), seconds }), failed ? 'warn' : 'success');
  }

  async function startProcessing() {
    const { queue, running, caps, settings } = get();
    if (running || !queue.length) return;
    if (settings.backend === 'auto') {
      if (!caps?.webgpu?.available && !caps?.webnn?.available && !caps?.wasm?.available) {
        toast(t('actions.noBackendAvailable'), 'error');
        return;
      }
    } else {
      const backend = caps?.[settings.backend];
      if (!backend?.available) {
        const label = getBackends()[settings.backend]?.label ?? settings.backend;
        toast(t('actions.backendUnavailable', { label }), 'error');
        return;
      }
    }
    if (!(await ensureModel())) return;
    const ready = get().queue.filter(i => i.status !== 'probing');
    let targets = ready.filter(i => i.status !== 'done');
    if (!targets.length) targets = ready; // everything is done: re-run with current settings
    if (!targets.length) return;
    await processItems(targets);
  }

  function cancelRun() {
    runAbort?.abort();
  }

  // ── models ───────────────────────────────────────────────────────────────
  async function downloadModel(name) {
    if (downloadAborts.has(name)) return false;
    const controller = new AbortController();
    downloadAborts.set(name, controller);
    const total = findModel(name)?.sizeBytes ?? 0;
    const setDownload = value => store.set(s => ({ downloads: { ...s.downloads, [name]: value } }));
    setDownload({ loaded: 0, total, phase: 'download' });
    try {
      await runtime.downloadModel(name, {
        mirrorBaseUrl: get().mirrorBaseUrl || undefined,
        signal: controller.signal,
        onProgress: p => setDownload({ loaded: p.loaded, total: p.total || total, phase: p.phase }),
      });
      store.set(s => {
        const downloads = { ...s.downloads };
        delete downloads[name];
        return { downloads };
      });
      await refreshModels();
      return true;
    } catch (error) {
      if (isAbort(error)) {
        store.set(s => {
          const downloads = { ...s.downloads };
          delete downloads[name];
          return { downloads };
        });
      } else {
        setDownload({ error: errorMessage(error) });
        toast(t('actions.downloadFailed', { error: errorMessage(error) }), 'error');
      }
      return false;
    } finally {
      downloadAborts.delete(name);
    }
  }

  const cancelDownload = name => downloadAborts.get(name)?.abort();

  function dismissDownloadError(name) {
    store.set(s => {
      const downloads = { ...s.downloads };
      delete downloads[name];
      return { downloads };
    });
  }

  async function deleteModel(name) {
    try {
      await runtime.deleteModel(name);
      await refreshModels();
    } catch (error) { toast(t('actions.deleteFailed', { error: errorMessage(error) }), 'error'); }
  }

  async function clearCache() {
    try {
      await runtime.clearModelCache();
      await refreshModels();
      toast(t('actions.cacheCleared'), 'info');
    } catch (error) { toast(t('actions.clearFailed', { error: errorMessage(error) }), 'error'); }
  }

  async function importModels(files) {
    const list = [...files].filter(isModelFile);
    if (!list.length) { toast(t('actions.needModelFiles'), 'warn'); return; }
    try {
      const items = await runtime.importModelFiles(list);
      await refreshModels();
      toast(t('actions.importedModels', { count: items.length }), 'success');
      if (items[0]) setSettings({ modelName: items[0].name });
    } catch (error) { toast(t('actions.importFailed', { error: errorMessage(error) }), 'error'); }
  }

  const selectModel = name => setSettings({ modelName: name });

  function setMirror(value) {
    store.set({ mirrorBaseUrl: value.trim() });
  }

  // ── exports ──────────────────────────────────────────────────────────────
  function downloadItem(id) {
    const item = findItem(id);
    if (!item?.result?.blob) return;
    downloadBlob(item.result.blob, item.result.filename || `${stripExt(item.name)}-upix.png`);
    // Saved: the persisted record is dropped, so a refresh destroys this image.
    forgetItems([id]);
  }

  async function downloadZip() {
    const done = get().queue.filter(i => i.status === 'done' && i.result?.blob);
    if (!done.length) return;
    const used = new Set();
    const entries = done.map(item => {
      let name = item.result.filename || `${stripExt(item.name)}-upix.png`;
      for (let n = 2; used.has(name); n++) name = name.replace(/(\.[^.]+)?$/, `-${n}$1`);
      used.add(name);
      return { name, bytes: item.result.blob };
    });
    try {
      const zip = await runtime.buildZipArchive(entries);
      downloadBlob(zip, 'upix-batch.zip');
      forgetItems(done.map(item => item.id));
    } catch (error) { toast(t('actions.zipFailed', { error: errorMessage(error) }), 'error'); }
  }

  const loadSample = async () => addFiles([await createSampleImage()]);

  // ── lab ──────────────────────────────────────────────────────────────────
  async function probeWebNn(device) {
    const setProbe = value => store.set(s => ({ lab: { ...s.lab, webnn: { ...s.lab.webnn, [device]: value } } }));
    setProbe({ status: 'probing' });
    try {
      const res = await runtime.probeWebNnDevice(device);
      setProbe({ status: res.available ? 'ok' : 'fail', reason: res.reason });
    } catch (error) {
      setProbe({ status: 'fail', reason: errorMessage(error) });
    }
  }

  const probeAllWebNn = () => Promise.all(['gpu', 'npu', 'cpu'].map(probeWebNn));

  async function runBenchmark(tileSize) {
    const { caps, settings, lab } = get();
    if (lab.bench?.running || !caps) return;
    const defs = benchRows(caps, lab);
    const runnable = r => r.available || r.probing;
    const rows = Object.fromEntries(defs.map(r => [
      r.id,
      runnable(r) ? { status: 'pending' } : { status: 'off', reason: r.reason },
    ]));
    const setBench = patch => store.set(s => ({ lab: { ...s.lab, bench: { ...s.lab.bench, ...patch } } }));
    store.set(s => ({ lab: { ...s.lab, bench: { running: true, tileSize, rows } } }));
    for (const def of defs.filter(runnable)) {
      const setRow = row => setBench({ rows: { ...get().lab.bench.rows, [def.id]: row } });
      setRow({ status: 'running' });
      try {
        const isNn = def.id.startsWith('webnn:');
        const res = await runtime.benchmark({
          backend: isNn ? 'webnn' : def.id,
          webnnDevice: isNn ? def.id.slice('webnn:'.length) : settings.webnnDevice,
          modelName: settings.modelName || undefined,
          tileSize,
          webnnPower: settings.webnnPower,
          preferFp16: settings.preferFp16,
          wasmThreads: settings.wasmThreads === 'auto' ? 0 : Number(settings.wasmThreads),
        });
        setRow({ status: 'done', ...res });
      } catch (error) {
        setRow({ status: 'error', reason: errorMessage(error) });
      }
    }
    setBench({ running: false });
  }

  function dispose() {
    runAbort?.abort();
    downloadAborts.forEach(c => c.abort());
    get().queue.forEach(revoke);
    runtime.dispose();
  }

  return {
    init, refreshCaps, refreshModels, setSettings, addFiles, removeItem, clearQueue, selectItem, setItemForce,
    startProcessing, cancelRun, downloadModel, cancelDownload, dismissDownloadError, deleteModel, clearCache,
    importModels, selectModel, setMirror, downloadItem, downloadZip, loadSample, probeWebNn, probeAllWebNn,
    runBenchmark, dispose,
  };
}
