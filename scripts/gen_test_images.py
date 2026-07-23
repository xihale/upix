#!/usr/bin/env python3
"""Generate deterministic synthetic test images for SR benchmarking.

Covers four content types (photo / anime / portrait / high-freq detail) at
varied sizes, so perf runs exercise different tile layouts and frequency
profiles. Output is reproducible (fixed seed) — see PERF_REPORT_REAL_IMAGES.md.

Usage:
    python3 scripts/gen_test_images.py [out_dir] [--sizes all|small|large]
"""
import sys, os, argparse
import numpy as np
from PIL import Image

def photo(w, h, name):
    """Real-photo-like: low-freq gradients + noise."""
    y, x = np.mgrid[0:h, 0:w]
    img = np.zeros((h, w, 3), dtype=np.float32)
    img[..., 0] = (np.sin(x/40) + np.cos(y/35)) * 50 + 128
    img[..., 1] = (np.sin((x+y)/60)) * 60 + 128
    img[..., 2] = (np.cos(x/25) * np.sin(y/25)) * 50 + 128
    img += np.random.normal(0, 12, img.shape)
    Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(name)

def anime(w, h, name):
    """Anime-like: flat color regions + sharp cell-shading edges."""
    img = np.full((h, w, 3), 240, dtype=np.uint8)
    for y in range(h):
        img[y, :, 2] = int(200 - y/h * 80)
        img[y, :, 0] = int(180 - y/h * 60)
    cv, cr = int(w*0.75), int(h*0.25)
    yy, xx = np.mgrid[0:h, 0:w]
    img[((xx-cv)**2 + (yy-cr)**2 < (min(w,h)*0.12)**2)] = [255, 220, 100]
    for i, base in enumerate([0.55, 0.7, 0.82]):
        shade = 80 + i*40
        edge = (h*base + np.sin(np.linspace(0, 4*np.pi, w))*h*0.04).astype(int)
        for x in range(w):
            img[edge[x]:, x] = [shade, shade+15, shade+10]
    Image.fromarray(img).save(name)

def text_img(w, h, name):
    """High-frequency detail: concentric rings XOR grid (edge-heavy)."""
    img = np.full((h, w, 3), 255, dtype=np.uint8)
    yy, xx = np.mgrid[0:h, 0:w]
    rings = (np.sin(np.sqrt((xx-w/2)**2 + (yy-h/2)**2) / 8) > 0.5)
    grid = ((xx % 24 < 2) | (yy % 24 < 2))
    img[rings ^ grid] = [30, 30, 30]
    Image.fromarray(img).save(name)

def portrait(w, h, name):
    """Portrait-ish: smooth skin tones, directional shading."""
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    cx, cy = w/2, h*0.45
    r = np.sqrt((xx-cx)**2 + (yy-cy)**2)
    base = np.zeros((h, w, 3), dtype=np.float32)
    base[..., 0] = 220; base[..., 1] = 180; base[..., 2] = 150
    face = r < min(w,h)*0.32
    base[~face] = [60, 70, 90]
    shade = np.clip(1 - r/(min(w,h)*0.35), 0, 1) * 30
    base[..., 0] += shade; base[..., 1] += shade; base[..., 2] += shade
    base += np.random.normal(0, 5, base.shape)
    Image.fromarray(np.clip(base, 0, 255).astype(np.uint8)).save(name)

SETS = {
    "all": [
        (photo,    256, 256, "photo_256.png"),
        (photo,    640, 480, "photo_640x480.png"),
        (photo,   1024,1024, "photo_1024.png"),
        (anime,    512, 512, "anime_512.png"),
        (anime,    768, 432, "anime_wide.png"),
        (text_img, 400, 400, "text_400.png"),
        (portrait, 512, 512, "portrait_512.png"),
    ],
    "small": [
        (photo, 256, 256, "photo_256.png"),
        (text_img, 400, 400, "text_400.png"),
        (anime, 512, 512, "anime_512.png"),
    ],
    "large": [
        (photo, 1024, 1024, "photo_1024.png"),
        (portrait, 512, 512, "portrait_512.png"),
    ],
}

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("out_dir", nargs="?", default="/tmp/srtest")
    ap.add_argument("--sizes", choices=list(SETS), default="all")
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    np.random.seed(42)
    for fn, w, h, name in SETS[args.sizes]:
        path = os.path.join(args.out_dir, name)
        fn(w, h, path)
        print(f"  {name:24s} {w}x{h}")
    print(f"generated {len(SETS[args.sizes])} images in {args.out_dir}")

if __name__ == "__main__":
    main()
