// Inspector: flush Swiss parameter sections (model, compute backend, output,
// collapsible advanced controls) and the docked primary action bar.

import { h, text } from './dom.js';
import { icon } from './icons.js';
import {
  OUTPUT_FORMAT_LABELS, formatBytes, getOutputFormats, inferInputFormat, isFollowOutputFormat, normalizeOutputFormat,
} from './format.js';
import {
  accordion, progressBar, rangeRow, recBadge, scaleBadge, sceneBadge, section, segmented, selectControl, stackField, switchRow,
} from './controls.js';
import { mountModelPicker } from './modelPicker.js';
import { t } from '../i18n/index.js';

export function mountInspector({ host, store, actions }) {
  host.classList.add('inspector');
  const picker = mountModelPicker({ store, actions });
  const set = actions.setSettings;

  // ── 1. Model ─────────────────────────────────────────────────────────────
  const modelState = h('span', { class: 'sec-meta mono' });
  const modelDl = h('button', { type: 'button', class: 'link-btn', hidden: true });
  const modelHeadEnd = h('span', { class: 'sec-end' }, modelState, modelDl);

  const modelName = h('span', { class: 'model-name mono truncate' });
  const modelTags = h('span', { class: 'model-tags' });
  const modelTrigger = h('button', {
    type: 'button', class: 'model-trigger', 'aria-haspopup': 'dialog',
    title: t('inspector.selectModelTitle'),
    onclick: () => picker.open(),
  },
  h('span', { class: 'model-main' }, modelName, modelTags),
  icon('chevron-down', 14, 'model-chevron'));
  const modelBar = progressBar(t('common.downloadProgress'));

  // ── 2. Output ────────────────────────────────────────────────────────────
  // Backend switching lives in the topbar capsule sheet; the workbench keeps
  // only per-run parameters.
  const format = segmented({
    label: t('inspector.outputFormat'), fill: true, size: 'md', value: store.get().settings.outputFormat,
    options: getOutputFormats(),
    onChange: v => set({ outputFormat: v }),
  });
  const followHint = h('p', { class: 'field-hint', hidden: true });
  const quality = rangeRow({
    label: t('inspector.quality'), min: 1, max: 100, value: store.get().settings.quality,
    onChange: v => set({ quality: v }),
  });
  const lossless = switchRow({
    label: t('inspector.lossless'), checked: store.get().settings.webpLossless,
    onChange: v => set({ webpLossless: v }),
  });
  const subsamplingSeg = segmented({
    label: t('inspector.jpegChromaLabel'), fill: true, size: 'sm', value: store.get().settings.jpegSubsampling,
    options: [{ value: '420', label: '4:2:0' }, { value: '422', label: '4:2:2' }, { value: '444', label: '4:4:4' }],
    onChange: v => set({ jpegSubsampling: v }),
  });
  const subsampling = stackField(t('inspector.chromaSubsampling'), subsamplingSeg.el);

  const pngSelect = selectControl({
    label: t('inspector.pngLevelLabel'),
    value: store.get().settings.pngLevel,
    options: [
      { value: 'auto', label: t('inspector.pngAuto') },
      { value: 'fast', label: t('inspector.pngFast') },
      { value: 'best', label: t('inspector.pngBest') },
    ],
    onChange: v => set({ pngLevel: v }),
  });
  const pngLevel = stackField(t('inspector.compressionLevel'), pngSelect.el, null, t('inspector.lossless'));

  // ── 3. Advanced (collapsed by default) ──────────────────────────────────
  const tileSelect = selectControl({
    label: t('inspector.tileSize'),
    value: String(store.get().settings.tileSize),
    options: [
      { value: 'auto', label: t('common.auto') },
      ...[128, 256, 384, 512].map(v => ({ value: String(v), label: `${v} px` })),
    ],
    onChange: v => set({ tileSize: v }),
  });
  const overlapSeg = segmented({
    label: t('inspector.overlap'), fill: true, size: 'sm', value: String(store.get().settings.tileOverlap),
    options: [8, 16, 32].map(v => ({ value: String(v), label: `${v}px` })),
    onChange: v => set({ tileOverlap: Number(v) }),
  });
  const threadsSelect = selectControl({
    label: t('inspector.cpuThreads'), value: String(store.get().settings.wasmThreads), options: [],
    onChange: v => set({ wasmThreads: v }),
  });
  const tileField = stackField(t('inspector.tileSize'), tileSelect.el);
  const overlapField = stackField(t('inspector.overlap'), overlapSeg.el);
  const threadsField = stackField(t('inspector.cpuThreads'), threadsSelect.el);
  const fp16 = switchRow({
    label: t('inspector.preferFp16'), checked: store.get().settings.preferFp16,
    onChange: v => set({ preferFp16: v }),
  });
  const force = switchRow({
    label: t('inspector.forceUpscale'), hint: t('inspector.forceHint'),
    checked: store.get().settings.force, onChange: v => set({ force: v }),
  });
  const advSection = accordion(t('inspector.secAdvanced'),
    tileField,
    overlapField,
    threadsField,
    fp16.el,
    force.el);

  // ── CTA Dock ─────────────────────────────────────────────────────────────
  const ctaLabel = h('span', { class: 'cta-label' });
  // Blocker / download status line under the button, wired via aria-describedby.
  const ctaHint = h('span', { class: 'cta-hint', id: 'cta-hint', hidden: true });
  let ctaMode = 'run'; // 'run' | 'cancel' | 'save'
  const cta = h('button', {
    type: 'button', class: 'btn btn-cta', 'aria-describedby': 'cta-hint',
    onclick: () => {
      if (ctaMode === 'cancel') { actions.cancelRun(); return; }
      if (ctaMode === 'save') { actions.downloadItem(store.get().activeId); return; }
      if (cta.getAttribute('aria-disabled') !== 'true') actions.startProcessing();
    },
  }, ctaLabel);
  const saveBtn = h('button', {
    type: 'button', class: 'btn btn-secondary', hidden: true,
    onclick: () => actions.downloadItem(store.get().activeId),
  }, t('inspector.saveImage'));
  const zipBtn = h('button', {
    type: 'button', class: 'btn btn-secondary', hidden: true,
    onclick: () => actions.downloadZip(),
  }, t('inspector.zipAll'));
  const dock = h('div', { class: 'cta-dock' }, cta, saveBtn, zipBtn, ctaHint);

  const modelSec = section(t('inspector.secModel'), modelHeadEnd, modelTrigger, modelBar.el);
  const outputSec = section(t('inspector.secOutput'), null, format.el, followHint, quality.el, lossless.el, subsampling, pngLevel);
  const scroll = h('div', { class: 'inspector-scroll' }, modelSec, outputSec, advSection.el);
  host.append(scroll, dock);

  // Global Ctrl/⌘ + K opens model picker when in workbench
  window.addEventListener('keydown', event => {
    if ((event.ctrlKey || event.metaKey) && !event.altKey && !event.shiftKey && event.key.toLowerCase() === 'k') {
      if (store.get().tab === 'workbench') {
        event.preventDefault();
        picker.open();
      }
    }
  });

  // ── subscriptions ────────────────────────────────────────────────────────
  const modelOf = s => s.models.find(m => m.name === s.settings.modelName);

  store.subscribe(['locale'], () => {
    modelTrigger.title = t('inspector.selectModelTitle');
    modelBar.setLabel(t('common.downloadProgress'));
    text(modelSec.querySelector('.sec-title'), t('inspector.secModel'));
    text(outputSec.querySelector('.sec-title'), t('inspector.secOutput'));
    format.setLabel(t('inspector.outputFormat'));
    format.setOptions(getOutputFormats());
    quality.setLabel(t('inspector.quality'));
    lossless.setText({ label: t('inspector.lossless') });
    subsamplingSeg.setLabel(t('inspector.jpegChromaLabel'));
    text(subsampling.querySelector('.field-label'), t('inspector.chromaSubsampling'));
    pngSelect.setLabel(t('inspector.pngLevelLabel'));
    pngSelect.setOptions([
      { value: 'auto', label: t('inspector.pngAuto') },
      { value: 'fast', label: t('inspector.pngFast') },
      { value: 'best', label: t('inspector.pngBest') },
    ]);
    text(pngLevel.querySelector('.field-label'), t('inspector.compressionLevel'));
    text(pngLevel.querySelector('.field-meta'), t('inspector.lossless'));
    tileSelect.setLabel(t('inspector.tileSize'));
    tileSelect.setOptions([
      { value: 'auto', label: t('common.auto') },
      ...[128, 256, 384, 512].map(v => ({ value: String(v), label: `${v} px` })),
    ]);
    text(tileField.querySelector('.field-label'), t('inspector.tileSize'));
    overlapSeg.setLabel(t('inspector.overlap'));
    text(overlapField.querySelector('.field-label'), t('inspector.overlap'));
    threadsSelect.setLabel(t('inspector.cpuThreads'));
    text(threadsField.querySelector('.field-label'), t('inspector.cpuThreads'));
    fp16.setText({ label: t('inspector.preferFp16') });
    force.setText({ label: t('inspector.forceUpscale'), hint: t('inspector.forceHint') });
    advSection.setTitle(t('inspector.secAdvanced'));
    text(saveBtn, t('inspector.saveImage'));
  });

  store.subscribe(['models', 'settings', 'downloads', 'modelsLoading', 'locale'], s => {
    const m = modelOf(s);
    const dl = m && s.downloads[m.name];
    text(modelName, m ? (m.shortName || m.name) : s.modelsLoading ? t('common.loading') : t('inspector.selectModel'));
    modelName.title = m?.name ?? '';
    modelTags.replaceChildren(...(m ? [scaleBadge(m.scale), sceneBadge(m.scene), ...(m.recommended ? [recBadge()] : [])] : []));
    modelDl.hidden = !m || m.installed;
    modelBar.el.hidden = !(dl && !dl.error);
    if (!m) {
      text(modelState, '');
      return;
    }
    if (dl && !dl.error) {
      const pct = dl.total ? (dl.loaded / dl.total) * 100 : 0;
      modelBar.set(pct);
      text(modelState, dl.phase === 'verify' ? t('inspector.verifying') : `${Math.round(pct)}%`);
      text(modelDl, t('common.cancel'));
      modelDl.onclick = () => actions.cancelDownload(m.name);
    } else if (m.installed) {
      text(modelState, '');
    } else {
      text(modelState, dl?.error ? t('inspector.downloadFailed') : formatBytes(m.sizeBytes));
      text(modelDl, dl?.error ? t('common.retry') : t('common.download'));
      modelDl.onclick = () => actions.downloadModel(m.name);
    }
  });

  store.subscribe(['caps', 'capsLoading', 'settings', 'locale'], ({ caps, settings }) => {
    const hc = navigator.hardwareConcurrency ?? 4;
    const maxThreads = Math.max(1, caps?.wasm?.features?.threads ?? hc);
    const autoThreads = caps?.wasm?.features?.autoThreads;
    threadsField.hidden = settings.backend !== 'wasm';
    threadsSelect.setOptions([
      { value: 'auto', label: autoThreads ? t('inspector.threadsAuto', { threads: autoThreads }) : t('common.auto') },
      ...[1, 2, 4, 8, 12, 16].filter(n => n <= maxThreads).map(n => ({
        value: String(n),
        label: t('inspector.threadsCount', { threads: n }),
      })),
    ]);
    threadsSelect.set(String(settings.wasmThreads));
    // A manual count saved on a stronger device can exceed this one's system
    // max; snap it back to auto instead of showing an unselectable value.
    if (settings.backend === 'wasm' && settings.wasmThreads !== 'auto'
      && Number(settings.wasmThreads) > maxThreads) set({ wasmThreads: 'auto' });
    const hasFp16 = !!caps?.webgpu?.features?.fp16;
    fp16.el.hidden = !(settings.backend === 'webgpu' && hasFp16);

    const tileText = settings.tileSize === 'auto'
      ? t('inspector.summaryTileAuto')
      : t('inspector.summaryTilePx', { size: settings.tileSize });
    const overlapText = t('inspector.summaryOverlapPx', { overlap: settings.tileOverlap });
    const summaryParts = [tileText, overlapText];
    if (settings.backend === 'wasm') {
      const n = settings.wasmThreads === 'auto' ? (autoThreads ?? maxThreads) : settings.wasmThreads;
      summaryParts.push(t('inspector.threadsCount', { threads: Number(n) }));
    } else if (settings.backend === 'webgpu' && hasFp16) {
      summaryParts.push(settings.preferFp16 ? 'FP16' : 'FP32');
    }
    if (settings.force) summaryParts.push(t('inspector.summaryForce'));
    advSection.setSummary(summaryParts.join(' · '));
  });

  store.subscribe(['settings', 'queue', 'activeId', 'locale'], ({ settings: s, queue, activeId }) => {
    format.set(s.outputFormat);
    const follow = isFollowOutputFormat(s.outputFormat);
    const active = queue.find(i => i.id === activeId) ?? null;
    const srcFmt = follow ? inferInputFormat(active) : '';
    const effectiveFmt = follow
      ? (srcFmt ? normalizeOutputFormat(srcFmt) : '')
      : normalizeOutputFormat(s.outputFormat);

    followHint.hidden = !follow;
    if (follow) {
      if (!active) {
        text(followHint, t('inspector.followInput'));
      } else if (srcFmt === 'avif') {
        text(followHint, queue.length > 1 ? t('inspector.followAvifBatch') : t('inspector.followAvifSingle'));
      } else if (effectiveFmt) {
        const label = OUTPUT_FORMAT_LABELS[effectiveFmt] ?? effectiveFmt.toUpperCase();
        text(followHint, queue.length > 1
          ? t('inspector.followBatch', { format: label })
          : t('inspector.followSingle', { format: label }));
      } else {
        text(followHint, t('inspector.followInput'));
      }
    }

    const lossy = effectiveFmt === 'jpeg' || (effectiveFmt === 'webp' && !s.webpLossless);
    quality.el.hidden = !lossy;
    quality.set(s.quality);
    lossless.el.hidden = effectiveFmt !== 'webp';
    lossless.set(s.webpLossless);
    subsampling.hidden = effectiveFmt !== 'jpeg';
    subsamplingSeg.set(s.jpegSubsampling);
    pngLevel.hidden = effectiveFmt !== 'png';
    pngSelect.set(s.pngLevel);
    tileSelect.set(String(s.tileSize));
    overlapSeg.set(String(s.tileOverlap));
    fp16.set(s.preferFp16);
    force.set(s.force);
  });

  store.subscribe(['queue', 'models', 'settings', 'caps', 'running', 'downloads', 'activeId', 'locale'], s => {
    const m = modelOf(s);
    const dl = m && s.downloads[m.name];
    const downloading = !!dl && !dl.error;
    const doneCount = s.queue.filter(i => i.status === 'done').length;
    const pending = s.queue.filter(i => i.status !== 'done' && i.status !== 'probing');
    const allDone = s.queue.length > 0 && pending.length === 0;

    // While a run is active the dock is the cancel control.
    if (s.running) {
      ctaMode = 'cancel';
      cta.classList.add('is-cancel');
      cta.classList.remove('is-disabled');
      cta.removeAttribute('aria-disabled');
      cta.title = t('inspector.cancelTitle');
      text(ctaLabel, t('common.cancel'));
      ctaHint.hidden = true;
      saveBtn.hidden = true;
      zipBtn.hidden = true;
      return;
    }
    cta.classList.remove('is-cancel');

    // Batch complete: the next intent is export, not another run.
    if (allDone) {
      ctaMode = 'save';
      cta.classList.remove('is-disabled');
      cta.removeAttribute('aria-disabled');
      cta.title = t('inspector.saveTitle', { name: s.queue.find(i => i.id === s.activeId)?.name ?? '' });
      text(ctaLabel, t('inspector.saveImage'));
      ctaHint.hidden = true;
      saveBtn.hidden = true;
      zipBtn.hidden = doneCount < 2;
      if (!zipBtn.hidden) {
        zipBtn.title = t('inspector.zipTitle', { count: doneCount });
        text(zipBtn, t('inspector.zipAllCount', { count: doneCount }));
      }
      return;
    }

    // Otherwise the dock is the run control.
    ctaMode = 'run';
    zipBtn.hidden = true;
    const backend = s.caps?.[s.settings.backend];
    const blocked = backend && !backend.available;
    const resuming = doneCount > 0 && pending.length > 0;
    const action = resuming ? t('inspector.ctaResume') : t('inspector.ctaStart');
    const buttonText = downloading ? (dl.phase === 'verify' ? t('inspector.ctaVerifyingModel') : t('inspector.ctaDownloadingModel'))
      : !s.queue.length ? t('inspector.ctaAddImages')
        : !m ? t('inspector.ctaSelectModel')
          : blocked ? t('inspector.ctaBackendUnavailable') : action;
    const hint = !downloading && s.queue.length && m
      ? (blocked ? t('inspector.hintSwitchBackend') : !m.installed ? t('inspector.hintDownloadModel') : '')
      : '';
    const locked = downloading || !s.queue.length || !m || blocked;
    cta.classList.toggle('is-disabled', locked);
    if (locked) cta.setAttribute('aria-disabled', 'true');
    else cta.removeAttribute('aria-disabled');
    cta.title = '';
    text(ctaHint, hint);
    ctaHint.hidden = !hint;
    if (!locked && pending.length > 1) cta.setAttribute('aria-label', t('inspector.ctaPendingAria', { action, count: pending.length }));
    else cta.removeAttribute('aria-label');
    text(ctaLabel, buttonText);
    // Mid-batch export: a finished active image can be saved at any time.
    const active = s.queue.find(i => i.id === s.activeId);
    saveBtn.hidden = !(doneCount > 0 && active?.status === 'done' && !downloading);
  });
}
