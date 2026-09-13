#!/usr/bin/env bash
# Serve daemon BRUTE-FORCE stress campaign. Wraps tests/serve_stress/run.py.
# Complements scripts/run_serve_tests.sh (handcrafted E2E) with randomized
# fuzzing, resource-leak soak, and lifecycle abuse.
#
#   --device gpu     run the shared daemon on Vulkan (default: cpu)
#   --binary PATH    upix-cli path (default: build/<plat>/<arch>/release)
#   --model NAME     primary model, must exist in --models-dir
#   --suite NAME     run a single suite: fuzz|semantic|churn|soak|lifecycle2
#   --seed N         fuzz seed (default: 20260913)
#   --quick          reduced iteration counts (smoke pass)
#   --keep-work      keep temp work dir for debugging
#
# First-time setup: scripts/build_cli.sh release
# and: upix-cli model download NomosUni-SPAN-x2
set -euo pipefail

cd "$(dirname "$0")/.."
exec python3 -u tests/serve_stress/run.py "$@"
