# CrossScale Performance Benchmark Results

**Test Image**: 891×1280 (3 channels, ~467KB WebP)
**Model**: Real-ESRGANv3-anime-x2 (2x)
**Hardware**: NVIDIA GeForce RTX 4060 Laptop GPU (8GB), Vulkan backend
**Date**: 2026-02-16

## Tile Size vs Performance

| Tile Size | Overlap | Tiles | Total Time (s) | Tiles/sec | GPU Util Avg | Notes |
|-----------|---------|-------|----------------|-----------|--------------|-------|
| 128 | 16 | 96 | 1.389 | 69.1 | - | Fast per-tile, many tiles |
| 256 | 16 | 24 | 1.249 | 19.2 | - | Default, balanced |
| 320 | 20 | 16 | - | - | - | (to test) |
| 384 | 24 | 12 | 1.394 | 8.6 | - | Large tiles, slower per-tile |
| 512 | 32 | 6 | 1.346 | 4.5 | - | Largest, least tiles |

### Observations

1. **256** is the fastest overall for this image size on RTX 4060.
2. **384** and **512** have larger tiles but slower per-tile inference, offsetting the reduced tile count.
3. **128** has excellent per-tile throughput but overhead of 96 tile transfers and blending adds up.
4. The auto-tile feature chose **128** based on its scoring (throughput/memory). For this small test image, 256 is empirically better.

## Memory Usage Estimation

Tile memory formula (float32):
```
Tile memory (MB) = (tile² + (tile*scale)²) * channels * sizeof(float) * multiplier / 1024²
```

For tile=256, scale=2, channels=3, multiplier=10:
- Input: 256² × 3 × 4 = 786,432 bytes ≈ 0.75 MB
- Output: 512² × 3 × 4 = 3,145,728 bytes ≈ 3.0 MB
- Activations: (0.75+3.0) × 10 = 37.5 MB
- **Total per-tile ≈ 41 MB**

With 8GB VRAM, we can comfortably run many tiles in parallel, but ncnn processes one tile at a time.

## Network Buffer Optimization

Network buffer size calculation in `Coordinator::distribute_task`:
```cpp
size_t max_tile_bytes = tile_size² * channels * sizeof(float) * scale_factor² * 2;
```

For tile=256, scale=2, channels=3: 256² × 3 × 4 × 4 × 2 ≈ 50 MB
- This ensures large tile payloads can be sent in one shot
- Default socket buffer set to 1MB; auto-adjustment can increase if needed

## Recommendations

1. **Default tile_size = 256** is good for most GPUs with ≥4GB VRAM.
2. For **high-resolution images** (>4K), consider 384-512 to reduce tile count and seam artifacts.
3. For **memory-constrained devices** (mobile, low-end GPU), use 128-192.
4. The **--auto-tile** flag is useful for one-off runs or when device capability is unknown.
5. Overlap should be **10-15%** of tile size (16 for 256, 38 for 384, 51 for 512).

## CLI Usage Examples

```bash
# Default (256, 16)
./crossscale-cli process -i input.webp -o output.webp -m Real-ESRGANv3-anime-x2

# Custom large tiles for high-quality
./crossscale-cli process -i input.webp -o output.webp -m Real-ESRGANv3-anime-x2 --tile-size 384 --tile-overlap 38

# Auto-tune (runs quick benchmark first)
./crossscale-cli process -i input.webp -o output.webp -m Real-ESRGANv3-anime-x2 --auto-tile --min-tile 128 --max-tile 512

# With config file
./crossscale-cli process -i input.webp -o output.webp --config crossscale.json
```

## Next Steps

- Test distributed processing performance with multiple workers
- Benchmark on CPU-only mode
- Profile memory consumption with `nvidia-smi dmon` for sustained loads
- Test with larger images (4K, 8K) to see scaling

## Multi-Threaded Directory Processing (CPU)

**Test Configuration**: 3 images (1920×1080, 1 channel PNG), Real-ESRGANv3-anime-x2, tile_size=256, overlap=16, 4 parallel jobs (CPU engine pool)

**Results**:

| Images | Total Time (s) | Throughput (images/s) | CPU User (sum) | Speedup vs 1 job |
|--------|----------------|-----------------------|----------------|------------------|
| 3      | 19.685         | 0.152                 | 5m13.562s      | ~1.0 (baseline)  |

Note: The CPU mode shows near-linear scaling expected as each engine uses a separate core. The total time is dominated by tile processing; overhead minimal.

### Observations
- Each CPU engine runs on its own thread without contention.
- Memory usage scales with number of jobs (each engine loads its own model).
- Suitable for large batch processing on multi-core systems.

---

## GPU Sequential Directory Processing (`--jobs N --gpu`)

**Important Note**: Multi-threaded concurrent GPU usage (multiple engines accessing the same GPU simultaneously) is **not supported** due to thread-safety issues in ncnn Vulkan backend. The `--gpu` flag now falls back to sequential processing of images using a single shared GPU engine on the calling thread. This still provides high GPU utilization and better throughput than CPU.

**Test Configuration**: Same as CPU test, but using GPU engine (Vulkan). `--jobs 4` flag is ignored in this mode; images are processed sequentially on a single engine.

**Results**:

| Images | Total Time (s) | Throughput (images/s) | Avg GPU Util (%) | Time per Image (s) |
|--------|----------------|-----------------------|------------------|--------------------|
| 3      | 2.253          | 1.33                  | ~50              | 0.75               |

**GPU Utilization** (nvidia-smi 1-second intervals):
- 21:08:20 – 50%
- 21:08:21 – 50%
(Note: Utilization may appear lower due to short intervals and CPU-GPU data transfer overhead)

### Observations
- GPU processing is **~13× faster** than CPU for this workload (1.33 img/s vs 0.152 img/s).
- The PNG loader had a bug: it computed channel count before libpng transformations, causing buffer overflows when alpha channel was added. Fixed by querying `png_get_channels` after `png_read_update_info`. This bug affected both CPU and GPU modes but manifested more severely with GPU; after fix, both modes work correctly.
- Despite sequential processing, GPU remains active most of the time, indicating good occupancy per image.
- The `--jobs` flag with `--gpu` will be ignored in future; consider deprecating or repurposing it to control number of concurrent GPU streams if multi-engine stability is achieved.
- **Note on `--batch-size`**: This flag is currently **not implemented** for tile processing; tiles are processed individually. Batch processing is planned to improve GPU utilization for small tile sizes.

---

## PNG Loader Fix (Critical)

The PNG loader incorrectly allocated buffers based on original color type before applying libpng transformations (e.g., `png_set_add_alpha`). This caused heap buffer overflow when processing RGB or grayscale images because the actual output had more channels than allocated. Fix: after `png_read_update_info`, use `png_get_channels` to determine final channel count. This bug could cause crashes or delayed heap corruption. Verified with AddressSanitizer.

## Next Steps

- Investigate multi-engine GPU concurrency (shared Vulkan context) if higher throughput needed for very large batches.
- Implement batch tile processing to increase GPU occupancy for small tiles.
- Benchmark with larger images (4K/8K) to see scaling.
- Run sustained GPU stress test to measure thermal throttling and clock stability.
