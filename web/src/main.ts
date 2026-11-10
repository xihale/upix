import { fetchManifest, fetchModel } from './manifest';
import { createSession, probeWebGpu, hasWebNn, type ExecutionProvider } from './pipeline/session';
import { decodeToRgba, upscaleImage, type ModelInfo } from './engine';
import { runBench } from './bench';
import type { InferenceSession } from 'onnxruntime-web/webgpu';

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;

const els = {
  drop: $('drop') as HTMLDivElement,
  file: $('file') as HTMLInputElement,
  model: $('model') as HTMLSelectElement,
  ep: $('ep') as HTMLSelectElement,
  tile: $('tile') as HTMLInputElement,
  overlap: $('overlap') as HTMLInputElement,
  run: $('run') as HTMLButtonElement,
  cancel: $('cancel') as HTMLButtonElement,
  download: $('download') as HTMLButtonElement,
  status: $('status') as HTMLSpanElement,
  barFill: $('bar-fill') as HTMLDivElement,
  notes: $('notes') as HTMLDivElement,
  compare: $('compare') as HTMLElement,
  frame: $('frame') as HTMLDivElement,
  before: $('before') as HTMLImageElement,
  beforeWrap: $('before-wrap') as HTMLDivElement,
  after: $('after') as HTMLImageElement,
  handle: $('handle') as HTMLDivElement,
  meta: $('meta') as HTMLParagraphElement,
};

let manifest: Awaited<ReturnType<typeof fetchManifest>> | null = null;
let bitmap: ImageBitmap | null = null;
let session: InferenceSession | null = null;
let sessionKey = '';
let abort: AbortController | null = null;
let resultBlob: Blob | null = null;

function setStatus(msg: string): void {
  els.status.textContent = msg;
}

function setNotes(lines: string[]): void {
  els.notes.textContent = lines.join('\n');
}

// --- capability badges ------------------------------------------------------
// WebGPU is reported only when an adapter really exists (see probeWebGpu).
void probeWebGpu().then((canGpu) => {
  const caps: string[] = [];
  caps.push(canGpu ? 'WebGPU ✓' : 'WebGPU ✗');
  caps.push(hasWebNn() ? 'WebNN ✓（实验）' : 'WebNN ✗');
  caps.push(crossOriginIsolated ? 'COOP/COEP ✓（wasm 多线程可用）' : 'COOP/COEP ✗（wasm 单线程）');
  setNotes([`环境：${caps.join(' · ')}`, '']);
});

// --- init -------------------------------------------------------------------
async function init(): Promise<void> {
  try {
    manifest = await fetchManifest();
    for (const m of manifest.models) {
      const opt = document.createElement('option');
      opt.value = m.name;
      opt.textContent = `${m.name}（${m.scale}×, ${m.size_bytes >= 1e6 ? `${(m.size_bytes / 1e6).toFixed(1)} MB` : `${Math.round(m.size_bytes / 1e3)} KB`}）`;
      els.model.append(opt);
    }
  } catch (err) {
    setStatus(`模型清单加载失败：${err instanceof Error ? err.message : err}`);
  }
}
init();

// --- file input -------------------------------------------------------------
els.drop.addEventListener('click', () => els.file.click());
els.file.addEventListener('change', () => void loadFile(els.file.files?.[0]));
els.drop.addEventListener('dragover', (e) => {
  e.preventDefault();
  els.drop.classList.add('over');
});
els.drop.addEventListener('dragleave', () => els.drop.classList.remove('over'));
els.drop.addEventListener('drop', (e) => {
  e.preventDefault();
  els.drop.classList.remove('over');
  void loadFile(e.dataTransfer?.files?.[0]);
});

async function loadFile(file: File | undefined): Promise<void> {
  if (!file) return;
  bitmap?.close();
  bitmap = await createImageBitmap(file);
  els.before.src = URL.createObjectURL(file);
  els.run.disabled = false;
  setStatus(`已载入 ${bitmap.width}×${bitmap.height} — ${file.name}`);
}

// --- session cache ----------------------------------------------------------
async function getSession(model: ModelInfo, ep: ExecutionProvider): Promise<InferenceSession> {
  const key = `${model.name}|${ep}`;
  if (session && sessionKey === key) return session;
  session?.release();
  session = null;
  setStatus('加载模型…');
  const bytes = await fetchModel(model);
  setStatus('创建会话…');
  const info = await createSession(bytes, ep);
  session = info.session;
  sessionKey = key;
  setNotes([...info.notes]);
  return session;
}

// --- run --------------------------------------------------------------------
els.run.addEventListener('click', () => void run());
els.cancel.addEventListener('click', () => abort?.abort());

async function run(): Promise<void> {
  if (!manifest || !bitmap) return;
  let model = manifest.models.find((m) => m.name === els.model.value);
  if (!model) return;
  const ep = els.ep.value as ExecutionProvider;
  // WebNN has no DepthToSpace (issue #2 path C): auto-switch to the
  // decomposed-PixelShuffle variant when one exists in the manifest.
  if (ep === 'webnn' && (model as ModelInfo & { pixelshuffle?: string }).pixelshuffle === 'native') {
    const wnn = manifest.models.find(
      (m) => m.name === `${model!.name}-wnn` || (m as ModelInfo & { pixelshuffle?: string }).pixelshuffle === 'decomposed',
    );
    if (wnn) {
      model = wnn;
      setStatus('WebNN：已切换到 PixelShuffle 拆解版模型（单分区图）');
    }
  }
  abort = new AbortController();
  els.run.disabled = true;
  els.cancel.disabled = false;

  try {
    const s = await getSession(model, ep);
    const source = await decodeToRgba(bitmap);
    const result = await upscaleImage(s, model.scale, source, {
      tileSize: Number(els.tile.value),
      overlap: Number(els.overlap.value),
      ep,
      signal: abort.signal,
      onProgress: (done, total) => {
        els.barFill.style.width = `${(done / total) * 100}%`;
        setStatus(`${done}/${total} 块`);
      },
    });

    // render
    const canvas = new OffscreenCanvas(result.width, result.height);
    const ctx2d = canvas.getContext('2d')!;
    const img = ctx2d.createImageData(result.width, result.height);
    img.data.set(result.rgba);
    ctx2d.putImageData(img, 0, 0);
    resultBlob = await canvas.convertToBlob({ type: 'image/png' });
    const url = URL.createObjectURL(resultBlob);
    els.after.src = url;
    // before image needs identical display size: scale both to the result box
    els.before.src = await (async () => {
      const c = new OffscreenCanvas(result.width, result.height);
      const b = new OffscreenCanvas(bitmap!.width, bitmap!.height);
      b.getContext('2d')!.drawImage(bitmap!, 0, 0);
      c.getContext('2d')!.drawImage(b, 0, 0, b.width, b.height, 0, 0, result.width, result.height);
      return URL.createObjectURL(await c.convertToBlob({ type: 'image/png' }));
    })();
    els.compare.hidden = false;
    els.download.disabled = false;
    setComparePos(0.5);

    const secs = (result.ms / 1000).toFixed(2);
    setStatus(`${result.tiles} 块 · ${result.ep} · ${secs}s · 输出 ${result.width}×${result.height}`);
    els.meta.textContent = `${model.name}（${model.scale}×，license: ${model.license}）`;
  } catch (err) {
    if (err instanceof DOMException && err.name === 'AbortError') {
      setStatus('已取消');
    } else {
      setStatus(`失败：${err instanceof Error ? err.message : err}`);
      console.error(err);
    }
  } finally {
    els.run.disabled = false;
    els.cancel.disabled = true;
    abort = null;
  }
}

els.download.addEventListener('click', () => {
  if (!resultBlob) return;
  const a = document.createElement('a');
  a.href = URL.createObjectURL(resultBlob);
  a.download = `upix-web-${Date.now()}.png`;
  a.click();
});

// --- bench mode (?bench=1, issue #2 path C) --------------------------------
if (new URLSearchParams(location.search).has('bench')) {
  void runBench((line) => setNotes([...els.notes.textContent!.split('\n'), line]));
}

// --- compare slider ---------------------------------------------------------
function setComparePos(t: number): void {
  const pct = `${t * 100}%`;
  els.beforeWrap.style.width = pct;
  els.handle.style.left = pct;
  els.before.style.transform = `translateX(${-els.frame.clientWidth * t}px)`;
}
els.handle.addEventListener('pointerdown', (e) => {
  els.handle.setPointerCapture(e.pointerId);
  const move = (ev: PointerEvent) => {
    const rect = els.frame.getBoundingClientRect();
    setComparePos(Math.min(1, Math.max(0, (ev.clientX - rect.left) / rect.width)));
  };
  const up = () => {
    els.handle.removeEventListener('pointermove', move);
    els.handle.removeEventListener('pointerup', up);
  };
  els.handle.addEventListener('pointermove', move);
  els.handle.addEventListener('pointerup', up);
});
window.addEventListener('resize', () => setComparePos(parseFloat(els.handle.style.left) / 100 || 0.5));
