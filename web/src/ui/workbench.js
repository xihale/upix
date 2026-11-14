// Workbench: stage + filmstrip on the left, inspector on the right.

import { h } from './dom.js';
import { mountStage } from './stage.js';
import { mountQueue } from './queue.js';
import { mountInspector } from './inspector.js';
import { t } from '../i18n/index.js';

export function mountWorkbench({ root, store, actions }) {
  const stageHost = h('div');
  const queueHost = h('div', { hidden: true });
  const inspectorHost = h('aside', { 'aria-label': t('inspector.asideLabel') });
  root.classList.add('workbench');
  root.append(h('div', { class: 'stage-col' }, stageHost, queueHost), inspectorHost);

  store.subscribe(['locale'], () => {
    inspectorHost.setAttribute('aria-label', t('inspector.asideLabel'));
  });

  mountStage({ host: stageHost, dropTarget: root, store, actions });
  mountQueue({ host: queueHost, store, actions });
  mountInspector({ host: inspectorHost, store, actions });

  // Paste images straight onto the workbench from anywhere on the page.
  window.addEventListener('paste', event => {
    if (store.get().tab !== 'workbench') return;
    if (event.target.closest?.('input, textarea, select, [contenteditable]')) return;
    const files = [...(event.clipboardData?.files ?? [])];
    if (!files.length) return;
    event.preventDefault();
    actions.addFiles(files);
  });
}
