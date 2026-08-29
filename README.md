# Upix - Distributed Image Super-Resolution

Upix is an image super-resolution system with a standalone CLI (and Android app) plus encrypted LAN offloading: the phone pairs with a PC and delegates the GPU work. It uses modern C++ (C++23), ncnn for inference, and supports popular image formats via libpng, libjpeg, libwebp, and (optionally) libavif.

## Features

- Single-image and batch directory processing
- Tile-based processing with configurable overlap
- Auto-tuning of tile size based on model and image dimensions
- Multi-threaded directory processing via `--jobs` (CPU engines)
- Offload to a PC over the LAN: QR pairing, end-to-end encrypted (X25519 + Ed25519 + secretbox)
- Outputs carry an XMP marker; re-feeding an already-upscaled image skips by default
- Cross-platform: Linux/macOS/Windows CLI + Android app

## Build

### Prerequisites

- xmake (v3.0+)
- C++23 compiler (GCC 11+, Clang 13+, MSVC 2022+)
- Dependencies: ncnn, libpng, libjpeg, libwebp (installed via `xmake require`)

### Build CLI

```bash
scripts/build_cli.sh release    # from the repo root
```

The script always reconfigures xmake for linux/x86_64 before building — xmake keeps a
single global config per project, so a bare `xmake` after an Android build would target
Android. The binary lands at `build/linux/x86_64/release/upix-cli`. Debug builds:
`scripts/build_cli.sh debug`.

### Build Android App

```bash
cd apps
./gradlew assembleDebug   # or open in Android Studio
```

The native engine (`libupix-native.so`) is built with xmake by a Gradle task (`xmakeNative` in `apps/app/build.gradle.kts`) that runs before each APK build; see `apps/xmake.lua`.

### Optional AVIF codecs (opt-in)

The default build is lean: the CLI dlopens the system libavif at runtime for
AVIF (encode needs one with an encoder, e.g. `pacman -S libavif`), and the
Android app has no on-device AVIF encode (AVIF output routes to a paired PC;
AVIF input decodes via the platform on Android 12+). Two compile-time options
bundle real codecs instead:

```bash
# CLI / host
xmake f --avif-encoder=y --avif-decoder=y && xmake     # both optional, independent

# Android
./gradlew -Pupix.avifEncoder=true -Pupix.avifDecoder=true assembleRelease
# or directly: scripts/build_native.sh release arm64-v8a --avif-encoder=y
# or:          UPIX_AVIF_ENCODER=1 UPIX_AVIF_DECODER=1 scripts/build_native.sh release arm64-v8a
```

| Option | Effect | arm64 `.so` cost (measured, 2026-08-26) |
| --- | --- | --- |
| `avif-encoder` | libavif+aom compiled in: on-device AVIF **output** (the CLI encodes standalone; the Android app unlocks the AVIF format for local processing) | 9.6 → 16.9 MB raw / +3.0 MB gzipped in the APK |
| `avif-decoder` | libavif+dav1d compiled in: native AVIF **input** decode — covers AVIF inputs below Android 12 and bypasses OEM decoder quirks | 9.6 → 11.0 MB raw / +0.6 MB gzipped in the APK |

Both enabled: 17.6 MB raw / 9.0 MB gzipped. Baseline is the lean default
`libupix-native.so`; "gzipped" approximates the per-ABI APK delta since APK
entries are deflate-compressed.

`upix --version` reports the build's codec capabilities (`+avif-encoder`,
`+avif-decoder`, or `avif:system-dlopen`). On-device AVIF encoding is
software (no phone SoC ships an AV1 hardware encoder): at quality 90 expect
roughly 1-2 s per 20 MP at speed 8 on a recent flagship, several seconds at
speed 6, and ~1 GB peak memory at 50 MP — speed 8 is the recommended default
for large images.

### AVIF over remote (phone ↔ PC)

AVIF capability is negotiated at pairing time: the PC's CONNECT_RESPONSE
carries `avif_decode` / `avif_encode` bits (probed from its system libavif),
and the app gates AVIF input/output on local-or-peer capability instead of
assuming the PC supports it:

- **Input**: an AVIF file can only be processed remotely if the PC advertises
  `avif_decode`; otherwise the submit is rejected up front ("PC 端不支持
  AVIF 解码：安装 libavif …").
- **Output**: the AVIF format entry is offered when either side can encode;
  selecting it with neither able to fails fast before any GPU work.
- A PC-side task rejection (decode/encode failure) is surfaced as a per-task
  error and no longer drops the connection.

## Usage

### Single Image

```bash
./upix-cli input.png                                # → input_Real-ESRGANv3-anime-x2.png
./upix-cli input.png output.png                     # explicit output
./upix-cli input.png -m Real-CUGAN-conservative-x2  # pick a model
```

The first form is the common one: no verb, no flags — the output name derives
from the input and the model that produced it (`<stem>_<model>.<ext>`, next to
the input). The flag form is equivalent when a script prefers it:

```bash
./upix-cli -i input.png -o output.png -m Real-ESRGANv3-anime-x2
```

Use `--output-quality <1-100>` to trade file size for fidelity and `--output-format <png|jpg|jpeg|webp|avif>` to override the saved container.

Per-format encoder knobs (also accepted in the `--config` JSON under `"encoder"`):

- `--png-level <0-9>` – PNG zlib compression level (default: auto, derived from `--output-quality`)
- `--jpeg-subsampling <420|422|444>` – JPEG chroma subsampling (default: 420)
- `--webp-method <0-6>` – WebP encode effort, higher = smaller but slower (default: auto, 4)
- `--webp-lossless` – Force lossless WebP output
- `--avif-speed <0-10>` – AVIF encoder speed, higher = much faster, larger files (default: 6)
- `--avif-subsampling <420|444>` – AVIF chroma subsampling (default: 420)
- `--avif-threads <0-64>` – AVIF encoder tile threads (default: auto, hardware concurrency)

Optional parameters:

- `--tile-size N` – Tile size (default: engine-recommended for your GPU — the daemon and inline paths apply the same rule)
- `--tile-overlap N` – Overlap between tiles (default: 16)
- `--batch-size N` – Batch size for tile inference (default: 1). Larger values can improve GPU occupancy by processing multiple tiles in a single GPU dispatch. For CPU, benefits are less pronounced.
- `--auto-tile` – Enable automatic tile size optimization
- `--min-tile N` / `--max-tile N` – Bounds for auto-tile
- `--config FILE` – Load configuration from JSON file
- `--no-daemon` – Skip the warm-engine daemon (process inline)

#### Warm-engine daemon

Single-image requests are served by a background warm-engine daemon that keeps
the GPU context and model loaded, skipping the ~490 ms init cost per call. It
is fully automatic: the first upscale starts one if none is running, and
it exits by itself after 600 idle seconds (`--idle-timeout` to change, `0` to
keep it forever). Manual control when wanted:

```bash
./upix-cli daemon status                 # is it running?
./upix-cli daemon stop                   # stop it now
./upix-cli daemon start --cpu --fg       # explicit start (foreground, CPU)
```

### Directory Processing

Process all images in a directory (recursively), preserving directory structure:

```bash
./upix-cli input_dir                     # → input_dir_<model>/ next to it

# Flag form, when preferred
./upix-cli -i input_dir -o output_dir -m Real-ESRGANv3-anime-x2 --jobs 4   # CPU parallel
./upix-cli -i input_dir -o output_dir --gpu                                # GPU sequential
```

**Important GPU behavior**:
- When `--gpu` is enabled, directory processing **does not** use `--jobs` to parallelize images. Instead, images are processed sequentially on a single shared GPU engine to avoid Vulkan context corruption.
- The `--batch-size` flag **does** affect GPU directory mode: it controls how many tiles are batched together per inference call, increasing GPU occupancy and throughput per image.
- For maximum directory throughput with GPU, use `--gpu` without `--jobs`. For CPU parallelism, use `--jobs` without `--gpu`.

### Model Management

Models are **not bundled** with the CLI or the Android app. They are
distributed via the [models GitHub release](https://github.com/xihale/upix/releases/tag/models)
and downloaded on demand (see `models/README.md`):

```bash
./upix-cli model list-remote                          # list models in the release
./upix-cli model download Real-ESRGANv3-anime-x2      # download into ./models
./upix-cli model list                                 # list installed models
./upix-cli model import ~/foo.param ~/foo.bin         # import local custom models
```

Models are discovered from the per-user store `$XDG_DATA_HOME/upix/models`
(`~/.local/share/upix/models` by default) — the same set from any working directory.
Override per invocation with `--models-dir` on any subcommand, or globally by exporting
`UPIX_MODELS_DIR`. Each model is a `.param` + `.bin` pair; both layouts work:

```
models/
  Real-ESRGANv3-anime-x2.param      # flat
  Real-ESRGANv3-anime-x2.bin
  Real-CUGAN-conservative/          # nested (name: "Real-CUGAN-conservative-x2")
    x2.param
    x2.bin
```

On Android, the Models page offers the same download list plus local import
of your own `.param`/`.bin`/`.zip` files; models on a paired PC can be listed,
pulled, and pushed from there too.

#### Which model to pick

The release carries 31 models in five families — the full catalog with
per-model descriptions lives in [models/README.md](models/README.md).
Quick guidance:

| Content | Light (phone-fast) | Heavy (best quality, PC-offload friendly) |
|---|---|---|
| Anime / illustration / game art | `Real-ESRGANv3-anime-x{2,3,4}` (1.2 MB), `AnimeJaNai-v2-Compact-x2` (1.2 MB, realtime-grade anime 2x), `HFA2k-LUDVAE-SPAN-x2` (0.8 MB, lightest, built for rough compressed sources), `ModernSpanimation-x2` (1.4 MB, modern-anime quality step-up), or `Real-CUGAN-pro-*-x{2,3}` (2.4 MB, best on compressed sources; `-conservative` keeps detail, `-denoise` cleans compression) | `AnimeSharp-x4` (32 MB, crisp line-art 4x) or `Real-ESRGAN-x4plus-anime` (8.3 MB, 4x) |
| Photos, 2x | `NomosUni-Compact-x2` (1.1 MB, trained across JPEG quality levels — best for chat-app/social-media images) or `NomosUni-SPAN-x2` (0.8 MB, same JPEG-degraded corpus in the SPAN arch — the lightest net in the catalog) | — |
| Photos, 4x | `Real-ESRGANv3-general-x4` (2.3 MB), `NomosUni-SPAN-x4` (0.8 MB, lightest photo 4x, JPEG-degraded training), `LSDIR-Compact-v2-x4` (1.2 MB, newer and stronger cleanup at the same speed), or `Nomos8kSC-x4` (31 MB, heavier JPEG/blur cleanup and texture) | `NomosWebPhoto-RealPLKSR-x4` (13.7 MB, best cleanup-per-MB on web sources), `Remacri-x4` (31 MB, the community's other classic all-rounder), `UltraSharp-x4` (32 MB, sharp classic), `NMKD-Siax-x4` (32 MB, texture-preserving classic for clean sources), or `Real-ESRGAN-x4plus` (31 MB) |

Most models are ncnn conversions of BSD/MIT-licensed upstream releases
(Real-ESRGAN, Real-CUGAN); the rest are OpenModelDB community models —
seven CC-BY-4.0 picks by Phhofm, two permissively-licensed nets
(`ModernSpanimation-x2`, MIT; `NMKD-Siax-x4`, WTFPL), and four
CC-BY-NC-SA classics (UltraSharp, AnimeSharp, AnimeJaNai, Remacri)
whose noncommercial terms apply to the weights themselves regardless of
the code license. See
the attribution table in [models/README.md](models/README.md).

### SR Marker (idempotent re-runs)

Every output embeds an XMP marker (`urn:upix:sr:1`) recording that — and with
which model/scale — the image was upscaled. Feeding a marked image back in
**skips it by default** (both CLI and Android), so batch pipelines that overlap
input and output directories don't waste GPU time re-upscaling results. Use
`-f` / `--force` to process a marked image anyway:

```bash
./upix-cli input.png -f    # re-upscale even though it carries the marker
```

### Distributed Mode

Start the coordinator for phones (`host`; bare form defaults to `start`):

```bash
./upix-cli host                    # listen on 9527, print the QR card
./upix-cli host -p 9527 --auto-approve   # auto-approve pairing (CI)
```

Show the connection card without starting anything:

```bash
./upix-cli host info
```

Distributed offloading is driven from the Android app, not the CLI. With `host`
running, open the Android app, scan the QR (or connect manually with the IP and
port from the card), and submit an image; the PC upscales it and returns the
result over the encrypted protocol.

Pairing is trust-on-first-use: both endpoints have persistent Ed25519 identities
(`~/.upix/identity` on the PC). On first connect the phone shows the PC's
8-hex-digit fingerprint for confirmation and the PC prompts `[y/n]`; afterwards
the peer is remembered in the trust store (`~/.upix/trusted_clients`) and
reconnects are automatic. All traffic after the handshake is encrypted
(X25519 key exchange, XSalsa20-Poly1305 frames) — no pre-shared password.

### Performance Probes

Probes ship as a separate `upix-perf` binary (built from `tools/`, lands next
to `upix-cli`) so the main CLI stays lean — they are developer/tuning tooling,
not an end-user feature:

```bash
./upix-perf list-probes           # list available probes
./upix-perf gpu-efficiency        # GPU vs CPU tile throughput
./upix-perf gpu-concurrency       # scaling across N engines
./upix-perf gpu-reuse             # fresh vs reused VkCompute
```

### Shell Completions

Completion for bash, zsh, and fish, driven dynamically by the binary itself:
the installed scripts are thin adapters that replay the command line being
completed into a hidden `__complete` command, which answers with candidates.
Everything therefore stays in sync automatically — sub-commands and flags come
from the CLI11 definitions, model names from the configured models dir, and
input-file suggestions are filtered to the formats this binary can actually
decode (png/jpg/jpeg/webp always, avif only where system libavif loads).
Directories stay navigable, and `upix <TAB>` offers sub-commands and
images side by side for the implicit upscale.

`--install` writes them into the shell's user completion directory
automatically:

```bash
./upix-cli completions bash --install   # ~/.local/share/bash-completion/completions/
./upix-cli completions zsh --install    # ~/.zfunc/_upix (or oh-my-zsh's dir)
./upix-cli completions fish --install   # ~/.config/fish/completions/
```

For zsh the command prints the `fpath=(...)` line to add to `~/.zshrc` when
`~/.zfunc` is not on fpath yet. bash and fish files are written for both
invocation names (`upix` and `upix-cli`); the scripts resolve the
binary from the command line being completed, so either name works. Note that
all three shells only fire completions for commands resolvable through
`PATH`, so install the binary somewhere on it (e.g. symlink into
`~/.local/bin`). Without
`--install` the script is printed to stdout for custom locations:

```bash
./upix-cli completions bash > /etc/bash_completion.d/upix
```

## Performance Notes

- **Tile size**: The optimal tile size depends on GPU memory and image resolution. When `--tile-size` is not given, both the warm-engine daemon and the inline path use the engine's VRAM-aware recommendation (identical output either way). 256 works for most GPUs with ≥4GB VRAM. Larger tiles (384, 512) reduce seam artifacts and overhead for high-resolution images (>4K). Smaller tiles (128) are better for memory-constrained devices.
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

- **No models found**: Run `./upix-cli model list-remote && ./upix-cli model download <name>`, or use `--models-dir` to point at an existing directory (or export `UPIX_MODELS_DIR`). On Android, use 模型 → 下载模型.
- **Out of memory**: Reduce tile size or reduce `--jobs`. For GPU mode, close other applications to free VRAM. If GPU memory is insufficient, try smaller tile sizes (128 or 192).
- **Slow GPU performance**: Increase `--batch-size` to improve GPU occupancy. Ensure you are not using `--jobs` with `--gpu` (they are incompatible). For large directories, consider using CPU mode with `--jobs` for parallel image processing, or process images individually with `--gpu`.
- **Crashes with GPU in directory mode**: This should no longer occur as GPU directory mode now uses a single shared engine sequentially. If you experience crashes, ensure you are using the latest version.

## Development

See [docs/README.md](docs/README.md) for documentation navigation.
[ARCHITECTURE.md](docs/ARCHITECTURE.md) is the single source of truth for system
layout; [DEVELOPMENT.md](docs/DEVELOPMENT.md) covers build/test/release workflows
and known pitfalls.

## License

Upix is licensed under the [GNU AGPL-3.0](LICENSE).

Copyright (c) 2026 xihale

You may use, modify, and redistribute Upix freely, including for commercial
purposes — but derivative works (including a modified version offered as a
network service) must be released under AGPL-3.0 with source. If you want to
build a closed-source product on Upix, contact the author for a commercial
license. Version history: releases up to and including `d98c461` (2026-08-29)
were MIT; `a502cd5` was PolyForm Noncommercial 1.0.0; later versions are
AGPL-3.0. Recipients keep their rights under the terms those versions were
distributed under.

### Third-party components

| Component | License | Notes |
|-----------|---------|-------|
| [ncnn](https://github.com/Tencent/ncnn) | BSD 3-Clause | Inference engine (Tencent). Copyright © 2017 Tencent |
| [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN) | BSD 3-Clause | Source of the Real-ESRGAN model weights (converted to ncnn format) |
| [Real-CUGAN](https://github.com/bilibili/ailab/tree/main/Real-CUGAN) / [realcugan-ncnn-vulkan](https://github.com/nihui/realcugan-ncnn-vulkan) | MIT (ncnn pack) | Source of the Real-CUGAN model weights (bilibili) and their ncnn conversion (nihui) |
| libpng / libjpeg / libwebp / libdeflate | libpng / IJG / BSD 3-Clause / MIT | Image codecs, SPIR-V pack decompression |
| glaze | MIT | JSON serialization |
| libsodium | ISC | Encrypted pairing channel |
| CLI11 | BSD 3-Clause | CLI parsing |
| OpenSSL / libcurl / libzip | Apache-2.0 / curl / BSD 3-Clause | Model download transport |
| AndroidX / Jetpack Compose / CameraX / zxing | Apache 2.0 | Android app UI, camera QR scanning |

Android dependencies are declared in `apps/app/build.gradle.kts`; the full C++ dependency
list lives in [xmake.lua](xmake.lua).

Third-party code dependencies are permissive-licensed and remain under their own
terms; their copyright notices must be retained in distributions. The bundled
models are separate works carrying their own licenses — see
[models/README.md](models/README.md); the community picks include CC-BY-NC-SA
models whose noncommercial terms bind whoever uses those weights, independent
of the code license.
