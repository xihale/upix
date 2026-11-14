// Image strip: flush horizontal strip docked below the stage whenever ≥1 image.

import { h, reconcile, text } from './dom.js';
import { icon } from './icons.js';
import { confirmButton, progressBar } from './controls.js';
import { pickImages } from './stage.js';

export const STATUS = {
  probing: { label: '读取中', kind: 'wait' },
  queued: { label: '排队', kind: 'wait' },
  processing: { label: '处理中', kind: 'active' },
  done: { label: '完成', kind: 'ok' },
  skipped: { label: '跳过', kind: 'off' },
  failed: { label: '失败', kind: 'error' },
};

export function mountQueue({ host, store, actions }) {
  host.classList.add('queue');
  const list = h('ul', { class: 'q-list', role: 'listbox', 'aria-label': '批量队列', 'aria-orientation': 'horizontal' });
  const addTile = h('button', {
    type: 'button', class: 'q-add', title: '添加图像', 'aria-label': '添加图像',
    onclick: () => pickImages(actions),
  }, icon('plus', 16));
  // Export lives in the workbench dock (保存当前图像 / 打包全部); this row only
  // reports progress and offers guarded cleanup.
  const clearBtn = confirmButton({
    label: '清空', confirmLabel: '确认清空', className: 'btn btn-quiet btn-sm',
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
    const bar = progressBar('处理进度');
    const rm = h('button', {
      type: 'button', class: 'icon-btn icon-btn-sm q-rm', 'aria-label': '移除图像', title: '移除图像',
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
        const thumbSrc = next.result?.url ?? next.url;
        if (thumb.dataset.src !== thumbSrc) { thumb.dataset.src = thumbSrc; thumb.src = thumbSrc; }
        dot.className = `status-dot is-${st.kind}`;
        el.dataset.status = next.status;
        select.setAttribute('aria-selected', String(next.id === store.get().activeId));
        select.setAttribute('aria-label', `${next.name}，${st.label}`);
        el.classList.toggle('is-active', next.id === store.get().activeId);
        bar.el.hidden = next.status !== 'processing';
        select.title = next.status === 'failed' ? `${st.label}：${next.error}` : `${next.name} · ${st.label}`;
      },
    };
  }

  let lastActive = null;
  store.subscribe(['queue', 'activeId'], ({ queue, activeId }) => {
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
