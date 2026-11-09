#!/usr/bin/env bash
# Model-effect regression oracle: CPU inference of the catalog case models,
# compared against committed golden PNGs. This is the drift gate for
# engine-side changes (ncnn bump, custom layer, model_graph analysis).
# Wraps tests/model_oracle/run.py (python3 + numpy + Pillow required).
#
#   --require-models  missing case model = failure (CI); default: skip + note
#   --update-golden   rewrite golden/<case>.png after printing the drift vs the
#                     previous goldens — review that output before committing
#   --case NAME       run a single case (repeatable)
#   --binary PATH     upix-cli path (default: target/release, then legacy xmake)
#   --models-dir DIR  model directory (default: ~/.local/share/upix/models)
#   --keep-work       keep the temp work dir (a failure keeps it anyway)
#
# First-time setup: cargo build --release -p upix-cli
# and: upix-cli model download NomosUni-SPAN-x2 Real-ESRGANv3-anime-x2 \
#        Archivist-Medium-Compact-x1 NomosWebPhoto-RealPLKSR-x4 \
#        Real-ESRGAN-x2plus Real-CUGAN-no-denoise-x2
# Case list and thresholds: tests/model_oracle/cases.py
set -euo pipefail

cd "$(dirname "$0")/.."
exec python3 tests/model_oracle/run.py "$@"
