#!/usr/bin/env sh
# Build the SDL3 + Vulkan + Dear ImGui desktop app via root xmake.
set -e
cd "$(dirname "$0")/.."
xmake f -m release -y "$@"
xmake build upix
echo "binary: desktop/build/upix"
