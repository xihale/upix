import './styles/index.css';
import { createUpixRuntime } from './runtime/index.js';
import { applyTheme, createInitialState, createStore, persist } from './store.js';
import { createActions } from './actions.js';
import { h } from './ui/dom.js';
import { createToaster } from './ui/controls.js';
import { mountShell } from './ui/shell.js';
import { mountWorkbench } from './ui/workbench.js';
import { mountModels } from './ui/models.js';
import { mountRuntimeLab } from './ui/runtimeLab.js';

const store = createStore(createInitialState());
applyTheme(store.get().theme);

const runtime = createUpixRuntime();

const root = document.getElementById('app');
const toastHost = h('div');
const toast = createToaster(toastHost);
const actions = createActions({ runtime, store, toast });
const shell = mountShell({ root, store, actions, toastHost });

mountWorkbench({ root: shell.views.workbench, store, actions });
mountModels({ root: shell.views.models, store, actions });
mountRuntimeLab({ root: shell.views.runtime, store, actions });

store.subscribe(['settings', 'mirrorBaseUrl', 'theme'], state => {
  applyTheme(state.theme);
  persist(state);
});

// A file dropped outside a drop target must never navigate away from the app.
for (const type of ['dragover', 'drop']) window.addEventListener(type, e => e.preventDefault());

window.addEventListener('pagehide', () => actions.dispose());
actions.init();
