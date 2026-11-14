// Image strip: flush horizontal strip docked below the stage whenever ≥1 image.

import { h, reconcile, text } from './dom.js';
import { icon } from './icons.js';
import { confirmButton, progressBar } from './controls.js';
import { pickImages } from './stage.js';
import { t } from '../i18n/index.js';

export const STATUS = {
  probing: { key: 'queue.status.probing', kind: 'wait' },
  queued: { key: 'queue.status.queued', kind: 'wait' },
  processing: { key: 'queue.status.processing', kind: 'active' },
  done: { key: 'queue.status.done', kind: 'ok' },
  skipped: { key: 'queue.status.skipped', kind: 'off' },
  failed: { key: 'queue.status.failed', kind: 'error' },
};

export function mountQueue({ host, store, actions }) {
  host.classList.add('queue');
  const list = h('ul', {
    class: 'q-list', role: 'listbox',
    'aria-label': t('queue.label'), 'aria-orientation': 'horizontal',
  });
  const addTile = h('button', {
    type: 'button', class: 'q-add',
    title: t('queue.addImages'), 'aria-label': t('queue.addImages'),
    onclick: () => pickImages(actions),
  }, icon('plus', 16));
  const clearBtn = confirmButton({
    label: t('queue.clear'), confirmLabel: t('queue.confirmClear'), className: 'btn btn-quiet btn-sm',
    onConfirm: () => actions.clearQueue(),
  });
  const summary = h('span', { class: 'q-summary mono' });
  host.append(
    h('div', { class: 'q-scroll' }, list, addTile),
    h('div', { class: 'q-actions' }, summary, clearBtn));

  const cache = new Map();

  function createRow(item) {
    const thumb = h('img', { class: 'q-thumb', alt: '', draggable: false, loading: 'lazy' });
    const dot = h('i', { class: 'status-dot' });
    const bar = progressBar(t('common.processingProgress'));
    const rm = h('button', {
      type: 'button', class: 'icon-btn icon-btn-sm q-rm',
      'aria-label': t('queue.removeImage'), title: t('queue.removeImage'),
      onclick: () => actions.removeItem(item.id),
    }, icon('x', 13));
    const select = h('button', {
      type: 'button', class: 'q-select', role: 'option', onclick: () => actions.selectItem(item.id),
    }, thumb, dot);
    const el = h('li', { class: 'q-item' }, select, rm, bar.el);
    return {
      el,
      bar,
      update(next) {
        const st = STATUS[next.status] ?? STATUS.queued;
        const stLabel = t(st.key);
        const thumbSrc = next.result?.url ?? next.url;
        if (thumb.dataset.src !== thumbSrc) { thumb.dataset.src = thumbSrc; thumb.src = thumbSrc; }
        dot.className = `status-dot is-${st.kind}`;
        el.dataset.status = next.status;
        select.setAttribute('aria-selected', String(next.id === store.get().activeId));
        select.setAttribute('aria-label', t('queue.itemAria', { name: next.name, status: stLabel }));
        el.classList.toggle('is-active', next.id === store.get().activeId);
        bar.el.hidden = next.status !== 'processing';
        bar.setLabel(t('common.processingProgress'));
        rm.title = t('queue.removeImage');
        rm.setAttribute('aria-label', t('queue.removeImage'));
        select.title = next.status === 'failed'
          ? t('queue.itemTitleFailed', { status: stLabel, error: next.error })
          : t('queue.itemTitle', { name: next.name, status: stLabel });
      },
    };
  }

  store.subscribe(['locale'], () => {
    list.setAttribute('aria-label', t('queue.label'));
    addTile.title = t('queue.addImages');
    addTile.setAttribute('aria-label', t('queue.addImages'));
    clearBtn.setLabels({ label: t('queue.clear'), confirmLabel: t('queue.confirmClear') });
  });

  let lastActive = null;
  store.subscribe(['queue', 'activeId', 'locale'], ({ queue, activeId }) => {
    host.hidden = queue.length === 0;
    reconcile(list, queue, i => i.id, createRow, cache);
    const done = queue.filter(i => i.status === 'done').length;
    text(summary, `${done}/${queue.length}`);
    if (activeId !== lastActive) {
      lastActive = activeId;
      list.querySelector('.is-active')?.scrollIntoView({ block: 'nearest', inline: 'nearest' });
    }
  });

  store.subscribe(['progress'], ({ progress }) => {
    if (!progress) return;
    cache.get(progress.itemId)?.bar.set(progress.percentage);
  });
}
