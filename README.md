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

## Usage

### Single Image

```bash
./crossscale-cli process -i input.png -o output.png -m Real-ESRGANv3-anime-x2
```

Optional parameters:

- `--tile-size N` – Tile size (default: 256)
- `--tile-overlap N` – Overlap between tiles (default: 16)
- `--batch-size N` – Batch size for tile inference (default: 1). Larger values can improve GPU occupancy by processing multiple tiles in a single GPU dispatch. For CPU, benefits are less pronounced.
- `--auto-tile` – Enable automatic tile size optimization
- `--min-tile N` / `--max-tile N` – Bounds for auto-tile
- `--config FILE` – Load configuration from JSON file

### Directory Processing

Process all images in a directory (recursively), preserving directory structure:

```bash
# Single-threaded (default)
./crossscale-cli process -i input_dir -o output_dir -m Real-ESRGANv3-anime-x2

# Multi-threaded (parallel images, CPU only)
./crossscale-cli process -i input_dir -o output_dir -m Real-ESRGANv3-anime-x2 --jobs 4

# GPU-accelerated (single-image sequential)
./crossscale-cli process -i input_dir -o output_dir -m Real-ESRGANv3-anime-x2 --gpu
```

**Important GPU behavior**:
- When `--gpu` is enabled, directory processing **does not** use `--jobs` to parallelize images. Instead, images are processed sequentially on a single shared GPU engine to avoid Vulkan context corruption.
- The `--batch-size` flag **does** affect GPU directory mode: it controls how many tiles are batched together per inference call, increasing GPU occupancy and throughput per image.
- For maximum directory throughput with GPU, use `--gpu` without `--jobs`. For CPU parallelism, use `--jobs` without `--gpu`.

### Model Management

List available models:

```bash
./crossscale-cli model list
```

Models are discovered from the `models` directory (either in current working directory or specified via `--models`). Each model should have a `.param` and a `.bin` file.

### Distributed Mode

Start a coordinator (server):

```bash
./crossscale-cli server start -p 9527 -k mysecret
```

Show connection info (QR code for Android app):

```bash
./crossscale-cli server info
```

Connect a worker (e.g., another machine or GPU-enabled local machine):

```bash
./crossscale-cli worker connect host:port -k mysecret
```

Client-side offloading:

```bash
./crossscale-cli client process -i input.png -o output.png -s host:port -k mysecret
```

### Benchmark

Run synthetic benchmark:

```bash
./crossscale-cli benchmark
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
  "max_memory_mb": 0,
  "num_threads": 0,
  "use_gpu": true,
  "model_name": "Real-ESRGANv3-anime-x2",
  "output_quality": 90,
  "output_format": ""
}
```

Load with `--config FILE`. CLI flags override the loaded configuration.

## Troubleshooting

- **No models found**: Ensure the `models` directory exists and contains `.param`/`.bin` files. Use `--models` to specify an alternative location.
- **Out of memory**: Reduce tile size or reduce `--jobs`. For GPU mode, close other applications to free VRAM. If GPU memory is insufficient, try smaller tile sizes (128 or 192).
- **Slow GPU performance**: Increase `--batch-size` to improve GPU occupancy. Ensure you are not using `--jobs` with `--gpu` (they are incompatible). For large directories, consider using CPU mode with `--jobs` for parallel image processing, or process images individually with `--gpu`.
- **Crashes with GPU in directory mode**: This should no longer occur as GPU directory mode now uses a single shared engine sequentially. If you experience crashes, ensure you are using the latest version.

## Development

See `AGENTS.md` for build, test, and lint commands, as well as coding style guidelines.

## License

[To be added]
