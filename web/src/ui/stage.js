// Main stage: Swiss optical reticle empty state, or recessed light-table well
// with precision hairline curtain, synchronized zoom + pan, flush SR-marker
// notice, and deterministic processing telemetry.

import { clamp, h, text } from './dom.js';
import { icon } from './icons.js';
import { BACKENDS, formatDims, formatNum } from './format.js';
import { progressBar } from './controls.js';

export function pickImages(actions) {
  const input = h('input', {
    type: 'file', multiple: true,
    accept: 'image/png,image/jpeg,image/webp,image/avif,.png,.jpg,.jpeg,.webp,.avif',
    onchange: () => { if (input.files?.length) actions.addFiles(input.files); },
  });
  input.click();
}

const PHASES = [
  [/decode|解码/, '解码'],
  [/load|model|加载/, '加载模型'],
  [/compile|pipeline|编译/, '编译管线'],
  [/blend|merge|stitch|fuse|融合/, '融合'],
  [/encode|write|编码/, '编码'],
];

export function phaseLabel(p) {
  const phase = String(p?.phase ?? '').toLowerCase();
  if (/infer|tile|run|推理/.test(phase)) {
    return p.totalTiles ? `推理 ${p.completedTiles}/${p.totalTiles}` : '推理';
  }
  return PHASES.find(([re]) => re.test(phase))?.[1] ?? (p?.phase || '准备中');
}

const ZOOMS = [
  { id: 'fit', label: '适应' },
  { id: 1, label: '1:1' },
  { id: 2, label: '2:1' },
  { id: 4, label: '4:1' },
];

export function mountStage({ host, dropTarget, store, actions }) {
  host.classList.add('stage');
  host.dataset.state = 'empty';

  // ── empty state: explicit rounded dropzone ───────────────────────────────
  const empty = h('div', { class: 'stage-empty' },
    h('div', {
      class: 'dropzone',
      onclick: e => { if (!e.target.closest('button')) pickImages(actions); },
    },
    h('div', { class: 'dz-icon', 'aria-hidden': 'true' }, icon('upload', 20)),
    h('h2', { class: 'dz-title' }, '拖放或粘贴图像'),
    h('div', { class: 'dz-actions' },
      h('button', { type: 'button', class: 'btn btn-primary', onclick: () => pickImages(actions) }, '选择图像'),
      h('button', { type: 'button', class: 'btn btn-secondary', onclick: () => actions.loadSample() }, '示例图')),
    h('p', { class: 'dz-hint' }, 'PNG · JPEG · WebP · AVIF')));

  // ── image well ───────────────────────────────────────────────────────────
  const imgBefore = h('img', { class: 'img', alt: '原图', draggable: false, decoding: 'async' });
  const imgAfter = h('img', { class: 'img', alt: '超分结果', draggable: false, decoding: 'async' });
  const canvasBefore = h('div', { class: 'canvas' }, imgBefore);
  const canvasAfter = h('div', { class: 'canvas' }, imgAfter);
  const layerAfter = h('div', { class: 'layer layer-after' }, canvasAfter);
  const layerBefore = h('div', { class: 'layer layer-before' }, canvasBefore);

  const handle = h('div', {
    class: 'curtain-handle', role: 'slider', tabindex: '0', 'aria-label': '前后对比位置',
    'aria-valuemin': '0', 'aria-valuemax': '100', 'aria-valuenow': '50',
    'aria-description': '方向键移动，Home / End 到两端',
  }, h('i', { class: 'curtain-index', 'aria-hidden': 'true' }));
  const curtain = h('div', { class: 'curtain' },
    h('i', { class: 'curtain-line' }), h('i', { class: 'curtain-hit' }), handle);
  const tagBefore = h('span', { class: 'stage-tag tag-before mono' });
  const tagAfter = h('span', { class: 'stage-tag tag-after mono' });
  const viewport = h('div', { class: 'viewport' }, layerAfter, layerBefore, curtain, tagBefore, tagAfter);

  const zoomReadout = h('span', { class: 'zoom-readout mono', 'aria-live': 'off' }, '100%');
  const zoomButtons = ZOOMS.map(z => h('button', {
    type: 'button', class: 'zoom-btn', dataset: { zoom: String(z.id) }, 'aria-pressed': 'false',
    onclick: () => (z.id === 'fit' ? zoomFit() : zoomTo(z.id / dpr())),
  }, z.label));
  const pixelBtn = h('button', {
    type: 'button', class: 'zoom-btn', 'aria-pressed': 'false', title: '像素渲染',
    onclick: () => actions.setSettings({ pixelated: !store.get().settings.pixelated }),
  }, '像素');
  // Zoom controls float at the bottom edge of the well — the thumb zone on
  // phones, editor-style on desktop.
  const zoomGroup = h('div', { class: 'zoom-group zoom-capsule', role: 'group', 'aria-label': '缩放比例' },
    ...zoomButtons, pixelBtn, zoomReadout);

  // Flush SR marker strip (never obscures image)
  const noticeText = h('span', { class: 'notice-text' });
  const noticeBtn = h('button', { type: 'button', class: 'link-btn' });
  const notice = h('div', { class: 'notice-strip', role: 'status', hidden: true }, noticeText, noticeBtn);

  // Telemetry docked cleanly at bottom of well
  const teleBar = progressBar('处理进度');
  const telePhase = h('span', { class: 'tele-phase' });
  const telePct = h('span', { class: 'tele-pct mono' });
  const teleStats = h('p', { class: 'tele-stats mono' });
  const teleQueue = h('span', { class: 'tele-queue mono', hidden: true });
  const telemetry = h('div', { class: 'telemetry', hidden: true },
    h('div', { class: 'tele-head' },
      h('span', { class: 'tele-title' }, teleQueue, telePhase),
      telePct),
    teleBar.el,
    teleStats);

  const failText = h('p', { class: 'fail-text mono' });
  const failure = h('div', { class: 'failure', role: 'alert', hidden: true },
    h('p', { class: 'fail-title' }, '处理失败'),
    failText,
    h('div', { class: 'fail-actions' },
      h('button', { type: 'button', class: 'btn btn-secondary btn-sm', onclick: () => actions.startProcessing() }, '重试'),
      h('button', {
        type: 'button', class: 'btn btn-quiet btn-sm',
        onclick: () => store.get().activeId != null && actions.removeItem(store.get().activeId),
      }, '移除')));

  const well = h('div', { class: 'well' }, viewport, zoomGroup, telemetry, failure);
  const view = h('div', { class: 'stage-view' }, notice, well);
  const veil = h('div', { class: 'drop-veil', 'aria-hidden': 'true' }, h('span', {}, '松开添加'));
  host.append(empty, view, veil);

  // ── view state (zoom / pan / curtain) ────────────────────────────────────
  let shown = { id: null, before: '', after: '' };
  let base = null;
  let mode = 'fit';
  let scale = 1;
  let tx = 0;
  let ty = 0;
  let fit = 1;
  let split = 0.5;
  let hasResult = false;
  let drag = null;
  const dpr = () => window.devicePixelRatio || 1;

  function activeItem() {
    const { queue, activeId } = store.get();
    return queue.find(i => i.id === activeId) ?? null;
  }

  function baseSize(item) {
    const r = item?.result;
    if (r && !r.skipped && r.url) return { w: r.outputWidth, h: r.outputHeight };
    if (item?.probe) return { w: item.probe.width, h: item.probe.height };
    if (imgBefore.naturalWidth) return { w: imgBefore.naturalWidth, h: imgBefore.naturalHeight };
    return null;
  }

  const isZoomed = () => !!base && (base.w * scale > viewport.clientWidth + 1 || base.h * scale > viewport.clientHeight + 1);

  function clampPan() {
    const vw = viewport.clientWidth;
    const vh = viewport.clientHeight;
    const w = base.w * scale;
    const hh = base.h * scale;
    tx = w <= vw ? (vw - w) / 2 : clamp(tx, vw - w, 0);
    ty = hh <= vh ? (vh - hh) / 2 : clamp(ty, vh - hh, 0);
  }

  function apply() {
    if (!base) return;
    const d = dpr();
    const x = Math.round(tx * d) / d;
    const y = Math.round(ty * d) / d;
    for (const c of [canvasBefore, canvasAfter]) {
      c.style.width = `${base.w}px`;
      c.style.height = `${base.h}px`;
      c.style.transform = `translate(${x}px, ${y}px) scale(${scale})`;
    }
    const pct = scale * d * 100;
    text(zoomReadout, `${pct >= 100 ? Math.round(pct) : pct.toFixed(1)}%`);
    for (const b of zoomButtons) {
      const z = b.dataset.zoom;
      const on = z === 'fit' ? mode === 'fit' : mode === 'free' && Math.abs(scale * d - Number(z)) < 0.004;
      b.setAttribute('aria-pressed', String(on));
    }
    viewport.dataset.zoomed = String(isZoomed());
  }

  function layout() {
    if (!base) return;
    const vw = viewport.clientWidth;
    const vh = viewport.clientHeight;
    if (!vw || !vh) return;
    const pad = 24;
    fit = Math.min((vw - pad * 2) / base.w, (vh - pad * 2) / base.h);
    if (!(fit > 0)) fit = Math.min(vw / base.w, vh / base.h);
    if (mode === 'fit') scale = fit;
    else scale = clamp(scale, Math.min(fit, 1 / dpr()), 32);
    if (mode === 'fit') { tx = (vw - base.w * scale) / 2; ty = (vh - base.h * scale) / 2; } else clampPan();
    apply();
  }

  function zoomFit() { mode = 'fit'; layout(); }

  function zoomTo(target, cx = viewport.clientWidth / 2, cy = viewport.clientHeight / 2) {
    if (!base) return;
    const next = clamp(target, Math.min(fit, 1 / dpr()), 32);
    tx = cx - (cx - tx) * (next / scale);
    ty = cy - (cy - ty) * (next / scale);
    scale = next;
    mode = 'free';
    clampPan();
    apply();
  }

  function setSplit(p) {
    split = clamp(p, 0, 1);
    viewport.style.setProperty('--split', String(split));
    handle.setAttribute('aria-valuenow', String(Math.round(split * 100)));
    handle.setAttribute('aria-valuetext', `原图 ${Math.round(split * 100)}%，结果 ${Math.round((1 - split) * 100)}%`);
  }

  new ResizeObserver(layout).observe(viewport);
  imgBefore.addEventListener('load', () => { base = baseSize(activeItem()); layout(); });

  viewport.addEventListener('pointerdown', event => {
    if (!base || (event.pointerType === 'mouse' && event.button !== 0)) return;
    const onCurtain = !!event.target.closest('.curtain-hit, .curtain-handle');
    if (hasResult && (onCurtain || !isZoomed())) drag = { kind: 'curtain' };
    else if (isZoomed()) drag = { kind: 'pan', x: event.clientX, y: event.clientY, tx, ty };
    else return;
    viewport.setPointerCapture(event.pointerId);
    viewport.dataset.dragging = drag.kind;
    if (drag.kind === 'curtain') {
      handle.focus({ preventScroll: true });
      setSplit((event.clientX - viewport.getBoundingClientRect().left) / viewport.clientWidth);
    }
    event.preventDefault();
  });
  viewport.addEventListener('pointermove', event => {
    if (!drag) return;
    if (drag.kind === 'curtain') {
      setSplit((event.clientX - viewport.getBoundingClientRect().left) / viewport.clientWidth);
    } else {
      tx = drag.tx + (event.clientX - drag.x);
      ty = drag.ty + (event.clientY - drag.y);
      mode = 'free';
      clampPan();
      apply();
    }
  });
  const endDrag = () => { drag = null; delete viewport.dataset.dragging; };
  viewport.addEventListener('pointerup', endDrag);
  viewport.addEventListener('pointercancel', endDrag);
  viewport.addEventListener('lostpointercapture', endDrag);

  viewport.addEventListener('wheel', event => {
    if (!base) return;
    event.preventDefault();
    const rect = viewport.getBoundingClientRect();
    const k = event.ctrlKey ? 0.012 : 0.0016;
    zoomTo(scale * Math.exp(-event.deltaY * k), event.clientX - rect.left, event.clientY - rect.top);
  }, { passive: false });

  handle.addEventListener('keydown', event => {
    const step = event.shiftKey ? 0.1 : 0.02;
    const next = {
      ArrowLeft: split - step, ArrowRight: split + step, ArrowDown: split - step, ArrowUp: split + step, Home: 0, End: 1,
    }[event.key];
    if (next == null) return;
    event.preventDefault();
    setSplit(next);
  });

  view.addEventListener('keydown', event => {
    if (event.target.closest('input, textarea, select') || event.ctrlKey || event.metaKey || event.altKey) return;
    const key = event.key;
    if (key === '+' || key === '=') zoomTo(scale * 1.25);
    else if (key === '-' || key === '_') zoomTo(scale / 1.25);
    else if (key === '0') zoomFit();
    else if (key === '1') zoomTo(1 / dpr());
    else return;
    event.preventDefault();
  });

  // ── render from store ────────────────────────────────────────────────────
  function render() {
    const { settings } = store.get();
    const item = activeItem();
    host.dataset.state = item ? 'image' : 'empty';
    pixelBtn.setAttribute('aria-pressed', String(settings.pixelated));
    viewport.dataset.pixelated = String(settings.pixelated);
    if (!item) { shown = { id: null, before: '', after: '' }; return; }

    const result = item.result && !item.result.skipped && item.result.url ? item.result : null;
    const afterUrl = result?.url ?? '';
    const sameItem = shown.id === item.id;
    const prevAfter = shown.after;
    if (shown.id !== item.id) {
      mode = 'fit';
      setSplit(0.5);
    } else if (shown.after !== afterUrl) {
      mode = 'fit';
    }
    if (shown.before !== item.url) imgBefore.src = item.url;
    if (shown.after !== afterUrl) { if (afterUrl) imgAfter.src = afterUrl; else imgAfter.removeAttribute('src'); }
    shown = { id: item.id, before: item.url, after: afterUrl };

    hasResult = !!result;
    well.dataset.result = String(hasResult);
    viewport.dataset.result = String(hasResult);
    curtain.hidden = !hasResult;
    tagBefore.hidden = !hasResult;
    tagAfter.hidden = !hasResult;
    layerAfter.hidden = !hasResult;
    if (hasResult) {
      text(tagBefore, `原图 ${formatDims(result.inputWidth, result.inputHeight)}`);
      text(tagAfter, `${result.scale}× ${formatDims(result.outputWidth, result.outputHeight)}`);
    }

    base = baseSize(item);
    // Collapse dead letterbox space before layout() reads container metrics.
    if (base) well.style.setProperty('--stage-ar', `${base.w} / ${base.h}`);
    layout();

    // The SR payoff is detail, not composition: on phones a fresh result snaps
    // to 1:1 so the improvement is actually visible.
    if (hasResult && sameItem && prevAfter !== afterUrl && afterUrl && matchMedia('(max-width: 860px)').matches) {
      zoomTo(1 / dpr());
    }

    // SR marker notice
    const marker = item.marker;
    const showNotice = !!marker && item.status !== 'done' && item.status !== 'processing';
    notice.hidden = !showNotice;
    if (showNotice) {
      const forced = item.force || settings.force;
      const model = marker.model ? `${marker.model}${marker.scale ? ` ${marker.scale}×` : ''}` : `${marker.scale}×`;
      text(noticeText, `已含超分标记 · ${model} · ${forced ? '将重新超分' : item.status === 'skipped' ? '已跳过' : '默认跳过'}`);
      noticeBtn.hidden = settings.force && !item.force;
      noticeBtn.textContent = item.force ? '撤销' : '重新超分';
      noticeBtn.onclick = () => actions.setItemForce(item.id, !item.force);
    }

    // failure
    failure.hidden = item.status !== 'failed';
    if (item.status === 'failed') text(failText, item.error || '未知错误');

    well.dataset.busy = String(item.status === 'processing');
  }

  function renderTelemetry() {
    const { progress } = store.get();
    const item = activeItem();
    const busy = item?.status === 'processing';
    telemetry.hidden = !busy;
    if (!busy) return;
    const p = progress?.itemId === item.id ? progress : null;
    teleBar.set(p?.percentage ?? 0);
    text(telePhase, p ? phaseLabel(p) : '准备中');
    text(telePct, `${Math.round(p?.percentage ?? 0)}%`);
    const showQueue = (p?.queueTotal ?? 0) > 1;
    teleQueue.hidden = !showQueue;
    if (showQueue) text(teleQueue, `${p.queueIndex + 1}/${p.queueTotal}`);
    const backend = BACKENDS[p?.backendUsed ?? store.get().settings.backend];
    const parts = [];
    if (p?.totalTiles) parts.push(`${p.completedTiles}/${p.totalTiles} 块`);
    if (p?.tileMs) parts.push(`${formatNum(p.tileMs)} ms/块`, `${formatNum(p.tilesPerSec)} 块/s`);
    parts.push(backend.short);
    if (p?.tileSizeUsed) parts.push(`${p.tileSizeUsed}px`);
    text(teleStats, parts.join(' · '));
  }

  store.subscribe(['queue', 'activeId', 'settings'], render);
  store.subscribe(['queue', 'activeId', 'progress', 'settings'], renderTelemetry);
  setSplit(0.5);

  // ── drag & drop (whole workbench is the target) ──────────────────────────
  let depth = 0;
  const hasFiles = e => [...(e.dataTransfer?.types ?? [])].includes('Files');
  dropTarget.addEventListener('dragenter', e => { if (hasFiles(e)) { depth++; host.dataset.drag = 'true'; } });
  dropTarget.addEventListener('dragover', e => { if (hasFiles(e)) { e.preventDefault(); e.dataTransfer.dropEffect = 'copy'; } });
  dropTarget.addEventListener('dragleave', e => { if (hasFiles(e) && --depth <= 0) { depth = 0; delete host.dataset.drag; } });
  dropTarget.addEventListener('drop', e => {
    if (!hasFiles(e)) return;
    e.preventDefault();
    depth = 0;
    delete host.dataset.drag;
    actions.addFiles(e.dataTransfer.files);
  });
}
