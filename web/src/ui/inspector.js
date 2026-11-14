// Inspector: flush Swiss parameter sections (model, compute backend, output,
// collapsible advanced controls) and the docked primary action bar.

import { h, text } from './dom.js';
import { icon } from './icons.js';
import {
  OUTPUT_FORMATS, OUTPUT_FORMAT_LABELS, formatBytes, inferInputFormat, isFollowOutputFormat, normalizeOutputFormat,
} from './format.js';
import {
  accordion, progressBar, rangeRow, recBadge, scaleBadge, sceneBadge, section, segmented, selectControl, stackField, switchRow,
} from './controls.js';
import { mountModelPicker } from './modelPicker.js';

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
    title: '选择模型 (Ctrl/⌘ + K)',
    onclick: () => picker.open(),
  },
  h('span', { class: 'model-main' }, modelName, modelTags),
  icon('chevron-down', 14, 'model-chevron'));
  const modelBar = progressBar('下载进度');

  // ── 2. Output ────────────────────────────────────────────────────────────
  // Backend switching lives in the topbar capsule sheet; the workbench keeps
  // only per-run parameters.
  const format = segmented({
    label: '输出格式', fill: true, size: 'md', value: store.get().settings.outputFormat,
    options: OUTPUT_FORMATS,
    onChange: v => set({ outputFormat: v }),
  });
  const followHint = h('p', { class: 'field-hint', hidden: true });
  const quality = rangeRow({
    label: '质量', min: 1, max: 100, value: store.get().settings.quality,
    onChange: v => set({ quality: v }),
  });
  const lossless = switchRow({
    label: '无损', checked: store.get().settings.webpLossless,
    onChange: v => set({ webpLossless: v }),
  });
  const subsamplingSeg = segmented({
    label: 'JPEG 色度抽样', fill: true, size: 'sm', value: store.get().settings.jpegSubsampling,
    options: [{ value: '420', label: '4:2:0' }, { value: '422', label: '4:2:2' }, { value: '444', label: '4:4:4' }],
    onChange: v => set({ jpegSubsampling: v }),
  });
  const subsampling = stackField('色度抽样', subsamplingSeg.el);

  const pngSelect = selectControl({
    label: 'PNG 压缩等级',
    value: store.get().settings.pngLevel,
    options: [
      { value: 'auto', label: '自动' },
      { value: 'fast', label: '快速' },
      { value: 'best', label: '最小' },
    ],
    onChange: v => set({ pngLevel: v }),
  });
  const pngLevel = stackField('压缩等级', pngSelect.el, null, '无损');

  // ── 3. Advanced (collapsed by default) ──────────────────────────────────
  const tileSelect = selectControl({
    label: '分块尺寸',
    value: String(store.get().settings.tileSize),
    options: [
      { value: 'auto', label: '自动' },
      ...[128, 256, 384, 512].map(v => ({ value: String(v), label: `${v} px` })),
    ],
    onChange: v => set({ tileSize: v }),
  });
  const overlapSeg = segmented({
    label: '边缘重叠', fill: true, size: 'sm', value: String(store.get().settings.tileOverlap),
    options: [8, 16, 32].map(v => ({ value: String(v), label: `${v}px` })),
    onChange: v => set({ tileOverlap: Number(v) }),
  });
  const threadsSelect = selectControl({
    label: 'CPU 线程', value: String(store.get().settings.wasmThreads), options: [],
    onChange: v => set({ wasmThreads: v }),
  });
  const threadsField = stackField('CPU 线程', threadsSelect.el);
  const fp16 = switchRow({
    label: '优先 FP16', checked: store.get().settings.preferFp16,
    onChange: v => set({ preferFp16: v }),
  });
  const force = switchRow({
    label: '强制超分', hint: '忽略超分标记',
    checked: store.get().settings.force, onChange: v => set({ force: v }),
  });
  const advSection = accordion('高级',
    stackField('分块尺寸', tileSelect.el),
    stackField('边缘重叠', overlapSeg.el),
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
  }, '保存图像');
  const zipBtn = h('button', {
    type: 'button', class: 'btn btn-secondary', hidden: true,
    onclick: () => actions.downloadZip(),
  }, '打包全部');
  const dock = h('div', { class: 'cta-dock' }, cta, saveBtn, zipBtn, ctaHint);

  const scroll = h('div', { class: 'inspector-scroll' },
    section('模型', modelHeadEnd, modelTrigger, modelBar.el),
    section('输出', null, format.el, followHint, quality.el, lossless.el, subsampling, pngLevel),
    advSection.el);
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

  store.subscribe(['models', 'settings', 'downloads', 'modelsLoading'], s => {
    const m = modelOf(s);
    const dl = m && s.downloads[m.name];
    text(modelName, m ? (m.shortName || m.name) : s.modelsLoading ? '加载中…' : '选择模型');
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
      text(modelState, dl.phase === 'verify' ? '校验中' : `${Math.round(pct)}%`);
      text(modelDl, '取消');
      modelDl.onclick = () => actions.cancelDownload(m.name);
    } else if (m.installed) {
      text(modelState, '');
    } else {
      text(modelState, dl?.error ? '下载失败' : formatBytes(m.sizeBytes));
      text(modelDl, dl?.error ? '重试' : '下载');
      modelDl.onclick = () => actions.downloadModel(m.name);
    }
  });

  store.subscribe(['caps', 'capsLoading', 'settings'], ({ caps, settings }) => {
    const hc = navigator.hardwareConcurrency ?? 4;
    const maxThreads = Math.max(1, caps?.wasm?.features?.threads ?? hc);
    const autoThreads = caps?.wasm?.features?.autoThreads;
    threadsField.hidden = settings.backend !== 'wasm';
    threadsSelect.setOptions([
      { value: 'auto', label: autoThreads ? `自动 (${autoThreads} 线程)` : '自动' },
      ...[1, 2, 4, 8, 12, 16].filter(n => n <= maxThreads).map(n => ({ value: String(n), label: `${n} 线程` })),
    ]);
    threadsSelect.set(String(settings.wasmThreads));
    // A manual count saved on a stronger device can exceed this one's system
    // max; snap it back to auto instead of showing an unselectable value.
    if (settings.backend === 'wasm' && settings.wasmThreads !== 'auto'
      && Number(settings.wasmThreads) > maxThreads) set({ wasmThreads: 'auto' });
    const hasFp16 = !!caps?.webgpu?.features?.fp16;
    fp16.el.hidden = !(settings.backend === 'webgpu' && hasFp16);

    const tileText = settings.tileSize === 'auto' ? '自动分块' : `${settings.tileSize}px 分块`;
    const overlapText = `${settings.tileOverlap}px 重叠`;
    const summaryParts = [tileText, overlapText];
    if (settings.backend === 'wasm') {
      const n = settings.wasmThreads === 'auto' ? (autoThreads ?? maxThreads) : settings.wasmThreads;
      summaryParts.push(`${n} 线程`);
    } else if (settings.backend === 'webgpu' && hasFp16) {
      summaryParts.push(settings.preferFp16 ? 'FP16' : 'FP32');
    }
    if (settings.force) summaryParts.push('强制');
    advSection.setSummary(summaryParts.join(' · '));
  });

  store.subscribe(['settings', 'queue', 'activeId'], ({ settings: s, queue, activeId }) => {
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
        text(followHint, '跟随原图格式');
      } else if (srcFmt === 'avif') {
        text(followHint, queue.length > 1 ? 'AVIF → WebP · 批量按原图导出' : 'AVIF 转为 WebP 导出');
      } else if (effectiveFmt) {
        const label = OUTPUT_FORMAT_LABELS[effectiveFmt] ?? effectiveFmt.toUpperCase();
        text(followHint, queue.length > 1 ? `当前 ${label} · 批量按原图导出` : `导出为 ${label}`);
      } else {
        text(followHint, '跟随原图格式');
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

  store.subscribe(['queue', 'models', 'settings', 'caps', 'running', 'downloads', 'activeId'], s => {
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
      cta.title = '取消处理';
      text(ctaLabel, '取消');
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
      cta.title = `保存 ${s.queue.find(i => i.id === s.activeId)?.name ?? ''}`;
      text(ctaLabel, '保存图像');
      ctaHint.hidden = true;
      saveBtn.hidden = true;
      zipBtn.hidden = doneCount < 2;
      if (!zipBtn.hidden) {
        zipBtn.title = `打包 ${doneCount} 张图像`;
        text(zipBtn, `打包全部 (${doneCount})`);
      }
      return;
    }

    // Otherwise the dock is the run control. The label is the action —
    // 开始超分, or 继续超分 once part of the batch is already done; batch size
    // lives in the queue strip, download state and blockers in the hint line.
    ctaMode = 'run';
    zipBtn.hidden = true;
    const backend = s.caps?.[s.settings.backend];
    const blocked = backend && !backend.available;
    const resuming = doneCount > 0 && pending.length > 0;
    const action = resuming ? '继续超分' : '开始超分';
    const buttonText = downloading ? (dl.phase === 'verify' ? '校验模型…' : '下载模型…')
      : !s.queue.length ? '请先添加图像'
        : !m ? '请先选择模型'
          : blocked ? '后端不可用' : action;
    const hint = !downloading && s.queue.length && m
      ? (blocked ? '可在顶栏切换后端' : !m.installed ? '需先下载模型' : '')
      : '';
    const locked = downloading || !s.queue.length || !m || blocked;
    cta.classList.toggle('is-disabled', locked);
    if (locked) cta.setAttribute('aria-disabled', 'true');
    else cta.removeAttribute('aria-disabled');
    cta.title = '';
    text(ctaHint, hint);
    ctaHint.hidden = !hint;
    if (!locked && pending.length > 1) cta.setAttribute('aria-label', `${action}，${pending.length} 张待处理`);
    else cta.removeAttribute('aria-label');
    text(ctaLabel, buttonText);
    // Mid-batch export: a finished active image can be saved at any time.
    const active = s.queue.find(i => i.id === s.activeId);
    saveBtn.hidden = !(doneCount > 0 && active?.status === 'done' && !downloading);
  });
}
