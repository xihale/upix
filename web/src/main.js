import './styles/index.css';
import { createUpixRuntime } from './runtime/index.js';
import { applyLocale, applyTheme, createInitialState, createStore, persist } from './store.js';
import { resolveLocale } from './i18n/index.js';
import { createActions } from './actions.js';
import { h } from './ui/dom.js';
import { createToaster } from './ui/controls.js';
import { mountShell } from './ui/shell.js';
import { mountWorkbench } from './ui/workbench.js';
import { mountModels } from './ui/models.js';
import { mountRuntimeLab } from './ui/runtimeLab.js';

const store = createStore(createInitialState());
applyTheme(store.get().theme);
applyLocale(store.get().locale);

// Update active i18n locale, <html lang>, and <title> BEFORE mounted views react.
store.subscribe(['locale'], ({ locale }) => {
  applyLocale(locale);
});

const runtime = createUpixRuntime();

const root = document.getElementById('app');
const toastHost = h('div');
const toast = createToaster(toastHost);
const actions = createActions({ runtime, store, toast });
const shell = mountShell({ root, store, actions, toastHost });

mountWorkbench({ root: shell.views.workbench, store, actions });
mountModels({ root: shell.views.models, store, actions });
mountRuntimeLab({ root: shell.views.runtime, store, actions });

store.subscribe(['settings', 'mirrorBaseUrl', 'theme', 'localePref'], state => {
  applyTheme(state.theme);
  persist(state);
});

let lastAppliedLocale = store.get().locale;
store.subscribe(['locale'], ({ locale, capsLoading, modelsLoading }) => {
  if (locale !== lastAppliedLocale) {
    lastAppliedLocale = locale;
    // Re-probe capabilities and re-read model list so backend/runtime messages reflect the new locale.
    if (!capsLoading) actions.refreshCaps({ silent: true });
    if (!modelsLoading) actions.refreshModels({ remote: false });
  }
});

window.addEventListener('languagechange', () => {
  const state = store.get();
  if (state.localePref !== 'auto') return;
  const nextLocale = resolveLocale('auto');
  if (nextLocale !== state.locale) store.set({ locale: nextLocale });
});

// A file dropped outside a drop target must never navigate away from the app.
for (const type of ['dragover', 'drop']) window.addEventListener(type, e => e.preventDefault());

window.addEventListener('pagehide', () => actions.dispose());
actions.init();
