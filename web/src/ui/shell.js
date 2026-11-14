// App shell: Swiss architectural top bar (brand, underline tabs, backend status,
// theme cycle) and the main view host. Zero transition theatre.

import { h, text } from './dom.js';
import { BACKENDS, BACKEND_IDS } from './format.js';
import { brandMark, icon } from './icons.js';
import { segmented, stackField, statusDot } from './controls.js';

export const TABS = [
  { value: 'workbench', label: '工作台' },
  { value: 'models', label: '模型' },
  { value: 'runtime', label: '引擎' },
];

const THEMES = {
  system: { next: 'light', icon: 'contrast', label: '跟随系统' },
  light: { next: 'dark', icon: 'sun', label: '浅色' },
  dark: { next: 'system', icon: 'moon', label: '深色' },
};

export function mountShell({ root, store, actions, toastHost }) {
  const tabsSeg = segmented({
    label: '主导航', kind: 'tabs', className: 'nav-tabs', value: store.get().tab,
    options: TABS.map(t => ({
      value: t.value, controls: `view-${t.value}`,
      label: t.label,
      title: `${t.label} (Ctrl/⌘ + ${TABS.indexOf(t) + 1})`,
    })),
    onChange: tab => store.set({ tab }),
  });
  tabsSeg.el.querySelectorAll('.seg-btn').forEach((b, i) => { b.id = `tab-${TABS[i].value}`; });

  const statusDotHost = h('span', { class: 'status-dot-host' });
  const statusName = h('span', { class: 'status-name mono' });

  // ── Backend sheet: the capsule owns backend switching; specs stay in the Engine tab ──
  const sheetSeg = segmented({
    label: '计算后端', fill: true, size: 'sm', value: store.get().settings.backend,
    options: [],
    onChange: backend => actions.setSettings({ backend }),
  });
  const sheetNote = h('p', { class: 'sec-caption' });
  const webnnDevice = segmented({
    label: 'WebNN 设备', fill: true, size: 'sm', value: store.get().settings.webnnDevice,
    options: ['gpu', 'npu', 'cpu'].map(d => ({ value: d, label: d.toUpperCase() })),
    onChange: v => actions.setSettings({ webnnDevice: v }),
  });
  const webnnPower = segmented({
    label: 'WebNN 功耗', fill: true, size: 'sm', value: store.get().settings.webnnPower,
    options: [
      { value: 'default', label: '默认' },
      { value: 'low-power', label: '低耗' },
      { value: 'high-performance', label: '性能' },
    ],
    onChange: v => actions.setSettings({ webnnPower: v }),
  });
  const webnnRows = h('div', { class: 'sub-group', hidden: true },
    stackField('设备', webnnDevice.el),
    stackField('功耗', webnnPower.el));
  const sheet = h('dialog', { class: 'sheet', 'aria-labelledby': 'sheet-title' },
    h('div', { class: 'sheet-head' },
      h('h2', { id: 'sheet-title', class: 'sheet-title' }, '计算后端'),
      h('button', {
        type: 'button', class: 'icon-btn icon-btn-sm', 'aria-label': '关闭', title: '关闭',
        onclick: () => sheet.close(),
      }, icon('x', 14))),
    h('div', { class: 'sheet-body' }, sheetSeg.el, sheetNote, webnnRows),
    h('div', { class: 'sheet-foot' },
      h('button', {
        type: 'button', class: 'link-btn',
        onclick: () => { sheet.close(); store.set({ tab: 'runtime' }); },
      }, '规格与基准')));
  sheet.addEventListener('click', e => { if (e.target === sheet) sheet.close(); });
  document.body.append(sheet);

  const statusBtn = h('button', {
    type: 'button', class: 'top-meta-btn',
    onclick: () => sheet.showModal(),
  }, statusDotHost, statusName);

  const themeBtn = h('button', {
    type: 'button', class: 'icon-btn theme-btn',
    onclick: () => {
      const cur = store.get().theme;
      store.set({ theme: (THEMES[cur] ?? THEMES.system).next });
    },
  });

  const header = h('header', { class: 'topbar' },
    h('div', { class: 'topbar-start' },
      h('a', {
        class: 'brand', href: '#workbench', 'aria-label': 'Upix Web',
        onclick: e => { e.preventDefault(); store.set({ tab: 'workbench' }); },
      }, brandMark(18), h('span', { class: 'wordmark' }, 'Upix')),
      h('i', { class: 'topbar-sep', 'aria-hidden': 'true' }),
      h('nav', { class: 'topbar-nav', 'aria-label': '主导航' }, tabsSeg.el)),
    h('div', { class: 'topbar-end' }, statusBtn, themeBtn));

  const views = Object.fromEntries(TABS.map(t => [t.value, h('section', {
    class: `view view-${t.value}`, id: `view-${t.value}`, role: 'tabpanel', 'aria-labelledby': `tab-${t.value}`,
  })]));
  const main = h('main', { class: 'view-host', id: 'main', tabindex: '-1' }, ...Object.values(views));
  root.append(header, main, toastHost);

  store.subscribe(['tab'], ({ tab }) => {
    for (const [name, el] of Object.entries(views)) el.hidden = name !== tab;
    tabsSeg.set(tab);
    if (location.hash.slice(1) !== tab) history.replaceState(null, '', `#${tab}`);
  });

  store.subscribe(['theme'], ({ theme }) => {
    const meta = THEMES[theme] ?? THEMES.system;
    themeBtn.replaceChildren(icon(meta.icon, 16));
    const tip = `外观：${meta.label}`;
    themeBtn.title = tip;
    themeBtn.setAttribute('aria-label', tip);
  });

  let lastTopStatusSig = '';
  store.subscribe(['caps', 'capsLoading', 'settings', 'progress'], ({ caps, capsLoading, settings, progress }) => {
    const id = progress?.backendUsed ?? settings.backend;
    const meta = BACKENDS[id];
    const cap = caps?.[id];
    const kind = capsLoading && !caps ? 'wait' : cap?.available ? 'ok' : 'off';
    const sig = `${id}|${kind}|${cap?.reason ?? ''}`;
    if (sig === lastTopStatusSig) return;
    lastTopStatusSig = sig;
    text(statusName, meta.short);
    statusDotHost.replaceChildren(statusDot(kind));
    const state = kind === 'wait' ? '检测中…' : cap?.available ? '可用' : `不可用${cap?.reason ? `：${cap.reason}` : ''}`;
    statusBtn.title = `${meta.label} · ${state}`;
    statusBtn.setAttribute('aria-label', `计算后端：${meta.label}，${state}`);
  });

  store.subscribe(['caps', 'capsLoading', 'settings'], ({ caps, capsLoading, settings }) => {
    const meta = BACKENDS[settings.backend];
    // Backend sheet mirrors hardware capabilities & user settings.
    sheetSeg.setOptions(BACKEND_IDS.map(bid => {
      const c = caps?.[bid];
      return {
        value: bid,
        label: BACKENDS[bid].short,
        disabled: !c?.available && !capsLoading,
        title: c?.available ? `${BACKENDS[bid].label}${c.deviceName ? ` · ${c.deviceName}` : ''}` : (c?.reason ?? BACKENDS[bid].shortFail),
      };
    }));
    sheetSeg.set(settings.backend);
    const activeCap = caps?.[settings.backend];
    if (capsLoading && !activeCap) text(sheetNote, '检测中…');
    else if (activeCap?.available) {
      text(sheetNote, settings.backend === 'wasm'
        ? `SIMD128 · 自动 ${activeCap.features?.autoThreads ?? 1} 线程`
        : activeCap.deviceName || `${meta.label} 就绪`);
    } else text(sheetNote, activeCap?.reason || BACKENDS[settings.backend].shortFail || '不可用');
    const isNn = settings.backend === 'webnn';
    webnnRows.hidden = !isNn;
    if (isNn) {
      const nn = caps?.webnn;
      const devices = nn?.features?.webnnDevices;
      webnnDevice.setOptions(['gpu', 'npu', 'cpu'].map(d => ({
        value: d, label: d.toUpperCase(),
        disabled: !nn?.available || devices?.[d] === false,
        title: !nn?.available || devices?.[d] === false ? `${d.toUpperCase()} 不可用` : undefined,
      })));
      webnnDevice.set(settings.webnnDevice);
      webnnPower.set(settings.webnnPower);
    }
  });

  const fromHash = () => {
    const tab = location.hash.slice(1);
    if (TABS.some(t => t.value === tab)) store.set({ tab });
  };
  fromHash();
  window.addEventListener('hashchange', fromHash);
  window.addEventListener('keydown', event => {
    if (!(event.ctrlKey || event.metaKey) || event.altKey || event.shiftKey) return;
    const tab = TABS[Number(event.key) - 1];
    if (!tab) return;
    event.preventDefault();
    store.set({ tab: tab.value });
    document.getElementById(`tab-${tab.value}`)?.focus();
  });

  return { views };
}
