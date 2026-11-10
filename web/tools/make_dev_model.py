#!/usr/bin/env python3
"""Generate deterministic ONNX dev models for the Upix web frontend (issue #2, path B1).

This is a fixture/template, NOT a model conversion: real catalog models come from
`scripts/convert_onnx.py` (torch → pnnx → ONNX, the CONVERTING.md procedure). This
script authors a small SPAN-style graph with onnx helpers only (no torch) so the
web tile pipeline can be smoke-tested end to end and the graph layout that real
conversions must replicate is written down in executable form:

  - traiNNer SPAN convention replicated verbatim (CONVERTING.md §2/§4):
    in-graph normalization `(x - mean) * 255`, output back at [0,1] scale.
  - PixelShuffle upsampling head, either as native DepthToSpace (WebGPU/wasm EP)
    or decomposed to Reshape→Transpose→Reshape (WebNN EP single-partition, issue
    #2 path C: WebNN has no DepthToSpace).

Usage:
    .venv/bin/python make_dev_model.py --out ../public/models/dev-span-x2.onnx \
        [--channels 16] [--blocks 2] [--scale 2] [--seed 42] [--decompose-pixelshuffle]

Weights are seeded → byte-identical output for identical args (sha256 in the
emitted .meta.json lets tests pin the fixture).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper


def conv(g: onnx.GraphProto, x: str, w: str, b: str, name: str) -> str:
    out = f"{name}/out"
    g.node.append(
        helper.make_node(
            "Conv", [x, w, b], [out], name=name,
            kernel_shape=[3, 3], pads=[1, 1, 1, 1], strides=[1, 1],
        )
    )
    return out


def leaky_relu(g: onnx.GraphProto, x: str, name: str, alpha: float = 0.1) -> str:
    out = f"{name}/out"
    g.node.append(helper.make_node("LeakyRelu", [x], [out], name=name, alpha=alpha))
    return out


def pixel_shuffle(g: onnx.GraphProto, x: str, name: str, r: int, c: int, h: int, w: int, decompose: bool) -> str:
    """[N, C*r*r, H, W] -> [N, C, H*r, W*r] (PyTorch PixelShuffle layout)."""
    if not decompose:
        out = f"{name}/out"
        g.node.append(
            helper.make_node("DepthToSpace", [x], [out], name=name, blocksize=r)
        )
        return out
    n = x  # dynamic N not needed: fixture uses N=1 explicitly
    shape1 = helper.make_tensor(f"{name}/sh1", TensorProto.INT64, [6], [1, c, r, r, h, w])
    g.node.append(helper.make_node("Reshape", [x, f"{name}/sh1"], [f"{name}/r1"], name=f"{name}/reshape1"))
    g.initializer.append(shape1)
    perm = [0, 1, 4, 2, 5, 3]
    g.node.append(helper.make_node("Transpose", [f"{name}/r1"], [f"{name}/t"], name=f"{name}/transpose", perm=perm))
    shape2 = helper.make_tensor(f"{name}/sh2", TensorProto.INT64, [4], [1, c, h * r, w * r])
    g.node.append(helper.make_node("Reshape", [f"{name}/t", f"{name}/sh2"], [f"{name}/r2"], name=f"{name}/reshape2"))
    g.initializer.append(shape2)
    return f"{name}/r2"


def build(out_path: pathlib.Path, channels: int, blocks: int, scale: int, seed: int,
          decompose: bool, fixed_hw: int | None, mean: float) -> dict:
    rng = np.random.default_rng(seed)
    dim = TensorProto.FLOAT

    inputs = [
        helper.make_tensor_value_info("input", dim, [1, 3, fixed_hw or "H", fixed_hw or "W"]),
    ]
    outputs = [helper.make_tensor_value_info("output", dim, [1, 3, None, None])]

    g = helper.make_graph(
        [], "upix_dev_span", inputs, outputs,
    )

    def init(name: str, arr: np.ndarray) -> str:
        g.initializer.append(numpy_helper.from_array(arr.astype(np.float32), name))
        return name

    # traiNNer SPAN in-graph normalization, verbatim: (x - mean) * 255
    init("mean", np.array([mean]))
    g.node.append(helper.make_node("Sub", ["input", "mean"], ["norm/sub"], name="norm/sub"))
    g.node.append(helper.make_node("Mul", ["norm/sub", "mean255"], ["norm/mul"], name="norm/mul"))
    init("mean255", np.array([255.0]))

    cur = "norm/mul"
    c_in = 3
    for i in range(blocks):
        b = f"block{i}"
        w = init(f"{b}/w", rng.normal(0, 0.05 / np.sqrt(3), (channels, c_in, 3, 3)))
        bias = init(f"{b}/b", rng.normal(0, 0.01, (channels,)))
        h = conv(g, cur, w, bias, f"{b}/conv")
        h = leaky_relu(g, h, f"{b}/act")
        # parameter-free attention flavor: sigmoid gate (spatial-channel mix) -> mul
        gw = init(f"{b}/gate_w", rng.normal(0, 0.05 / np.sqrt(3), (channels, channels, 3, 3)))
        gb = init(f"{b}/gate_b", rng.normal(0, 0.01, (channels,)))
        gate = conv(g, h, gw, gb, f"{b}/gate")
        g.node.append(helper.make_node("Sigmoid", [gate], [f"{b}/gate_sig"], name=f"{b}/gate_sig"))
        g.node.append(helper.make_node("Mul", [h, f"{b}/gate_sig"], [f"{b}/att"], name=f"{b}/att"))
        if c_in == channels:
            g.node.append(helper.make_node("Add", [h, f"{b}/att"], [f"{b}/res"], name=f"{b}/res"))
            cur = f"{b}/res"
        else:
            cur = f"{b}/att"
        c_in = channels

    # upsampling head: conv 3->3*r*r then PixelShuffle
    head = "head"
    r2 = scale * scale
    w = init(f"{head}/w", rng.normal(0, 0.05 / np.sqrt(3), (3 * r2, c_in, 3, 3)))
    bias = init(f"{head}/b", rng.normal(0, 0.01, (3 * r2,)))
    h = conv(g, cur, w, bias, f"{head}/conv")
    if decompose and fixed_hw is None:
        raise SystemExit("--decompose-pixelshuffle needs --fixed-hw (static shape) for the reshape dims")
    h = pixel_shuffle(g, h, f"{head}/shuffle", scale, 3, fixed_hw, fixed_hw, decompose)
    # output back at [0,1] scale (spandrel applies clamp_ externally; fixture uses Sigmoid)
    g.node.append(helper.make_node("Sigmoid", [h], ["output"], name="out_sigmoid"))

    model = helper.make_model(
        g, opset_imports=[helper.make_opsetid("", 17)], producer_name="upix-dev-model-gen",
    )
    model.ir_version = 8  # ORT-web/webnn-friendly
    onnx.checker.check_model(model)
    onnx.save_model(model, str(out_path))

    meta = {
        "name": out_path.stem,
        "arch": "span-dev",
        "scale": scale,
        "channels": channels,
        "blocks": blocks,
        "seed": seed,
        "mean": mean,
        "pixelshuffle": "decomposed" if decompose else "native",
        "input": "NCHW float32 [1,3,H,W], values in [0,1]",
        "output": "NCHW float32 [1,3,H*r,W*r], values in [0,1] (Sigmoid)",
        "sha256": hashlib.sha256(out_path.read_bytes()).hexdigest(),
    }
    meta_path = out_path.with_suffix(".meta.json")
    meta_path.write_text(json.dumps(meta, indent=2) + "\n")
    return meta


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, type=pathlib.Path)
    ap.add_argument("--channels", type=int, default=16)
    ap.add_argument("--blocks", type=int, default=2)
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--fixed-hw", type=int, default=None, help="pin H=W for the decomposed variant")
    ap.add_argument("--mean", type=float, default=0.5)
    ap.add_argument("--decompose-pixelshuffle", action="store_true")
    args = ap.parse_args()

    args.out.parent.mkdir(parents=True, exist_ok=True)
    meta = build(args.out, args.channels, args.blocks, args.scale, args.seed,
                 args.decompose_pixelshuffle, args.fixed_hw, args.mean)
    print(json.dumps(meta, indent=2))


if __name__ == "__main__":
    main()
