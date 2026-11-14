// Model picker: clean Swiss modal index (<dialog>) with instant search,
// family grouping, keyboard navigation, and inline download actions.

import { h, reconcile, text } from './dom.js';
import { icon } from './icons.js';
import { downloadProgress, formatBytes, groupModelsByFamily, matchesModelQuery } from './format.js';
import { progressBar, recBadge, scaleBadge, sceneBadge } from './controls.js';
import { getLocale, t } from '../i18n/index.js';

export function mountModelPicker({ store, actions }) {
  let query = '';
  const pickerTitle = h('h2', { id: 'picker-title', class: 'sr-only' }, t('picker.title'));
  const search = h('input', {
    type: 'search', class: 'picker-input', placeholder: t('picker.searchPlaceholder'), 'aria-label': t('picker.searchLabel'),
    autocomplete: 'off', spellcheck: false,
    oninput: () => { query = search.value.trim().toLowerCase(); render(); },
  });
  const closeBtn = h('button', {
    type: 'button', class: 'icon-btn icon-btn-sm', 'aria-label': t('common.close'), title: t('common.close'),
    onclick: () => dialog.close(),
  }, icon('x', 14));
  const list = h('div', { class: 'picker-list', role: 'listbox', 'aria-label': t('picker.listLabel') });
  const empty = h('p', { class: 'picker-empty muted', hidden: true }, t('picker.empty'));
  const manageBtn = h('button', {
    type: 'button', class: 'link-btn',
    onclick: () => { dialog.close(); store.set({ tab: 'models' }); },
  }, t('picker.manageModels'));
  const dialog = h('dialog', { class: 'picker', 'aria-labelledby': 'picker-title' },
    pickerTitle,
    h('div', { class: 'picker-bar' }, icon('search', 16), search, closeBtn),
    list,
    empty,
    h('div', { class: 'picker-foot' },
      h('span', { class: 'picker-keys mono', 'aria-hidden': 'true' }, '↑↓ · ↵ · Esc'),
      manageBtn));
  document.body.append(dialog);

  dialog.addEventListener('click', e => { if (e.target === dialog) dialog.close(); });
  dialog.addEventListener('keydown', e => {
    if (e.key === 'Enter' && document.activeElement === search) {
      const first = dialog.querySelector('.pick-select');
      if (first) { e.preventDefault(); first.click(); }
      return;
    }
    if (e.key !== 'ArrowDown' && e.key !== 'ArrowUp') return;
    const rows = [...dialog.querySelectorAll('.pick-select')];
    const at = rows.indexOf(document.activeElement);
    if (at < 0 && document.activeElement !== search) return;
    eventPrevent(e);
    const next = e.key === 'ArrowDown' ? rows[at + 1] ?? rows[0] : at <= 0 ? search : rows[at - 1];
    next?.focus();
  });

  function eventPrevent(e) { e.preventDefault(); }

  const cache = new Map();

  function createItem(node) {
    if (node.kind === 'group') {
      const title = h('span', { class: 'group-title' });
      const count = h('span', { class: 'group-count mono' });
      return {
        el: h('div', { class: 'group-head', role: 'presentation' }, title, count),
        update(n) { text(title, n.family); text(count, String(n.count)); },
      };
    }
    const dot = h('i', { class: 'pick-dot', 'aria-hidden': 'true' });
    const name = h('span', { class: 'pick-name mono truncate' });
    const tags = h('span', { class: 'pick-tags' });
    const meta = h('span', { class: 'pick-meta mono' });
    const bar = progressBar(t('common.downloadProgress'));
    const select = h('button', { type: 'button', class: 'pick-select', role: 'option' },
      dot, h('span', { class: 'pick-main' }, name, tags), meta);
    const pctEl = h('span', { class: 'pick-pct mono', hidden: true });
    const cancelBtn = h('button', {
      type: 'button', class: 'icon-btn icon-btn-sm', title: t('common.cancel'), hidden: true,
      onclick: () => model && actions.cancelDownload(model.name),
    }, icon('x', 14));
    const cachedEl = h('span', { class: 'pick-state', hidden: true }, t('picker.cached'));
    const dlBtn = h('button', {
      type: 'button', class: 'btn btn-secondary btn-sm', hidden: true,
      onclick: () => model && actions.downloadModel(model.name),
    });
    const action = h('span', { class: 'pick-action' }, pctEl, cancelBtn, cachedEl, dlBtn);
    const el = h('div', { class: 'pick-row' }, select, action, bar.el);
    let model = null;
    let lastSig = '';
    select.addEventListener('click', () => { actions.selectModel(model.name); dialog.close(); });
    return {
      el,
      update(n) {
        model = n.model;
        const dlState = downloadProgress(store.get().downloads[model.name]);
        const selected = store.get().settings.modelName === model.name;
        bar.el.hidden = !dlState.busy;
        if (dlState.busy) {
          bar.set(dlState.pct);
          text(pctEl, dlState.label);
        }
        const sig = [
          getLocale(), model.name, model.installed, model.scale, model.recommended, selected, dlState.busy, dlState.error,
        ].join('|');
        if (sig === lastSig) return;
        lastSig = sig;
        bar.setLabel(t('common.downloadProgress'));
        text(name, model.shortName || model.name);
        select.setAttribute('aria-selected', String(selected));
        el.classList.toggle('is-selected', selected);
        select.title = model.name;
        tags.replaceChildren(scaleBadge(model.scale), sceneBadge(model.scene), ...(model.recommended ? [recBadge()] : []));
        text(meta, formatBytes(model.sizeBytes));
        pctEl.hidden = !dlState.busy;
        cancelBtn.hidden = !dlState.busy;
        cancelBtn.title = t('common.cancel');
        cancelBtn.setAttribute('aria-label', t('picker.cancelDownloadAria', { name: model.name }));
        cachedEl.hidden = dlState.busy || !model.installed;
        text(cachedEl, t('picker.cached'));
        dlBtn.hidden = dlState.busy || model.installed;
        text(dlBtn, dlState.error ? t('common.retry') : t('common.download'));
        dlBtn.title = dlState.error || t('common.download');
        dlBtn.setAttribute('aria-label', dlState.error
          ? t('picker.retryDownloadAria', { name: model.name })
          : t('picker.downloadAria', { name: model.name }));
      },
    };
  }

  function render() {
    const { models } = store.get();
    const q = query;
    const nodes = groupModelsByFamily(models.filter(m => matchesModelQuery(m, q)));
    empty.hidden = nodes.length > 0;
    reconcile(list, nodes, n => n.key, createItem, cache);
  }

  store.subscribe(['locale'], () => {
    text(pickerTitle, t('picker.title'));
    search.placeholder = t('picker.searchPlaceholder');
    search.setAttribute('aria-label', t('picker.searchLabel'));
    closeBtn.title = t('common.close');
    closeBtn.setAttribute('aria-label', t('common.close'));
    list.setAttribute('aria-label', t('picker.listLabel'));
    text(empty, t('picker.empty'));
    text(manageBtn, t('picker.manageModels'));
  });

  store.subscribe(['models', 'downloads', 'settings', 'locale'], () => { if (dialog.open) render(); });

  return {
    open() {
      query = '';
      search.value = '';
      render();
      dialog.showModal();
      requestAnimationFrame(() => {
        (dialog.querySelector('.pick-row.is-selected .pick-select') ?? search).focus({ preventScroll: false });
        dialog.querySelector('.pick-row.is-selected')?.scrollIntoView({ block: 'center' });
      });
    },
  };
}
