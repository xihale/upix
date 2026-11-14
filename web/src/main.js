import './styles/index.css';
import { applyLocale, applyTheme, createInitialState, createStore, persist } from './store.js';
import { resolveLocale } from './i18n/index.js';
import { h } from './ui/dom.js';
import { mountLanding } from './ui/landing.js';

const STUDIO_TABS = new Set(['workbench', 'models', 'runtime']);
const hashName = () => location.hash.replace(/^#/, '');
const isStudioHash = () => STUDIO_TABS.has(hashName());

const store = createStore(createInitialState());
applyTheme(store.get().theme);
applyLocale(store.get().locale);

// Update active i18n locale, <html lang>, and <title> BEFORE mounted views react.
store.subscribe(['locale'], ({ locale }) => {
  applyLocale(locale);
});

store.subscribe(['settings', 'mirrorBaseUrl', 'theme', 'localePref'], state => {
  applyTheme(state.theme);
  persist(state);
});

window.addEventListener('languagechange', () => {
  const state = store.get();
  if (state.localePref !== 'auto') return;
  const nextLocale = resolveLocale('auto');
  if (nextLocale !== state.locale) store.set({ locale: nextLocale });
});

const landingRoot = document.getElementById('landing');
const appRoot = document.getElementById('app');

let landingMounted = false;
let studioPromise = null;
let studioActions = null;

function ensureLanding() {
  if (landingMounted || !landingRoot) return;
  landingMounted = true;
  mountLanding({ root: landingRoot, store });
}

function ensureStudio() {
  if (studioPromise || !appRoot) return studioPromise;
  studioPromise = (async () => {
    const [
      { createUpixRuntime },
      { createActions },
      { createToaster },
      { mountShell },
      { mountWorkbench },
      { mountModels },
      { mountRuntimeLab },
    ] = await Promise.all([
      import('./runtime/index.js'),
      import('./actions.js'),
      import('./ui/controls.js'),
      import('./ui/shell.js'),
      import('./ui/workbench.js'),
      import('./ui/models.js'),
      import('./ui/runtimeLab.js'),
    ]);

    const runtime = createUpixRuntime();
    const toastHost = h('div');
    const toast = createToaster(toastHost);
    const actions = createActions({ runtime, store, toast });
    studioActions = actions;

    const currentTab = hashName();
    if (STUDIO_TABS.has(currentTab) && store.get().tab !== currentTab) {
      store.set({ tab: currentTab });
    }

    const shell = mountShell({ root: appRoot, store, actions, toastHost });
    mountWorkbench({ root: shell.views.workbench, store, actions });
    mountModels({ root: shell.views.models, store, actions });
    mountRuntimeLab({ root: shell.views.runtime, store, actions });

    let lastAppliedLocale = store.get().locale;
    store.subscribe(['locale'], ({ locale, capsLoading, modelsLoading }) => {
      if (locale !== lastAppliedLocale) {
        lastAppliedLocale = locale;
        // Re-probe capabilities and re-read model list so backend/runtime messages reflect the new locale.
        if (!capsLoading) actions.refreshCaps({ silent: true });
        if (!modelsLoading) actions.refreshModels({ remote: false });
      }
    });

    actions.init();
    return actions;
  })().catch(err => {
    studioPromise = null;
    throw err;
  });
  return studioPromise;
}

function syncRoute() {
  if (isStudioHash()) {
    const tab = hashName();
    if (store.get().tab !== tab) store.set({ tab });
    if (landingRoot) landingRoot.hidden = true;
    if (appRoot) appRoot.hidden = false;
    ensureStudio();
  } else {
    if (appRoot) appRoot.hidden = true;
    if (landingRoot) landingRoot.hidden = false;
    ensureLanding();
    const hName = hashName();
    if (landingRoot && (!hName || hName === 'top')) {
      landingRoot.scrollTop = 0;
    }
  }
}

window.addEventListener('hashchange', syncRoute);
syncRoute();

// A file dropped outside a drop target must never navigate away from the app.
for (const type of ['dragover', 'drop']) window.addEventListener(type, e => e.preventDefault());

window.addEventListener('pagehide', () => studioActions?.dispose());
