#!/usr/bin/env sh
# Configure and build the Qt Quick desktop app. Extra args go to cmake configure.
set -e
cd "$(dirname "$0")"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -G Ninja "$@"
cmake --build build
echo "binary: build/upix"
