import { defineConfig } from 'vite';
import { upixDevPlugin } from './scripts/vite-upix-plugin.mjs';

// Cross-origin isolation unlocks SharedArrayBuffer & high-res timers while
// 'credentialless' allows cross-origin model downloads without CORP headers.
const isolation = {
  'Cross-Origin-Opener-Policy': 'same-origin',
  'Cross-Origin-Embedder-Policy': 'credentialless',
};

export default defineConfig({
  // Relative base: dist/ serves from any sub-path or a local static server.
  base: './',
  plugins: [upixDevPlugin()],
  server: { headers: isolation },
  preview: { headers: isolation },
  worker: { format: 'es' },
  build: { target: 'es2022', sourcemap: false },
});
