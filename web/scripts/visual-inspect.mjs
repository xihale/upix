// Visual inspection harness: drives the real app in headless Chrome and
// captures every key UI state to /tmp/upix-shots/*.png. Debug tooling only.
import puppeteer from 'puppeteer-core';
import { mkdirSync } from 'node:fs';

const CHROME = process.env.CHROME_BIN
  ?? `${process.env.HOME}/.cache/puppeteer/chrome-headless-shell/linux-156.0.8078.47/chrome-headless-shell-linux64/chrome-headless-shell`;
const BASE = 'http://127.0.0.1:5173';
const OUT = '/tmp/upix-shots';
mkdirSync(OUT, { recursive: true });

const sleep = ms => new Promise(r => setTimeout(r, ms));

const browser = await puppeteer.launch({
  executablePath: CHROME,
  args: ['--no-sandbox', '--disable-gpu', '--font-render-hinting=none'],
});

let seq = 0;
async function shot(page, name, { width = 1280, height = 800, theme, before } = {}) {
  await page.setViewport({ width, height, deviceScaleFactor: 2 });
  await page.goto(`${BASE}/?s=${++seq}#workbench`, { waitUntil: 'networkidle0', timeout: 30000 }).catch(() => {});
  await page.evaluate(t => { document.documentElement.dataset.theme = t ?? ''; }, theme ?? '');
  await sleep(400);
  if (before) await before(page);
  await sleep(700);
  await page.screenshot({ path: `${OUT}/${name}.png` });
  console.log('shot', name);
}

const page = await browser.newPage();

// 1. Workbench — empty stage
await shot(page, '01-wb-empty-light');

// 2. Workbench — sample loaded (stage, curtain, filmstrip)
await shot(page, '02-wb-sample-light', {
  before: async p => {
    await p.click('.dz-actions .btn:nth-child(2)');
    await sleep(1500);
  },
});

// 3. Workbench — run: load sample, start processing, catch telemetry/result
await shot(page, '03-wb-run-light', {
  before: async p => {
    await p.click('.dz-actions .btn:nth-child(2)');
    await sleep(1500);
    const btn = await p.$('.cta-dock .btn-cta');
    if (btn) { await btn.click().catch(() => {}); await sleep(1200); }
  },
});

// 10. Filmstrip — two images so the batch queue strip renders
await shot(page, '10-wb-filmstrip-light', {
  before: async p => {
    await p.click('.dz-actions .btn:nth-child(2)');
    await sleep(1200);
    await p.evaluate(async () => {
      const c = document.createElement('canvas');
      c.width = 320; c.height = 200;
      const x = c.getContext('2d');
      x.fillStyle = '#ffffff'; x.fillRect(0, 0, 320, 200);
      x.fillStyle = '#0070f3'; x.fillRect(24, 24, 96, 96);
      x.fillStyle = '#171717'; x.font = '600 40px Geist, Arial'; x.fillText('Upix', 140, 80);
      const blob = await new Promise(r => c.toBlob(r, 'image/png'));
      const dt = new DataTransfer();
      dt.items.add(new File([blob], 'checker-320x200.png', { type: 'image/png' }));
      window.dispatchEvent(new ClipboardEvent('paste', { clipboardData: dt }));
    });
    await sleep(1500);
  },
});

// 4. Model picker modal
await shot(page, '04-picker-light', {
  before: async p => {
    await p.click('.model-trigger');
    await sleep(600);
  },
});

// 5. Models catalog
await shot(page, '05-models-light', {
  before: async p => {
    await p.evaluate(() => { location.hash = '#models'; });
    await sleep(800);
  },
});

// 6. Engine matrix
await shot(page, '06-runtime-light', {
  before: async p => {
    await p.evaluate(() => { location.hash = '#runtime'; });
    await sleep(800);
  },
});

// 7–8. Dark variants
await shot(page, '07-wb-sample-dark', {
  theme: 'dark',
  before: async p => {
    const b = await p.$('.dz-actions .btn:nth-child(2)');
    if (b) { await b.click().catch(() => {}); await sleep(1500); }
  },
});
await shot(page, '08-models-dark', {
  theme: 'dark',
  before: async p => {
    await p.evaluate(() => { location.hash = '#models'; });
    await sleep(800);
  },
});

// 9. Narrow mobile workbench
await shot(page, '09-wb-sample-narrow', {
  width: 390, height: 844,
  before: async p => {
    const b = await p.$('.dz-actions .btn:nth-child(2)');
    if (b) { await b.click().catch(() => {}); await sleep(1500); }
  },
});

await browser.close();
console.log('done');
