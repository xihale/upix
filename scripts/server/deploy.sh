#!/usr/bin/env bash
set -euo pipefail

# Webhook deploy script executed by /usr/local/lib/webhook-deploy on gx.
# Contract:
#   $1           -> git ref (e.g. refs/tags/web-v0.2.0)
#   $ZP_PUSH_SHA -> expected commit SHA exported by webhook-deploy (optional in manual runs)

REF="${1:?usage: deploy.sh <git-ref>}"
REPO_DIR="${UPIX_REPO_DIR:-$HOME/upix}"
SITE_DIR="${UPIX_WEB_SITE_DIR:-/srv/upix-web}"
ATTIC_DIR="${SITE_DIR}/.attic"
ZIG_VERSION="0.17.0"
ZIG_SHA256="1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026"
ZIG_DIR="${HOME}/.local/share/zvm/${ZIG_VERSION}"
LOCK_FILE="${HOME}/.deploy.lock"

exec 9>"$LOCK_FILE"
flock -x 9

echo "=== [$(date -Is)] deploy ref=${REF} expected_sha=${ZP_PUSH_SHA:-none} ==="

ensure_zig() {
  if [[ -x "${ZIG_DIR}/zig" ]]; then
    export PATH="${ZIG_DIR}:$PATH"
    return
  fi
  local arch zig_arch tarball tmp_tar tmp_dir
  arch="$(uname -m)"
  case "$arch" in
    x86_64) zig_arch="x86_64-linux" ;;
    aarch64) zig_arch="aarch64-linux" ;;
    *)
      echo "unsupported architecture for bundled zig download: ${arch}" >&2
      exit 1
      ;;
  esac
  tarball="zig-${zig_arch}-${ZIG_VERSION}.tar.xz"
  tmp_tar="$(mktemp /tmp/zig-XXXXXX.tar.xz)"
  tmp_dir="$(mktemp -d /tmp/zig-unpack-XXXXXX)"
  echo "installing zig ${ZIG_VERSION} into ${ZIG_DIR}..."
  curl -fsSL "https://ziglang.org/download/${ZIG_VERSION}/${tarball}" -o "$tmp_tar"
  if [[ "$arch" == "x86_64" ]]; then
    echo "${ZIG_SHA256}  ${tmp_tar}" | sha256sum -c -
  fi
  mkdir -p "$ZIG_DIR"
  tar -xf "$tmp_tar" -C "$tmp_dir" --strip-components=1
  mv "$tmp_dir"/* "$ZIG_DIR"/
  rm -rf "$tmp_tar" "$tmp_dir"
  export PATH="${ZIG_DIR}:$PATH"
}

git_fetch_ref() {
  local attempt
  for attempt in 1 2 3 4; do
    if git -C "$REPO_DIR" fetch --depth=1 --force origin "$REF"; then
      return 0
    fi
    echo "git fetch failed (attempt $attempt/4), retrying in $((attempt * 4))s..." >&2
    sleep $((attempt * 4))
  done
  return 1
}

git_fetch_ref

BUILT_SHA="$(git -C "$REPO_DIR" rev-parse FETCH_HEAD)"
if [[ -n "${ZP_PUSH_SHA:-}" && "${ZP_PUSH_SHA}" != "0000000000000000000000000000000000000000" ]]; then
  if [[ "$BUILT_SHA" != "$ZP_PUSH_SHA" ]]; then
    echo "WARNING: FETCH_HEAD (${BUILT_SHA}) differs from ZP_PUSH_SHA (${ZP_PUSH_SHA}); checking out ZP_PUSH_SHA directly" >&2
    git -C "$REPO_DIR" fetch --depth=1 origin "$ZP_PUSH_SHA"
    BUILT_SHA="$(git -C "$REPO_DIR" rev-parse FETCH_HEAD)"
  fi
fi

git -C "$REPO_DIR" checkout --force --detach "$BUILT_SHA"
git -C "$REPO_DIR" clean -ffd -- wasm web

ensure_zig

cd "${REPO_DIR}/web"
npm ci --no-audit --no-fund
npm run lint
npm run build:wasm
npm run build

DIST_DIR="${REPO_DIR}/web/dist"
test -f "${DIST_DIR}/index.html"
test -f "${DIST_DIR}/wasm/upix_wasm.wasm"
test -f "${DIST_DIR}/wasm/upix_wasm_scalar.wasm"

# GitHub Pages CNAME is obsolete once served from web.upix.xeed.ink.
rm -f "${DIST_DIR}/CNAME"

# Precompress static assets for Caddy `precompressed gzip`.
find "$DIST_DIR" -type f \( -name '*.wasm' -o -name '*.js' -o -name '*.css' -o -name '*.svg' \) -size +1024c \
  -exec gzip -9 -k -f {} +

cat > "${DIST_DIR}/deploy-meta.json" <<EOF
{"ref":"${REF}","sha":"${BUILT_SHA}","builtAt":"$(date -u +%Y-%m-%dT%H:%M:%SZ)"}
EOF

mkdir -p "$SITE_DIR" "$ATTIC_DIR"
find "$ATTIC_DIR" -type f -mtime +14 -delete 2>/dev/null || true
if [[ -d "${SITE_DIR}/assets" ]]; then
  rsync -a "${SITE_DIR}/assets/" "$ATTIC_DIR/"
fi

rsync -a --delete --exclude '.attic' "${DIST_DIR}/" "${SITE_DIR}/"
if [[ -d "$ATTIC_DIR" ]] && compgen -G "${ATTIC_DIR}/*" >/dev/null; then
  mkdir -p "${SITE_DIR}/assets"
  rsync -a --ignore-existing "${ATTIC_DIR}/" "${SITE_DIR}/assets/"
fi

echo "=== [$(date -Is)] deployed ${REF} (${BUILT_SHA:0:7}) -> ${SITE_DIR} ==="
