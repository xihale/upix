import { defineConfig } from 'vite';
import { upixDevPlugin } from './scripts/vite-upix-plugin.mjs';

// Cross-origin isolation unlocks SharedArrayBuffer & high-res timers while
// 'credentialless' allows cross-origin model downloads without CORP headers.
const isolation = {
  'Cross-Origin-Opener-Policy': 'same-origin',
  'Cross-Origin-Embedder-Policy': 'credentialless',
};

const FALLBACK_TAGS = {
  gui: 'gui-v0.2.0-beta.1',
  android: 'android-v0.1.1',
  cli: 'cli-v0.2.0-beta.1',
};

const PREFIXED_TAG = /^(gui|android|cli)-[A-Za-z0-9._-]+$/;

let cachedTagsPromise = null;

async function resolveTags() {
  try {
    const headers = { accept: 'application/vnd.github+json' };
    if (process.env.GITHUB_TOKEN) headers.authorization = `Bearer ${process.env.GITHUB_TOKEN}`;
    const response = await fetch('https://api.github.com/repos/xihale/upix/releases?per_page=100', {
      headers,
      signal: AbortSignal.timeout(10_000),
    });
    if (!response.ok) throw new Error(`GitHub API ${response.status}`);
    const releases = await response.json();
    const tags = { ...FALLBACK_TAGS };
    for (const prefix of Object.keys(FALLBACK_TAGS)) {
      const hit = releases.find((release) => !release.draft
        && typeof release.tag_name === 'string'
        && release.tag_name.startsWith(`${prefix}-`)
        && PREFIXED_TAG.test(release.tag_name));
      if (hit) tags[prefix] = hit.tag_name;
    }
    return tags;
  } catch (error) {
    console.warn(`[release-links] falling back to pinned tags: ${error.message}`);
    return { ...FALLBACK_TAGS };
  }
}

function getResolvedTags() {
  if (!cachedTagsPromise) cachedTagsPromise = resolveTags();
  return cachedTagsPromise;
}

function upixReleaseLinksPlugin() {
  return {
    name: 'upix-release-links',
    async config(_config, { command }) {
      const tags = command === 'build' ? await getResolvedTags() : { ...FALLBACK_TAGS };
      return {
        define: {
          __UPIX_RELEASES__: JSON.stringify(tags),
        },
      };
    },
    async transformIndexHtml(html, ctx) {
      if (ctx?.server) return html;
      const tags = await getResolvedTags();
      return Object.entries(tags).reduce(
        (acc, [prefix, tag]) => acc.replace(
          new RegExp(`releases/tag/${prefix}-[A-Za-z0-9._-]+`, 'g'),
          `releases/tag/${tag}`,
        ),
        html,
      );
    },
  };
}

export default defineConfig({
  // Relative base: dist/ serves from any sub-path or a local static server.
  base: './',
  plugins: [upixDevPlugin(), upixReleaseLinksPlugin()],
  server: { headers: isolation },
  preview: { headers: isolation },
  worker: { format: 'es' },
  build: { target: 'es2022', sourcemap: false },
});
