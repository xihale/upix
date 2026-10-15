import { defineConfig } from 'vite';

// Download links in index.html (github.com/xihale/upix/releases/tag/…) resolve
// at build time: the newest android-* / cli-* release replaces the pinned
// fallbacks, so a Pages deploy right after a release never goes stale. CI
// passes GITHUB_TOKEN (5000 req/h); anonymous or offline builds fall back to
// the tags below.
const FALLBACK_TAGS = { android: 'android-v0.1.1', cli: 'cli-v0.1.0' };

async function resolveTags() {
  try {
    const headers = { accept: 'application/vnd.github+json' };
    if (process.env.GITHUB_TOKEN) headers.authorization = `Bearer ${process.env.GITHUB_TOKEN}`;
    const response = await fetch('https://api.github.com/repos/xihale/upix/releases?per_page=100', { headers });
    if (!response.ok) throw new Error(`GitHub API ${response.status}`);
    const releases = await response.json();
    const tags = { ...FALLBACK_TAGS };
    for (const prefix of Object.keys(FALLBACK_TAGS)) {
      const hit = releases.find(release => !release.draft && release.tag_name.startsWith(`${prefix}-`));
      if (hit) tags[prefix] = hit.tag_name;
    }
    return tags;
  } catch (error) {
    console.warn(`[release-links] falling back to pinned tags: ${error.message}`);
    return FALLBACK_TAGS;
  }
}

export default defineConfig({
  // Relative base: dist/ serves from any sub-path (xihale.github.io/upix/) or
  // a local static server without rebuild.
  base: './',
  plugins: [{
    name: 'upix-release-links',
    apply: 'build',
    async transformIndexHtml(html) {
      const tags = await resolveTags();
      return Object.entries(tags).reduce(
        (acc, [prefix, tag]) => acc.replace(new RegExp(`releases/tag/${prefix}-[\\w.]+`), `releases/tag/${tag}`),
        html,
      );
    },
  }],
});
