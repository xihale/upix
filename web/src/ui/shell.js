// App shell: Swiss architectural top bar (brand, underline tabs, backend status,
// language switcher, theme cycle) and the main view host. Zero transition theatre.

import { h, text } from './dom.js';
import { BACKEND_IDS, getBackends } from './format.js';
import { brandMark, icon } from './icons.js';
import { segmented, stackField, statusDot } from './controls.js';
import { resolveLocale, t } from '../i18n/index.js';

export const TABS = [
  { value: 'workbench', key: 'nav.workbench' },
  { value: 'models', key: 'nav.models' },
  { value: 'runtime', key: 'nav.runtime' },
];

const THEMES = {
  system: { next: 'light', icon: 'contrast', key: 'theme.system' },
  light: { next: 'dark', icon: 'sun', key: 'theme.light' },
  dark: { next: 'system', icon: 'moon', key: 'theme.dark' },
};

function createLangSwitcher({ store }) {
  const menuId = 'lang-menu';
  const labelEl = h('span', { class: 'lang-btn-label' });
  const btn = h('button', {
    type: 'button',
    class: 'top-meta-btn lang-btn',
    'aria-haspopup': 'listbox',
    'aria-expanded': 'false',
    'aria-controls': menuId,
  }, icon('globe', 14), labelEl);

  const menu = h('div', {
    id: menuId,
    class: 'select-menu',
    role: 'listbox',
    popover: 'manual',
  });

  let optionEls = [];

  function close(restoreFocus = false) {
    if (menu.matches(':popover-open')) {
      try { menu.hidePopover(); } catch { /* ignore */ }
    }
    if (restoreFocus) btn.focus();
  }

  function choose(pref) {
    const nextLocale = resolveLocale(pref);
    store.set({ localePref: pref, locale: nextLocale });
    close(true);
  }

  function open() {
    if (menu.matches(':popover-open')) { close(false); return; }
    if (!btn.isConnected) return;
    const r = btn.getBoundingClientRect();
    const width = Math.max(168, r.width);
    const left = Math.max(8, Math.min(window.innerWidth - width - 8, r.right - width));
    menu.style.left = `${left}px`;
    menu.style.width = `${width}px`;
    menu.style.top = `${r.bottom + 4}px`;
    try { menu.showPopover(); } catch { return; }
    const mh = menu.offsetHeight;
    const vh = window.innerHeight;
    const top = r.bottom + 4 + mh <= vh - 8 ? r.bottom + 4 : Math.max(8, r.top - 4 - mh);
    menu.style.top = `${top}px`;
    const cur = store.get().localePref;
    (optionEls.find(b => b.dataset.value === cur) ?? optionEls[0])?.focus();
  }

  const onOutsidePointer = e => {
    if (!btn.contains(e.target) && !menu.contains(e.target)) close(false);
  };
  const onScroll = e => { if (e.target !== menu) close(false); };
  const onResize = () => close(false);
  const onEscape = e => {
    if (e.key === 'Escape') { e.preventDefault(); close(true); }
  };

  menu.addEventListener('toggle', e => {
    const isOpen = e.newState === 'open';
    btn.setAttribute('aria-expanded', String(isOpen));
    btn.classList.toggle('is-open', isOpen);
    if (isOpen) {
      document.addEventListener('pointerdown', onOutsidePointer, true);
      document.addEventListener('keydown', onEscape, true);
      window.addEventListener('resize', onResize);
      window.addEventListener('scroll', onScroll, true);
    } else {
      document.removeEventListener('pointerdown', onOutsidePointer, true);
      document.removeEventListener('keydown', onEscape, true);
      window.removeEventListener('resize', onResize);
      window.removeEventListener('scroll', onScroll, true);
    }
  });

  btn.addEventListener('click', () => {
    if (menu.matches(':popover-open')) close(false);
    else open();
  });
  btn.addEventListener('keydown', e => {
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      e.preventDefault();
      open();
    } else if (e.key === 'Escape' && menu.matches(':popover-open')) {
      e.preventDefault();
      close(true);
    }
  });

  menu.addEventListener('keydown', e => {
    if (!optionEls.length) return;
    const at = optionEls.indexOf(document.activeElement);
    if (e.key === 'ArrowDown') {
      e.preventDefault();
      optionEls[(at + 1) % optionEls.length].focus();
    } else if (e.key === 'ArrowUp') {
      e.preventDefault();
      optionEls[(at - 1 + optionEls.length) % optionEls.length].focus();
    } else if (e.key === 'Home') {
      e.preventDefault();
      optionEls[0].focus();
    } else if (e.key === 'End') {
      e.preventDefault();
      optionEls[optionEls.length - 1].focus();
    } else if (e.key === 'Tab') {
      close(false);
    }
  });

  function render({ localePref, locale }) {
    const resolvedName = locale === 'zh-CN' ? t('lang.zhCN') : t('lang.en');
    const prefName = localePref === 'auto'
      ? t('lang.auto')
      : localePref === 'zh-CN' ? t('lang.zhCN') : t('lang.en');
    const tipLabel = localePref === 'auto'
      ? t('lang.autoWithLocale', { locale: resolvedName })
      : prefName;
    const tip = t('lang.tooltip', { label: tipLabel });
    text(labelEl, prefName);
    btn.title = tip;
    btn.setAttribute('aria-label', tip);
    menu.setAttribute('aria-label', t('lang.label'));

    const opts = [
      { value: 'auto', label: t('lang.auto'), meta: resolvedName },
      { value: 'zh-CN', label: t('lang.zhCN') },
      { value: 'en', label: t('lang.en') },
    ];
    optionEls = opts.map(o => {
      const selected = o.value === localePref;
      return h('button', {
        type: 'button',
        role: 'option',
        class: `select-opt${selected ? ' is-selected' : ''}`,
        'aria-selected': String(selected),
        dataset: { value: o.value },
        onclick: () => choose(o.value),
      },
      h('span', { class: 'select-opt-label truncate' }, o.label),
      h('span', { class: 'select-opt-end' },
        o.meta && h('span', { class: 'select-opt-meta' }, o.meta),
        icon('check', 14, 'select-check')));
    });
    menu.replaceChildren(...optionEls);
  }

  return { btn, menu, render };
}

export function mountShell({ root, store, actions, toastHost }) {
  const tabOptions = () => TABS.map((tab, i) => {
    const label = t(tab.key);
    return {
      value: tab.value,
      controls: `view-${tab.value}`,
      label,
      title: t('nav.tabTitle', { label, index: i + 1 }),
    };
  });

  const tabsSeg = segmented({
    label: t('nav.label'), kind: 'tabs', className: 'nav-tabs', value: store.get().tab,
    options: tabOptions(),
    onChange: tab => store.set({ tab }),
  });
  const syncTabIds = () => {
    tabsSeg.el.querySelectorAll('.seg-btn').forEach((b, i) => { b.id = `tab-${TABS[i].value}`; });
  };
  syncTabIds();

  const statusDotHost = h('span', { class: 'status-dot-host' });
  const statusName = h('span', { class: 'status-name mono' });

  // ── Backend sheet: the capsule owns backend switching; specs stay in the Engine tab ──
  const sheetSeg = segmented({
    label: t('sheet.title'), fill: true, size: 'sm', value: store.get().settings.backend,
    options: [],
    onChange: backend => actions.setSettings({ backend }),
  });
  const sheetNote = h('p', { class: 'sec-caption' });
  const webnnDevice = segmented({
    label: t('sheet.webnnDeviceLabel'), fill: true, size: 'sm', value: store.get().settings.webnnDevice,
    options: ['gpu', 'npu', 'cpu'].map(d => ({ value: d, label: d.toUpperCase() })),
    onChange: v => actions.setSettings({ webnnDevice: v }),
  });
  const webnnPower = segmented({
    label: t('sheet.webnnPowerLabel'), fill: true, size: 'sm', value: store.get().settings.webnnPower,
    options: [],
    onChange: v => actions.setSettings({ webnnPower: v }),
  });
  const deviceField = stackField(t('sheet.device'), webnnDevice.el);
  const powerField = stackField(t('sheet.power'), webnnPower.el);
  const webnnRows = h('div', { class: 'sub-group', hidden: true }, deviceField, powerField);
  const sheetTitle = h('h2', { id: 'sheet-title', class: 'sheet-title' }, t('sheet.title'));
  const sheetCloseBtn = h('button', {
    type: 'button', class: 'icon-btn icon-btn-sm',
    'aria-label': t('common.close'), title: t('common.close'),
    onclick: () => sheet.close(),
  }, icon('x', 14));
  const specsBtn = h('button', {
    type: 'button', class: 'link-btn',
    onclick: () => { sheet.close(); store.set({ tab: 'runtime' }); },
  }, t('sheet.specsLink'));

  const sheet = h('dialog', { class: 'sheet', 'aria-labelledby': 'sheet-title' },
    h('div', { class: 'sheet-head' }, sheetTitle, sheetCloseBtn),
    h('div', { class: 'sheet-body' }, sheetSeg.el, sheetNote, webnnRows),
    h('div', { class: 'sheet-foot' }, specsBtn));
  sheet.addEventListener('click', e => { if (e.target === sheet) sheet.close(); });
  document.body.append(sheet);

  const statusBtn = h('button', {
    type: 'button', class: 'top-meta-btn',
    onclick: () => sheet.showModal(),
  }, statusDotHost, statusName);

  const langSwitcher = createLangSwitcher({ store });

  const themeBtn = h('button', {
    type: 'button', class: 'icon-btn theme-btn',
    onclick: () => {
      const cur = store.get().theme;
      store.set({ theme: (THEMES[cur] ?? THEMES.system).next });
    },
  });

  const navEl = h('nav', { class: 'topbar-nav', 'aria-label': t('nav.label') }, tabsSeg.el);
  const brandEl = h('a', {
    class: 'brand', href: '#top', 'aria-label': t('nav.brandLabel'),
    onclick: e => {
      e.preventDefault();
      if (location.hash !== '#top') location.hash = '#top';
      window.scrollTo(0, 0);
    },
  }, brandMark(18), h('span', { class: 'wordmark' }, 'Upix'));

  const header = h('header', { class: 'topbar' },
    h('div', { class: 'topbar-start' },
      brandEl,
      h('i', { class: 'topbar-sep', 'aria-hidden': 'true' }),
      navEl),
    h('div', { class: 'topbar-end' }, statusBtn, langSwitcher.btn, langSwitcher.menu, themeBtn));

  const views = Object.fromEntries(TABS.map(tab => [tab.value, h('section', {
    class: `view view-${tab.value}`, id: `view-${tab.value}`, role: 'tabpanel', 'aria-labelledby': `tab-${tab.value}`,
  })]));
  const main = h('main', { class: 'view-host', id: 'main', tabindex: '-1' }, ...Object.values(views));
  root.append(header, main, toastHost);

  store.subscribe(['tab'], ({ tab }) => {
    for (const [name, el] of Object.entries(views)) el.hidden = name !== tab;
    tabsSeg.set(tab);
    if (!root.hidden && location.hash.slice(1) !== tab) history.replaceState(null, '', `#${tab}`);
  });

  store.subscribe(['theme', 'locale'], ({ theme }) => {
    const meta = THEMES[theme] ?? THEMES.system;
    themeBtn.replaceChildren(icon(meta.icon, 16));
    const tip = t('theme.tooltip', { label: t(meta.key) });
    themeBtn.title = tip;
    themeBtn.setAttribute('aria-label', tip);
  });

  store.subscribe(['localePref', 'locale'], s => {
    langSwitcher.render(s);
  });

  store.subscribe(['locale'], () => {
    tabsSeg.setLabel(t('nav.label'));
    tabsSeg.setOptions(tabOptions());
    syncTabIds();
    navEl.setAttribute('aria-label', t('nav.label'));
    brandEl.setAttribute('aria-label', t('nav.brandLabel'));
    sheetSeg.setLabel(t('sheet.title'));
    webnnDevice.setLabel(t('sheet.webnnDeviceLabel'));
    webnnPower.setLabel(t('sheet.webnnPowerLabel'));
    webnnPower.setOptions([
      { value: 'default', label: t('sheet.powerDefault') },
      { value: 'low-power', label: t('sheet.powerLow') },
      { value: 'high-performance', label: t('sheet.powerHigh') },
    ]);
    text(deviceField.querySelector('.field-label'), t('sheet.device'));
    text(powerField.querySelector('.field-label'), t('sheet.power'));
    text(sheetTitle, t('sheet.title'));
    sheetCloseBtn.title = t('common.close');
    sheetCloseBtn.setAttribute('aria-label', t('common.close'));
    text(specsBtn, t('sheet.specsLink'));
  });

  let lastTopStatusSig = '';
  store.subscribe(['caps', 'capsLoading', 'settings', 'progress', 'locale'], ({ caps, capsLoading, settings, progress, locale }) => {
    const backends = getBackends();
    const id = progress?.backendUsed ?? settings.backend;
    const meta = backends[id];
    const cap = caps?.[id];
    const kind = capsLoading && !caps ? 'wait' : cap?.available ? 'ok' : 'off';
    const sig = `${locale}|${id}|${kind}|${cap?.reason ?? ''}`;
    if (sig === lastTopStatusSig) return;
    lastTopStatusSig = sig;
    text(statusName, meta.short);
    statusDotHost.replaceChildren(statusDot(kind));
    const state = kind === 'wait'
      ? t('common.checkingEllipsis')
      : cap?.available
        ? t('common.available')
        : (cap?.reason
          ? t('sheet.statusUnavailableReason', { reason: cap.reason })
          : t('sheet.statusUnavailable'));
    statusBtn.title = `${meta.label} · ${state}`;
    statusBtn.setAttribute('aria-label', t('sheet.statusAria', { label: meta.label, state }));
  });

  store.subscribe(['caps', 'capsLoading', 'settings', 'locale'], ({ caps, capsLoading, settings }) => {
    const backends = getBackends();
    const meta = backends[settings.backend];
    // Backend sheet mirrors hardware capabilities & user settings.
    sheetSeg.setOptions(BACKEND_IDS.map(bid => {
      const c = caps?.[bid];
      return {
        value: bid,
        label: backends[bid].short,
        disabled: !c?.available && !capsLoading,
        title: c?.available ? `${backends[bid].label}${c.deviceName ? ` · ${c.deviceName}` : ''}` : (c?.reason ?? backends[bid].shortFail),
      };
    }));
    sheetSeg.set(settings.backend);
    const activeCap = caps?.[settings.backend];
    if (capsLoading && !activeCap) text(sheetNote, t('common.checkingEllipsis'));
    else if (activeCap?.available) {
      text(sheetNote, settings.backend === 'wasm'
        ? t('sheet.wasmReady', { threads: activeCap.features?.autoThreads ?? 1 })
        : activeCap.deviceName || t('sheet.backendReady', { label: meta.label }));
    } else text(sheetNote, activeCap?.reason || backends[settings.backend].shortFail || t('common.unavailable'));
    const isNn = settings.backend === 'webnn';
    webnnRows.hidden = !isNn;
    if (isNn) {
      const nn = caps?.webnn;
      const devices = nn?.features?.webnnDevices;
      webnnDevice.setOptions(['gpu', 'npu', 'cpu'].map(d => ({
        value: d, label: d.toUpperCase(),
        disabled: !nn?.available || devices?.[d] === false,
        title: !nn?.available || devices?.[d] === false ? t('sheet.deviceUnavailable', { device: d.toUpperCase() }) : undefined,
      })));
      webnnDevice.set(settings.webnnDevice);
      webnnPower.set(settings.webnnPower);
    }
  });

  const fromHash = () => {
    const tab = location.hash.slice(1);
    if (TABS.some(entry => entry.value === tab)) store.set({ tab });
  };
  fromHash();
  window.addEventListener('hashchange', fromHash);
  window.addEventListener('keydown', event => {
    if (root.hidden) return;
    if (!(event.ctrlKey || event.metaKey) || event.altKey || event.shiftKey) return;
    const tab = TABS[Number(event.key) - 1];
    if (!tab) return;
    event.preventDefault();
    store.set({ tab: tab.value });
    document.getElementById(`tab-${tab.value}`)?.focus();
  });

  return { views };
}
