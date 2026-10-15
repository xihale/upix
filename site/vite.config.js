import { defineConfig } from 'vite';

// Download links in index.html (github.com/xihale/upix/releases/tag/…) resolve
// at build time: the newest android-* / cli-* release replaces the pinned
// fallbacks, so a Pages deploy right after a release never goes stale. CI
// passes GITHUB_TOKEN (5000 req/h); anonymous or offline builds fall back to
// the tags below.
const FALLBACK_TAGS = { android: 'android-v0.1.1', cli: 'cli-v0.1.0' };

// Tag names go into index.html verbatim, so only a charset that is inert in
// both an HTML attribute and a URL path is accepted; git itself allows '"',
// '<', '>' and '&' in ref names. A rejected tag keeps the pinned fallback.
const PREFIXED_TAG = new RegExp(`^(${Object.keys(FALLBACK_TAGS).join('|')})-[A-Za-z0-9._-]+$`);

async function resolveTags() {
  try {
    const headers = { accept: 'application/vnd.github+json' };
    if (process.env.GITHUB_TOKEN) headers.authorization = `Bearer ${process.env.GITHUB_TOKEN}`;
    const response = await fetch('https://api.github.com/repos/xihale/upix/releases?per_page=100', {
      headers,
      // Node's undici defaults hang for 300s; the Pages job only has 5 minutes,
      // so a stalled API must fall through to the pinned tags instead.
      signal: AbortSignal.timeout(10_000),
    });
    if (!response.ok) throw new Error(`GitHub API ${response.status}`);
    const releases = await response.json();
    const tags = { ...FALLBACK_TAGS };
    for (const prefix of Object.keys(FALLBACK_TAGS)) {
      const hit = releases.find(release => !release.draft
        && release.tag_name.startsWith(`${prefix}-`)
        && PREFIXED_TAG.test(release.tag_name));
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
      // Consume the whole pinned token, '-suffix' included, so the replacement
      // can never leave a truncated anchor behind (tags are pre-validated).
      return Object.entries(tags).reduce(
        (acc, [prefix, tag]) => acc.replace(
          new RegExp(`releases/tag/${prefix}-[A-Za-z0-9._-]+`, 'g'),
          `releases/tag/${tag}`,
        ),
        html,
      );
    },
  }],
});
