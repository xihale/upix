// Engine view: 3-column Swiss technical specification matrix (WebGPU, WASM CPU,
// WebNN) and a clean horizontal tile benchmark.

import { h, reconcile, replace, text } from './dom.js';
import { icon } from './icons.js';
import { benchRows, formatBytes, formatNum } from './format.js';
import { segmented, statusDot } from './controls.js';
import { formatNumber, t } from '../i18n/index.js';

const FLAG_URL = 'chrome://flags/#web-machine-learning-neural-network';

function kv(label, value) {
  return [
    h('dt', {}, label),
    h('dd', { class: 'mono' }, value),
  ];
}

function stateText(ok, yes = t('common.supported'), no = '—') {
  return h('span', { class: ok ? 'val-ok' : 'muted' }, ok ? yes : no);
}

function specColumn(id, title, tagText) {
  const status = h('span', { class: 'spec-status mono' });
  const tag = tagText !== undefined ? h('span', { class: 'tag-meta' }, tagText) : null;
  const body = h('div', { class: 'spec-body' });
  const foot = h('div', { class: 'spec-foot' });
  const el = h('section', { class: 'spec-col', 'aria-labelledby': `spec-${id}` },
    h('header', { class: 'spec-head' },
      h('div', { class: 'spec-titles' },
        h('h2', { id: `spec-${id}` }, title),
        tag),
      status),
    body,
    foot);
  return { el, status, tag, body, foot };
}

function paintStatus(host, cap, loading) {
  const kind = loading && !cap ? 'wait' : cap?.available ? 'ok' : 'off';
  host.replaceChildren(
    statusDot(kind),
    loading && !cap ? t('common.checking') : cap?.available ? t('common.available') : t('common.unavailable'),
  );
}

export function mountRuntimeLab({ root, store, actions }) {
  root.classList.add('page');

  const gpu = specColumn('webgpu', 'WebGPU');
  const wasm = specColumn('wasm', 'WASM CPU');
  const nn = specColumn('webnn', 'WebNN', t('lab.betaTag'));

  const flagCopyBtn = h('button', {
    type: 'button', class: 'icon-btn icon-btn-sm', title: t('common.copy'), 'aria-label': t('lab.copyFlagAria'),
    onclick: async e => {
      const btn = e.currentTarget;
      try { await navigator.clipboard.writeText(FLAG_URL); } catch { return; }
      btn.replaceChildren(icon('check', 14));
      setTimeout(() => btn.replaceChildren(icon('copy', 14)), 1400);
    },
  }, icon('copy', 14));

  const guideSummary = h('span', {}, t('lab.howToEnable'));
  const guidePrompt = h('p', { class: 'field-hint' }, t('lab.webnnFlagPrompt'));
  const guide = h('details', { class: 'disclosure' },
    h('summary', {}, guideSummary),
    h('div', { class: 'guide-box' },
      guidePrompt,
      h('div', { class: 'flag-row' },
        h('code', { class: 'flag mono' }, FLAG_URL),
        flagCopyBtn)));
  nn.foot.append(guide);

  // ── Benchmark ────────────────────────────────────────────────────────────
  const tileSeg = segmented({
    label: t('lab.tileSizeLabel'), size: 'sm', value: '128',
    options: ['64', '128', '256'].map(v => ({ value: v, label: `${v}px` })),
    onChange: () => {},
  });
  const runBtn = h('button', {
    type: 'button', class: 'btn btn-primary btn-sm',
    onclick: () => actions.runBenchmark(Number(tileSeg.value)),
  }, t('lab.run'));
  const benchTitle = h('h2', { id: 'bench-title' }, t('lab.benchTitle'));
  const benchMeta = h('p', { class: 'page-sub mono' });
  const benchRowsHost = h('div', { class: 'bench-rows' });
  const bench = h('section', { class: 'bench-sec', 'aria-labelledby': 'bench-title' },
    h('header', { class: 'bench-head' },
      h('div', { class: 'page-titles' },
        benchTitle,
        benchMeta),
      h('div', { class: 'bench-controls' }, tileSeg.el, runBtn)),
    benchRowsHost);

  const pageTitle = h('h1', {}, t('lab.title'));
  const recheckBtn = h('button', {
    type: 'button', class: 'btn btn-secondary btn-sm', onclick: () => actions.refreshCaps(),
  }, t('lab.recheck'));

  root.append(
    h('header', { class: 'page-head' },
      h('div', { class: 'page-head-row' },
        h('div', { class: 'page-titles' }, pageTitle),
        recheckBtn)),
    h('div', { class: 'spec-matrix' }, gpu.el, wasm.el, nn.el),
    bench);

  function renderCaps({ caps, capsLoading, lab }) {
    paintStatus(gpu.status, caps?.webgpu, capsLoading);
    paintStatus(wasm.status, caps?.wasm, capsLoading);
    paintStatus(nn.status, caps?.webnn, capsLoading);

    const g = caps?.webgpu;
    const gf = g?.features ?? {};
    replace(gpu.body,
      h('dl', { class: 'kv' },
        ...kv(t('lab.adapter'), g?.deviceName ?? '—'),
        ...kv('FP16', stateText(!!gf.fp16, t('common.supported'), t('lab.fp16OnlyFp32'))),
        ...kv(t('lab.maxBuffer'), gf.maxBufferSize ? formatBytes(gf.maxBufferSize) : '—'),
        ...kv(t('lab.maxStorageBinding'), gf.maxStorageBufferBindingSize ? formatBytes(gf.maxStorageBufferBindingSize) : '—')),
      g && !g.available && g.reason ? h('p', { class: 'spec-note' }, g.reason) : null);

    const w = caps?.wasm;
    const wf = w?.features ?? {};
    replace(wasm.body,
      h('dl', { class: 'kv' },
        ...kv('SIMD128', stateText(!!wf.simd128)),
        ...kv(t('lab.logicalCores'), navigator.hardwareConcurrency != null ? formatNumber(navigator.hardwareConcurrency) : '—'),
        ...kv(t('lab.workerThreads'), formatNumber(wf.threads ?? 1)),
        ...kv(t('lab.crossOriginIsolated'), stateText(!!wf.crossOriginIsolated, t('common.enabled'), t('common.disabled')))),
      w && !w.available && w.reason ? h('p', { class: 'spec-note' }, w.reason) : null);

    const n = caps?.webnn;
    const hasMl = 'ml' in navigator;
    const devs = n?.features?.webnnDevices;
    const devRow = d => {
      const probe = lab.webnn[d];
      let val;
      if (probe?.status === 'probing') val = h('span', { class: 'muted' }, t('lab.probing'));
      else if (probe) val = stateText(probe.status === 'ok', t('common.ready'), t('common.unavailable'));
      else val = stateText(!!devs?.[d], t('common.ready'), '—');
      return [h('dt', {}, d.toUpperCase()), h('dd', { class: 'mono', title: probe?.reason ?? undefined }, val)];
    };
    replace(nn.body,
      h('dl', { class: 'kv' },
        ...kv('navigator.ml', stateText(hasMl, t('common.supported'), t('common.disabled'))),
        ...devRow('gpu'),
        ...devRow('npu'),
        ...devRow('cpu')),
      n && !n.available && n.reason ? h('p', { class: 'spec-note' }, n.reason) : null);
  }

  const benchCache = new Map();

  function createBenchRow() {
    const labelEl = h('span', { class: 'bench-label' });
    const barFill = h('i', { class: 'bar-fill', style: { '--p': '0' } });
    const barEl = h('span', { class: 'bar bench-bar', role: 'img' }, barFill);
    const msVal = h('b');
    const tpsVal = h('b');
    const mpixVal = h('b');
    const msUnit = h('span');
    const tpsUnit = h('span');
    const mpixUnit = h('span');
    const metricsEl = h('span', { class: 'bench-values mono', hidden: true },
      h('span', {}, msVal, msUnit),
      h('span', {}, tpsVal, tpsUnit),
      h('span', {}, mpixVal, mpixUnit));
    const noteEl = h('span', { class: 'bench-values bench-note', hidden: true });
    const valuesHost = h('span', { class: 'bench-values-host' }, metricsEl, noteEl);
    const el = h('div', { class: 'bench-row' }, labelEl, barEl, valuesHost);
    return {
      el,
      update(r) {
        text(labelEl, r.label);
        el.classList.toggle('is-off', r.status === 'off');
        el.dataset.status = r.status;
        barEl.classList.toggle('is-best', r.isBest);
        barEl.setAttribute('aria-label', t('lab.benchBarAria', { label: r.label, pct: Math.round(r.pct) }));
        barFill.style.setProperty('--p', String(r.pct / 100));
        const done = r.status === 'done';
        metricsEl.hidden = !done;
        noteEl.hidden = done;
        if (done) {
          text(msVal, formatNum(r.avgTileMs));
          text(msUnit, ` ${t('lab.unitMsTile')}`);
          text(tpsVal, formatNum(r.tilesPerSec));
          text(tpsUnit, ` ${t('lab.unitTilesSec')}`);
          text(mpixVal, formatNum(r.mpixPerSec, 2));
          text(mpixUnit, ` ${t('lab.unitMpSec')}`);
        } else {
          noteEl.classList.toggle('is-error', r.status === 'error');
          if (r.reason) noteEl.title = r.reason; else noteEl.removeAttribute('title');
          text(noteEl, r.status ? t(`lab.status.${r.status}`) : '');
        }
      },
    };
  }

  function renderBench({ caps, lab, settings }) {
    const b = lab.bench;
    runBtn.disabled = !caps || !!b?.running;
    text(runBtn, b?.running ? t('lab.running') : b ? t('lab.rerun') : t('lab.run'));
    text(benchMeta, `${settings.modelName || t('lab.defaultModel')} · ${b?.tileSize ?? tileSeg.value}px`);
    const rows = benchRows(caps, lab).map(r => ({
      ...r,
      ...(b?.rows[r.id] ?? ((r.available || r.probing) ? { status: 'idle' } : { status: 'off', reason: r.reason })),
    }));
    const done = rows.filter(r => r.status === 'done');
    const best = Math.max(0, ...done.map(r => r.tilesPerSec));
    const fastest = done.find(r => r.tilesPerSec === best)?.id;
    const items = rows.map(r => ({
      ...r,
      pct: r.status === 'done' && best ? (r.tilesPerSec / best) * 100 : 0,
      isBest: r.id === fastest,
    }));
    reconcile(benchRowsHost, items, r => r.id, createBenchRow, benchCache);
  }

  store.subscribe(['locale'], () => {
    text(pageTitle, t('lab.title'));
    text(recheckBtn, t('lab.recheck'));
    if (nn.tag) text(nn.tag, t('lab.betaTag'));
    flagCopyBtn.title = t('common.copy');
    flagCopyBtn.setAttribute('aria-label', t('lab.copyFlagAria'));
    text(guideSummary, t('lab.howToEnable'));
    text(guidePrompt, t('lab.webnnFlagPrompt'));
    tileSeg.setLabel(t('lab.tileSizeLabel'));
    text(benchTitle, t('lab.benchTitle'));
  });

  store.subscribe(['caps', 'capsLoading', 'lab', 'locale'], renderCaps);
  store.subscribe(['caps', 'lab', 'settings', 'locale'], renderBench);

  // Auto-probe WebNN devices once, on first visit to the Engine tab.
  let deviceProbed = false;
  store.subscribe(['tab'], ({ tab }) => {
    if (tab !== 'runtime' || deviceProbed) return;
    deviceProbed = true;
    actions.probeAllWebNn();
  });
}
