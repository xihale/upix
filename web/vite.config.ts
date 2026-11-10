import { defineConfig, type Plugin } from 'vite';
import { cpSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';

/**
 * Copy the ORT wasm/jsep runtime into public/ort so the app is self-contained
 * (no CDN — the whole app must work offline, like the rest of Upix).
 * ort.env.wasm.wasmPaths = 'ort/' (src/pipeline/session.ts).
 */
function copyOrtRuntime(): Plugin {
  const files = [
    'ort-wasm-simd-threaded.jsep.mjs',
    'ort-wasm-simd-threaded.jsep.wasm',
    'ort-wasm-simd-threaded.asyncify.mjs',
    'ort-wasm-simd-threaded.asyncify.wasm',
    'ort-wasm-simd-threaded.mjs',
    'ort-wasm-simd-threaded.wasm',
  ];
  return {
    name: 'upix-copy-ort-runtime',
    // configResolved runs BEFORE the dev server's static middleware snapshots
    // publicDir: copying in buildStart leaves a first run serving the SPA
    // fallback (text/html) for /ort/*.mjs, which the browser refuses to import.
    configResolved() {
      const dst = join(__dirname, 'public', 'ort');
      mkdirSync(dst, { recursive: true });
      for (const f of files) {
        const src = join(__dirname, 'node_modules', 'onnxruntime-web', 'dist', f);
        try {
          cpSync(src, join(dst, f));
        } catch {
          // tolerate missing optional files; session.ts fails loudly if the
          // needed one is absent at runtime
        }
      }
    },
  };
}

export default defineConfig({
  base: './',
  plugins: [copyOrtRuntime()],
  server: {
    port: 5175,
    // Local dev with SharedArrayBuffer (ORT wasm threads). Production note:
    // GitHub Pages cannot set response headers — wasm threads stay off there
    // (see issue #2); WebGPU EP is unaffected.
    headers: {
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
    },
  },
  build: {
    target: 'es2022',
    assetsInlineLimit: 0,
  },
});
