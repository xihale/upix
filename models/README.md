# Upix Models

Upix uses ncnn-format super-resolution models. Model files are **not
bundled** with the CLI or the Android app — they are distributed via the
[models GitHub release](https://github.com/xihale/upix/releases/tag/models)
and downloaded on demand.

## Model Catalog

The release ships 20 models covering four families. Sizes are the
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

Two curated picks from [OpenModelDB](https://openmodeldb.info) by Helaman
(Phhofm), both CC-BY-4.0 (commercial use allowed). These fill the gaps
the Real-ESRGAN/CUGAN families leave: a dedicated photo 2x, and a 4x
tuned for real-world photo degradation rather than anime.

| Model | Size | Use it for |
|---|---|---|
| `NomosUni-Compact-x2` | 1.1 MB | Photos at 2x — the catalog's only photo 2x. SRVGG compact trained against heavy JPEG degradation (down to quality 40) and mixed downscaling kernels; restores social-media / messenger-compressed images. Fast enough for on-phone batch. |
| `Nomos8kSC-x4` | 31 MB | Photos at 4x with strong JPEG-compression and blur removal (OTF-trained on Nomos8k). Fuller texture reconstruction than `v3-general-x4`; sits between it and `x4plus` in weight/compute, with a sharper, dehazed signature. |

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
- **Community models with non-commercial licenses**: the popular picks
  are mostly CC-BY-NC(-SA) and are deliberately not redistributed —
  4x-UltraSharp and 4x-AnimeSharp (Kim2091), AnimeJaNai v1–v3
  (torphedo), IllustrationJaNai. Import any ncnn `.param`/`.bin` pair
  yourself via `upix model import` for private use.
- **Anime4K** (and ArtCNN): not usable here by design — these are
  real-time GLSL shader pipelines for mpv with weights baked into
  shader code, not neural nets with importable weights; their niche
  (live video on modest GPUs) is already covered by the CUGAN/v3
  compacts for offline images.
- **waifu2x / SRMD**: available in ncnn form (nihui packs, MIT) but
  strictly outdated in quality by everything above; not worth the
  catalog weight.
- Community ESRGAN nets beyond the two curated above (Remacri,
  NMKD-Siax, LSDIR series, …): mixed licensing, not redistributed;
  import them yourself.

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
      "size_bytes": 17825792,
      "sha256": "…",
      "url": "https://github.com/xihale/upix/releases/download/models/realesrganv3-anime-x2.zip"
    }
  ]
}
```

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

See the root [LICENSE](../LICENSE) for the Upix code license. When
redistributing the models release, retain the notices above.
