// Tiny DOM toolkit: hyperscript, keyed list reconciliation, a few helpers.

import { t } from '../i18n/index.js';

export function h(tag, props, ...children) {
  const el = document.createElement(tag);
  if (props) {
    for (const [key, value] of Object.entries(props)) {
      if (value == null || value === false) continue;
      if (key === 'class') el.className = value;
      else if (key === 'style' && typeof value === 'object') {
        for (const [sk, sv] of Object.entries(value)) {
          if (sv == null) continue;
          if (sk.startsWith('--')) el.style.setProperty(sk, String(sv));
          else el.style[sk] = sv;
        }
      }
      else if (key === 'dataset') Object.assign(el.dataset, value);
      else if (key.startsWith('on') && typeof value === 'function') {
        el.addEventListener(key.slice(2).toLowerCase(), value);
      } else if (key in el && !key.includes('-') && key !== 'list') el[key] = value;
      else el.setAttribute(key, value === true ? '' : value);
    }
  }
  append(el, children);
  return el;
}

export function append(parent, children) {
  for (const child of children.flat(Infinity)) {
    if (child == null || child === false) continue;
    parent.append(child instanceof Node ? child : document.createTextNode(String(child)));
  }
  return parent;
}

export function replace(parent, ...children) {
  parent.replaceChildren();
  return append(parent, children);
}

export function text(el, value) {
  if (el.textContent !== value) el.textContent = value;
}

export function attr(el, name, value) {
  if (value == null || value === false) el.removeAttribute(name);
  else el.setAttribute(name, value === true ? '' : String(value));
}

/**
 * Keyed list reconciliation. `create(item)` returns `{ el, update(item) }`;
 * existing rows are updated in place so focus, hover and scroll survive.
 */
export function reconcile(parent, items, getKey, create, cache) {
  const seen = new Set();
  let cursor = parent.firstElementChild;
  for (const item of items) {
    const key = getKey(item);
    seen.add(key);
    let row = cache.get(key);
    if (!row) {
      row = create(item);
      cache.set(key, row);
    }
    row.update(item);
    if (row.el === cursor) cursor = cursor.nextElementSibling;
    else parent.insertBefore(row.el, cursor);
  }
  for (const [key, row] of cache) {
    if (!seen.has(key)) {
      row.dispose?.();
      row.el.remove();
      cache.delete(key);
    }
  }
}

export function clamp(value, min, max) {
  return Math.min(max, Math.max(min, value));
}

export function sleep(ms, signal) {
  return new Promise((resolve, reject) => {
    if (signal?.aborted) return reject(abortError());
    const timer = setTimeout(() => { signal?.removeEventListener('abort', onAbort); resolve(); }, ms);
    const onAbort = () => { clearTimeout(timer); reject(abortError()); };
    signal?.addEventListener('abort', onAbort, { once: true });
  });
}

export function abortError() {
  return new DOMException(t('common.cancelled'), 'AbortError');
}

export function isAbort(error) {
  return error?.name === 'AbortError';
}

export function downloadBlob(blob, filename) {
  const url = URL.createObjectURL(blob);
  const a = h('a', { href: url, download: filename });
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 10_000);
}
