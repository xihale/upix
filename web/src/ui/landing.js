// Landing page: a quiet Geist surface (top bar, hero + 4× comparison stage,
// four architecture cells, download strip, one-line footer). Pure DOM, no
// runtime / WASM imports, so it paints instantly. Theme + locale come from the
// shared store; artwork is swapped by the *resolved* theme, not by <source media>.

import { h, text, clamp } from './dom.js';
import { brandMark, icon } from './icons.js';
import { LOCALE_PREFS, resolveLocale, t } from '../i18n/index.js';

const REPO = 'https://github.com/xihale/upix';
const CREDIT_URL = 'https://re.bluepoch.com/home/detail.html#wallpaper';
const FALLBACK_TAGS = { gui: 'gui-v0.2.0-beta.1', android: 'android-v0.1.1', cli: 'cli-v0.2.0-beta.1' };
const RELEASES = typeof __UPIX_RELEASES__ !== 'undefined' ? __UPIX_RELEASES__ : FALLBACK_TAGS;
const releaseUrl = key => `${REPO}/releases/tag/${RELEASES[key] ?? FALLBACK_TAGS[key]}`;

// Source 640-wide previews → 2560-wide restored renders (4×). Sizes are fixed
// here so layout is reserved (no CLS) and captions are right before decode.
const ART = {
  light: {
    source: './images/source-light.webp', restored: './images/restored-light.webp',
    sw: 640, sh: 360, rw: 2560, rh: 1440,
  },
  dark: {
    source: './images/source-dark.webp', restored: './images/restored-dark.webp',
    sw: 640, sh: 427, rw: 2560, rh: 1708,
  },
};

const THEMES = {
  system: { next: 'light', icon: 'contrast', key: 'theme.system' },
  light: { next: 'dark', icon: 'sun', key: 'theme.light' },
  dark: { next: 'system', icon: 'moon', key: 'theme.dark' },
};

const DARK_QUERY = '(prefers-color-scheme: dark)';

export function resolveTheme(theme) {
  if (theme === 'light' || theme === 'dark') return theme;
  return window.matchMedia(DARK_QUERY).matches ? 'dark' : 'light';
}

const dims = (w, h2) => `${w} × ${h2}`;

export function mountLanding({ root, store }) {
  const cleanups = [];
  const refreshers = [];

  /** Element whose text follows the active locale. */
  const tx = (tag, props, key, params) => {
    const el = h(tag, props);
    const paint = () => text(el, t(key, typeof params === 'function' ? params() : params));
    paint();
    refreshers.push(paint);
    return el;
  };
  /** Attribute that follows the active locale. */
  const txAttr = (el, name, key, params) => {
    const paint = () => el.setAttribute(name, t(key, typeof params === 'function' ? params() : params));
    paint();
    refreshers.push(paint);
    return el;
  };

  // ── Top bar ────────────────────────────────────────────────────────────
  const langLabel = h('span', { class: 'lang-btn-label' });
  const langBtn = h('button', {
    type: 'button', class: 'top-meta-btn lang-btn',
    'aria-haspopup': 'listbox', 'aria-expanded': 'false', 'aria-controls': 'lp-lang-menu',
  }, icon('globe', 14), langLabel);
  const langMenu = h('div', { id: 'lp-lang-menu', class: 'select-menu', role: 'listbox', popover: 'manual' });
  const lang = createLangMenu({ store, btn: langBtn, menu: langMenu, label: langLabel });
  cleanups.push(lang.destroy);

  const themeBtn = h('button', {
    type: 'button', class: 'icon-btn theme-btn',
    onclick: () => store.set({ theme: (THEMES[store.get().theme] ?? THEMES.system).next }),
  });

  const brand = h('a', { class: 'lp-brand', href: '#top' }, brandMark(18), h('span', { class: 'lp-wordmark' }, 'Upix'));
  txAttr(brand, 'aria-label', 'landing.nav.brand');

  const top = h('header', { class: 'lp-top' },
    h('div', { class: 'lp-top-in' },
      brand,
      h('div', { class: 'lp-top-end' },
        h('a', { class: 'top-meta-btn lp-gh', href: REPO, target: '_blank', rel: 'noopener noreferrer' }, 'GitHub'),
        langBtn, langMenu, themeBtn,
        tx('a', { class: 'lp-btn-sm', href: '#workbench' }, 'landing.nav.cta'))));

  // ── Hero + comparison stage ────────────────────────────────────────────
  const stage = createStage({ store, tx, txAttr, refreshers, cleanups });

  const hero = h('section', { class: 'lp-hero' },
    h('div', { class: 'lp-wrap' },
      h('div', { class: 'lp-hero-copy' },
        tx('h1', { class: 'lp-h1' }, 'landing.hero.title'),
        tx('p', { class: 'lp-sub' }, 'landing.hero.sub'),
        h('div', { class: 'lp-cta-row' },
          h('a', { class: 'lp-btn lp-btn-primary', href: '#workbench' },
            tx('span', null, 'landing.hero.ctaStudio'), icon('chevron-right', 16)),
          tx('a', { class: 'lp-btn lp-btn-secondary', href: '#downloads' }, 'landing.hero.ctaGet'))),
      stage.el));

  // ── Architecture: four text-only cells ─────────────────────────────────
  const cell = (key, extra) => h('article', { class: 'lp-cell' },
    tx('h3', { class: 'lp-h3' }, `landing.arch.${key}.title`),
    tx('p', null, `landing.arch.${key}.body`),
    extra);
  const arch = h('section', { class: 'lp-band lp-arch', id: 'architecture' },
    h('div', { class: 'lp-wrap' },
      tx('h2', { class: 'lp-h2' }, 'landing.arch.title'),
      h('div', { class: 'lp-grid' },
        cell('offload'),
        cell('frontends'),
        cell('engine'),
        cell('models', tx('a', { class: 'lp-arrow', href: '#models' }, 'landing.arch.models.link')))));

  // ── Downloads strip ────────────────────────────────────────────────────
  const dl = (key, spec, href, external) => h('div', { class: 'lp-dl' },
    tx('h3', { class: 'lp-h3-sm' }, `landing.get.${key}.title`),
    h('p', { class: 'lp-spec' }, spec),
    tx('a', {
      class: 'lp-arrow', href,
      ...(external ? { target: '_blank', rel: 'noopener noreferrer' } : null),
    }, external ? 'landing.get.download' : 'landing.get.open'));
  const downloads = h('section', { class: 'lp-band lp-downloads', id: 'downloads' },
    h('div', { class: 'lp-wrap' },
      tx('h2', { class: 'lp-h2' }, 'landing.get.title'),
      h('div', { class: 'lp-strip' },
        dl('web', 'WebGPU · WebNN · WASM', '#workbench', false),
        dl('desktop', 'SDL3 · Vulkan · Dear ImGui', releaseUrl('gui'), true),
        dl('android', 'Compose · JNI', releaseUrl('android'), true),
        dl('cli', 'upix-cli', releaseUrl('cli'), true))));

  const main = h('main', { class: 'lp-main', id: 'main', tabindex: '-1' }, hero, arch, downloads);

  // ── Footer: one line ───────────────────────────────────────────────────
  const footer = h('footer', { class: 'lp-footer' },
    h('div', { class: 'lp-wrap lp-footer-in' },
      h('span', { class: 'lp-foot-brand' }, brandMark(14), 'Upix'),
      h('nav', { class: 'lp-foot-links' },
        h('a', { href: REPO, target: '_blank', rel: 'noopener noreferrer' }, 'GitHub'),
        tx('a', { href: `${REPO}/blob/HEAD/LICENSE`, target: '_blank', rel: 'noopener noreferrer' }, 'landing.footer.license'))));

  root.classList.add('landing');
  root.replaceChildren(h('span', { id: 'top', class: 'lp-top-anchor' }), top, main, footer);

  // ── Theme / locale reactions ───────────────────────────────────────────
  const unsubTheme = store.subscribe(['theme', 'locale'], ({ theme }) => {
    const meta = THEMES[theme] ?? THEMES.system;
    themeBtn.replaceChildren(icon(meta.icon, 16));
    const tip = t('theme.tooltip', { label: t(meta.key) });
    themeBtn.title = tip;
    themeBtn.setAttribute('aria-label', tip);
  });
  cleanups.push(unsubTheme);

  const unsubLocale = store.subscribe(['locale', 'localePref'], () => {
    for (const paint of refreshers) paint();
    lang.render();
    syncMeta();
  });
  cleanups.push(unsubLocale);

  // The skip-link targets #main. Studio owns the same id, so the landing only
  // claims it while it is the visible view; page title/description follow suit.
  function syncMeta() {
    const visible = !root.hidden;
    main.id = visible ? 'main' : 'landing-main';
    document.title = t(visible ? 'landing.meta.title' : 'meta.title');
    document.querySelector('meta[name="description"]')
      ?.setAttribute('content', t(visible ? 'landing.meta.description' : 'meta.description'));
  }
  const visibility = new MutationObserver(syncMeta);
  visibility.observe(root, { attributes: true, attributeFilter: ['hidden'] });
  cleanups.push(() => visibility.disconnect());
  syncMeta();

  return {
    destroy() {
      for (const fn of cleanups.splice(0)) fn();
      root.classList.remove('landing');
      root.replaceChildren();
    },
  };
}

// ── Comparison stage ─────────────────────────────────────────────────────

function createStage({ store, tx, txAttr, refreshers, cleanups }) {
  let split = 50;
  let zoom = 1;
  let dragging = false;

  const art = () => ART[resolveTheme(store.get().theme)];
  const first = art();

  // Restored = LCP image: fetchpriority high, never loading=lazy. `src` is
  // assigned last so priority hints are in place before the request starts.
  const restoredImg = h('img', {
    class: 'lp-img', decoding: 'async', fetchpriority: 'high',
    width: first.rw, height: first.rh, alt: '', src: first.restored,
  });
  const sourceImg = h('img', {
    class: 'lp-img', decoding: 'async',
    width: first.sw, height: first.sh, alt: '', src: first.source,
  });
  restoredImg.draggable = false;
  sourceImg.draggable = false;
  txAttr(restoredImg, 'alt', 'landing.stage.altRestored');
  txAttr(sourceImg, 'alt', 'landing.stage.altSource');

  const view = h('div', { class: 'lp-view', dataset: { art: resolveTheme(store.get().theme), zoom: '1' } },
    h('div', { class: 'lp-pane' }, h('div', { class: 'lp-zoom' }, restoredImg)),
    h('div', { class: 'lp-pane lp-pane-source' }, h('div', { class: 'lp-zoom' }, sourceImg)),
    h('div', { class: 'lp-line', 'aria-hidden': 'true' }));

  const range = h('input', {
    class: 'lp-range', type: 'range', min: '0', max: '100', step: '0.5', value: '50',
  });
  txAttr(range, 'aria-label', 'landing.stage.range');
  const rangeRow = h('div', { class: 'lp-range-row' },
    tx('span', null, 'landing.stage.before'),
    range,
    tx('span', null, 'landing.stage.after'));

  const frame = h('figure', { class: 'lp-frame' }, view, rangeRow);

  const resolution = h('span', { class: 'lp-res' });
  const credit = h('a', { class: 'lp-credit', href: CREDIT_URL, target: '_blank', rel: 'noopener noreferrer' }, '1999');
  txAttr(credit, 'aria-label', 'landing.stage.creditLabel');
  const caption = h('p', { class: 'lp-caption' }, resolution, ' · ', credit);

  const zoomBtns = [1, 2].map(z => h('button', {
    type: 'button', class: 'lp-zoom-btn', 'aria-pressed': String(z === 1),
    onclick: () => setZoom(z),
  }, `${z}×`));
  const toggle = h('div', { class: 'lp-zoom-toggle', role: 'group' }, ...zoomBtns);
  txAttr(toggle, 'aria-label', 'landing.stage.zoom');

  const el = h('section', { class: 'lp-stage', id: 'compare', 'aria-labelledby': 'lp-stage-title' },
    tx('h2', { class: 'sr-only', id: 'lp-stage-title' }, 'landing.stage.title'),
    frame,
    h('div', { class: 'lp-stage-foot' }, caption, toggle));

  function paintValueText() {
    const pct = Math.round(split);
    range.setAttribute('aria-valuetext', t('landing.stage.valueText', { pct, rest: 100 - pct }));
  }
  refreshers.push(paintValueText);

  function setSplit(value) {
    split = clamp(Math.round(value * 10) / 10, 0, 100);
    view.style.setProperty('--split', `${split}%`);
    if (Number(range.value) !== split) range.value = String(split);
    paintValueText();
  }

  function setOrigin(x, y) {
    view.style.setProperty('--ox', `${x}%`);
    view.style.setProperty('--oy', `${y}%`);
  }

  function setZoom(z) {
    zoom = z;
    view.dataset.zoom = String(z);
    zoomBtns.forEach((b, i) => b.setAttribute('aria-pressed', String(i + 1 === z)));
    if (z === 1) setOrigin(50, 50);
  }

  function pointer(e) {
    const r = view.getBoundingClientRect();
    return {
      x: clamp(((e.clientX - r.left) / r.width) * 100, 0, 100),
      y: clamp(((e.clientY - r.top) / r.height) * 100, 0, 100),
    };
  }
  function track(e) {
    const p = pointer(e);
    if (dragging) setSplit(p.x);
    if (zoom === 2) setOrigin(p.x, p.y);
  }

  view.addEventListener('pointerdown', e => {
    if (e.button !== 0) return;
    dragging = true;
    view.dataset.dragging = '';
    try { view.setPointerCapture(e.pointerId); } catch { /* synthetic pointers */ }
    track(e);
  });
  view.addEventListener('pointermove', track);
  const stop = () => { dragging = false; delete view.dataset.dragging; };
  view.addEventListener('pointerup', stop);
  view.addEventListener('pointercancel', stop);
  view.addEventListener('pointerleave', () => { if (zoom === 2 && !dragging) setOrigin(50, 50); });
  range.addEventListener('input', () => setSplit(Number(range.value)));

  function paintArt() {
    const resolved = resolveTheme(store.get().theme);
    const a = ART[resolved];
    view.dataset.art = resolved;
    if (!restoredImg.src.endsWith(a.restored.slice(1))) {
      restoredImg.width = a.rw; restoredImg.height = a.rh; restoredImg.src = a.restored;
    }
    if (!sourceImg.src.endsWith(a.source.slice(1))) {
      sourceImg.width = a.sw; sourceImg.height = a.sh; sourceImg.src = a.source;
    }
    text(resolution, `${dims(a.sw, a.sh)} → ${dims(a.rw, a.rh)} · ${a.rw / a.sw}×`);
  }

  const mq = window.matchMedia(DARK_QUERY);
  mq.addEventListener('change', paintArt);
  cleanups.push(() => mq.removeEventListener('change', paintArt));
  cleanups.push(store.subscribe(['theme'], paintArt));
  paintValueText();

  return { el };
}

// ── Language menu (same popover pattern as the Studio top bar) ────────────

function createLangMenu({ store, btn, menu, label }) {
  let options = [];

  const isOpen = () => menu.matches(':popover-open');
  function close(restoreFocus = false) {
    if (isOpen()) { try { menu.hidePopover(); } catch { /* ignore */ } }
    if (restoreFocus) btn.focus();
  }
  function choose(pref) {
    store.set({ localePref: pref, locale: resolveLocale(pref) });
    close(true);
  }
  function open() {
    if (isOpen()) { close(false); return; }
    if (!btn.isConnected) return;
    const r = btn.getBoundingClientRect();
    const width = Math.max(168, r.width);
    menu.style.left = `${Math.max(8, Math.min(window.innerWidth - width - 8, r.right - width))}px`;
    menu.style.width = `${width}px`;
    menu.style.top = `${r.bottom + 4}px`;
    try { menu.showPopover(); } catch { return; }
    (options.find(b => b.dataset.value === store.get().localePref) ?? options[0])?.focus();
  }

  const onOutside = e => { if (!btn.contains(e.target) && !menu.contains(e.target)) close(false); };
  const onKey = e => { if (e.key === 'Escape') { e.preventDefault(); close(true); } };
  const onDismiss = () => close(false);
  menu.addEventListener('toggle', e => {
    const opened = e.newState === 'open';
    btn.setAttribute('aria-expanded', String(opened));
    const act = opened ? 'addEventListener' : 'removeEventListener';
    document[act]('pointerdown', onOutside, true);
    document[act]('keydown', onKey, true);
    window[act]('resize', onDismiss);
    window[act]('scroll', onDismiss, true);
  });
  btn.addEventListener('click', open);
  btn.addEventListener('keydown', e => {
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') { e.preventDefault(); open(); }
  });
  menu.addEventListener('keydown', e => {
    if (!options.length) return;
    const at = options.indexOf(document.activeElement);
    const go = i => { e.preventDefault(); options[(i + options.length) % options.length].focus(); };
    if (e.key === 'ArrowDown') go(at + 1);
    else if (e.key === 'ArrowUp') go(at - 1);
    else if (e.key === 'Home') go(0);
    else if (e.key === 'End') go(-1);
  });

  const prefLabel = pref => (pref === 'zh-CN' ? t('lang.zhCN') : pref === 'en' ? t('lang.en') : t('lang.auto'));

  function render() {
    const { localePref } = store.get();
    text(label, prefLabel(localePref));
    const tip = t('lang.tooltip', { label: prefLabel(localePref) });
    btn.title = tip;
    btn.setAttribute('aria-label', tip);
    menu.setAttribute('aria-label', t('lang.label'));
    options = LOCALE_PREFS.map(pref => {
      const selected = pref === localePref;
      return h('button', {
        type: 'button', role: 'option', class: `select-opt${selected ? ' is-selected' : ''}`,
        'aria-selected': String(selected), dataset: { value: pref }, onclick: () => choose(pref),
      },
      h('span', { class: 'select-opt-label truncate' },
        pref === 'auto' ? t('lang.autoWithLocale', { locale: resolveLocale('auto') }) : prefLabel(pref)),
      h('span', { class: 'select-opt-end' }, icon('check', 14, 'select-check')));
    });
    menu.replaceChildren(...options);
  }
  render();

  return { render, destroy: () => close(false) };
}
