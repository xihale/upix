// IndexedDB-backed workspace persistence (`upix-workspace-v1`).
// Queue items — input File/Blob + processing result Blob — survive page
// refreshes. A record is deleted the moment its output is downloaded, so the
// next refresh destroys exactly the images the user already saved.

import { t } from '../i18n/index.js';

const DB_NAME = 'upix-workspace-v1';
const DB_VERSION = 1;
const STORE_ITEMS = 'items';

function openDb() {
  return new Promise((resolve, reject) => {
    if (typeof indexedDB === 'undefined') {
      reject(new Error(t('runtime.idbUnsupported')));
      return;
    }
    const req = indexedDB.open(DB_NAME, DB_VERSION);
    req.onupgradeneeded = () => {
      const db = req.result;
      if (!db.objectStoreNames.contains(STORE_ITEMS)) {
        db.createObjectStore(STORE_ITEMS, { keyPath: 'id' });
      }
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error || new Error(t('runtime.idbOpenFailed')));
  });
}

async function withStore(mode, fn) {
  const db = await openDb();
  try {
    return await new Promise((resolve, reject) => {
      const tx = db.transaction(STORE_ITEMS, mode);
      const result = fn(tx.objectStore(STORE_ITEMS));
      tx.oncomplete = () => resolve(result);
      tx.onabort = () => reject(tx.error || new Error(t('runtime.wsAborted')));
      tx.onerror = () => reject(tx.error || new Error(t('runtime.wsFailed')));
    });
  } finally {
    db.close();
  }
}

/** All persisted queue records, oldest first. */
export async function loadWorkspaceItems() {
  const records = await withStore('readonly', (store) => {
    const all = [];
    store.openCursor().onsuccess = (event) => {
      const cursor = event.target.result;
      if (cursor) {
        all.push(cursor.value);
        cursor.continue();
      }
    };
    return all;
  });
  return records.sort((a, b) => a.id - b.id);
}

export function putWorkspaceItem(record) {
  return withStore('readwrite', (store) => store.put(record));
}

export function deleteWorkspaceItems(ids) {
  if (!ids.length) return Promise.resolve();
  return withStore('readwrite', (store) => {
    for (const id of ids) store.delete(id);
  });
}

export function clearWorkspaceItems() {
  return withStore('readwrite', (store) => store.clear());
}
