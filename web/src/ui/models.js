// Models view: Swiss tabular catalog with inline storage readout, collapsible
// mirror bar, underline scene filters, and family-grouped index rows.

import { h, reconcile, text } from './dom.js';
import { icon } from './icons.js';
import {
  downloadProgress, formatBytes, groupModelsByFamily, isModelFile, matchesModelQuery, sceneKey,
} from './format.js';
import { confirmButton, progressBar, recBadge, scaleBadge, sceneBadge, segmented } from './controls.js';
import { resolveWebModelSource } from '../runtime/model-store.js';

const FILTERS = [
  { value: 'all', label: '全部' },
  { value: 'rec', label: '推荐' },
  { value: 'anime', label: '动漫' },
  { value: 'photo', label: '照片' },
  { value: 'general', label: '综合' },
  { value: 'restore', label: '修复' },
];

const SOURCE_OPTIONS = [
  { value: 'hf-mirror', label: 'HF-Mirror' },
  { value: 'hf', label: 'Hugging Face' },
  {
    value: 'github',
    label: 'GitHub',
    disabled: true,
    title: 'GitHub Release 缺少浏览器跨域 (CORS) 头，Web 端已锁定，请使用 HF-Mirror 或 Hugging Face',
  },
];

export function mountModels({ root, store, actions }) {
  root.classList.add('page');
  let filter = 'all';
  let query = '';
  let quota = 0;

  // ── Header & actions ─────────────────────────────────────────────────────
  const storageMeta = h('p', { class: 'page-sub mono' });
  const importInput = h('input', {
    type: 'file', multiple: true, accept: '.param,.bin,.zip', class: 'sr-only', tabindex: '-1',
    onchange: () => { if (importInput.files?.length) actions.importModels(importInput.files); importInput.value = ''; },
  });
  const importBtn = h('button', {
    type: 'button', class: 'btn btn-secondary btn-sm', onclick: () => importInput.click(),
  }, '导入');
  const mirrorToggle = h('button', {
    type: 'button', class: 'btn btn-quiet btn-sm', 'aria-expanded': 'false',
    onclick: () => {
      mirrorPanel.hidden = !mirrorPanel.hidden;
      mirrorToggle.setAttribute('aria-expanded', String(!mirrorPanel.hidden));
    },
  }, '镜像源');
  const clearBtn = confirmButton({
    label: '清空缓存', confirmLabel: '确认清空', className: 'btn btn-danger-quiet btn-sm',
    iconName: null, onConfirm: () => actions.clearCache(),
  });

  // ── Collapsible mirror bar ───────────────────────────────────────────────
  const initialSource = resolveWebModelSource(store.get().mirrorBaseUrl);
  const sourceSeg = segmented({
    label: '模型下载源',
    size: 'sm',
    value: initialSource,
    options: SOURCE_OPTIONS,
    onChange: (v) => {
      actions.setMirror(v);
      actions.refreshModels({ remote: true });
    },
  });
  const mirrorPanel = h('div', { class: 'mirror-panel', hidden: true },
    h('div', { class: 'mirror-controls' },
      h('span', { class: 'field-label' }, '下载源'),
      sourceSeg.el));

  // ── Filters + Search on one shared rule ──────────────────────────────────
  const filterSeg = segmented({
    label: '场景筛选', kind: 'tabs', value: filter, options: FILTERS, className: 'filter-tabs',
    onChange: v => { filter = v; render(); },
  });
  const search = h('input', {
    type: 'search', class: 'search-line', placeholder: '搜索…', 'aria-label': '搜索模型',
    oninput: () => { query = search.value.trim().toLowerCase(); render(); },
  });
  const toolbar = h('div', { class: 'models-toolbar' },
    h('div', { class: 'filter-scroll' }, filterSeg.el),
    h('div', { class: 'search-wrap' }, icon('search', 14), search));

  const remoteNote = h('p', { class: 'inline-note', role: 'status', hidden: true });
  const list = h('div', { class: 'model-list' });
  const empty = h('p', { class: 'empty-row muted', hidden: true }, '无匹配模型');
  const veil = h('div', { class: 'drop-veil', 'aria-hidden': 'true' }, h('span', {}, '松开导入'));

  root.append(
    h('header', { class: 'page-head' },
      h('div', { class: 'page-head-row' },
        h('div', { class: 'page-titles' },
          h('h1', {}, '模型库'),
          storageMeta),
        h('div', { class: 'page-actions' }, importBtn, mirrorToggle, clearBtn)),
      importInput),
    mirrorPanel,
    toolbar,
    remoteNote,
    list,
    empty,
    veil);

  // ── Tabular rows ─────────────────────────────────────────────────────────
  const cache = new Map();

  function createItem(node) {
    if (node.kind === 'group') {
      const title = h('span', { class: 'group-title' });
      const count = h('span', { class: 'group-count mono' });
      return {
        el: h('h2', { class: 'group-head' }, title, count),
        update(n) { text(title, n.family); text(count, `${n.count}`); },
      };
    }
    let model = null;
    const dot = h('i', { class: 'row-dot', 'aria-hidden': 'true' });
    const name = h('span', { class: 'row-name mono' });
    const extraTags = h('span', { class: 'row-tags' });
    const metaCol = h('span', { class: 'row-spec' });
    const size = h('span', { class: 'row-size mono' });
    const source = h('span', { class: 'tag-meta' });
    const select = h('button', {
      type: 'button', class: 'row-select',
      onclick: () => model && actions.selectModel(model.name),
    },
    dot,
    h('div', { class: 'row-main' }, name, extraTags),
    metaCol,
    size);
    const pctEl = h('span', { class: 'row-progress mono', hidden: true });
    const cancelBtn = h('button', {
      type: 'button', class: 'icon-btn icon-btn-sm', title: '取消', hidden: true,
      onclick: () => model && actions.cancelDownload(model.name),
    }, icon('x', 14));
    const errEl = h('span', { class: 'row-error', hidden: true }, '失败');
    const downloadBtn = h('button', {
      type: 'button', class: 'icon-btn icon-btn-accent', hidden: true,
      onclick: () => model && actions.downloadModel(model.name),
    }, icon('download', 14));
    const action = h('div', { class: 'row-action' }, pctEl, cancelBtn, errEl, downloadBtn);
    let deleteBtn = null;
    const bar = progressBar('下载进度');
    const el = h('div', { class: 'model-row' }, select, action, bar.el);
    let lastSig = '';
    return {
      el,
      update(n) {
        model = n.model;
        const s = store.get();
        const dlState = downloadProgress(s.downloads[model.name]);
        const current = s.settings.modelName === model.name;
        bar.el.hidden = !dlState.busy;
        if (dlState.busy) {
          bar.set(dlState.pct);
          text(pctEl, dlState.label);
        }
        const sig = [model.name, model.installed, model.source, model.scale, model.recommended, current, dlState.busy, dlState.error].join('|');
        if (sig === lastSig) return;
        lastSig = sig;
        text(name, model.name);
        select.setAttribute('aria-pressed', String(current));
        select.setAttribute('aria-label', `选择模型 ${model.name}`);
        extraTags.replaceChildren(
          ...(model.recommended ? [recBadge()] : []),
          ...(model.installed && model.source === 'imported' ? [(text(source, '导入'), source)] : []));
        metaCol.replaceChildren(scaleBadge(model.scale), sceneBadge(model.scene));
        text(size, formatBytes(model.sizeBytes));
        el.classList.toggle('is-current', current);
        pctEl.hidden = !dlState.busy;
        cancelBtn.hidden = !dlState.busy;
        cancelBtn.setAttribute('aria-label', `取消下载 ${model.name}`);
        if (model.installed && !dlState.busy) {
          if (!deleteBtn) {
            deleteBtn = confirmButton({
              label: `删除 ${model.name}`, confirmLabel: '删除', iconName: 'trash', className: 'icon-btn icon-btn-danger',
              onConfirm: () => model && actions.deleteModel(model.name),
            });
            action.append(deleteBtn);
          }
          deleteBtn.hidden = false;
        } else if (deleteBtn) {
          deleteBtn.hidden = true;
        }
        errEl.hidden = dlState.busy || model.installed || !dlState.error;
        if (dlState.error) errEl.title = dlState.error;
        downloadBtn.hidden = dlState.busy || model.installed;
        downloadBtn.title = dlState.error ? '重试' : '下载';
        downloadBtn.setAttribute('aria-label', dlState.error ? `重试下载 ${model.name}` : `下载 ${model.name}`);
      },
    };
  }

  function render() {
    const { models } = store.get();
    const match = m => {
      if (filter === 'rec' && !m.recommended) return false;
      if (['anime', 'photo', 'general', 'restore'].includes(filter) && sceneKey(m.scene) !== filter) return false;
      return matchesModelQuery(m, query);
    };
    const nodes = groupModelsByFamily(models.filter(match));
    empty.hidden = nodes.length > 0;
    reconcile(list, nodes, n => n.key, createItem, cache);
  }

  async function refreshQuota() {
    try { quota = (await navigator.storage?.estimate?.())?.quota ?? 0; } catch { quota = 0; }
    paintStorage();
  }

  function paintStorage() {
    const { models, storageUsed } = store.get();
    const n = models.filter(m => m.installed).length;
    const parts = [`已缓存 ${n} 个`];
    if (storageUsed) parts.push(formatBytes(storageUsed));
    if (quota) parts.push(`可用 ${formatBytes(quota)}`);
    text(storageMeta, parts.join(' · '));
    clearBtn.disabled = n === 0;
  }

  store.subscribe(['models', 'downloads', 'settings'], render);
  store.subscribe(['models', 'storageUsed'], paintStorage);
  store.subscribe(['remoteError'], ({ remoteError }) => {
    remoteNote.hidden = !remoteError;
    text(remoteNote, remoteError
      ? (remoteError.includes('Hugging Face 无法连接') ? `${remoteError}，仅显示本地目录` : '远程目录不可用，仅显示本地目录')
      : '');
  });
  refreshQuota();

  // ── Drag & drop model files ──────────────────────────────────────────────
  let depth = 0;
  const hasFiles = e => [...(e.dataTransfer?.types ?? [])].includes('Files');
  root.addEventListener('dragenter', e => { if (hasFiles(e)) { depth++; root.dataset.drag = 'true'; } });
  root.addEventListener('dragover', e => { if (hasFiles(e)) { e.preventDefault(); e.dataTransfer.dropEffect = 'copy'; } });
  root.addEventListener('dragleave', e => { if (hasFiles(e) && --depth <= 0) { depth = 0; delete root.dataset.drag; } });
  root.addEventListener('drop', e => {
    if (!hasFiles(e)) return;
    e.preventDefault();
    depth = 0;
    delete root.dataset.drag;
    const files = [...e.dataTransfer.files].filter(isModelFile);
    if (files.length) actions.importModels(files);
    else actions.addFiles(e.dataTransfer.files);
  });
}
