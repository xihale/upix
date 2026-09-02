#!/usr/bin/env bash
# Serve E2E suite: daemon protocol robustness, concurrency, lifecycle, IO abuse.
# Wraps tests/serve/run.py (python3 + numpy + Pillow required).
#
#   --device gpu     run the shared daemon on Vulkan (default: cpu)
#   --binary PATH    upix-cli path (default: build/<plat>/<arch>/release)
#   --model NAME     primary model, must exist in --models-dir
#   --suite NAME     run a single suite: protocol|concurrency|io|lifecycle
#   --keep-work      keep temp work dir for debugging
#
# First-time setup: scripts/build_cli.sh release
# and: upix-cli model download NomosUni-SPAN-x2 [HFA2k-LUDVAE-SPAN-x2]
set -euo pipefail

cd "$(dirname "$0")/.."
exec python3 tests/serve/run.py "$@"
