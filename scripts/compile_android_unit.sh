#!/usr/bin/env bash
# Compile debug + unit-test sources. Does not run tests, does not build native.
set -euo pipefail

cd "$(dirname "$0")/../apps"
# Runtime-jar is consumed by testDebugUnitTest but is not part of
# compileDebugUnitTestSources; build it here so the test job can -x both.
./gradlew compileDebugUnitTestSources bundleDebugClassesToRuntimeJar \
    -x xmakeNative --no-daemon
