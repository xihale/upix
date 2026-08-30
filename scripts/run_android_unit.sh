#!/usr/bin/env bash
# Run already-compiled unit tests. Skips Kotlin/Java compile tasks so a
# downloaded apps/app/build from the compile job is not rebuilt.
set -euo pipefail

cd "$(dirname "$0")/../apps"
./gradlew testDebugUnitTest -x xmakeNative \
    -x compileDebugUnitTestSources \
    -x compileDebugKotlin \
    -x compileDebugJavaWithJavac \
    -x compileDebugUnitTestKotlin \
    -x compileDebugUnitTestJavaWithJavac \
    -x bundleDebugClassesToRuntimeJar \
    -x bundleDebugClassesToCompileJar \
    --no-daemon
