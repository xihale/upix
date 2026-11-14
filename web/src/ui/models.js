// Models view: Swiss tabular catalog with inline storage readout, collapsible
// mirror bar, underline scene filters, and family-grouped index rows.

import { h, reconcile, text } from './dom.js';
import { icon } from './icons.js';
import {
  downloadProgress, formatBytes, groupModelsByFamily, isModelFile, matchesModelQuery, sceneKey,
} from './format.js';
import { confirmButton, progressBar, recBadge, scaleBadge, sceneBadge, segmented } from './controls.js';
import { resolveWebModelSource } from '../runtime/model-store.js';
import { getLocale, t } from '../i18n/index.js';

function getFilters() {
  return [
    { value: 'all', label: t('models.filterAll') },
    { value: 'rec', label: t('models.filterRec') },
    { value: 'anime', label: t('scenes.anime') },
    { value: 'photo', label: t('scenes.photo') },
    { value: 'general', label: t('scenes.general') },
    { value: 'restore', label: t('scenes.restore') },
  ];
}

function getSourceOptions() {
  return [
    { value: 'hf-mirror', label: 'HF-Mirror' },
    { value: 'hf', label: 'Hugging Face' },
    {
      value: 'github',
      label: 'GitHub',
      disabled: true,
      title: t('models.githubLockedTitle'),
    },
  ];
}

export function mountModels({ root, store, actions }) {
  root.classList.add('page');
  let filter = 'all';
  let query = '';
  let quota = 0;

  // ── Header & actions ─────────────────────────────────────────────────────
  const pageTitle = h('h1', {}, t('models.title'));
  const storageMeta = h('p', { class: 'page-sub mono' });
  const importInput = h('input', {
    type: 'file', multiple: true, accept: '.param,.bin,.zip', class: 'sr-only', tabindex: '-1',
    onchange: () => { if (importInput.files?.length) actions.importModels(importInput.files); importInput.value = ''; },
  });
  const importBtn = h('button', {
    type: 'button', class: 'btn btn-secondary btn-sm', onclick: () => importInput.click(),
  }, t('models.importBtn'));
  const mirrorToggle = h('button', {
    type: 'button', class: 'btn btn-quiet btn-sm', 'aria-expanded': 'false',
    onclick: () => {
      mirrorPanel.hidden = !mirrorPanel.hidden;
      mirrorToggle.setAttribute('aria-expanded', String(!mirrorPanel.hidden));
    },
  }, t('models.sourceToggle'));
  const clearBtn = confirmButton({
    label: t('models.clearCache'), confirmLabel: t('models.confirmClear'), className: 'btn btn-danger-quiet btn-sm',
    iconName: null, onConfirm: () => actions.clearCache(),
  });

  // ── Collapsible mirror bar ───────────────────────────────────────────────
  const initialSource = resolveWebModelSource(store.get().mirrorBaseUrl);
  const sourceLabel = h('span', { class: 'field-label' }, t('models.sourceLabel'));
  const sourceSeg = segmented({
    label: t('models.downloadSourceLabel'),
    size: 'sm',
    value: initialSource,
    options: getSourceOptions(),
    onChange: (v) => {
      actions.setMirror(v);
      actions.refreshModels({ remote: true });
    },
  });
  const mirrorPanel = h('div', { class: 'mirror-panel', hidden: true },
    h('div', { class: 'mirror-controls' },
      sourceLabel,
      sourceSeg.el));

  // ── Filters + Search on one shared rule ──────────────────────────────────
  const filterSeg = segmented({
    label: t('models.sceneFilter'), kind: 'tabs', value: filter, options: getFilters(), className: 'filter-tabs',
    onChange: v => { filter = v; render(); },
  });
  const search = h('input', {
    type: 'search', class: 'search-line', placeholder: t('models.searchPlaceholder'), 'aria-label': t('models.searchLabel'),
    oninput: () => { query = search.value.trim().toLowerCase(); render(); },
  });
  const toolbar = h('div', { class: 'models-toolbar' },
    h('div', { class: 'filter-scroll' }, filterSeg.el),
    h('div', { class: 'search-wrap' }, icon('search', 14), search));

  const remoteNote = h('p', { class: 'inline-note', role: 'status', hidden: true });
  const list = h('div', { class: 'model-list' });
  const empty = h('p', { class: 'empty-row muted', hidden: true }, t('models.empty'));
  const veilText = h('span', {}, t('models.dropToImport'));
  const veil = h('div', { class: 'drop-veil', 'aria-hidden': 'true' }, veilText);

  root.append(
    h('header', { class: 'page-head' },
      h('div', { class: 'page-head-row' },
        h('div', { class: 'page-titles' },
          pageTitle,
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
      type: 'button', class: 'icon-btn icon-btn-sm', title: t('common.cancel'), hidden: true,
      onclick: () => model && actions.cancelDownload(model.name),
    }, icon('x', 14));
    const errEl = h('span', { class: 'row-error', hidden: true }, t('models.failed'));
    const downloadBtn = h('button', {
      type: 'button', class: 'icon-btn icon-btn-accent', hidden: true,
      onclick: () => model && actions.downloadModel(model.name),
    }, icon('download', 14));
    const action = h('div', { class: 'row-action' }, pctEl, cancelBtn, errEl, downloadBtn);
    let deleteBtn = null;
    const bar = progressBar(t('common.downloadProgress'));
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
        const sig = [
          getLocale(), model.name, model.installed, model.source, model.scale, model.recommended, current, dlState.busy, dlState.error,
        ].join('|');
        if (sig === lastSig) return;
        lastSig = sig;
        bar.setLabel(t('common.downloadProgress'));
        text(name, model.name);
        select.setAttribute('aria-pressed', String(current));
        select.setAttribute('aria-label', t('models.selectModelAria', { name: model.name }));
        extraTags.replaceChildren(
          ...(model.recommended ? [recBadge()] : []),
          ...(model.installed && model.source === 'imported' ? [(text(source, t('models.importedBadge')), source)] : []));
        metaCol.replaceChildren(scaleBadge(model.scale), sceneBadge(model.scene));
        text(size, formatBytes(model.sizeBytes));
        el.classList.toggle('is-current', current);
        pctEl.hidden = !dlState.busy;
        cancelBtn.hidden = !dlState.busy;
        cancelBtn.title = t('common.cancel');
        cancelBtn.setAttribute('aria-label', t('models.cancelDownloadAria', { name: model.name }));
        if (model.installed && !dlState.busy) {
          if (!deleteBtn) {
            deleteBtn = confirmButton({
              label: t('models.deleteModelAria', { name: model.name }), confirmLabel: t('common.delete'),
              iconName: 'trash', className: 'icon-btn icon-btn-danger',
              onConfirm: () => model && actions.deleteModel(model.name),
            });
            action.append(deleteBtn);
          } else {
            deleteBtn.setLabels({
              label: t('models.deleteModelAria', { name: model.name }),
              confirmLabel: t('common.delete'),
            });
          }
          deleteBtn.hidden = false;
        } else if (deleteBtn) {
          deleteBtn.hidden = true;
        }
        errEl.hidden = dlState.busy || model.installed || !dlState.error;
        text(errEl, t('models.failed'));
        if (dlState.error) errEl.title = dlState.error;
        downloadBtn.hidden = dlState.busy || model.installed;
        downloadBtn.title = dlState.error ? t('common.retry') : t('common.download');
        downloadBtn.setAttribute('aria-label', dlState.error
          ? t('models.retryDownloadAria', { name: model.name })
          : t('models.downloadAria', { name: model.name }));
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
    const parts = [t('models.cachedCount', { count: n })];
    if (storageUsed) parts.push(formatBytes(storageUsed));
    if (quota) parts.push(t('models.quotaAvailable', { size: formatBytes(quota) }));
    text(storageMeta, parts.join(' · '));
    clearBtn.disabled = n === 0;
  }

  store.subscribe(['locale'], () => {
    text(pageTitle, t('models.title'));
    text(importBtn, t('models.importBtn'));
    text(mirrorToggle, t('models.sourceToggle'));
    clearBtn.setLabels({ label: t('models.clearCache'), confirmLabel: t('models.confirmClear') });
    text(sourceLabel, t('models.sourceLabel'));
    sourceSeg.setLabel(t('models.downloadSourceLabel'));
    sourceSeg.setOptions(getSourceOptions());
    filterSeg.setLabel(t('models.sceneFilter'));
    filterSeg.setOptions(getFilters());
    search.placeholder = t('models.searchPlaceholder');
    search.setAttribute('aria-label', t('models.searchLabel'));
    text(empty, t('models.empty'));
    text(veilText, t('models.dropToImport'));
  });

  store.subscribe(['models', 'downloads', 'settings', 'locale'], render);
  store.subscribe(['models', 'storageUsed', 'locale'], paintStorage);
  store.subscribe(['remoteError', 'remoteErrorKind', 'locale'], ({ remoteError, remoteErrorKind }) => {
    remoteNote.hidden = !remoteError;
    text(remoteNote, remoteError
      ? (remoteErrorKind === 'hf'
        ? t('models.remoteErrorLocalOnly', { error: remoteError })
        : t('models.remoteUnavailLocalOnly'))
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
