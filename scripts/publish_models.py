#!/usr/bin/env python3
"""Publish models to the GitHub "models" release via the gh CLI.

Builds one zip per model (<name>.param + <name>.bin), computes sha256,
generates manifest.json, then creates/updates the release with tag "models"
and uploads all assets with `gh release create/upload/delete-asset`.

Models already in the release are skipped by default (their manifest entry
reuses the release asset's sha256/size); --overwrite re-uploads the selected
ones. Assets absent from the local catalog are pruned unless --no-prune.

Layout consumed by both clients (cli/src/model_downloader.cpp and
apps/.../ModelReleaseRepository.kt):

    releases/download/models/manifest.json
    releases/download/models/<model-name>.zip

Usage:
    ./scripts/publish_models.py [models_dir] [NAME | MODEL STEM ...]

Examples:
    ./scripts/publish_models.py --dry-run              # build everything
    ./scripts/publish_models.py models Real-CUGAN-conservative x2
    ./scripts/publish_models.py models realesrganv3-anime-x2 --overwrite

The models dir defaults to ./models and must contain *.param/*.bin pairs
(the pairs may sit in per-model subdirectories).

Requires: gh authenticated with repo scope (gh auth login).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

DEFAULT_REPO = "xihale/upix"
RELEASE_TAG = "models"
MANIFEST_NAME = "manifest.json"


def gh(*args: str, repo: str, check: bool = True) -> subprocess.CompletedProcess:
    """Run a gh command against `repo`; raises RuntimeError on failure.
    Force-color env vars (CLICOLOR_FORCE/FORCE_COLOR) are stripped so
    --json output stays parseable."""
    cmd = ["gh", *args, "--repo", repo]
    env = {k: v for k, v in os.environ.items() if k not in ("CLICOLOR_FORCE", "FORCE_COLOR")}
    env["NO_COLOR"] = "1"
    proc = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if check and proc.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)}\nfailed ({proc.returncode}): {proc.stderr.strip()}")
    return proc


def find_model_pairs(models_dir: Path) -> list[tuple[str, Path, Path]]:
    """Return (name, param, bin) triples. Names follow ModelManager's rule
    (share/core/model_manager.cpp): files directly under models_dir are named
    by stem; files in a subdirectory are "<dir>-<stem>". This keeps the name
    identical before and after download/install."""
    params: dict[str, Path] = {}
    bins: dict[str, Path] = {}
    for path in sorted(models_dir.rglob("*")):
        if not path.is_file() or path.is_symlink():
            continue
        ext = path.suffix.lower()
        # ModelManager name: stem in root, "<parent>-<stem>" one level down.
        rel_parent = path.parent.relative_to(models_dir)
        name = path.stem if rel_parent == Path(".") else f"{rel_parent.parts[0]}-{path.stem}"
        key = name.lower()
        if ext == ".param":
            params.setdefault(key, path)
        elif ext == ".bin":
            bins.setdefault(key, path)
    pairs = []
    for key, param in params.items():
        bin_path = bins.get(key)
        if bin_path is None:
            print(f"warning: no .bin for {param}", file=sys.stderr)
            continue
        name = param.stem if param.parent == models_dir else f"{param.parent.name}-{param.stem}"
        pairs.append((name, param, bin_path))
    for key in bins.keys() - params.keys():
        print(f"warning: no .param for {bins[key]}", file=sys.stderr)
    return pairs


def select_pairs(pairs, names):
    """Filter pairs by user names. Each name matches a full model name
    (case/underscore-insensitive); two names also match "MODEL STEM"
    (e.g. 'Real-CUGAN-conservative x2' -> Real-CUGAN-conservative-x2)."""
    if not names:
        return pairs
    norm = lambda n: n.lower().replace("_", "-")
    want = {norm(n) for n in names}
    if len(names) == 2:
        want.add(f"{norm(names[0])}-{norm(names[1])}")
    return [p for p in pairs if norm(p[0]) in want]


def build_zip(name: str, param: Path, bin_path: Path, out_dir: Path) -> Path:
    """Write <name>.zip. Entry names carry the full model name so a flattened
    extract yields <name>.param/<name>.bin in the models root."""
    zip_path = out_dir / f"{name}.zip"
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        zf.write(param, arcname=f"{name}.param")
        zf.write(bin_path, arcname=f"{name}.bin")
    return zip_path


def scale_from_name(name: str) -> int:
    """Infer scale from the trailing -xN / _xN / xN convention. The number
    may be followed by non-digits (e.g. "Real-ESRGAN-x4plus", "…-x4v3")."""
    scale = 2
    for m in re.finditer(r"[xX](\d+)(?!\d)", name):
        if 2 <= int(m.group(1)) <= 8:
            scale = int(m.group(1))
    return scale


# Per-model license, surfaced as an informational "license" field in
# manifest.json (both clients ignore unknown fields; terms live in
# models/README.md). The Upix code is AGPL-3.0, but these CC-BY-NC-SA
# weights stay noncommercial-only — a commercial user of the code must
# drop or replace them.
LICENSES = {
    "UltraSharp-x4": "CC-BY-NC-SA-4.0",
    "AnimeSharp-x4": "CC-BY-NC-SA-4.0",
    "AnimeJaNai-v2-Compact-x2": "CC-BY-NC-SA-4.0",
    "Remacri-x4": "CC-BY-NC-SA-4.0",
    "Nomos8kSC-x4": "CC-BY-4.0",
    "NomosUni-Compact-x2": "CC-BY-4.0",
    "LSDIR-Compact-v2-x4": "CC-BY-4.0",
    "NomosWebPhoto-RealPLKSR-x4": "CC-BY-4.0",
    "NomosUni-SPAN-x4": "CC-BY-4.0",
    "NomosUni-SPAN-x2": "CC-BY-4.0",
    "HFA2k-LUDVAE-SPAN-x2": "CC-BY-4.0",
    "ModernSpanimation-x2": "MIT",
    "NMKD-Siax-x4": "WTFPL",
}


def license_from_name(name: str) -> str:
    """License for a model; refuses to guess so a new model can never ship
    with its license silently unclassified."""
    if name in LICENSES:
        return LICENSES[name]
    if name.startswith("Real-CUGAN"):
        return "MIT"
    if name.startswith("Real-ESRGAN"):
        return "BSD-3-Clause"
    raise ValueError(f"license not classified for {name!r}; add it to LICENSES")


def fetch_release_assets(repo: str, tag: str) -> dict[str, dict] | None:
    """Existing release assets as {name: asset}; None if the release
    itself doesn't exist yet (distinct from an empty-asset release)."""
    proc = gh("release", "view", tag, "--json", "assets", repo=repo, check=False)
    if proc.returncode != 0:
        return None
    assets = json.loads(proc.stdout).get("assets", [])
    return {a["name"]: a for a in assets}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("models_dir", nargs="?", default="models", type=Path)
    parser.add_argument("names", nargs="*",
                        help="only publish these models: full name, or MODEL STEM "
                             "(e.g. 'Real-CUGAN-conservative x2'); default all")
    parser.add_argument("--repo", default=DEFAULT_REPO, help="owner/name")
    parser.add_argument("--tag", default=RELEASE_TAG)
    parser.add_argument("--out", default=None, type=Path,
                        help="staging dir for zips/manifest (default: temp)")
    parser.add_argument("--dry-run", action="store_true",
                        help="build zips + manifest, skip upload")
    parser.add_argument("--overwrite", action="store_true",
                        help="re-upload selected models already in the release "
                             "(default: skip them, reuse their sha256/size)")
    parser.add_argument("--no-prune", dest="prune", action="store_false",
                        help="keep release assets that no longer exist locally")
    args = parser.parse_args()

    if shutil.which("gh") is None:
        print("error: gh CLI not found (gh auth login first)", file=sys.stderr)
        return 1

    models_dir = args.models_dir.resolve()
    if not models_dir.is_dir():
        print(f"error: {models_dir} is not a directory", file=sys.stderr)
        return 1

    pairs = find_model_pairs(models_dir)
    if not pairs:
        print(f"error: no .param/.bin pairs under {models_dir}", file=sys.stderr)
        return 1
    selected = select_pairs(pairs, args.names)
    if not selected:
        available = ", ".join(p[0] for p in pairs)
        print(f"error: no model matches {' '.join(args.names)}; available: {available}",
              file=sys.stderr)
        return 1
    selected_names = {name for name, _, _ in selected}

    staging = args.out or Path(tempfile.mkdtemp(prefix="upix-models-"))
    staging.mkdir(parents=True, exist_ok=True)

    release = fetch_release_assets(args.repo, args.tag)
    existing = release or {}

    if args.dry_run:
        for name, param, bin_path in selected:
            zip_path = build_zip(name, param, bin_path, staging)
            print(f"built {zip_path.name}: {zip_path.stat().st_size} bytes")
        print("dry run: skipping upload")
        return 0

    # 1. Create the release if missing (not marked latest).
    if release is None:
        gh("release", "create", args.tag,
           "--title", "Models",
           "--notes", "Model files for Upix CLI and Android. "
                      "Regenerated by scripts/publish_models.py.",
           "--latest=false", repo=args.repo)
        print(f"created release {args.tag}")

    # 2. Build manifest entries: models already in the release are skipped
    #    (reuse the asset's digest/size so the manifest stays consistent);
    #    --overwrite rebuilds and re-uploads the selected ones.
    def entry(name, size, sha256):
        return {
            "name": name,
            "scale": scale_from_name(name),
            "license": license_from_name(name),
            "size_bytes": size,
            "sha256": sha256,
            "url": f"https://github.com/{args.repo}/releases/download/{args.tag}/{name}.zip",
        }

    entries, to_upload, doomed = [], [], set()
    for name, param, bin_path in pairs:
        asset = existing.get(f"{name}.zip")
        digest = (asset or {}).get("digest") or ""
        reusable = asset and digest.startswith("sha256:") \
            and not (args.overwrite and name in selected_names)
        if reusable:
            entries.append(entry(name, asset["size"], digest.removeprefix("sha256:")))
            print(f"skip {name}.zip (already in release)")
            continue
        if name not in selected_names:
            print(f"warning: {name} has no release asset and was not selected; "
                  "omitted from manifest", file=sys.stderr)
            continue
        if asset:
            doomed.add(asset["name"])  # overwritten
        zip_path = build_zip(name, param, bin_path, staging)
        sha256 = hashlib.sha256(zip_path.read_bytes()).hexdigest()
        entries.append(entry(name, zip_path.stat().st_size, sha256))
        to_upload.append(zip_path)
        print(f"built {zip_path.name}: {zip_path.stat().st_size} bytes sha256={sha256[:12]}…")

    manifest = {"version": 1, "models": entries}
    manifest_path = staging / MANIFEST_NAME
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"wrote {manifest_path} ({len(entries)} models)")

    # 3. Delete: overwritten zips, the old manifest, and (unless --no-prune)
    #    assets absent from the manifest.
    wanted = {MANIFEST_NAME} | {f"{e['name']}.zip" for e in entries}
    if args.prune:
        doomed |= set(existing) - wanted
    if MANIFEST_NAME in existing:
        doomed.add(MANIFEST_NAME)
    for asset_name in sorted(doomed):
        gh("release", "delete-asset", args.tag, asset_name, "--yes", repo=args.repo)
        print(f"deleted old asset {asset_name}")

    # 4. Upload. Overwritten zips and the old manifest were deleted above,
    #    so no name collisions remain (`gh release upload` errors on
    #    same-name assets rather than skipping).
    uploads = to_upload + [manifest_path]
    if uploads:
        gh("release", "upload", args.tag, *[str(p) for p in uploads], repo=args.repo)
        for path in uploads:
            print(f"uploaded {path.name}")

    print("done")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except RuntimeError as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)
