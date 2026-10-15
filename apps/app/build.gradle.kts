plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
    // Required for @Serializable (kotlinx-serialization-json is already a
    // dependency); version must match the Kotlin toolchain version.
    id("org.jetbrains.kotlin.plugin.serialization") version libs.versions.kotlin.get()
}

android {
    namespace = "io.upix"
    compileSdk = libs.versions.compileSdk.get().toInt()

    defaultConfig {
        applicationId = "io.upix"
        minSdk = libs.versions.minSdk.get().toInt()
        targetSdk = libs.versions.targetSdk.get().toInt()
        // Android release channel — independent of the CLI version (root
        // xmake.lua). Release tags: android-v*.
        versionCode = 2
        versionName = "0.1.1"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        vectorDrawables {
            useSupportLibrary = true
        }
        ndk {
            abiFilters += "arm64-v8a"
        }
    }

    // Security fix: Define signing configs separately. Release build uses
    // environment variables for the signing key, not the debug keystore.
    // A release build without credentials must never silently fall back to
    // the debug key: warn always, and fail hard when a release APK is
    // actually requested (see buildTypes.release below).
    val releaseKeystorePath = System.getenv("UPIX_RELEASE_STORE_FILE")
    // Explicit opt-out for local testing: -Pupix.allowDebugSignedRelease.
    val allowDebugSignedRelease =
        findProperty("upix.allowDebugSignedRelease")?.let { it != "false" } ?: false
    // True when this invocation assembles a release APK, matching plain and
    // qualified task names ("assembleRelease", ":apps:app:assembleRelease").
    val requestsReleaseAssembly = gradle.startParameter.taskNames.any { taskName ->
        taskName.substringAfterLast(':').equals("assembleRelease", ignoreCase = true)
    }

    signingConfigs {
        // Pin the debug keystore explicitly: depending on the environment AGP
        // resolves it to either ~/.android/debug.keystore or
        // ~/.config/.android/debug.keystore, and machines having both end up
        // with the app and its androidTest APK signed by DIFFERENT keys —
        // breaking adb install updates and instrumentation with a confusing
        // "signature matching" permission denial.
        getByName("debug") {
            storeFile = file("${System.getProperty("user.home")}/.android/debug.keystore")
        }
        create("release") {
            if (releaseKeystorePath != null) {
                storeFile = file(releaseKeystorePath)
                storePassword = System.getenv("UPIX_RELEASE_STORE_PASSWORD") ?: ""
                keyAlias = System.getenv("UPIX_RELEASE_KEY_ALIAS") ?: ""
                keyPassword = System.getenv("UPIX_RELEASE_KEY_PASSWORD") ?: ""
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            // Security fix: dedicated release signing config. Without
            // credentials the build used to silently sign with the debug key;
            // now it warns, and refuses to assemble a release APK unless the
            // caller opts out explicitly via -Pupix.allowDebugSignedRelease.
            signingConfig = if (signingConfigs.getByName("release").storeFile != null) {
                signingConfigs.getByName("release")
            } else {
                logger.warn(
                    "UPIX: UPIX_RELEASE_STORE_FILE is not set; the build falls back to the " +
                        "DEBUG keystore. Fine for debug variants, NOT publishable."
                )
                if (requestsReleaseAssembly && !allowDebugSignedRelease) {
                    throw GradleException(
                        "Refusing to assemble a release APK signed with the debug key. " +
                            "Configure release signing by exporting UPIX_RELEASE_STORE_FILE, " +
                            "UPIX_RELEASE_STORE_PASSWORD, UPIX_RELEASE_KEY_ALIAS and " +
                            "UPIX_RELEASE_KEY_PASSWORD, or opt out explicitly for local " +
                            "testing with -Pupix.allowDebugSignedRelease."
                    )
                }
                signingConfigs.getByName("debug")
            }
        }
        debug {
            isDebuggable = true
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_21
        targetCompatibility = JavaVersion.VERSION_21
    }

    buildFeatures {
        compose = true
    }

    // Models are NOT bundled: they are downloaded from the GitHub "models"
    // release (Models screen) or imported from local files by the user.

    packaging {
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
        jniLibs {
            // Compress .so in the APK. Install still extracts them, so 16 KB
            // page alignment is preserved; sideload size is what shrinks.
            useLegacyPackaging = true
            // LLVM OpenMP TSan helper, copied from the libomp xrepo package
            // and unused at runtime (libupix-native does not NEEDED it).
            excludes += "**/libarcher.so"
        }
    }

    ndkVersion = "29.0.14206865"
}

val repoRoot = rootProject.projectDir.parentFile
val buildNativeScript = rootProject.projectDir.resolve("scripts/build_native.sh")

// Native is always xmake -m release (-O3 / stripped), matching the old CMake
// flags. Debug APKs still load this .so; JNI debugging is not the debug build.
// Optional AVIF codecs: -Pupix.avifEncoder=true (+~5 MB .so, on-device AVIF
// output) / -Pupix.avifDecoder=true (+~1 MB, native AVIF input decode). Both
// flags are passed explicitly (y/n) so the properties are the single source
// of truth — xmake options are sticky across `xmake f` runs otherwise.
tasks.register<Exec>("xmakeNative") {
    group = "build"
    description = "Build libupix-native.so with xmake"
    workingDir = repoRoot
    val nativeArgs = mutableListOf(
        buildNativeScript.absolutePath,
        "release",
        "arm64-v8a",
        "--avif-encoder=" + if (findProperty("upix.avifEncoder")?.toString()?.equals("true", ignoreCase = true) == true) "y" else "n",
        "--avif-decoder=" + if (findProperty("upix.avifDecoder")?.toString()?.equals("true", ignoreCase = true) == true) "y" else "n"
    )
    commandLine(nativeArgs)
    inputs.files(
        repoRoot.resolve("xmake.lua"),
        repoRoot.resolve("apps/xmake.lua"),
        repoRoot.resolve("scripts/build_native.sh"),
        buildNativeScript,
    )
    inputs.dir(repoRoot.resolve("share"))
    inputs.dir(repoRoot.resolve("packages"))
    inputs.dir(file("src/main/cpp"))
    outputs.dir(file("src/main/jniLibs/arm64-v8a"))
}

// Package-time only. Hooking preDebugBuild pulled a full NDK/xmake compile
// into testDebugUnitTest (and every other debug compile), which is why CI
// could not run Kotlin unit tests without rebuilding ncnn.
tasks.matching {
    it.name == "mergeDebugJniLibFolders" || it.name == "mergeReleaseJniLibFolders"
}.configureEach {
    dependsOn("xmakeNative")
}

kotlin {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_21)
    }
}

dependencies {
    // AndroidX Core
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.activity.compose)

    // Compose BOM
    val composeBom = platform(libs.androidx.compose.bom)
    implementation(composeBom)
    androidTestImplementation(composeBom)

    // Compose UI
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.graphics)
    implementation(libs.androidx.compose.ui.tooling.preview)
    // Expressive API（MaterialExpressiveTheme/MotionScheme）在 material3 1.4 为 internal，
    // 版本在 catalog 钉到 1.5；BOM 仍管 UI/adaptive 系列其余工件。
    implementation(libs.androidx.compose.material3)
    // MD3 adaptive: window adaptive info + list-detail pane scaffold (BOM-managed)
    implementation(libs.androidx.compose.adaptive)
    implementation(libs.androidx.compose.adaptive.layout)
    implementation(libs.androidx.compose.adaptive.navigation)

    // Material icon vector sets. Not part of the Compose BOM; pinned via
    // libs.materialIcons (1.7.8, final release of the independent version line).
    // material-icons-core comes in transitively via -extended.
    implementation(libs.androidx.compose.material.icons.extended)

    // ViewModel
    implementation(libs.androidx.lifecycle.viewmodel.compose)
    // lifecycle-aware Compose state collection (collectAsStateWithLifecycle)
    implementation(libs.androidx.lifecycle.runtime.compose)

    // EXIF orientation for image decode paths
    implementation(libs.androidx.exifinterface)

    // Navigation
    implementation(libs.androidx.navigation.compose)

    // Coroutines
    implementation(libs.kotlinx.coroutines.android)

    // CameraX for QR scanning
    implementation(libs.androidx.camera.core)
    implementation(libs.androidx.camera.camera2)
    implementation(libs.androidx.camera.lifecycle)
    implementation(libs.androidx.camera.view)

    // QR decode. Bundled ML Kit barcode-scanning pulled ~5 MB native/ABI
    // (libbarhopper) plus tflite models; this app only scans QR payloads.
    implementation(libs.zxing.core)

    // JSON
    implementation(libs.kotlinx.serialization.json)

    // Testing
    testImplementation(libs.junit)
    testImplementation(libs.kotlinx.coroutines.test)
    testImplementation(libs.robolectric)
    androidTestImplementation(libs.androidx.test.ext.junit)
    androidTestImplementation(libs.androidx.test.runner)
    debugImplementation(libs.androidx.compose.ui.tooling)
}
