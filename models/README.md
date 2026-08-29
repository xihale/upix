# Upix Models

Upix uses ncnn-format super-resolution models. Model files are **not
bundled** with the CLI or the Android app — they are distributed via the
[models GitHub release](https://github.com/xihale/upix/releases/tag/models)
and downloaded on demand.

## Model Catalog

The release ships 34 models covering six families. Sizes are the
on-disk `.param + .bin` download (zipped); "phone" vs "PC" is a rough
speed class for a recent flagship at 256–384 tiles — the heavy models
are fine anywhere but can take tens of seconds per 12 MP image on a
phone, where the PC-offload path shines.

### Real-ESRGAN v3 — tiny general-purpose workhorses

Converted from [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN)
(BSD 3-Clause). SRVGGNetCompact nets, 1–2 MB each — fast enough for
on-phone batch use and the default recommendation in both apps.

| Model | Size | Use it for |
|---|---|---|
| `Real-ESRGANv3-anime-x2` / `-x3` / `-x4` | 1.2 MB | Anime, illustrations, game art, screenshots. Trained for anime video frames, so it keeps line art crisp and cleans compression noise. The best first choice for any drawn content. |
| `Real-ESRGANv3-general-x4` | 2.3 MB | Real-world photos at 4x (the animevideov3 net's general-scene sibling, `realesr-general-x4v3`). Small, robust to JPEG/WEBP source noise. Best photo default on the phone. |

### Real-ESRGAN x4plus — heavy reference quality

Full RRDBNet models, BSD 3-Clause. Noticeably better texture
restoration than the v3 compacts, at 7–25× the compute — ideal for the
PC side of a paired offload, or when quality matters more than time.

| Model | Size | Use it for |
|---|---|---|
| `Real-ESRGAN-x4plus` | 31 MB | Photos, 4x. The classic general restoration net; strongest detail reconstruction of the catalog. First pick for PC-offloaded photo jobs. |
| `Real-ESRGAN-x4plus-anime` | 8.3 MB | Anime/illustration, 4x, reference quality when the v3 anime net's smaller capacity is visible (dense detail, large canvases). |

### Real-CUGAN — anime specialist (bilibili)

Weights © bilibili (ailab); ncnn conversion from
[realcugan-ncnn-vulkan](https://github.com/nihui/realcugan-ncnn-vulkan)
(MIT). Trained specifically on anime with real-world degradations
(compression, blurring); often beats Real-ESRGAN on heavy JPEG/H.264
anime sources. Each scale ships three denoise variants:

| Variant | Behavior | Use it for |
|---|---|---|
| `-conservative` | alters the image as little as possible | Sources you trust; preserving original grain and detail verbatim. |
| `-denoise` | strong denoise (upstream denoise3x) | Heavily compressed or noisy anime (old rips, streamed screenshots). |
| `-no-denoise` | no denoising | Clean sources where any smoothing is unwanted. |

| Model | Size |
|---|---|
| `Real-CUGAN-{conservative,denoise,no-denoise}-x2` / `-x3` | 2.4 MB |
| `Real-CUGAN-pro-{conservative,denoise,no-denoise}-x2` / `-x3` | 2.4 MB |

The `pro` models are bilibili's second-generation release (2022-07) and
generalize better; prefer them over the same-variant `se` model.

### Community photo models (OpenModelDB, CC-BY-4.0)

Six curated picks from [OpenModelDB](https://openmodeldb.info) by Helaman
(Phhofm), all CC-BY-4.0 (commercial use allowed). These fill the gaps
the Real-ESRGAN/CUGAN families leave: two photo 2x compacts and two
fast 4x compacts, plus two 4x nets tuned for real-world photo
degradation rather than anime — one classic RRDB, one modern RealPLKSR.

| Model | Size | Use it for |
|---|---|---|
| `NomosUni-Compact-x2` | 1.1 MB | Photos at 2x — SRVGG compact (the `multijpg` variant, trained across many JPEG quality levels with mixed downscaling kernels); restores social-media / messenger-compressed images. Fast enough for on-phone batch. |
| `NomosUni-SPAN-x2` | 0.8 MB | Photos at 2x on the same JPEG-degraded NomosUni corpus, in the lightweight SPAN arch (Swift Parameter-free Attention) — the lightest net in the catalog; pairs with the 2x compact for when every kilobyte counts. |
| `LSDIR-Compact-v2-x4` | 1.2 MB | Photos at 4x on the phone. Newer-generation SRVGG compact (LSDIR training, v2 multi-degradation): visibly stronger JPEG/blur cleanup than `v3-general-x4` at the same speed class. |
| `NomosUni-SPAN-x4` | 0.8 MB | Photos at 4x on modest hardware. SPAN (Swift Parameter-free Attention) — the lightest photo 4x in the catalog, trained on the same JPEG-degraded NomosUni corpus as the 2x compact (mixed downscale kernels, JPEG quality down to 40); a quality step above plain bicubic-style upscaling at batch-friendly speed. |
| `Nomos8kSC-x4` | 31 MB | Photos at 4x with strong JPEG-compression and blur removal (OTF-trained on Nomos8k). Fuller texture reconstruction than `v3-general-x4`; sits between it and `x4plus` in weight/compute, with a sharper, dehazed signature. |
| `NomosWebPhoto-RealPLKSR-x4` | 13.7 MB | Photos at 4x on web/social sources. RealPLKSR — a modern convnet (partial large-kernel blocks) at half the weight of the RRDB nets, with stronger cleanup of heavy web JPEG; the quality/size sweet spot of the photo 4x row. |

### Community 1x restoration nets (OpenModelDB)

Three same-recipe RealPLKSR nets that clean an image **in place** —
scale 1, output size equals input size, the catalog's first no-resize
models. All three are Phhofm nets trained on the NomosV2 photo corpus
(DeNoise and DeH264 fine-tuned from the DeJPG weights), CC-BY-4.0, and
run faster than any upscaler since they emit no extra pixels. Reach for
them when a source is already the right size but degraded.

| Model | Size | Use it for |
|---|---|---|
| `DeJPG-RealPLKSR-x1` | 14 MB | JPEG artifact removal at original size. OTF-trained on degradations down to JPEG quality 40 — chat-app/messenger re-compressions, re-saved downloads. Flattens 8×8 blocking while keeping texture (blocking-energy ratio 1.15 → 1.00 in our checks). |
| `DeNoise-RealPLKSR-x1` | 14 MB | Denoise at original size (200k iterations on OTF noise; also handles mild JPEG) — noisy phone-camera shots, high-ISO stills. |
| `DeH264-RealPLKSR-x1` | 14 MB | H.264 compression removal — video frames, screen recordings, streamed screenshots. |

### Community anime SPAN nets (OpenModelDB)

Two SPAN nets for anime at 2x from different trainers, filling the
ladder between the ~1 MB compacts and the heavy 4x classics: a 0.8 MB
restoration net for rough sources, and a 1.4 MB mid-weight net for
modern animation.

| Model | Size | Use it for |
|---|---|---|
| `HFA2k-LUDVAE-SPAN-x2` | 0.8 MB | Anime at 2x from rough sources. Trained on the HFA2k-LUDVAE corpus with realistic degradations (compression, noise, blur) — the anime counterpart of `NomosUni-Compact-x2`'s photo niche, and the lightest anime restoration net in the catalog. |
| `ModernSpanimation-x2` | 1.4 MB | Modern anime at 2x. TNTwise's Spanimation — a wider 64-channel SPAN trained on compression/blur/un-sharpening degradations; the quality step up from the 1.2 MB compacts while staying ~20× lighter than the 4x classics. |

### Community classics (OpenModelDB)

The five most-requested community nets. The Upix code is AGPL-3.0, but
four of these weights keep their own CC-BY-NC-SA-4.0 license:
noncommercial use only, so a commercial user of the code must swap them
out; `NMKD-Siax-x4` is WTFPL, the most permissive license in the
catalog. Each ncnn file is an adaptation of the upstream weights and
keeps the same license; conversions were verified against spandrel —
the community's reference PyTorch loader — with the ncnn nets matching
within uint8 quantization on CPU and inside each family's fp16 baseline
on GPU.

| Model | Size | Use it for |
|---|---|---|
| `UltraSharp-x4` | 32 MB | Photos at 4x. Kim2091's classic ESRGAN-arch net — for years the community's default all-rounder: sharp, texture-preserving, low over-smoothing. The middle ground between `Nomos8kSC-x4` (stronger cleanup) and `x4plus` (strongest reconstruction). |
| `Remacri-x4` | 31 MB | Photos at 4x. foolhardy's Remacri — alongside UltraSharp, the community's other long-time default all-rounder: natural mid-ground texture, gentle sharpening, tolerant of clean and compressed sources alike. |
| `AnimeSharp-x4` | 32 MB | Anime at 4x. Kim2091's line-art specialist — sharpens edges and flattens compression noise on illustrations; the 4x complement to the CUGAN/v3 anime sets when you want crisper lines rather than CUGAN's restoration character. |
| `AnimeJaNai-v2-Compact-x2` | 1.2 MB | Anime at 2x. torphedo's compact SRVGG net (traiNNer `learn_residual` arch) trained for mpv realtime playback — the anime counterpart of `NomosUni-Compact-x2`'s photo niche. Fast enough for on-phone batch. |
| `NMKD-Siax-x4` | 32 MB | Photos & textures at 4x. NMKD's 2020 classic ESRGAN-arch net, made for clean-to-mildly-compressed sources (JPEG quality 75+): strong texture and detail preservation with little smoothing — the detail-preserving counterpoint to the restoration-heavy nets, same era and weight class as UltraSharp. |

### Known exclusions

- **Real-CUGAN se x4** (official `up4x-*`): deliberately not shipped. The
  net uses a PixelShuffle-trunk topology that the currently pinned ncnn
  (20260113) executes to garbage on both CPU and Vulkan, while upstream
  ncnn 2022 ran it fine; x2/x3 from the same families are unaffected.
  The engine logs a warning when it detects this topology from an
  imported model. Revisit when the ncnn pin moves.
- **Real-CUGAN se x2 denoise1x/2x**: the release only carries the strong
  (`denoise3x`) variant; import the milder ones yourself via
  `upix model import` if you want finer denoise control.
- **Community models beyond the curated picks**: with Upix itself now
  noncommercially licensed, CC-BY-NC models are no longer excluded for
  license reasons — the catalog simply stays curated. Still not shipped:
  AnimeJaNai v1/v3 and the UltraCompact variants, IllustrationJaNai,
  the remaining SPAN and RealPLKSR siblings of the shipped nets, the
  bigger LSDIR variants, … Import any ncnn `.param`/`.bin`
  pair yourself via `upix model import` — but check the upstream license
  before redistributing.
- **Anime4K** (and ArtCNN): not usable here by design — these are
  real-time GLSL shader pipelines for mpv with weights baked into
  shader code, not neural nets with importable weights; their niche
  (live video on modest GPUs) is already covered by the CUGAN/v3
  compacts for offline images.
- **waifu2x / SRMD**: available in ncnn form (nihui packs, MIT) but
  strictly outdated in quality by everything above; not worth the
  catalog weight.
- Community ESRGAN nets beyond the curated picks are covered above;
  what limits the catalog is curation and verification bandwidth, not
  license mixing — each shipped model gets its license classified and
  documented either way.

## Model Format

Each model is a pair of files:
- `.param` — model architecture definition
- `.bin` — model weights

Both files must share the same stem (e.g. `foo.param` + `foo.bin`).

## Getting Models

### CLI

```bash
upix model list-remote          # list models in the GitHub release
upix model download realesrganv3-anime-x2
upix model list                 # list models installed locally
upix model import ~/my-model.param ~/my-model.bin   # local custom models
```

Models live in the per-user store `$XDG_DATA_HOME/upix/models`
(`~/.local/share/upix/models` by default) — independent of the working
directory, so running from anywhere finds the same models. Override per
invocation with `--models-dir` or globally with `$UPIX_MODELS_DIR`.
The directory is created on demand by `model download` / `model import`.

### Android

Models page → 下载模型 (download icon): fetches the release manifest, lists
available models, and downloads them into the app's model directory.
本地自定义模型 (your own `.param`/`.bin`/`.zip`) can still be imported via
导入模型.

## Layout

Models are discovered recursively, so both layouts work:

```
models/
  Real-ESRGANv3-anime-x2.param      # flat
  Real-ESRGANv3-anime-x2.bin
  Real-ESRGANv3-anime/
    x2.param                        # nested (name becomes "Real-ESRGANv3-anime-x2")
    x2.bin
```

A `models/models` symlink pointing at a machine-local store is followed by
the scanner when you pass that directory explicitly (`--models-dir`); the
default resolution no longer depends on any cwd-relative path.

## Publishing (maintainers)

Models live in the git tag `models` / release `models`. Use
`scripts/publish_models.py` to (re)build the release: it packs each model
directory into a zip, computes sha256/size, and writes `manifest.json`:

```bash
./scripts/publish_models.py                                          # publish new models, skip existing
./scripts/publish_models.py models Real-ESRGANv3-anime-x2            # one model (full name or MODEL STEM)
./scripts/publish_models.py models Real-ESRGANv3-anime-x2 --overwrite
```

Requires the `gh` CLI authenticated with repo scope (`gh auth login`); no
token env vars. Models already in the release are **skipped** by default
(their manifest entry reuses the asset's sha256/size); `--overwrite`
re-uploads the selected ones. Assets absent from the local catalog are
pruned unless `--no-prune`.

```json
{
  "version": 1,
  "models": [
    {
      "name": "realesrganv3-anime-x2",
      "scale": 2,
      "license": "BSD-3-Clause",
      "size_bytes": 17825792,
      "sha256": "…",
      "url": "https://github.com/xihale/upix/releases/download/models/realesrganv3-anime-x2.zip"
    }
  ]
}
```

The `license` field is informational (SPDX id; both clients ignore it —
full terms in the attribution table above). `license_from_name` refuses
to classify an unknown model, so a new catalog entry cannot ship with
its license silently unset.

## Model Metadata

Upix automatically infers:
- Scale factor (2x, 4x, etc., from the `x2`/`x4` filename suffix)
- Input channels (RGB or Grayscale, from the param file)

## Attribution & Licensing

Model weights are redistributed in ncnn format; each family keeps its
upstream license:

| Family | Source | License |
|---|---|---|
| Real-ESRGAN v3 compacts + x4plus | [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN), official ncnn pack (`realesrgan-ncnn-vulkan`) | BSD 3-Clause © Xintao Wang and contributors |
| `Real-ESRGANv3-general-x4` | realesr-general-x4v3 weights (BSD 3-Clause); ncnn conversion via [tumuyan2/realsr-models](https://huggingface.co/tumuyan2/realsr-models) (RealSR-NCNN-Android) | BSD 3-Clause (weights) |
| Real-CUGAN (se + pro) | [bilibili ailab Real-CUGAN](https://github.com/bilibili/ailab/tree/main/Real-CUGAN), ncnn pack [realcugan-ncnn-vulkan](https://github.com/nihui/realcugan-ncnn-vulkan) | MIT (ncnn pack, © nihui); model weights follow the upstream Real-CUGAN release terms |
| `Nomos8kSC-x4` | 4x-Nomos8kSC by [Helaman / Phhofm](https://github.com/Phhofm/models); ncnn conversion from [upscayl/custom-models](https://github.com/upscayl/custom-models) | CC-BY-4.0 (weights); upscayl pack license applies to its conversion |
| `NomosUni-Compact-x2` | 2x-NomosUni-compact-multijpg by [Helaman / Phhofm](https://github.com/Phhofm/models) (SRVGGNetCompact, `learn_residual=True`); converted to ncnn with pnnx and verified against a fp32 PyTorch reference | CC-BY-4.0 (weights) |
| `UltraSharp-x4` | 4x-UltraSharp by [Kim2091](https://github.com/Kim2091) (ESRGAN RRDB arch); pth from the author's [HuggingFace mirror](https://huggingface.co/Kim2091/UltraSharp), converted to ncnn with pnnx and verified against a fp32 PyTorch reference | CC-BY-NC-SA-4.0 (weights and ncnn adaptation) |
| `AnimeSharp-x4` | 4x-AnimeSharp by [Kim2091](https://github.com/Kim2091) (ESRGAN RRDB arch); pth from the author's [HuggingFace mirror](https://huggingface.co/Kim2091/AnimeSharp), converted to ncnn with pnnx and verified against a fp32 PyTorch reference | CC-BY-NC-SA-4.0 (weights and ncnn adaptation) |
| `AnimeJaNai-v2-Compact-x2` | 2x_AnimeJaNai_V2 Compact by [torphedo / the-database](https://github.com/the-database/AnimeJaNai) (SRVGGNetCompact, `learn_residual=True`, `params_ema`); converted to ncnn with pnnx and verified against a fp32 PyTorch reference | CC-BY-NC-SA-4.0 (weights and ncnn adaptation) |
| `Remacri-x4` | 4x_foolhardy_Remacri by foolhardy (ESRGAN RRDB arch); pth from the [HuggingFace mirror](https://huggingface.co/FacehugmanIII/4x_foolhardy_Remacri) listed on OpenModelDB, byte-matched to its recorded sha256; converted to ncnn with pnnx and verified against a fp32 PyTorch reference | CC-BY-NC-SA-4.0 (weights and ncnn adaptation) |
| `LSDIR-Compact-v2-x4` | 4x-LSDIRCompact-v2 by [Helaman / Phhofm](https://github.com/Phhofm/models) (SRVGGNetCompact, `learn_residual=True`); fp16 safetensors from the author's own [HuggingFace](https://huggingface.co/Phips/4xLSDIRCompact2), converted to ncnn with pnnx and verified against a fp32 PyTorch reference | CC-BY-4.0 (weights) |
| `NomosUni-SPAN-x4` | 4x-NomosUni_span_multijpg by [Helaman / Phhofm](https://github.com/Phhofm/models) (SPAN — Swift Parameter-free Attention); pth sha256-matched against the OpenModelDB record; the eval-path Conv3XC reparameterization (1x1-sk + bottleneck → single 3x3, spandrel's eval semantics) replicated exactly, converted to ncnn with pnnx, and verified against spandrel's own fp32 output (the traiNNer nets normalize input by (x−mean)·255 but return [0,1]-scale output — the graph replicates that verbatim) | CC-BY-4.0 (weights) |
| `NomosUni-SPAN-x2` | 2x-NomosUni_span_multijpg by [Helaman / Phhofm](https://github.com/Phhofm/models), same SPAN recipe and conversion path as the 4x above (pth sha256-matched against the OpenModelDB record, verified against spandrel's fp32 output) | CC-BY-4.0 (weights) |
| `HFA2k-LUDVAE-SPAN-x2` | 2x-HFA2k-LUDVAE-SPAN by [Helaman / Phhofm](https://github.com/Phhofm/models) (SPAN); pth from the OpenModelDB-linked Google Drive, sha256-matched against the record; converted to ncnn with pnnx and verified against spandrel's fp32 output | CC-BY-4.0 (weights) |
| `ModernSpanimation-x2` | 2x-ModernSpanimationV1 by [TNTwise](https://github.com/TNTwise) (SPAN, 64nf); pth from the author's GitHub release, sha256-matched against the OpenModelDB record; converted to ncnn with pnnx and verified against spandrel's fp32 output | MIT (weights) |
| `NMKD-Siax-x4` | 4x_NMKD-Siax_200k by [NMKD](https://nmkd.de) (ESRGAN RRDB arch); pth via a HuggingFace mirror, sha256-matched against the OpenModelDB record; converted to ncnn with pnnx and verified against spandrel's fp32 output | WTFPL (weights) |
| `NomosWebPhoto-RealPLKSR-x4` | 4xNomosWebPhoto_RealPLKSR by [Helaman / Phhofm](https://github.com/Phhofm/models) (RealPLKSR arch); pth from the author's [models release](https://github.com/Phhofm/models), byte-matched to its recorded sha256; converted to ncnn with pnnx (the channel-repeat residual rewritten as nearest-interp, bitwise identical) and verified against a fp32 PyTorch reference | CC-BY-4.0 (weights) |
| `DeJPG` / `DeNoise` / `DeH264-RealPLKSR-x1` | 1xDeJPG_realplksr_otf, 1xDeNoise_realplksr_otf and 1xDeH264_realplksr by [Helaman / Phhofm](https://github.com/Phhofm/models) (RealPLKSR at scale 1 — the upsampler degenerates to identity, so the net is a pure conv stack with a global residual); pth sha256-matched against the OpenModelDB records; converted to ncnn with pnnx and verified against spandrel's fp32 output (0 LSB) | CC-BY-4.0 (weights) |

See the root [LICENSE](../LICENSE) for the Upix code license. When
redistributing the models release, retain the notices above.
