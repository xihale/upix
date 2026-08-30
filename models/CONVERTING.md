# Adding a model: pth → ncnn, end to end

How a model gets into the catalog: source vetting, architecture
identification, conversion, the verification gate, publishing, and the
docs that must move in the same commit. The catalog was converted with
session-local scripts following exactly this procedure — the procedure
is normative, the scripts are not shipped. Before starting, read the
[catalog](README.md) ("Known exclusions" in particular) and the
publishing section there.

The working rule that produced the current catalog: **spandrel is the
ground truth for everything** — architecture, key semantics, and
numerics. OpenModelDB arch labels are helpful but have been wrong;
when they disagree with spandrel, spandrel wins.

## 1. Source and vet the weights

- Find the [OpenModelDB](https://openmodeldb.info) record: it carries
  the license, a sha256 of the weights, and the download link. To
  screen the whole database at once, fetch the data repo tarball
  (`codeload.github.com/OpenModelDB/open-model-database/tar.gz/refs/heads/main`)
  and grep the records locally.
- Download from the author's own hosting (GitHub releases, HuggingFace)
  and **byte-match the sha256 before anything else**. Mediafire links
  don't expose a direct URL — scrape it from the `/file/` page.
- Watch for fp16-*storage* checkpoints: file size is not parameter
  count. (`4xNomos2_otf_esrgan` is a full 64nf RRDB stored in fp16 —
  33.7 MB, not a 32nf half-weight net, despite what the size suggests.)
- The license must be classifiable: `scripts/publish_models.py` has a
  `LICENSES` table and **raises on an unknown model name on purpose**,
  so no catalog entry can ship with its license silently unset. NC
  licenses are acceptable in the catalog (flagged in the docs), but
  they bind whoever redistributes or uses those weights.

## 2. Identify the architecture

Load the pth with [spandrel](https://github.com/kohya-ss/spandrel) and
dump the key layout, conv shapes, and spandrel's detected arch/scale.
If torchvision is unavailable in your Python, stub it — spandrel only
touches `torchvision.transforms.functional.{InterpolationMode,rotate,
normalize}`. Family notes for the arches in this catalog:

- **SRVGGNetCompact** (v3 compacts, Archivist, pokepress): traiNNer
  compacts usually use `learn_residual=True` — the net is `body(x) + x`.
  At scale 1 the PixelShuffle/upsampler degenerates to identity, so an
  x1 compact is just a conv stack with a global residual.
- **ESRGAN RRDB**: two key layouts exist. The "old arch" layout
  (`model.0`, `model.1.sub.N` with `.0.` wrappers) needs a remap to a
  clean `conv_first / trunk.N / lr_conv / ups.N / HRconv / conv_last`
  form; **KAIR nets (BSRGAN) use their own layout** (`conv_first /
  RRDB_trunk.N.RDB*.convK / trunk_conv / upconv1[,upconv2] / HRconv /
  conv_last`, no `.0.` wrappers). Count the remaining top-level keys to
  disambiguate the tail.
- **SPAN**: the stored `Conv3XC` must be reparameterized exactly the
  way spandrel does it in eval (1x1-sk + bottleneck folded into one
  3x3) — spandrel recomputes this on every forward, any archived
  `eval_conv` key is dead. traiNNer-trained SPANs normalize input as
  `(x − mean) · 255` in-graph but return `[0,1]`-scale output; the
  ncnn graph must replicate that verbatim or the output is black.
- **RealPLKSR**: partial large-kernel blocks; the channel-repeat
  residual can be rewritten as a nearest-interp concat, bitwise
  identical, and pnnx lowers that cleanly.

**Pitfall — the descriptor clamp**: spandrel's
`ImageModelDescriptor.__call__` applies `output.clamp_(0,1)`. For
bit-exact anchoring compare against the **raw** `m.model(x)` (or clamp
your own side identically), otherwise out-of-range values masquerade
as ~1e-1 "reimplementation divergence" that doesn't exist.

## 3. Reimplement in plain torch, anchor at 0 LSB

Rebuild the net as a plain `nn.Module` with your own key remap, load
the state dict, and forward a real image crop. Max abs diff vs spandrel
must be **exactly 0.0**. When it isn't, debug in this order (each step
caught a real bug before): per-tensor weight diff after remap (all
zero), then per-stage forward diff to isolate where divergence starts,
then re-check the descriptor-clamp pitfall above. Save the reference
output PNG and the traced TorchScript for the next step.

## 4. Convert with pnnx

- Get pnnx from `github.com/pnnx/pnnx` releases — Tencent/ncnn releases
  no longer carry pnnx assets.
- **Re-trace the TorchScript at a small input (64×64) first.** pnnx
  takes the input shape from the traced inputs and *ignores a
  mismatched `inputshape=` argument* — it re-runs the whole graph at
  trace-time shape. An RRDB traced at 256² makes one pnnx process
  allocate many GB and the machine runs out of memory; traced at 64²,
  conversion is light. Pure-conv nets are shape-agnostic, so the small
  trace changes nothing about the output graph.
- Even with small traces, run conversions **serially** (two concurrent
  at most) — pnnx still spikes hard per process.
- pnnx writes **fp16 weights by default** (`fp16=1`); also produce an
  fp32 graph (`fp16=0`) — step 5 needs both.
- Output files replace dashes with underscores
  (`Foo-x4.ts` → `Foo_x4.ncnn.param`); rename to the exact catalog stem
  when deploying.
- Known lowering notes: pixel-unshuffle heads (Real-ESRGAN-x2plus)
  lower to ncnn **Reorg**, and the engine compensates (input channels
  ÷ stride², advertised scale ÷ stride); nearest upsampling and the
  RealPLKSR residual rewrite lower cleanly. The pinned ncnn
  (20260113) has two behavior changes vs upstream 2022: `from_pixels`
  returns float, and **Real-CUGAN se x4's PixelShuffle-trunk executes
  as garbage on both CPU and Vulkan** — that topology is excluded from
  the catalog and the engine logs a warning on import. Re-test both
  when the ncnn pin moves.

## 5. The verification gate (every model, no exceptions)

1. **sha256** vs the OMDB record (done in step 1).
2. **torch reimpl vs spandrel raw**: 0.0.
3. **ncnn CPU vs torch truth** on a real crop: expect roughly the
   **44–51 dB** band. That band is ncnn/torch execution numerics, not
   conversion loss. Compute PSNR on `[0,1]` floats — feeding 0-255
   values produces a fake ~10.8 dB alarm.
4. **fp16/fp32 graph A/B**: the fp16-graph output vs the `fp16=0`
   fp32-graph output must be **byte-identical** (≥99 dB). This is what
   attributes all observed drift to execution numerics rather than the
   conversion — without it, step 3's band is just a hope.
5. **GPU vs CPU and vs truth**, inside the family's fp16 baseline.
   Baselines differ per family: RRDB nets sit around **~32 dB GPU/CPU**
   as normal fp16 drift (the x4plus baseline established this), lighter
   nets are far tighter. Compare like-for-like across the family, not
   against an absolute number.
6. **Odd sizes exact** (e.g. 127×125 in → 508×500 out) and **tiled ==
   whole** (≥99 dB) — catches padding/overlap bugs on both paths.
7. **Hot-swap chain**: run the new model sequentially with several
   others in one daemon/process. ncnn's `Net::load_param` into a live
   net does not clear state and reuses blob indices by name — a second
   load entangles the blob tables (`extract` returns −1, shapes go
   wrong, output can be silently wrong). The engine unloads before
   every load; this step proves it for the new net.
8. **Publish round-trip**: `scripts/publish_models.py` (local dir must
   contain *all* catalog pairs — assets absent from it get pruned),
   then verify the release **asset count == models + manifest** (the gh
   upload has silently truncated before), spot-check manifest
   scale/license fields, and re-download a zip byte-identical.

## 6. Integration checklist — three parsers, four docs

The `xN` scale parser exists in **three copies that must stay in
sync**: `scripts/publish_models.py`, the Android `ModelManager`, and
the C++ engine. Scale must be a *trailing* `-xN` suffix (a `1xFoo`
prefix does not parse — hence `DeJPG-RealPLKSR-x1`, not `1xDeJPG`).
Scale 1 is legal (identity upsampler); graph-based scale detection can
fail on such nets, and the Android client never calls `set_scale_factor`
— the filename suffix is the fallback that keeps every model working,
so keep it intact.

Same-commit checklist:

- [ ] `scripts/publish_models.py` `LICENSES` entry (SPDX id).
- [ ] `models/README.md`: catalog section row + attribution table row
      (source, sha256/verification provenance, license).
- [ ] Root `README.md`: the "carries N models" count, quick-guidance
      table rows, license paragraph counts.
- [ ] `apps/app/src/main/res/values/strings.xml`: `help_family_*` /
      `help_scenario_*` — the family cards mirror the models/README
      sections 1:1, the scenario layer crosses families.
- [ ] `docs/ARCHITECTURE.md`, only if engine-visible behavior changed
      (a new topology, a new lowering quirk the engine compensates for).
