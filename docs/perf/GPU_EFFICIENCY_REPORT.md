# GPU Efficiency Report

## Test Configuration
- GPU: NVIDIA GeForce RTX 4060 Laptop
- Image: 1280x720 → 2560x1440
- Model: Real-ESRGANv3-anime-x2
- Tile: 256x256, 18 tiles total

## Results

| Batch Size | Time (s) | Avg GPU Util (%) | Avg Mem (MB) | Speedup |
|------------|----------|------------------|--------------|---------|
| 1          | 2.22     | 13.1             | 108          | 1.0x    |
| 2          | 1.44     | 7.0              | 65           | 1.5x    |
| 4          | 1.46     | 6.7              | 66           | 1.5x    |
| 8          | 1.47     | 6.3              | 65           | 1.5x    |

## Analysis

### Low GPU Utilization (6-13%)
- Small tile size (256x256) causes GPU underutilization
- Inference time per tile is very short (~80ms)
- CPU-GPU data transfer overhead dominates
- GPU spends most time idle waiting for data

### Batch Processing Impact
- batch_size=2 provides 1.5x speedup
- Larger batches (4, 8) show no additional benefit
- Bottleneck is likely tile extraction/blending on CPU

## Recommendations

1. **Increase tile size to 384 or 512** for better GPU occupancy
2. **Optimize CPU-side tile extraction** (currently sequential)
3. **Use pinned memory** for faster CPU-GPU transfers
4. **Pipeline processing**: overlap tile extraction with inference

