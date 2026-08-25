# CrossScale - Distributed Image Super-Resolution

CrossScale is a distributed image super-resolution system that supports both standalone CLI and distributed computing across multiple devices. It uses modern C++ (C++23), ncnn for inference, and supports popular image formats via libpng, libjpeg, and libwebp.

## Features

- Single-image and batch directory processing
- Tile-based processing with configurable overlap
- Auto-tuning of tile size based on model and image dimensions
- Multi-threaded directory processing via `--jobs` (CPU engines)
- Distributed computing coordinator/worker model (QR code pairing)
- Cross-platform: Linux/macOS/Windows CLI + Android app

## Build

### Prerequisites

- xmake (v3.0+)
- C++23 compiler (GCC 11+, Clang 13+, MSVC 2022+)
- Dependencies: ncnn, libpng, libjpeg, libwebp (installed via `xmake require`)

### Build CLI

```bash
cd cli
xmake
```

The binary will be located at `build/linux/x86_64/release/crossscale-cli` (path may vary by platform).

### Build Android App

```bash
cd apps
./gradlew assembleDebug   # or open in Android Studio
```

The native engine (`libcrossscale-native.so`) is built with xmake by a Gradle task (`xmakeNative` in `apps/app/build.gradle.kts`) that runs before each APK build; see `apps/xmake.lua`.

## Usage

### Single Image

```bash
./crossscale-cli input.png                                # → input_Real-ESRGANv3-anime-x2.png
./crossscale-cli input.png output.png                     # explicit output
./crossscale-cli input.png -m Real-CUGAN-conservative-x2  # pick a model
```

The first form is the common one: no verb, no flags — the output name derives
from the input and the model that produced it (`<stem>_<model>.<ext>`, next to
the input). The legacy long form is fully supported:

```bash
./crossscale-cli process -i input.png -o output.png -m Real-ESRGANv3-anime-x2
```

Use `--output-quality <1-100>` to trade file size for fidelity and `--output-format <png|jpg|jpeg|webp|avif>` to override the saved container.

Optional parameters:

- `--tile-size N` – Tile size (default: 256)
- `--tile-overlap N` – Overlap between tiles (default: 16)
- `--batch-size N` – Batch size for tile inference (default: 1). Larger values can improve GPU occupancy by processing multiple tiles in a single GPU dispatch. For CPU, benefits are less pronounced.
- `--auto-tile` – Enable automatic tile size optimization
- `--min-tile N` / `--max-tile N` – Bounds for auto-tile
- `--config FILE` – Load configuration from JSON file
- `--no-daemon` – Skip the warm-engine daemon (process inline)

#### Warm-engine daemon

Single-image requests are served by a background warm-engine daemon that keeps
the GPU context and model loaded, skipping the ~490 ms init cost per call. It
is fully automatic: the first `process` call starts one if none is running, and
it exits by itself after 600 idle seconds (`--idle-timeout` to change, `0` to
keep it forever). Manual control when wanted:

```bash
./crossscale-cli daemon status                 # is it running?
./crossscale-cli daemon stop                   # stop it now
./crossscale-cli daemon start --cpu --fg       # explicit start (foreground, CPU)
```

### Directory Processing

Process all images in a directory (recursively), preserving directory structure:

```bash
./crossscale-cli input_dir                     # → input_dir_<model>/ next to it

# Legacy / explicit forms
./crossscale-cli process -i input_dir -o output_dir -m Real-ESRGANv3-anime-x2 --jobs 4   # CPU parallel
./crossscale-cli process -i input_dir -o output_dir --gpu                                # GPU sequential
```

**Important GPU behavior**:
- When `--gpu` is enabled, directory processing **does not** use `--jobs` to parallelize images. Instead, images are processed sequentially on a single shared GPU engine to avoid Vulkan context corruption.
- The `--batch-size` flag **does** affect GPU directory mode: it controls how many tiles are batched together per inference call, increasing GPU occupancy and throughput per image.
- For maximum directory throughput with GPU, use `--gpu` without `--jobs`. For CPU parallelism, use `--jobs` without `--gpu`.

### Model Management

Models are **not bundled** with the CLI or the Android app. They are
distributed via the [models GitHub release](https://github.com/xihale/crossscale/releases/tag/models)
and downloaded on demand (see `models/README.md`):

```bash
./crossscale-cli model list-remote                          # list models in the release
./crossscale-cli model download Real-ESRGANv3-anime-x2      # download into ./models
./crossscale-cli model list                                 # list installed models
./crossscale-cli model import ~/foo.param ~/foo.bin         # import local custom models
```

Models are discovered from the `models` directory (default `./models`, override
with `--models-dir` on any subcommand, or set `CROSSSCALE_MODELS_DIR` to work
from any directory). Each model is a `.param` + `.bin` pair; both layouts work:

```
models/
  Real-ESRGANv3-anime-x2.param      # flat
  Real-ESRGANv3-anime-x2.bin
  Real-CUGAN-conservative/          # nested (name: "Real-CUGAN-conservative-x2")
    x2.param
    x2.bin
```

On Android, the Models page offers the same download list plus local import
of your own `.param`/`.bin`/`.zip` files.

### Distributed Mode

Start the coordinator for phones (`host`; bare form defaults to `start`):

```bash
./crossscale-cli host                    # listen on 9527, print the QR card
./crossscale-cli host -p 9527 --auto-approve   # auto-approve pairing (CI)
```

Show the connection card without starting anything:

```bash
./crossscale-cli host info
```

Distributed offloading is driven from the Android app, not the CLI. With `host`
running, open the Android app, scan the QR (or connect manually with the IP and
port from the card), and submit an image; the PC upscales it and returns the
result over the encrypted protocol. Pairing is approved on the PC the first
time and remembered afterwards.

### Performance Probes

```bash
./crossscale-cli perf list-probes           # list available probes
./crossscale-cli perf gpu-efficiency        # GPU vs CPU tile throughput
./crossscale-cli perf gpu-concurrency       # scaling across N engines
./crossscale-cli perf gpu-reuse             # fresh vs reused VkCompute
```

## Performance Notes

- **Tile size**: The optimal tile size depends on GPU memory and image resolution. Default 256 works for most GPUs with ≥4GB VRAM. Larger tiles (384, 512) reduce seam artifacts and overhead for high-resolution images (>4K). Smaller tiles (128) are better for memory-constrained devices.
- **Overlap**: 10-15% of tile size is recommended (e.g., 16 for 256).
- **CPU vs GPU**: Single-image and directory modes both support GPU via `--gpu`. For directory mode, GPU forces sequential image processing to avoid Vulkan context corruption; CPU mode uses `--jobs` for parallel image processing.
- **Batch size**: The `--batch-size` flag controls how many tiles are batched together in a single inference call. Tiles are only batched if they have identical dimensions (same width and height), so edge tiles may be processed separately. Increasing batch size can improve GPU occupancy and throughput per image; the CPU also benefits but to a lesser extent. The default is 1 (no batching).
- **Parallelism**: `--jobs` controls image-level parallelism for CPU engines only. For best CPU performance, set `--jobs` to the number of physical cores (or slightly higher if I/O bound).
- **GPU utilization**: In GPU directory mode with `--batch-size 1`, typical GPU utilization is ~50%. Using a larger `--batch-size` can increase occupancy and improve throughput.

## Configuration File

You can save frequently used settings in a JSON file:

```json
{
  "tile_size": 256,
  "tile_overlap": 16,
  "batch_size": 1,
  "auto_tile_size": false,
  "min_tile_size": 64,
  "max_tile_size": 512,
  "use_gpu": true,
  "model_name": "Real-ESRGANv3-anime-x2",
  "output_quality": 90,
  "output_format": ""
}
```
`output_quality` and `output_format` from the config (or via `--output-quality`/`--output-format`) determine the compression level and file container used for each processed image.

Load with `--config FILE`. CLI flags override the loaded configuration.

## Troubleshooting

- **No models found**: Run `./crossscale-cli model list-remote && ./crossscale-cli model download <name>`, or use `--models-dir` to point at an existing directory (or export `CROSSSCALE_MODELS_DIR`). On Android, use 模型 → 下载模型.
- **Out of memory**: Reduce tile size or reduce `--jobs`. For GPU mode, close other applications to free VRAM. If GPU memory is insufficient, try smaller tile sizes (128 or 192).
- **Slow GPU performance**: Increase `--batch-size` to improve GPU occupancy. Ensure you are not using `--jobs` with `--gpu` (they are incompatible). For large directories, consider using CPU mode with `--jobs` for parallel image processing, or process images individually with `--gpu`.
- **Crashes with GPU in directory mode**: This should no longer occur as GPU directory mode now uses a single shared engine sequentially. If you experience crashes, ensure you are using the latest version.

## Development

See [docs/README.md](docs/README.md) for documentation navigation.
[ARCHITECTURE.md](docs/ARCHITECTURE.md) is the single source of truth for system layout.

## License

CrossScale is licensed under the [MIT License](LICENSE).

### Third-party components

| Component | License | Notes |
|-----------|---------|-------|
| [ncnn](https://github.com/Tencent/ncnn) | BSD 3-Clause | Inference engine (Tencent). Copyright © 2017 Tencent |
| [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN) | BSD 3-Clause | Source of the bundled super-resolution models (converted to ncnn format) |
| libpng / libjpeg / libwebp | libpng / IJG / BSD 3-Clause | Image codecs |
| glaze | MIT | JSON serialization |
| libsodium | ISC | Encrypted pairing channel |
| CLI11 | BSD 3-Clause | CLI parsing |
| OpenSSL / libcurl | Apache-2.0 / curl | Model download transport |
| AndroidX / Jetpack Compose / CameraX / zxing | Apache 2.0 | Android app UI, camera QR scanning |

Android dependencies are declared in `apps/app/build.gradle.kts`; the full C++ dependency
list lives in [xmake.lua](xmake.lua).

All third-party licenses are permissive and compatible with redistribution under MIT;
the respective copyright notices above must be retained in distributions.
