// Reusable Swiss UI primitives: segmented controls, tactile switches, hairline
// sliders, flush sections, determinate progress rules, inline confirm, toasts.

import { clamp, h, text } from './dom.js';
import { icon } from './icons.js';
import { sceneLabel } from './format.js';
import { formatNumber, t } from '../i18n/index.js';

let seq = 0;
const nextId = p => `${p}-${++seq}`;

/**
 * Segmented control with a sliding indicator:
 * - kind === 'tabs': sliding 2px bottom ink rule
 * - kind === 'radiogroup': crisp tactile thumb inside a 1px grid frame
 */
export function segmented({ label, options, value, kind = 'radiogroup', size = 'md', fill = false, className = '', onChange }) {
  const isTabs = kind === 'tabs';
  const pill = h('span', { class: 'seg-pill', 'aria-hidden': 'true' });
  const el = h('div', {
    class: `seg seg-${size}${isTabs ? ' seg-tabs' : ''}${fill ? ' seg-fill' : ''} ${className}`.trim(),
    role: isTabs ? 'tablist' : 'radiogroup',
    'aria-label': label,
  }, pill);

  let current = value;
  let items = options;
  let buttons = [];
  let wasShown = false;
  let animatingUntil = 0;

  function place(animate) {
    const active = buttons.find(b => b.dataset.value === String(current));
    if (!active || !active.offsetWidth) {
      wasShown = false;
      pill.style.opacity = '0';
      return;
    }
    const shouldAnimate = animate && wasShown;
    if (shouldAnimate) animatingUntil = performance.now() + 200;
    else pill.style.transition = 'none';
    pill.style.opacity = '1';
    pill.style.width = `${active.offsetWidth}px`;
    pill.style.transform = `translateX(${active.offsetLeft}px)`;
    if (!shouldAnimate) { pill.getBoundingClientRect(); pill.style.transition = ''; }
    wasShown = true;
  }

  function paint() {
    for (const b of buttons) {
      const on = b.dataset.value === String(current);
      b.setAttribute(isTabs ? 'aria-selected' : 'aria-checked', String(on));
      b.tabIndex = on ? 0 : -1;
      b.classList.toggle('is-on', on);
    }
    if (!buttons.some(b => b.tabIndex === 0)) {
      buttons.find(b => !b.disabled && b.getAttribute('aria-disabled') !== 'true')?.setAttribute('tabindex', '0');
    }
  }

  function choose(next, animate = true) {
    if (String(next) === String(current)) return;
    const opt = items.find(o => String(o.value) === String(next));
    current = opt ? opt.value : next;
    paint();
    place(animate);
    onChange?.(current);
  }

  function build(opts) {
    items = opts;
    buttons.forEach(b => b.remove());
    buttons = opts.map(opt => {
      // Standardized segmented option: plain text only — no dots, no icons.
      const b = h('button', {
        type: 'button',
        class: 'seg-btn',
        role: isTabs ? 'tab' : 'radio',
        title: opt.title,
        dataset: { value: String(opt.value) },
        onclick: () => { if (!opt.disabled) choose(opt.value); },
      },
      opt.label != null && h('span', { class: 'seg-label', dataset: { label: String(opt.label) } }, opt.label));
      if (opt.ariaLabel) b.setAttribute('aria-label', opt.ariaLabel);
      if (opt.disabled) { b.setAttribute('aria-disabled', 'true'); b.classList.add('is-disabled'); }
      if (opt.controls) b.setAttribute('aria-controls', opt.controls);
      return b;
    });
    el.append(...buttons);
    paint();
    requestAnimationFrame(() => place(false));
  }

  el.addEventListener('keydown', event => {
    const keys = { ArrowRight: 1, ArrowDown: 1, ArrowLeft: -1, ArrowUp: -1 };
    if (event.key === 'Home' || event.key === 'End' || event.key in keys) {
      const enabled = buttons.filter(b => b.getAttribute('aria-disabled') !== 'true');
      if (!enabled.length) return;
      const at = enabled.indexOf(document.activeElement);
      let next;
      if (event.key === 'Home') next = enabled[0];
      else if (event.key === 'End') next = enabled[enabled.length - 1];
      else next = enabled[(at + keys[event.key] + enabled.length) % enabled.length];
      event.preventDefault();
      next.focus();
      choose(next.dataset.value);
    }
  });

  new ResizeObserver(() => {
    if (wasShown && performance.now() < animatingUntil) {
      place(true);
      return;
    }
    place(false);
  }).observe(el);
  document.fonts?.ready.then(() => place(false));

  build(options);
  return {
    el,
    set(next) { if (String(next) !== String(current)) { current = next; paint(); place(wasShown); } },
    setLabel(nextLabel) { el.setAttribute('aria-label', nextLabel); },
    setOptions(opts) {
      const sig = JSON.stringify(opts.map(o => [o.value, o.label, !!o.disabled, o.title]));
      if (sig === el.dataset.sig) return;
      el.dataset.sig = sig;
      build(opts);
    },
    get value() { return current; },
  };
}

/** Tactile Swiss toggle switch. */
export function switchRow({ label, hint, checked = false, onChange }) {
  const knob = h('span', { class: 'switch-knob' });
  const labelEl = h('span', { class: 'switch-label' }, label);
  const hintEl = hint ? h('span', { class: 'field-hint' }, hint) : null;
  const btn = h('button', {
    type: 'button', class: 'switch-row', role: 'switch', 'aria-checked': String(checked),
    onclick: () => {
      const next = btn.getAttribute('aria-checked') !== 'true';
      btn.setAttribute('aria-checked', String(next));
      onChange?.(next);
    },
  },
  h('span', { class: 'switch-text' }, labelEl, hintEl),
  h('span', { class: 'switch-track' }, knob));
  return {
    el: btn,
    set(v) { btn.setAttribute('aria-checked', String(!!v)); },
    setText({ label: nextLabel, hint: nextHint }) {
      if (nextLabel != null) text(labelEl, nextLabel);
      if (hintEl && nextHint != null) text(hintEl, nextHint);
    },
  };
}

/** Hairline range slider with tabular numeric readout. */
export function rangeRow({ label, min, max, value, step = 1, unit = '', onChange }) {
  const id = nextId('range');
  const labelEl = h('label', { class: 'field-label', htmlFor: id }, label);
  const out = h('output', { class: 'range-out mono', htmlFor: id });
  const input = h('input', { type: 'range', id, class: 'range', min, max, step, value });
  const paint = () => {
    input.style.setProperty('--fill', `${((input.value - min) / (max - min)) * 100}%`);
    const formatted = formatNumber(Number(input.value), { maximumFractionDigits: 0 });
    text(out, `${formatted}${unit}`);
    input.setAttribute('aria-valuetext', `${formatted}${unit}`);
  };
  input.addEventListener('input', () => { paint(); onChange?.(Number(input.value)); });
  paint();
  return {
    el: h('div', { class: 'field-stack' },
      h('div', { class: 'field-head' }, labelEl, out),
      input),
    set(v) { input.value = v; paint(); },
    setLabel(nextLabel) { text(labelEl, nextLabel); },
  };
}

/** Custom Geist popover listbox select matching 6px functional chrome. */
export function selectControl({ label, options, value, onChange }) {
  const menuId = nextId('select');
  const valueEl = h('span', { class: 'select-val truncate' });
  const trigger = h('button', {
    type: 'button',
    class: 'select',
    'aria-haspopup': 'listbox',
    'aria-expanded': 'false',
    'aria-controls': menuId,
    'aria-label': label,
  }, valueEl, icon('chevron-down', 14, 'select-chevron'));

  const menu = h('div', {
    id: menuId,
    class: 'select-menu',
    role: 'listbox',
    popover: 'manual',
    'aria-label': label,
  });

  let current = String(value ?? '');
  let items = [];
  let optionEls = [];
  let sig = '';

  function labelFor(val) {
    return items.find(o => String(o.value) === String(val))?.label ?? String(val ?? '');
  }

  function paint() {
    text(valueEl, labelFor(current));
    for (const btn of optionEls) {
      const on = btn.dataset.value === String(current);
      btn.setAttribute('aria-selected', String(on));
      btn.classList.toggle('is-selected', on);
    }
  }

  function close(restoreFocus = false) {
    if (menu.matches(':popover-open')) {
      try { menu.hidePopover(); } catch { /* ignore */ }
    }
    if (restoreFocus) trigger.focus();
  }

  function choose(val) {
    const next = String(val);
    const changed = next !== String(current);
    current = next;
    paint();
    close(true);
    if (changed) onChange?.(current);
  }

  function open(focusIndex) {
    if (menu.matches(':popover-open')) { close(false); return; }
    if (!trigger.isConnected) return;
    const r = trigger.getBoundingClientRect();
    menu.style.left = `${r.left}px`;
    menu.style.width = `${r.width}px`;
    menu.style.top = `${r.bottom + 4}px`;
    try { menu.showPopover(); } catch { return; }
    const mh = menu.offsetHeight;
    const vh = window.innerHeight;
    const top = r.bottom + 4 + mh <= vh - 8 ? r.bottom + 4 : Math.max(8, r.top - 4 - mh);
    menu.style.top = `${top}px`;
    const target = focusIndex != null
      ? optionEls[focusIndex]
      : (optionEls.find(b => b.dataset.value === String(current) && !b.disabled)
        ?? optionEls.find(b => !b.disabled));
    target?.focus();
  }

  const onOutsidePointer = e => {
    if (!trigger.contains(e.target) && !menu.contains(e.target)) close(false);
  };
  const onScroll = e => {
    if (e.target !== menu) close(false);
  };
  const onResize = () => close(false);
  const onEscape = e => {
    if (e.key === 'Escape') {
      e.preventDefault();
      close(true);
    }
  };
  menu.addEventListener('toggle', e => {
    const isOpen = e.newState === 'open';
    trigger.setAttribute('aria-expanded', String(isOpen));
    trigger.classList.toggle('is-open', isOpen);
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

  trigger.addEventListener('click', () => {
    if (menu.matches(':popover-open')) close(false);
    else open();
  });
  trigger.addEventListener('keydown', e => {
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      e.preventDefault();
      open();
    } else if (e.key === 'Escape' && menu.matches(':popover-open')) {
      e.preventDefault();
      close(true);
    }
  });

  menu.addEventListener('keydown', e => {
    const enabled = optionEls.filter(b => !b.disabled);
    if (!enabled.length) return;
    const at = enabled.indexOf(document.activeElement);
    if (e.key === 'ArrowDown') {
      e.preventDefault();
      enabled[(at + 1) % enabled.length].focus();
    } else if (e.key === 'ArrowUp') {
      e.preventDefault();
      enabled[(at - 1 + enabled.length) % enabled.length].focus();
    } else if (e.key === 'Home') {
      e.preventDefault();
      enabled[0].focus();
    } else if (e.key === 'End') {
      e.preventDefault();
      enabled[enabled.length - 1].focus();
    } else if (e.key === 'Tab') {
      close(false);
    }
  });

  function setOptions(opts) {
    const nextSig = JSON.stringify(opts.map(o => [o.value, o.label, !!o.disabled]));
    if (nextSig === sig) return;
    sig = nextSig;
    items = opts;
    optionEls = opts.map(o => h('button', {
      type: 'button',
      role: 'option',
      class: 'select-opt',
      disabled: !!o.disabled,
      dataset: { value: String(o.value) },
      onclick: () => { if (!o.disabled) choose(o.value); },
    },
    h('span', { class: 'select-opt-label truncate' }, o.label),
    icon('check', 14, 'select-check')));
    menu.replaceChildren(...optionEls);
    paint();
  }

  setOptions(options);
  return {
    el: h('div', { class: 'select-wrap' }, trigger, menu),
    set(next) {
      current = String(next);
      paint();
    },
    setLabel(nextLabel) {
      trigger.setAttribute('aria-label', nextLabel);
      menu.setAttribute('aria-label', nextLabel);
    },
    setOptions,
    get value() { return current; },
  };
}

/** Single-line key/control Swiss row. */
export function rowField(label, control) {
  return h('div', { class: 'field-inline' },
    h('span', { class: 'field-label' }, label),
    control);
}

/** Full-width stacked field. */
export function stackField(label, control, hint, meta) {
  return h('div', { class: 'field-stack' },
    (label || meta) && h('div', { class: 'field-head' },
      label && h('span', { class: 'field-label' }, label),
      meta && h('span', { class: 'field-meta mono' }, meta)),
    control,
    hint && h('p', { class: 'field-hint' }, hint));
}

/** Collapsible disclosure section (collapsed by default) with live summary. */
export function accordion(title, ...children) {
  let open = false;
  const bodyId = nextId('acc');
  const titleEl = h('span', { class: 'sec-title' }, title);
  const summaryEl = h('span', { class: 'acc-summary truncate' });
  const trigger = h('button', {
    type: 'button',
    class: 'acc-trigger',
    'aria-expanded': 'false',
    'aria-controls': bodyId,
    onclick: () => setOpen(!open),
  },
  h('span', { class: 'acc-titles' }, titleEl, summaryEl),
  icon('chevron-right', 14, 'acc-chevron'));
  const body = h('div', { class: 'acc-body', id: bodyId, hidden: true }, ...children);

  function setOpen(next) {
    open = !!next;
    trigger.setAttribute('aria-expanded', String(open));
    body.hidden = !open;
  }

  return {
    el: h('section', { class: 'acc-sec' }, trigger, body),
    setTitle(val) { text(titleEl, val); },
    setSummary(val) { text(summaryEl, val); summaryEl.hidden = !val; },
    setOpen,
  };
}

/** Flush inspector section separated by 1px architectural rule. */
export function section(title, headerEnd, ...children) {
  return h('section', { class: 'pane-sec' },
    (title || headerEnd) && h('div', { class: 'sec-head' },
      title && h('h3', { class: 'sec-title' }, title),
      headerEnd),
    ...children);
}

export function scaleBadge(scale) {
  return h('span', { class: 'tag-meta tag-scale mono', title: t('common.scaleTitle', { scale }) }, `${scale}×`);
}

export function sceneBadge(scene) {
  return h('span', { class: 'tag-meta' }, sceneLabel(scene));
}

export function recBadge() {
  return h('span', { class: 'tag-meta tag-rec' }, t('common.recommended'));
}

export function statusDot(kind) {
  return h('i', { class: `status-dot is-${kind}`, 'aria-hidden': 'true' });
}

export function progressBar(label) {
  const fill = h('i', { class: 'bar-fill' });
  const el = h('div', { class: 'bar', role: 'progressbar', 'aria-valuemin': '0', 'aria-valuemax': '100', 'aria-label': label }, fill);
  return {
    el,
    setLabel(nextLabel) { el.setAttribute('aria-label', nextLabel); },
    set(percentage) {
      const p = clamp(percentage ?? 0, 0, 100);
      fill.style.setProperty('--p', String(p / 100));
      el.setAttribute('aria-valuenow', String(Math.round(p)));
    },
  };
}

/** Destructive button that asks twice inline without modal interruption. */
export function confirmButton({ label, confirmLabel = t('common.confirm'), iconName, className = '', onConfirm, title }) {
  let timer = 0;
  let curLabel = label;
  let curConfirm = confirmLabel;
  const idleSlot = iconName ? icon(iconName, 14) : h('span', {}, label);
  const armedSlot = h('span', { hidden: true }, confirmLabel);
  const btn = h('button', {
    type: 'button', class: className, title: title ?? label, 'aria-label': label,
    onclick: () => {
      if (btn.dataset.armed === 'true') { reset(); onConfirm(); return; }
      arm();
    },
    onblur: e => {
      // Reset only when focus explicitly moves to another element (never on touch/pointer blur).
      if (e.relatedTarget && !btn.contains(e.relatedTarget)) reset();
    },
  }, idleSlot, armedSlot);
  const onOutsidePointer = e => {
    if (!btn.contains(e.target)) reset();
  };
  function arm() {
    btn.dataset.armed = 'true';
    btn.setAttribute('aria-label', `${curConfirm} ${curLabel}`);
    idleSlot.hidden = true;
    armedSlot.hidden = false;
    window.addEventListener('pointerdown', onOutsidePointer, true);
    timer = setTimeout(reset, 4000);
  }
  function reset() {
    clearTimeout(timer);
    window.removeEventListener('pointerdown', onOutsidePointer, true);
    if (!btn.dataset.armed) return;
    delete btn.dataset.armed;
    btn.setAttribute('aria-label', curLabel);
    idleSlot.hidden = false;
    armedSlot.hidden = true;
  }
  btn.setLabels = ({ label: nextLabel, confirmLabel: nextConfirm, title: nextTitle }) => {
    if (nextLabel != null) {
      curLabel = nextLabel;
      if (!iconName) text(idleSlot, nextLabel);
      btn.title = nextTitle ?? nextLabel;
    }
    if (nextConfirm != null) {
      curConfirm = nextConfirm;
      text(armedSlot, nextConfirm);
    }
    btn.setAttribute('aria-label', btn.dataset.armed === 'true' ? `${curConfirm} ${curLabel}` : curLabel);
  };
  return btn;
}

// ── toast ───────────────────────────────────────────────────────────────────
export function createToaster(host) {
  host.classList.add('toasts');
  host.setAttribute('popover', 'manual');
  host.setAttribute('role', 'status');
  host.setAttribute('aria-live', 'polite');
  const syncTopLayer = () => {
    try {
      if (!host.children.length) {
        if (host.matches(':popover-open')) host.hidePopover?.();
      } else if (!host.matches(':popover-open')) {
        host.showPopover?.();
      }
    } catch { /* fallback to z-index when Popover API unavailable */ }
  };
  const dismiss = item => {
    item.remove();
    syncTopLayer();
  };
  return function toast(message, kind = 'info') {
    const item = h('button', { type: 'button', class: `toast is-${kind}`, onclick: () => dismiss(item) },
      h('i', { class: 'toast-mark', 'aria-hidden': 'true' }),
      h('span', {}, message));
    host.append(item);
    while (host.children.length > 3) host.firstElementChild.remove();
    syncTopLayer();
    setTimeout(() => dismiss(item), kind === 'error' ? 7000 : 4000);
  };
}
