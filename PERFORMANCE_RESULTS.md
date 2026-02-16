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
