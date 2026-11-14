// Minimal observable store. State is replaced immutably, so subscribers can
// compare by reference and re-render only the slice they care about.

export function createStore(initial) {
  let state = initial;
  const subs = new Set();
  return {
    get: () => state,
    set(patch) {
      const prev = state;
      state = { ...prev, ...(typeof patch === 'function' ? patch(prev) : patch) };
      for (const sub of [...subs]) sub(state, prev);
    },
    /** Calls fn immediately, then whenever one of `keys` changes by reference. */
    subscribe(keys, fn) {
      const sub = (next, prev) => {
        if (keys.some(k => next[k] !== prev[k])) fn(next, prev);
      };
      subs.add(sub);
      fn(state, state);
      return () => subs.delete(sub);
    },
  };
}

const STORAGE_KEY = 'upix.web.settings.v1';
const THEME_KEY = 'upix.web.theme';
const MIRROR_KEY = 'upix.web.mirror';

export const DEFAULT_SETTINGS = {
  modelName: '',
  backend: 'webgpu',
  webnnDevice: 'gpu',
  webnnPower: 'default',
  outputFormat: 'follow',
  quality: 90,
  jpegSubsampling: '420',
  pngLevel: 'auto',
  webpLossless: false,
  tileSize: 'auto',
  tileOverlap: 16,
  wasmThreads: 'auto',
  preferFp16: true,
  force: false,
  pixelated: false,
};

function read(key) {
  try { return localStorage.getItem(key); } catch { return null; }
}
function write(key, value) {
  try { localStorage.setItem(key, value); } catch { /* storage blocked: session-only */ }
}
function remove(key) {
  try { localStorage.removeItem(key); } catch { /* ignore */ }
}

export function applyTheme(theme) {
  const root = document.documentElement;
  if (theme === 'light' || theme === 'dark') root.dataset.theme = theme;
  else delete root.dataset.theme;
}

export function loadPersisted() {
  let settings = { ...DEFAULT_SETTINGS };
  try {
    const saved = JSON.parse(read(STORAGE_KEY) ?? '{}');
    for (const key of Object.keys(DEFAULT_SETTINGS)) {
      if (key in saved && typeof saved[key] === typeof DEFAULT_SETTINGS[key]) settings[key] = saved[key];
    }
  } catch { settings = { ...DEFAULT_SETTINGS }; }
  const savedTheme = read(THEME_KEY);
  const theme = savedTheme === 'light' || savedTheme === 'dark' ? savedTheme : 'system';
  return {
    theme,
    settings,
    mirrorBaseUrl: read(MIRROR_KEY) ?? '',
  };
}

export function persist(state) {
  write(STORAGE_KEY, JSON.stringify(state.settings));
  write(MIRROR_KEY, state.mirrorBaseUrl);
  if (state.theme === 'light' || state.theme === 'dark') write(THEME_KEY, state.theme);
  else remove(THEME_KEY);
}

export function createInitialState() {
  const persisted = loadPersisted();
  return {
    tab: 'workbench',
    ...persisted,
    caps: null,
    capsLoading: true,
    bootError: '',

    models: [],
    modelsLoading: true,
    remoteError: '',
    storageUsed: 0,
    downloads: {}, // name -> { loaded, total, phase } | { error }

    queue: [],
    activeId: null,
    running: false,
    progress: null,

    lab: { webnn: {}, bench: null },
  };
}
