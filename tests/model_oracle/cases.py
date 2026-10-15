"""Model-effect oracle: the case set and the deterministic input.

One case = one model from the published catalog + a fixed synthetic input, run
through the real single-image CPU path. The golden PNG pins the pixels that
model produced; see docs/model-oracle-design.md for why each case is here and
how the thresholds were chosen.

Nothing here may depend on a random API whose stream is not guaranteed stable
across library versions: the input is built from an explicit LCG plus fixed
integer patterns, so `input.sha256` in golden/meta.json only changes when
someone edits this file on purpose.
"""

import hashlib
from pathlib import Path

import numpy as np

INPUT_SIZE = 64
INPUT_SEED = 20261014

GOLDEN_DIR = Path(__file__).resolve().parent / "golden"
META_PATH = GOLDEN_DIR / "meta.json"

# Comparison gates for a case output vs its golden. Same machine + same build is
# bit-identical (measured, see the design doc); these bound the drift that is
# still "the same model": sub-LSB kernel/ISA scheduling differences across CPUs
# and ncnn releases. A broken graph, normalization or upsampler lands at 10-30
# dB, far below the floor.
MIN_PSNR = 45.0
MAX_DIFF = 3.0

# Engine-independent sanity gates (a dead/black/saturated output fails these
# even with no golden to compare against).
MIN_STD = 2.0
MEAN_RANGE = (8.0, 247.0)

# One input pixel-grid feeds every case; 256/16 keeps the whole image in a
# single tile (no seam), which is the most reproducible reference. Tile borders
# legitimately change SR pixels — see the design doc.
TILE_SIZE = 256
TILE_OVERLAP = 16

CASES = [
    ("span-x2", "NomosUni-SPAN-x2", 2,
     "SPAN：Conv3XC 重参数化（1x1-sk + bottleneck 折进单个 3x3）"),
    ("compact-anime-x2", "Real-ESRGANv3-anime-x2", 2,
     "SRVGGNetCompact + PixelShuffle 尾巴（手机默认动漫网）"),
    ("compact-archivist-x1", "Archivist-Medium-Compact-x1", 1,
     "x1：上采样器退化为恒等 + 全局残差"),
    ("realplksr-x4", "NomosWebPhoto-RealPLKSR-x4", 4,
     "RealPLKSR：partial large-kernel 块 + x4 上采样"),
    ("rrdb-x2plus", "Real-ESRGAN-x2plus", 2,
     "RRDB 主干 + pixel-unshuffle 头（x2 命名、x4 干）"),
    ("cugan-x2", "Real-CUGAN-no-denoise-x2", 2,
     "Real-CUGAN：deconv 上采样尾巴 + Crop/pre-pad 图分析分支"),
]


def _lcg_bytes(count: int, seed: int = INPUT_SEED) -> np.ndarray:
    """GLIBC-style LCG, integer math only — identical on every platform/version."""
    x = seed
    out = np.empty(count, dtype=np.uint8)
    for i in range(count):
        x = (1103515245 * x + 12345) % (1 << 31)
        out[i] = (x >> 16) & 0xFF
    return out


def make_input(size: int = INPUT_SIZE) -> np.ndarray:
    """Synthetic HWC u8 image: noise + hard black/white blocks + R gradient +
    B sinusoid. The blocks catch normalization/scale errors (they saturate), the
    gradient catches channel swaps and off-by-one shifts, the noise/sinusoid
    catch smoothing and dead outputs."""
    img = _lcg_bytes(size * size * 3).reshape(size, size, 3).copy()
    yy, xx = np.mgrid[0:size, 0:size]
    block = max(6, size // 8)
    img[6:6 + block, 6:6 + block] = 0
    img[6:6 + block, size - 6 - block:size - 6] = 255
    img[:, :, 0] = (xx * 255 // (size - 1)).astype(np.uint8)
    img[:, :, 2] = (128 + 110 * np.sin(xx / 7.0) * np.cos(yy / 9.0)).astype(np.uint8)
    return img


def input_sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()
