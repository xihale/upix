// Engine view: 3-column Swiss technical specification matrix (WebGPU, WASM CPU,
// WebNN) and a clean horizontal tile benchmark.

import { h, reconcile, replace, text } from './dom.js';
import { icon } from './icons.js';
import { benchRows, formatBytes, formatNum } from './format.js';
import { segmented, statusDot } from './controls.js';

const FLAG_URL = 'chrome://flags/#web-machine-learning-neural-network';

function kv(label, value) {
  return [
    h('dt', {}, label),
    h('dd', { class: 'mono' }, value),
  ];
}

function stateText(ok, yes = '支持', no = '—') {
  return h('span', { class: ok ? 'val-ok' : 'muted' }, ok ? yes : no);
}

function specColumn(id, title, tagText) {
  const status = h('span', { class: 'spec-status mono' });
  const body = h('div', { class: 'spec-body' });
  const foot = h('div', { class: 'spec-foot' });
  const el = h('section', { class: 'spec-col', 'aria-labelledby': `spec-${id}` },
    h('header', { class: 'spec-head' },
      h('div', { class: 'spec-titles' },
        h('h2', { id: `spec-${id}` }, title),
        tagText && h('span', { class: 'tag-meta' }, tagText)),
      status),
    body,
    foot);
  return { el, status, body, foot };
}

function paintStatus(host, cap, loading) {
  const kind = loading && !cap ? 'wait' : cap?.available ? 'ok' : 'off';
  host.replaceChildren(statusDot(kind), loading && !cap ? '检测中' : cap?.available ? '可用' : '不可用');
}

export function mountRuntimeLab({ root, store, actions }) {
  root.classList.add('page');

  const gpu = specColumn('webgpu', 'WebGPU');
  const wasm = specColumn('wasm', 'WASM CPU');
  const nn = specColumn('webnn', 'WebNN', '实验');

  const flagCopyBtn = h('button', {
    type: 'button', class: 'icon-btn icon-btn-sm', title: '复制', 'aria-label': '复制 flag 地址',
    onclick: async e => {
      const btn = e.currentTarget;
      try { await navigator.clipboard.writeText(FLAG_URL); } catch { return; }
      btn.replaceChildren(icon('check', 14));
      setTimeout(() => btn.replaceChildren(icon('copy', 14)), 1400);
    },
  }, icon('copy', 14));

  const guide = h('details', { class: 'disclosure' },
    h('summary', {}, h('span', {}, '启用方式')),
    h('div', { class: 'guide-box' },
      h('p', { class: 'field-hint' }, 'Chrome / Edge 中设为 Enabled 并重启：'),
      h('div', { class: 'flag-row' },
        h('code', { class: 'flag mono' }, FLAG_URL),
        flagCopyBtn)));
  nn.foot.append(guide);

  // ── Benchmark ────────────────────────────────────────────────────────────
  const tileSeg = segmented({
    label: '分块尺寸', size: 'sm', value: '128',
    options: ['64', '128', '256'].map(v => ({ value: v, label: `${v}px` })),
    onChange: () => {},
  });
  const runBtn = h('button', {
    type: 'button', class: 'btn btn-primary btn-sm',
    onclick: () => actions.runBenchmark(Number(tileSeg.value)),
  }, '运行');
  const benchMeta = h('p', { class: 'page-sub mono' });
  const benchRowsHost = h('div', { class: 'bench-rows' });
  const bench = h('section', { class: 'bench-sec', 'aria-labelledby': 'bench-title' },
    h('header', { class: 'bench-head' },
      h('div', { class: 'page-titles' },
        h('h2', { id: 'bench-title' }, '吞吐基准'),
        benchMeta),
      h('div', { class: 'bench-controls' }, tileSeg.el, runBtn)),
    benchRowsHost);

  root.append(
    h('header', { class: 'page-head' },
      h('div', { class: 'page-head-row' },
        h('div', { class: 'page-titles' },
          h('h1', {}, '推理引擎')),
        h('button', {
          type: 'button', class: 'btn btn-secondary btn-sm', onclick: () => actions.refreshCaps(),
        }, '重新检测'))),
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
        ...kv('适配器', g?.deviceName ?? '—'),
        ...kv('FP16', stateText(!!gf.fp16, '支持', '仅 FP32')),
        ...kv('最大缓冲区', gf.maxBufferSize ? formatBytes(gf.maxBufferSize) : '—'),
        ...kv('存储绑定上限', gf.maxStorageBufferBindingSize ? formatBytes(gf.maxStorageBufferBindingSize) : '—')),
      g && !g.available && g.reason ? h('p', { class: 'spec-note' }, g.reason) : null);

    const w = caps?.wasm;
    const wf = w?.features ?? {};
    replace(wasm.body,
      h('dl', { class: 'kv' },
        ...kv('SIMD128', stateText(!!wf.simd128)),
        ...kv('逻辑核心', `${navigator.hardwareConcurrency ?? '—'}`),
        ...kv('并发线程', String(wf.threads ?? 1)),
        ...kv('跨源隔离', stateText(!!wf.crossOriginIsolated, '已启用', '未启用'))),
      w && !w.available && w.reason ? h('p', { class: 'spec-note' }, w.reason) : null);

    const n = caps?.webnn;
    const hasMl = 'ml' in navigator;
    const devs = n?.features?.webnnDevices;
    const devRow = d => {
      const probe = lab.webnn[d];
      let val;
      if (probe?.status === 'probing') val = h('span', { class: 'muted' }, '探测中…');
      else if (probe) val = stateText(probe.status === 'ok', '就绪', '不可用');
      else val = stateText(!!devs?.[d], '就绪', '—');
      return [h('dt', {}, d.toUpperCase()), h('dd', { class: 'mono', title: probe?.reason ?? undefined }, val)];
    };
    replace(nn.body,
      h('dl', { class: 'kv' },
        ...kv('navigator.ml', stateText(hasMl, '支持', '未启用')),
        ...devRow('gpu'),
        ...devRow('npu'),
        ...devRow('cpu')),
      n && !n.available && n.reason ? h('p', { class: 'spec-note' }, n.reason) : null);
  }

  const benchCache = new Map();
  const STATUS_TEXT = { idle: '待测', pending: '排队', running: '测试中…', off: '不可用', error: '失败' };

  function createBenchRow() {
    const labelEl = h('span', { class: 'bench-label' });
    const barFill = h('i', { class: 'bar-fill', style: { '--p': '0' } });
    const barEl = h('span', { class: 'bar bench-bar', role: 'img' }, barFill);
    const msVal = h('b');
    const tpsVal = h('b');
    const mpixVal = h('b');
    const metricsEl = h('span', { class: 'bench-values mono', hidden: true },
      h('span', {}, msVal, ' ms/块'),
      h('span', {}, tpsVal, ' 块/s'),
      h('span', {}, mpixVal, ' MP/s'));
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
        barEl.setAttribute('aria-label', `${r.label} 相对速度 ${Math.round(r.pct)}%`);
        barFill.style.setProperty('--p', String(r.pct / 100));
        const done = r.status === 'done';
        metricsEl.hidden = !done;
        noteEl.hidden = done;
        if (done) {
          text(msVal, formatNum(r.avgTileMs));
          text(tpsVal, formatNum(r.tilesPerSec));
          text(mpixVal, formatNum(r.mpixPerSec, 2));
        } else {
          noteEl.classList.toggle('is-error', r.status === 'error');
          if (r.reason) noteEl.title = r.reason; else noteEl.removeAttribute('title');
          text(noteEl, STATUS_TEXT[r.status] ?? '');
        }
      },
    };
  }

  function renderBench({ caps, lab, settings }) {
    const b = lab.bench;
    runBtn.disabled = !caps || !!b?.running;
    text(runBtn, b?.running ? '测试中…' : b ? '重新运行' : '运行');
    text(benchMeta, `${settings.modelName || '默认'} · ${b?.tileSize ?? tileSeg.value}px`);
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

  store.subscribe(['caps', 'capsLoading', 'lab'], renderCaps);
  store.subscribe(['caps', 'lab', 'settings'], renderBench);

  // Auto-probe WebNN devices once, on first visit to the Engine tab.
  let deviceProbed = false;
  store.subscribe(['tab'], ({ tab }) => {
    if (tab !== 'runtime' || deviceProbed) return;
    deviceProbed = true;
    actions.probeAllWebNn();
  });
}
