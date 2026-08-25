# CrossScale Models

CrossScale uses ncnn-format super-resolution models. Model files are **not
bundled** with the CLI or the Android app — they are distributed via the
[models GitHub release](https://github.com/xihale/crossscale/releases/tag/models)
and downloaded on demand.

## Model Format

Each model is a pair of files:
- `.param` — model architecture definition
- `.bin` — model weights

Both files must share the same stem (e.g. `foo.param` + `foo.bin`).

## Getting Models

### CLI

```bash
crossscale model list-remote          # list models in the GitHub release
crossscale model download realesrganv3-anime-x2
crossscale model list                 # list models installed locally
crossscale model import ~/my-model.param ~/my-model.bin   # local custom models
```

Models live in the per-user store `$XDG_DATA_HOME/crossscale/models`
(`~/.local/share/crossscale/models` by default) — independent of the working
directory, so running from anywhere finds the same models. Override per
invocation with `--models-dir` or globally with `$CROSSSCALE_MODELS_DIR`.
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
      "url": "https://github.com/xihale/crossscale/releases/download/models/realesrganv3-anime-x2.zip"
    }
  ]
}
```

## Model Metadata

CrossScale automatically infers:
- Scale factor (2x, 4x, etc., from the `x2`/`x4` filename suffix)
- Input channels (RGB or Grayscale, from the param file)

## Attribution & Licensing

Bundled models are converted from [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN)
(© Xintao Wang and contributors, BSD 3-Clause License) to ncnn format. The Real-ESRGAN
license applies to the model weights; see the root [LICENSE](../LICENSE) for the
CrossScale code license.
