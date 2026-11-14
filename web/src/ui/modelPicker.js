// Model picker: clean Swiss modal index (<dialog>) with instant search,
// family grouping, keyboard navigation, and inline download actions.

import { h, reconcile, text } from './dom.js';
import { icon } from './icons.js';
import { downloadProgress, formatBytes, groupModelsByFamily, matchesModelQuery } from './format.js';
import { progressBar, recBadge, scaleBadge, sceneBadge } from './controls.js';

export function mountModelPicker({ store, actions }) {
  let query = '';
  const search = h('input', {
    type: 'search', class: 'picker-input', placeholder: '搜索模型…', 'aria-label': '搜索模型',
    autocomplete: 'off', spellcheck: false,
    oninput: () => { query = search.value.trim().toLowerCase(); render(); },
  });
  const list = h('div', { class: 'picker-list', role: 'listbox', 'aria-label': '模型列表' });
  const empty = h('p', { class: 'picker-empty muted', hidden: true }, '无匹配模型');
  const dialog = h('dialog', { class: 'picker', 'aria-labelledby': 'picker-title' },
    h('h2', { id: 'picker-title', class: 'sr-only' }, '选择模型'),
    h('div', { class: 'picker-bar' },
      icon('search', 16),
      search,
      h('button', {
        type: 'button', class: 'icon-btn icon-btn-sm', 'aria-label': '关闭', title: '关闭',
        onclick: () => dialog.close(),
      }, icon('x', 14))),
    list,
    empty,
    h('div', { class: 'picker-foot' },
      h('span', { class: 'picker-keys mono', 'aria-hidden': 'true' }, '↑↓ · ↵ · Esc'),
      h('button', {
        type: 'button', class: 'link-btn',
        onclick: () => { dialog.close(); store.set({ tab: 'models' }); },
      }, '管理模型')));
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
    const bar = progressBar('下载进度');
    const select = h('button', { type: 'button', class: 'pick-select', role: 'option' },
      dot, h('span', { class: 'pick-main' }, name, tags), meta);
    const pctEl = h('span', { class: 'pick-pct mono', hidden: true });
    const cancelBtn = h('button', {
      type: 'button', class: 'icon-btn icon-btn-sm', title: '取消', hidden: true,
      onclick: () => model && actions.cancelDownload(model.name),
    }, icon('x', 14));
    const cachedEl = h('span', { class: 'pick-state', hidden: true }, '已缓存');
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
        const sig = [model.name, model.installed, model.scale, model.recommended, selected, dlState.busy, dlState.error].join('|');
        if (sig === lastSig) return;
        lastSig = sig;
        text(name, model.shortName || model.name);
        select.setAttribute('aria-selected', String(selected));
        el.classList.toggle('is-selected', selected);
        select.title = model.name;
        tags.replaceChildren(scaleBadge(model.scale), sceneBadge(model.scene), ...(model.recommended ? [recBadge()] : []));
        text(meta, formatBytes(model.sizeBytes));
        pctEl.hidden = !dlState.busy;
        cancelBtn.hidden = !dlState.busy;
        cancelBtn.setAttribute('aria-label', `取消下载 ${model.name}`);
        cachedEl.hidden = dlState.busy || !model.installed;
        dlBtn.hidden = dlState.busy || model.installed;
        text(dlBtn, dlState.error ? '重试' : '下载');
        dlBtn.title = dlState.error || '下载';
        dlBtn.setAttribute('aria-label', dlState.error ? `重试下载 ${model.name}` : `下载 ${model.name}`);
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

  store.subscribe(['models', 'downloads', 'settings'], () => { if (dialog.open) render(); });

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
