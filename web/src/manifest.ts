/**
 * Model catalog fetch + integrity checks.
 *
 * Dev builds use `public/models/manifest.json` (the two generated fixtures).
 * Production uses the same schema as the catalog `manifest.json` published to
 * the GitHub `models` release (name/scale/license/size_bytes/sha256/url) — the
 * web app only needs ONNX assets added to that release (issue #2, B1).
 */
import type { ModelInfo } from './engine';

export interface ModelManifest {
  version: number;
  models: ModelInfo[];
}

export async function fetchManifest(url = 'models/manifest.json'): Promise<ModelManifest> {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`manifest fetch failed: ${res.status}`);
  return (await res.json()) as ModelManifest;
}

/** Fetch + sha256-verify a model (crypto.subtle, mirrors catalog byte-match gate). */
export async function fetchModel(model: ModelInfo): Promise<Uint8Array> {
  const res = await fetch(model.url);
  if (!res.ok) throw new Error(`model fetch failed: ${res.status} ${model.url}`);
  const bytes = new Uint8Array(await res.arrayBuffer());
  const digest = await crypto.subtle.digest('SHA-256', bytes);
  const got = [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, '0')).join('');
  if (got !== model.sha256) {
    throw new Error(`sha256 mismatch for ${model.name}: got ${got}, want ${model.sha256}`);
  }
  return bytes;
}
