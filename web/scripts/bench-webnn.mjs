// WebNN & Cross-Backend Benchmark Suite for Upix Web (Desktop & Remote Android CDP).
//
// Desktop usage:
//   npm run bench:webnn
//   CHROME_BIN=/usr/bin/google-chrome-beta node ./scripts/bench-webnn.mjs --out ./benchmarks/webnn-desktop-linux-rtx4060.json
//
// Android Chrome (over ADB + DevTools CDP) usage:
//   adb forward tcp:9222 localabstract:chrome_devtools_remote
//   adb reverse tcp:5179 tcp:5179
//   node ./scripts/bench-webnn.mjs --cdp http://127.0.0.1:9222 --port 5179 --platform android --out ./benchmarks/webnn-android.json

import { execSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import puppeteer from 'puppeteer-core';
import { createServer } from 'vite';

const SCRIPT_DIR = path.dirname(fileURLToPath(import.meta.url));
const WEB_ROOT = path.resolve(SCRIPT_DIR, '..');

function parseArgs(argv) {
  const opts = {
    platform: 'desktop',
    cdp: '',
    port: 0,
    out: '',
    chromeBin: process.env.CHROME_BIN || '',
    profile: 'full', // 'full' | 'mobile'
  };
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (a === '--platform' && argv[i + 1]) { opts.platform = argv[++i]; }
    else if (a === '--cdp' && argv[i + 1]) { opts.cdp = argv[++i]; }
    else if (a === '--port' && argv[i + 1]) { opts.port = Number(argv[++i]) || 0; }
    else if (a === '--out' && argv[i + 1]) { opts.out = argv[++i]; }
    else if (a === '--chrome' && argv[i + 1]) { opts.chromeBin = argv[++i]; }
    else if (a === '--profile' && argv[i + 1]) { opts.profile = argv[++i]; }
  }
  if (!opts.out) {
    const slug = opts.platform === 'android' ? 'webnn-android' : 'webnn-desktop-linux-rtx4060';
    opts.out = path.join(WEB_ROOT, 'benchmarks', `${slug}.json`);
  } else {
    opts.out = path.resolve( process.cwd(), opts.out );
  }
  if (opts.platform === 'android' && opts.profile === 'full') {
    opts.profile = 'mobile';
  }
  return opts;
}

function detectChromeBinary(explicit) {
  const candidates = [
    explicit,
    '/usr/bin/google-chrome-beta',
    '/usr/bin/google-chrome-unstable',
    '/usr/bin/google-chrome-stable',
    '/usr/bin/google-chrome',
    '/usr/bin/chromium',
  ].filter(Boolean);
  for (const bin of candidates) {
    if (existsSync(bin)) return bin;
  }
  throw new Error('未找到可用的 Chrome / Chromium 可执行文件，请通过 CHROME_BIN 或 --chrome 指定');
}

function readHostHardware(platform, chromeBin) {
  if (platform !== 'desktop') {
    return {
      platform,
      note: 'Remote target via Chrome DevTools Protocol (hardware queried from browser runtime)',
    };
  }
  let cpuModel = os.cpus()?.[0]?.model || 'Unknown CPU';
  try {
    const cpuinfo = readFileSync('/proc/cpuinfo', 'utf8');
    const m = cpuinfo.match(/^model name\s*:\s*(.+)$/m);
    if (m) cpuModel = m[1].trim();
  } catch { /* ignore */ }

  let gpuInfo = '';
  try {
    gpuInfo = execSync('nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv,noheader', {
      encoding: 'utf8',
      stdio: ['ignore', 'pipe', 'ignore'],
    }).trim();
  } catch { /* ignore */ }

  let browserVersion = '';
  try {
    browserVersion = execSync(`"${chromeBin}" --version`, {
      encoding: 'utf8',
      stdio: ['ignore', 'pipe', 'ignore'],
    }).trim();
  } catch { /* ignore */ }

  return {
    platform: 'desktop',
    os: `${os.type()} ${os.release()} (${os.arch()})`,
    cpuModel,
    logicalCores: os.cpus()?.length || 0,
    totalRamGiB: +(os.totalmem() / (1024 ** 3)).toFixed(1),
    nvidiaSmi: gpuInfo || null,
    browserBinary: chromeBin,
    browserVersion: browserVersion || null,
  };
}

function log(msg) {
  const ts = new Date().toTimeString().slice(0, 8);
  process.stderr.write(`[${ts}] ${msg}\n`);
}

const DESKTOP_MODEL_SUITE = [
  { family: 'SPAN', modelName: 'NomosUni-SPAN-x2', tileSizes: [128, 256, 384], webnnDevices: ['gpu', 'cpu', 'npu'] },
  { family: 'SPAN', modelName: 'NomosUni-SPAN-x4', tileSizes: [128, 256], webnnDevices: ['gpu', 'cpu'] },
  { family: 'Compact (SRVGG)', modelName: 'AnimeJaNai-v2-Compact-x2', tileSizes: [128, 256, 384], webnnDevices: ['gpu', 'cpu', 'npu'] },
  { family: 'Compact (SRVGG)', modelName: 'LSDIR-Compact-v2-x4', tileSizes: [128, 256], webnnDevices: ['gpu', 'cpu'] },
  { family: 'Real-ESRGANv3', modelName: 'Real-ESRGANv3-anime-x2', tileSizes: [128, 256], webnnDevices: ['gpu', 'cpu'] },
  { family: 'Real-CUGAN', modelName: 'Real-CUGAN-no-denoise-x2', tileSizes: [128, 256], webnnDevices: ['gpu', 'cpu'] },
  { family: 'Real-CUGAN', modelName: 'Real-CUGAN-denoise-x3', tileSizes: [128, 192], webnnDevices: ['gpu', 'cpu'] },
  { family: 'RealPLKSR (17x17)', modelName: 'NomosWebPhoto-RealPLKSR-x4', tileSizes: [128, 192], webnnDevices: ['gpu', 'cpu'] },
  { family: 'RealPLKSR (17x17)', modelName: 'DeJPG-RealPLKSR-x1', tileSizes: [128, 192], webnnDevices: ['gpu'] },
  { family: 'RRDB (23-block)', modelName: 'BSRGAN-x2', tileSizes: [128, 192], webnnDevices: ['gpu'] },
];

const MOBILE_MODEL_SUITE = [
  { family: 'SPAN', modelName: 'NomosUni-SPAN-x2', tileSizes: [128, 192], webnnDevices: ['gpu', 'npu', 'cpu'] },
  { family: 'Compact (SRVGG)', modelName: 'AnimeJaNai-v2-Compact-x2', tileSizes: [128, 192], webnnDevices: ['gpu', 'npu', 'cpu'] },
  { family: 'Real-ESRGANv3', modelName: 'Real-ESRGANv3-anime-x2', tileSizes: [128], webnnDevices: ['gpu', 'npu', 'cpu'] },
  { family: 'Real-CUGAN', modelName: 'Real-CUGAN-no-denoise-x2', tileSizes: [128], webnnDevices: ['gpu', 'npu', 'cpu'] },
  { family: 'RealPLKSR (17x17)', modelName: 'NomosWebPhoto-RealPLKSR-x4', tileSizes: [128], webnnDevices: ['gpu', 'npu'] },
];

async function main() {
  const opts = parseArgs(process.argv.slice(2));
  const chromeBin = opts.cdp ? '' : detectChromeBinary(opts.chromeBin);
  const hostInfo = readHostHardware(opts.platform, chromeBin);

  log(`Starting Vite server for Upix Web (${opts.platform} profile=${opts.profile})...`);
  const server = await createServer({
    root: WEB_ROOT,
    server: {
      host: '127.0.0.1',
      port: opts.port || 0,
      strictPort: Boolean(opts.port),
    },
    logLevel: 'silent',
  });
  await server.listen();
  const port = server.httpServer.address().port;
  const pageUrl = `http://127.0.0.1:${port}/`;
  log(`Vite ready at ${pageUrl}`);

  const launchFlags = [
    '--headless=new',
    '--no-sandbox',
    '--enable-unsafe-webgpu',
    '--ignore-gpu-blocklist',
    '--use-angle=gl',
    '--use-vulkan=native',
    '--enable-features=Vulkan,WebGPUDeveloperFeatures,WebMachineLearningNeuralNetwork',
  ];

  const browser = opts.cdp
    ? await puppeteer.connect({ browserURL: opts.cdp, defaultViewport: null })
    : await puppeteer.launch({
      executablePath: chromeBin,
      headless: false,
      ignoreDefaultArgs: ['--disable-gpu'],
      args: launchFlags,
    });

  try {
    const page = await browser.newPage();
    await page.setRequestInterception(true);
    page.on('request', (req) => {
      const u = req.url();
      if (u.includes('/api/upix-models/manifest.json') || u.includes('github.com') || u.includes('ghfast.top')) {
        req.abort();
      } else {
        req.continue();
      }
    });
    page.on('pageerror', (err) => log(`[pageerror] ${err.message || err}`));

    await page.goto(pageUrl, { waitUntil: 'domcontentloaded' });

    const env = await page.evaluate(async () => {
      const { createUpixRuntime } = await import('/src/runtime/index.js');
      const rt = createUpixRuntime();
      window.__benchRt = rt;

      window.__makeBenchPng = async (w, h) => {
        const c = new OffscreenCanvas(w, h);
        const ctx = c.getContext('2d');
        const g = ctx.createLinearGradient(0, 0, w, h);
        g.addColorStop(0, '#f4efe6');
        g.addColorStop(0.5, '#d8c3a5');
        g.addColorStop(1, '#1d2d44');
        ctx.fillStyle = g;
        ctx.fillRect(0, 0, w, h);
        ctx.strokeStyle = '#e63946';
        ctx.lineWidth = 2;
        for (let i = 16; i < Math.min(w, h); i += 32) {
          ctx.strokeRect(i, i, w - i * 2, h - i * 2);
        }
        ctx.fillStyle = '#11161d';
        ctx.font = 'bold 26px monospace';
        ctx.fillText('UPIX WEBNN BENCH', 24, Math.floor(h / 2));
        const blob = await c.convertToBlob({ type: 'image/png' });
        return new File([blob], `bench-${w}x${h}.png`, { type: 'image/png' });
      };

      const t0 = performance.now();
      await rt.init();
      const initMs = +(performance.now() - t0).toFixed(2);
      const caps = await rt.getCapabilities({ refresh: true });
      const { models } = await rt.listModels({ refreshRemote: false });
      const installed = models.filter((m) => m.installed).map((m) => m.name);

      // Detailed WebNN per-device & power-preference probe
      const webnnProbe = {};
      if (navigator.ml && typeof globalThis.MLGraphBuilder !== 'undefined') {
        for (const deviceType of ['gpu', 'npu', 'cpu']) {
          webnnProbe[deviceType] = {};
          for (const powerPreference of ['high-performance', 'low-power']) {
            try {
              const tc = performance.now();
              const ctx = await navigator.ml.createContext({ deviceType, powerPreference });
              const createMs = +(performance.now() - tc).toFixed(2);
              let limits = null;
              if (typeof ctx.opSupportLimits === 'function') {
                const lim = ctx.opSupportLimits();
                limits = {
                  preferredInputLayout: lim.preferredInputLayout || null,
                  maxTensorByteLength: lim.maxTensorByteLength || null,
                  inputDataTypes: lim.input?.dataTypes ? Array.from(lim.input.dataTypes) : null,
                  conv2dDataTypes: lim.conv2d?.input?.dataTypes ? Array.from(lim.conv2d.input.dataTypes) : null,
                  convTranspose2dDataTypes: lim.convTranspose2d?.input?.dataTypes
                    ? Array.from(lim.convTranspose2d.input.dataTypes)
                    : null,
                  preluExposed: Boolean(lim.prelu),
                };
              }
              ctx.destroy?.();
              webnnProbe[deviceType][powerPreference] = { available: true, createMs, limits };
            } catch (e) {
              webnnProbe[deviceType][powerPreference] = {
                available: false,
                error: e instanceof Error ? e.message : String(e),
              };
            }
          }
        }
      }

      return {
        userAgent: navigator.userAgent,
        hardwareConcurrency: navigator.hardwareConcurrency || 0,
        deviceMemoryGiB: navigator.deviceMemory || null,
        crossOriginIsolated: globalThis.crossOriginIsolated === true,
        initMs,
        caps,
        webnnProbe,
        installedModels: installed,
      };
    });

    log(`Runtime ready in ${env.initMs}ms | WebGPU=${env.caps.webgpu.deviceName} | WebNN=${env.caps.webnn.deviceName} | Installed=${env.installedModels.length}`);

    const suite = (opts.profile === 'mobile' ? MOBILE_MODEL_SUITE : DESKTOP_MODEL_SUITE)
      .filter((entry) => env.installedModels.includes(entry.modelName));

    const tileMatrix = [];

    for (const item of suite) {
      for (const tileSize of item.tileSizes) {
        // 1. WebNN devices (`gpu`, `cpu`, `npu`) with separate Cold Compile vs Warm Steady-State
        for (const dev of item.webnnDevices) {
          if (!env.caps.webnn?.features?.webnnDevices?.[dev]) continue;
          // Keep CPU/NPU matrix fast on large 256px tiles for slow heavy models
          if (dev !== 'gpu' && tileSize > 192 && (item.family.includes('CUGAN') || item.family.includes('PLKSR'))) {
            continue;
          }

          const res = await page.evaluate(
            async ({ family, modelName, deviceType, ts }) => {
              const rt = window.__benchRt;
              try {
                const payload = await rt.resolveModelPayload(modelName);
                const { scale, modelChannels: ch } = rt.getModelBriefMeta(payload);
                const parsed = rt.webnnBackend.ensureModelParsed(rt.wasm, payload);
                const { context, key: ctxKey } = await rt.webnnBackend.getContext(deviceType, 'high-performance');
                const { feedW, feedH } = rt.webnnBackend.computeFeedShape(parsed.meta, ts, ts);
                const cacheKey = `${parsed.name}:${ctxKey}:${feedW}x${feedH}x${ch}`;

                // Drop any prior cached graph for this exact key so cold build is measured cleanly
                const prev = rt.webnnBackend.graphCache.get(cacheKey);
                if (prev) {
                  prev.inTensor?.destroy?.();
                  prev.outTensor?.destroy?.();
                  rt.webnnBackend.graphCache.delete(cacheKey);
                }

                const dummy = new Float32Array(ts * ts * ch).fill(0.5);
                const tCompile0 = performance.now();
                await rt.webnnBackend.compileGraphForShape(context, ctxKey, parsed, feedW, feedH, ch);
                const compileMs = +(performance.now() - tCompile0).toFixed(2);

                // Boost / warmup runs
                const wStart = performance.now();
                for (let w = 0; w < 5; w += 1) {
                  await rt.webnnBackend.inferTileWebNn(context, ctxKey, parsed, dummy, ts, ts, ch);
                  if (performance.now() - wStart >= 80) break;
                }

                const runs = devRunsCount(deviceType, ts, family);
                const samples = [];
                for (let i = 0; i < runs; i += 1) {
                  const t0 = performance.now();
                  await rt.webnnBackend.inferTileWebNn(context, ctxKey, parsed, dummy, ts, ts, ch);
                  samples.push(+(performance.now() - t0).toFixed(2));
                }

                const sorted = [...samples].sort((a, b) => a - b);
                const avgTileMs = +(samples.reduce((s, v) => s + v, 0) / samples.length).toFixed(2);
                const p50TileMs = sorted[Math.floor(sorted.length / 2)];
                const minTileMs = sorted[0];
                const maxTileMs = sorted[sorted.length - 1];
                const tilesPerSec = +(1000 / Math.max(0.05, avgTileMs)).toFixed(2);
                const inputMpixPerSec = +(((ts * ts) / 1e6) * tilesPerSec).toFixed(3);
                const outputMpixPerSec = +(((ts * scale * ts * scale) / 1e6) * tilesPerSec).toFixed(3);

                return {
                  ok: true,
                  backend: `webnn:${deviceType}`,
                  deviceName: `WebNN (${deviceType.toUpperCase()})`,
                  family,
                  modelName,
                  scale,
                  tileSize: ts,
                  feedShape: `${feedW}x${feedH}`,
                  compileMs,
                  avgTileMs,
                  p50TileMs,
                  minTileMs,
                  maxTileMs,
                  samples,
                  tilesPerSec,
                  inputMpixPerSec,
                  outputMpixPerSec,
                };
              } catch (e) {
                return {
                  ok: false,
                  backend: `webnn:${deviceType}`,
                  family,
                  modelName,
                  tileSize: ts,
                  error: e instanceof Error ? e.message : String(e),
                };
              }

              function devRunsCount(dt, size, fam) {
                if (dt !== 'gpu' && fam.includes('PLKSR')) return 3;
                if (size >= 256 && dt !== 'gpu') return 3;
                return 5;
              }
            },
            { family: item.family, modelName: item.modelName, deviceType: dev, ts: tileSize },
          );

          tileMatrix.push(res);
          if (res.ok) {
            log(`${res.backend.padEnd(10)} | ${item.modelName.padEnd(28)} @${String(tileSize).padStart(3)}px | build=${String(res.compileMs).padStart(7)}ms | warm=${String(res.avgTileMs).padStart(7)}ms (min ${res.minTileMs}ms) | ${res.inputMpixPerSec.toFixed(2)} inMP/s`);
          } else {
            log(`${res.backend.padEnd(10)} | ${item.modelName.padEnd(28)} @${String(tileSize).padStart(3)}px | ERR: ${res.error}`);
          }
        }

        // 2. WebGPU FP16 baseline at the exact same (model, tileSize)
        if (env.caps.webgpu?.available) {
          const gpuRes = await page.evaluate(
            async ({ family, modelName, ts }) => {
              const rt = window.__benchRt;
              try {
                const payload = await rt.resolveModelPayload(modelName);
                const { scale, modelChannels: ch } = rt.getModelBriefMeta(payload);
                const tPrep0 = performance.now();
                const model = await rt.webgpuBackend.prepareModel(rt.wasm, payload, true);
                rt.webgpuBackend.getOrCreateTilePlan(model, ts, ts, ch);
                const compileMs = +(performance.now() - tPrep0).toFixed(2);

                const dummy = new Float32Array(ts * ts * ch).fill(0.5);
                const wStart = performance.now();
                for (let w = 0; w < 5; w += 1) {
                  await rt.webgpuBackend.inferTileGpu(model, dummy, ts, ts, ch);
                  if (performance.now() - wStart >= 80) break;
                }

                const samples = [];
                for (let i = 0; i < 5; i += 1) {
                  const t0 = performance.now();
                  await rt.webgpuBackend.inferTileGpu(model, dummy, ts, ts, ch);
                  samples.push(+(performance.now() - t0).toFixed(2));
                }
                const sorted = [...samples].sort((a, b) => a - b);
                const avgTileMs = +(samples.reduce((s, v) => s + v, 0) / samples.length).toFixed(2);
                const tilesPerSec = +(1000 / Math.max(0.05, avgTileMs)).toFixed(2);
                return {
                  ok: true,
                  backend: 'webgpu',
                  deviceName: rt.webgpuBackend.deviceName,
                  family,
                  modelName,
                  scale,
                  tileSize: ts,
                  compileMs,
                  avgTileMs,
                  p50TileMs: sorted[Math.floor(sorted.length / 2)],
                  minTileMs: sorted[0],
                  maxTileMs: sorted[sorted.length - 1],
                  samples,
                  tilesPerSec,
                  inputMpixPerSec: +(((ts * ts) / 1e6) * tilesPerSec).toFixed(3),
                  outputMpixPerSec: +(((ts * scale * ts * scale) / 1e6) * tilesPerSec).toFixed(3),
                };
              } catch (e) {
                return {
                  ok: false,
                  backend: 'webgpu',
                  family,
                  modelName,
                  tileSize: ts,
                  error: e instanceof Error ? e.message : String(e),
                };
              }
            },
            { family: item.family, modelName: item.modelName, ts: tileSize },
          );
          tileMatrix.push(gpuRes);
          if (gpuRes.ok) {
            log(`${'webgpu'.padEnd(10)} | ${item.modelName.padEnd(28)} @${String(tileSize).padStart(3)}px | build=${String(gpuRes.compileMs).padStart(7)}ms | warm=${String(gpuRes.avgTileMs).padStart(7)}ms (min ${gpuRes.minTileMs}ms) | ${gpuRes.inputMpixPerSec.toFixed(2)} inMP/s`);
          }
        }

        // 3. WASM SIMD128 (Auto Multi-Worker + 1-Worker) reference at @128px on core models
        if (
          tileSize === 128
          && ['NomosUni-SPAN-x2', 'AnimeJaNai-v2-Compact-x2', 'Real-CUGAN-no-denoise-x2'].includes(item.modelName)
        ) {
          for (const threads of [0, 1]) {
            const label = threads === 0 ? 'wasm:auto' : 'wasm:1t';
            const wasmRes = await page.evaluate(
              async ({ family, modelName, ts, th, backendLabel }) => {
                const rt = window.__benchRt;
                const payload = await rt.resolveModelPayload(modelName);
                const { scale } = rt.getModelBriefMeta(payload);
                const r = await rt.benchmark({
                  backend: 'wasm',
                  modelName,
                  tileSize: ts,
                  wasmThreads: th,
                });
                return {
                  ok: true,
                  backend: backendLabel,
                  deviceName: r.deviceName,
                  family,
                  modelName,
                  scale,
                  tileSize: ts,
                  compileMs: 0,
                  avgTileMs: +r.avgTileMs.toFixed(2),
                  p50TileMs: +r.avgTileMs.toFixed(2),
                  minTileMs: +r.avgTileMs.toFixed(2),
                  maxTileMs: +r.avgTileMs.toFixed(2),
                  tilesPerSec: +r.tilesPerSec.toFixed(2),
                  inputMpixPerSec: +r.mpixPerSec.toFixed(3),
                  outputMpixPerSec: +(r.mpixPerSec * scale * scale).toFixed(3),
                };
              },
              { family: item.family, modelName: item.modelName, ts: tileSize, th: threads, backendLabel: label },
            );
            tileMatrix.push(wasmRes);
            log(`${label.padEnd(10)} | ${item.modelName.padEnd(28)} @${String(tileSize).padStart(3)}px | ${wasmRes.deviceName.padEnd(25)} | warm=${String(wasmRes.avgTileMs).padStart(7)}ms | ${wasmRes.inputMpixPerSec.toFixed(3)} inMP/s`);
          }
        }
      }
    }

    log('Running End-to-End image pipeline & PSNR parity benchmarks...');
    const e2eAndParity = await page.evaluate(
      async ({ isMobile, installed }) => {
        const rt = window.__benchRt;
        const has = (m) => installed.includes(m);
        const hdW = isMobile ? 512 : 1024;
        const hdH = isMobile ? 384 : 768;
        const hdFile = await window.__makeBenchPng(hdW, hdH);
        const halfFile = await window.__makeBenchPng(512, 384);
        const parityFile = await window.__makeBenchPng(256, 192);

        // Reset WebNN graphCache before E2E so Cold vs Warm E2E is explicitly captured
        rt.webnnBackend.dispose();

        async function runE2E(label, file, modelName, runOpts) {
          const phases = {};
          let lastT = performance.now();
          let lastPhase = 'init';
          const t0 = performance.now();
          const res = await rt.processImage(file, {
            modelName,
            force: true,
            outputFormat: 'png',
            ...runOpts,
            onProgress: (p) => {
              const now = performance.now();
              if (p.phase !== lastPhase) {
                phases[lastPhase] = +((phases[lastPhase] || 0) + (now - lastT)).toFixed(2);
                lastPhase = p.phase;
                lastT = now;
              }
            },
          });
          const endNow = performance.now();
          phases[lastPhase] = +((phases[lastPhase] || 0) + (endNow - lastT)).toFixed(2);
          return {
            label,
            modelName,
            backend: runOpts.backend === 'webnn' ? `webnn:${runOpts.webnnDevice || 'gpu'}` : runOpts.backend,
            deviceName: res.deviceName,
            inputSize: `${res.inputWidth}x${res.inputHeight}`,
            outputSize: `${res.outputWidth}x${res.outputHeight}`,
            tileSizeUsed: res.tileSizeUsed,
            tilesTotal: res.tilesTotal,
            totalMs: +(endNow - t0).toFixed(2),
            outputBytes: res.bytes.byteLength,
            phases,
          };
        }

        const e2eRuns = [];
        if (has('NomosUni-SPAN-x2')) {
          e2eRuns.push(await runE2E('span-x2-webnn-gpu-cold', hdFile, 'NomosUni-SPAN-x2', { backend: 'webnn', webnnDevice: 'gpu' }));
          e2eRuns.push(await runE2E('span-x2-webnn-gpu-warm', hdFile, 'NomosUni-SPAN-x2', { backend: 'webnn', webnnDevice: 'gpu' }));
          e2eRuns.push(await runE2E('span-x2-webnn-cpu', hdFile, 'NomosUni-SPAN-x2', { backend: 'webnn', webnnDevice: 'cpu' }));
          if (!isMobile) {
            e2eRuns.push(await runE2E('span-x2-webgpu-warm', hdFile, 'NomosUni-SPAN-x2', { backend: 'webgpu' }));
          }
        }
        if (has('AnimeJaNai-v2-Compact-x2')) {
          e2eRuns.push(await runE2E('compact-x2-webnn-gpu-cold', hdFile, 'AnimeJaNai-v2-Compact-x2', { backend: 'webnn', webnnDevice: 'gpu' }));
          e2eRuns.push(await runE2E('compact-x2-webnn-gpu-warm', hdFile, 'AnimeJaNai-v2-Compact-x2', { backend: 'webnn', webnnDevice: 'gpu' }));
          e2eRuns.push(await runE2E('compact-x2-webnn-cpu', hdFile, 'AnimeJaNai-v2-Compact-x2', { backend: 'webnn', webnnDevice: 'cpu' }));
          if (!isMobile) {
            e2eRuns.push(await runE2E('compact-x2-webgpu-warm', hdFile, 'AnimeJaNai-v2-Compact-x2', { backend: 'webgpu' }));
            e2eRuns.push(await runE2E('compact-x2-wasm-auto', hdFile, 'AnimeJaNai-v2-Compact-x2', { backend: 'wasm' }));
          }
        }
        if (has('Real-CUGAN-no-denoise-x2')) {
          e2eRuns.push(await runE2E('cugan-x2-webnn-gpu-cold', hdFile, 'Real-CUGAN-no-denoise-x2', { backend: 'webnn', webnnDevice: 'gpu' }));
          e2eRuns.push(await runE2E('cugan-x2-webnn-gpu-warm', hdFile, 'Real-CUGAN-no-denoise-x2', { backend: 'webnn', webnnDevice: 'gpu' }));
          if (!isMobile) {
            e2eRuns.push(await runE2E('cugan-x2-webgpu-warm', hdFile, 'Real-CUGAN-no-denoise-x2', { backend: 'webgpu' }));
          }
        }
        if (!isMobile && has('NomosWebPhoto-RealPLKSR-x4')) {
          e2eRuns.push(await runE2E('plksr-x4-webnn-gpu-cold', halfFile, 'NomosWebPhoto-RealPLKSR-x4', { backend: 'webnn', webnnDevice: 'gpu' }));
          e2eRuns.push(await runE2E('plksr-x4-webnn-gpu-warm', halfFile, 'NomosWebPhoto-RealPLKSR-x4', { backend: 'webnn', webnnDevice: 'gpu' }));
          e2eRuns.push(await runE2E('plksr-x4-webgpu-warm', halfFile, 'NomosWebPhoto-RealPLKSR-x4', { backend: 'webgpu' }));
        }

        async function comparePsnr(modelName) {
          const nn = await rt.processImage(parityFile, {
            modelName,
            backend: 'webnn',
            webnnDevice: 'gpu',
            tileSize: 128,
            force: true,
            outputFormat: 'png',
          });
          const ref = await rt.processImage(parityFile, {
            modelName,
            backend: 'wasm',
            tileSize: 128,
            force: true,
            outputFormat: 'png',
          });
          const bmpA = await createImageBitmap(new Blob([nn.bytes], { type: 'image/png' }));
          const bmpB = await createImageBitmap(new Blob([ref.bytes], { type: 'image/png' }));
          const oc = new OffscreenCanvas(bmpA.width, bmpA.height);
          const ctx = oc.getContext('2d', { willReadFrequently: true });
          ctx.drawImage(bmpA, 0, 0);
          const dA = ctx.getImageData(0, 0, bmpA.width, bmpA.height).data;
          ctx.clearRect(0, 0, bmpA.width, bmpA.height);
          ctx.drawImage(bmpB, 0, 0);
          const dB = ctx.getImageData(0, 0, bmpB.width, bmpB.height).data;
          let mse = 0;
          let maxDiff = 0;
          let count = 0;
          for (let i = 0; i < dA.length; i += 4) {
            for (let c = 0; c < 3; c += 1) {
              const diff = Math.abs(dA[i + c] - dB[i + c]);
              if (diff > maxDiff) maxDiff = diff;
              mse += diff * diff;
              count += 1;
            }
          }
          mse /= count;
          bmpA.close();
          bmpB.close();
          return {
            modelName,
            reference: 'wasm-f32',
            candidate: 'webnn:gpu',
            psnrDb: mse === 0 ? 99.99 : +(10 * Math.log10((255 * 255) / mse)).toFixed(2),
            maxAbsDiffU8: maxDiff,
          };
        }

        const parity = [];
        for (const m of ['NomosUni-SPAN-x2', 'AnimeJaNai-v2-Compact-x2', 'Real-CUGAN-no-denoise-x2']) {
          if (has(m)) parity.push(await comparePsnr(m));
        }

        return { e2eRuns, parity };
      },
      { isMobile: opts.profile === 'mobile', installed: env.installedModels },
    );

    for (const r of e2eAndParity.e2eRuns) {
      log(`E2E ${r.label.padEnd(26)} | ${r.inputSize}->${r.outputSize} (${String(r.tilesTotal).padStart(2)} tiles @${r.tileSizeUsed}px) | ${String(r.totalMs).padStart(7)}ms | compile=${r.phases.compile || 0}ms infer=${r.phases.infer || 0}ms`);
    }
    for (const p of e2eAndParity.parity) {
      log(`PSNR ${p.modelName.padEnd(26)} | WebNN(GPU) vs WASM(f32): ${p.psnrDb} dB (maxDiff=${p.maxAbsDiffU8}/255)`);
    }

    const report = {
      schemaVersion: 1,
      generatedAt: new Date().toISOString(),
      platform: opts.platform,
      profile: opts.profile,
      launchFlags: opts.cdp ? ['remote-cdp'] : launchFlags,
      host: hostInfo,
      browserEnv: {
        userAgent: env.userAgent,
        hardwareConcurrency: env.hardwareConcurrency,
        deviceMemoryGiB: env.deviceMemoryGiB,
        crossOriginIsolated: env.crossOriginIsolated,
        runtimeInitMs: env.initMs,
        capabilities: env.caps,
        webnnDeviceMatrix: env.webnnProbe,
      },
      tileBenchmarks: tileMatrix,
      e2eBenchmarks: e2eAndParity.e2eRuns,
      accuracyParity: e2eAndParity.parity,
    };

    mkdirSync(path.dirname(opts.out), { recursive: true });
    writeFileSync(opts.out, `${JSON.stringify(report, null, 2)}\n`, 'utf8');
    log(`Saved benchmark report -> ${opts.out}`);
    process.stdout.write(`${opts.out}\n`);
  } finally {
    if (opts.cdp) {
      browser.disconnect();
    } else {
      await browser.close();
    }
    await server.close();
  }
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
