# AGENTS.md - Development Guide for CrossScale

This repository contains CrossScale, a distributed image super-resolution system. It features cross-platform CLI (C++), Android app (Kotlin + C++ NDK), and shared C++ core logic. The system supports distributed computing across multiple devices via QR code connection and weight-based task allocation.

## 🛠 Build, Test, and Lint Commands

### App Platforms (Kotlin/C++)
The multiplatform app workspace is located in `apps/`. It contains the Android app, shared Compose UI, and desktop Compose app. It uses Gradle and CMake/xmake for NDK.
- **Build Android app**: `cd apps && ./gradlew assembleDebug`
- **Clean**: `cd apps && ./gradlew clean`
- **Lint**: `cd apps && ./gradlew lint`
- **Run Unit Tests**: `cd apps && ./gradlew testDebugUnitTest`
- **Run a Specific Test**: `cd apps && ./gradlew testDebugUnitTest --tests "com.example.package.TestClassName"`
- **Android Instrumented Tests**: `cd apps && ./gradlew connectedAndroidTest`
- **Run Desktop app**: `cd apps && ./gradlew :desktopApp:run`

### CLI (C++)
The CLI version is located in `cli/`. It uses `xmake` for dependency and build management.
- **Build**: `cd cli && xmake`
- **Run**: `cd cli && xmake run`
- **Clean**: `cd cli && xmake clean`
- **Rebuild**: `cd cli && xmake build -r`
- **Install Dependencies**: `cd cli && xmake require`
- **Compile Commands**: `cd cli && xmake project -k compile_commands`

### Shared C++ (Core)
Shared code in `share/` built via xmake (included by cli and apps).
- **Build**: `cd share && xmake`
- **Clean**: `cd share && xmake clean`

---

## 🎨 Code Style Guidelines

### General
- **Indentation**: 4 spaces across all file types.
- **Encoding**: UTF-8.
- **Line Endings**: LF.
- **Line Length**: Soft limit of 120 characters.

### Kotlin (Android)
- **Naming**:
  - Classes/Composables: `PascalCase` (e.g., `MainActivity`, `DistributedManager`)
  - Functions/Variables: `camelCase` (e.g., `handleDeviceConnection`, `selectedDevice`)
  - Constants: `UPPER_SNAKE_CASE`
- **Imports**: Avoid wildcard imports except for Compose UI libraries. Group by package:
  1. Kotlin stdlib
  2. Android/Compose
  3. Third-party
  4. Project packages
- **Async**: Prefer Kotlin Coroutines (`viewModelScope`, `lifecycleScope`, `Dispatchers.IO`) over manual threads.
- **State**: Use `mutableStateOf` and `collectAsState()` for Jetpack Compose UI.
- **Error Handling**: Use `try-catch` and log via the logging system with proper error levels.

### C++ (Shared & CLI)
- **Standard**: C++23 for CLI, C++14/17 for Android JNI.
- **Naming**:
  - Classes: `PascalCase` (e.g., `RealSR`, `DistributedEngine`)
  - Functions/Methods: `snake_case` (e.g., `get_available_models()`, `process_tile()`)
  - Variables: `snake_case` (e.g., `should_cancel`, `device_weight`)
  - Member variables: Use trailing underscore (`engine_`, `running_`) or no prefix
  - Constants: `UPPER_CASE` (e.g., `MAX_DEVICES`, `DEFAULT_TIMEOUT`)
- **Files**:
  - Core logic: `share/core/` (upscaling engines, distributed algorithms)
  - Utilities: `share/utils/` (image processing, logging, networking)
  - JNI bridge: `apps/app/src/main/cpp/`
- **Memory Management**: Strictly use `std::unique_ptr` and `std::shared_ptr`. Never raw `new`/`delete`.
- **Type Safety**: Prefer `std::string_view` for string parameters, use `auto` with type deduction for complex types.
- **Error Handling**:
  - Return `int` status codes (0 for success, non-zero for error)
  - Use `std::atomic<bool> should_cancel` for interruptible long-running tasks
  - Use `try-catch` with structured logging
- **Modern C++**: Use `std::filesystem`, `std::print`, ranges, and standard algorithms.

### Logging
All logging must use the centralized logging system (console.hpp for C++, Kotlin logger for Android):
- **Levels**: DEBUG, INFO, WARNING, ERROR, CRITICAL
- **Format**: Tag-based with structured message, e.g., `[Engine] Processing tile {}/{}`
- **Colors**: C++ console output supports colored output; Android uses log levels
- **Platform abstraction**: Use cross-platform logging helpers that work on both CLI and Android

---

## 📁 Repository Structure
- `apps/`: Multiplatform app workspace.
  - `app/`: Android application module.
  - `shared-ui/`: KMP shared Compose UI.
  - `desktopApp/`: Compose Multiplatform desktop host.
- `cli/`: Command-line interface version with distributed computing support.
- `share/`: Shared C++ core and utilities.
  - `core/`: Upscaling engines (RealSR, etc.) and distributed task scheduler.
  - `utils/`: Image processing, logging, and network communication helpers.

## 🚀 Important Notes for Agents
- **Shared Code First**: Upscaling algorithm changes must be in `share/core/` to affect both Android and CLI.
- **No Suppression**: Never suppress type errors or compiler warnings. Fix the root cause.
- **Distributed Computing**: When working on distributed features, consider device weight calculation, task allocation, and network protocol stability.
- **Output**: For CLI, use `console::log()`/`console::error()` or `std::print`. For Android, use proper LogCat logging.
- **Android Assets**: Models are copied from `apps/app` assets to internal storage on startup. Update asset copying logic if adding new models.
- **Performance**: Distributed computing must strive for 99%+ efficiency across devices (CPU cores, GPUs).
