"""Deterministic test-image corpus (fixed seed — pixel comparisons depend on
byte-stable inputs). Written into the suite work dir, nothing in-repo."""

import os

import numpy as np
from PIL import Image


def _gradient_noise(w, h, rng):
    yy, xx = np.mgrid[0:h, 0:w]
    img = np.zeros((h, w, 3), np.uint8)
    img[..., 0] = (xx * 255 // max(w - 1, 1)).astype(np.uint8)
    img[..., 1] = (yy * 255 // max(h - 1, 1)).astype(np.uint8)
    img[..., 2] = ((xx + yy) * 255 // max(w + h - 2, 1)).astype(np.uint8)
    noise = rng.integers(0, 24, (h, w, 3), dtype=np.uint8)
    return np.clip(img.astype(np.int16) + noise - 12, 0, 255).astype(np.uint8)


def build(work_dir):
    """Generate the corpus; returns dict of name -> absolute path."""
    d = os.path.join(work_dir, "corpus")
    os.makedirs(d, exist_ok=True)
    rng = np.random.default_rng(42)

    def save(name, arr, mode="RGB", **kw):
        p = os.path.join(d, name)
        Image.fromarray(arr, mode).save(p, **kw)
        return p

    paths = {
        # main 256² workhorse (single tile at auto tile size)
        "std256.png": save("std256.png", _gradient_noise(256, 256, rng)),
        # tile-boundary and extreme-aspect edges — each hits a distinct
        # prepadding / tiling branch, not interchangeable
        "odd201x199.png": save("odd201x199.png", _gradient_noise(201, 199, rng)),
        "wide300x1.png": save("wide300x1.png", _gradient_noise(300, 1, rng)),
        "tall1x300.png": save("tall1x300.png", _gradient_noise(1, 300, rng)),
        "one1x1.png": save("one1x1.png", _gradient_noise(1, 1, rng)),
        # multi-tile at 256 auto tile size (2x2 tiles)
        "sq511.png": save("sq511.png", _gradient_noise(511, 511, rng)),
        # grayscale input → channel-normalization branch
        "gray256.png": save("gray256.png", _gradient_noise(256, 256, rng)[..., 0],
                            mode="L"),
    }

    # JPEG carrying EXIF orientation 6 (90° CW), left edge painted red so the
    # upright orientation is verifiable in the output
    arr = _gradient_noise(320, 200, rng)
    arr[:, :16] = (255, 0, 0)
    exif = Image.Exif()
    exif[274] = 6  # Orientation
    p = os.path.join(d, "exif_rotated.jpg")
    Image.fromarray(arr, "RGB").save(p, "JPEG", exif=exif)
    paths["exif_rotated.jpg"] = p

    return paths
