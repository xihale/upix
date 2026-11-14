// Vite dev/preview middleware plugin for Upix Web:
// 1. Ensures `application/wasm` MIME header for `WebAssembly.compileStreaming`
// 2. Exposes read-only `/api/local-models/*` from the developer's local Upix models dir (`~/.local/share/upix/models`)
// 3. Proxies `/api/upix-models/*` to GitHub Releases (`https://github.com/xihale/upix/releases/download/models/*`)
//    with automatic 302 redirect following so dev/preview avoids S3 CORS restrictions.

import { createReadStream, existsSync, readdirSync, statSync } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';
import https from 'node:https';

const RELEASE_BASE = 'https://github.com/xihale/upix/releases/download/models';

function resolveLocalModelsDir() {
  const candidates = [];
  if (process.env.UPIX_MODELS_DIR) {
    candidates.push(process.env.UPIX_MODELS_DIR);
  }
  if (process.env.XDG_DATA_HOME) {
    candidates.push(join(process.env.XDG_DATA_HOME, 'upix/models'));
  }
  const home = homedir();
  candidates.push(
    join(home, '.local/share/upix/models'),
    join(home, 'Library/Application Support/upix/models'),
  );
  if (process.env.APPDATA) {
    candidates.push(join(process.env.APPDATA, 'upix/models'));
  }
  for (const dir of candidates) {
    if (dir && existsSync(dir)) {
      return dir;
    }
  }
  return null;
}

function proxyHttpsWithRedirects(targetUrl, res, maxRedirects = 5) {
  if (maxRedirects < 0) {
    res.statusCode = 502;
    res.end('Too many redirects');
    return;
  }
  const req = https.get(
    targetUrl,
    { headers: { 'User-Agent': 'upix-web-dev-proxy/0.3' } },
    (upstream) => {
      const status = upstream.statusCode || 500;
      if ([301, 302, 303, 307, 308].includes(status) && upstream.headers.location) {
        upstream.resume();
        const nextUrl = new URL(upstream.headers.location, targetUrl).href;
        proxyHttpsWithRedirects(nextUrl, res, maxRedirects - 1);
        return;
      }
      res.statusCode = status;
      if (upstream.headers['content-type']) {
        res.setHeader('Content-Type', upstream.headers['content-type']);
      }
      if (upstream.headers['content-length']) {
        res.setHeader('Content-Length', upstream.headers['content-length']);
      }
      res.setHeader('Cache-Control', 'public, max-age=600');
      upstream.pipe(res);
    },
  );
  req.on('error', (err) => {
    if (!res.headersSent) {
      res.statusCode = 502;
    }
    res.end(err.message || 'Upstream fetch error');
  });
}

function attachMiddlewares(middlewares) {
  const modelsDir = resolveLocalModelsDir();

  middlewares.use((req, res, next) => {
    const rawUrl = (req.url || '').split('?')[0];

    if (rawUrl.endsWith('.wasm')) {
      res.setHeader('Content-Type', 'application/wasm');
    }

    if (rawUrl === '/api/local-models/index.json') {
      res.setHeader('Content-Type', 'application/json; charset=utf-8');
      if (!modelsDir) {
        res.end('[]');
        return;
      }
      try {
        const files = readdirSync(modelsDir);
        const set = new Set(files);
        const items = [];
        for (const f of files) {
          if (!f.endsWith('.param')) continue;
          const stem = f.slice(0, -6);
          if (!set.has(`${stem}.bin`)) continue;
          const pSize = statSync(join(modelsDir, f)).size;
          const bSize = statSync(join(modelsDir, `${stem}.bin`)).size;
          items.push({ name: stem, sizeBytes: pSize + bSize });
        }
        res.end(JSON.stringify(items));
      } catch {
        res.end('[]');
      }
      return;
    }

    if (rawUrl.startsWith('/api/local-models/')) {
      if (!modelsDir) {
        res.statusCode = 404;
        res.end('Not found');
        return;
      }
      const file = decodeURIComponent(rawUrl.slice('/api/local-models/'.length));
      if (
        !file
        || file.includes('..')
        || file.includes('/')
        || file.includes('\\')
        || (!file.endsWith('.param') && !file.endsWith('.bin'))
      ) {
        res.statusCode = 400;
        res.end('Invalid model file');
        return;
      }
      const fullPath = join(modelsDir, file);
      if (!existsSync(fullPath)) {
        res.statusCode = 404;
        res.end('Not found');
        return;
      }
      const st = statSync(fullPath);
      res.setHeader(
        'Content-Type',
        file.endsWith('.param') ? 'text/plain; charset=utf-8' : 'application/octet-stream',
      );
      res.setHeader('Content-Length', String(st.size));
      createReadStream(fullPath).pipe(res);
      return;
    }

    if (rawUrl.startsWith('/api/upix-models/')) {
      const file = decodeURIComponent(rawUrl.slice('/api/upix-models/'.length));
      if (
        !file
        || file.includes('..')
        || file.includes('/')
        || file.includes('\\')
        || (!file.endsWith('.zip') && file !== 'manifest.json')
      ) {
        res.statusCode = 400;
        res.end('Invalid release asset');
        return;
      }
      proxyHttpsWithRedirects(`${RELEASE_BASE}/${encodeURIComponent(file)}`, res);
      return;
    }

    next();
  });
}

export function upixDevPlugin() {
  return {
    name: 'upix-web-dev-plugin',
    configureServer(server) {
      attachMiddlewares(server.middlewares);
    },
    configurePreviewServer(server) {
      attachMiddlewares(server.middlewares);
    },
  };
}
