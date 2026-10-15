#!/usr/bin/env bash
# 按页面文案重切 Noto Sans SC 子集（public/fonts/noto-sc-400.woff2）。
# 页面改动文案后必须重跑，否则新字会落到系统字体上，字重与字形跳变。
#
#   ./scripts/subset-font.sh              # 从 Google Fonts 拉取变体字体（需要网络）
#   ./scripts/subset-font.sh path/to/font.ttf
#
# 源字体是 Noto Sans SC 可变字体（wght 100–900），脚本把它实例化到 wght=400，
# 与 Manrope 400 同级；字形轮廓与既有子集逐字一致（同一次上游发布）。
set -euo pipefail

site_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="$site_dir/public/fonts/noto-sc-400.woff2"
source_url="https://raw.githubusercontent.com/google/fonts/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf"
source_font="${1:-}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

python3 -c 'import fontTools, brotli' 2>/dev/null || {
  echo "需要 fonttools 与 brotli（pip install fonttools brotli）" >&2; exit 1; }

if [ -z "$source_font" ]; then
  source_font="$work/NotoSansSC-var.ttf"
  echo "下载源字体：$source_url"
  curl -fsSL --max-time 300 -o "$source_font" "$source_url"
fi

python3 - "$site_dir" "$source_font" "$work" <<'PY'
import glob, re, subprocess, sys
from pathlib import Path
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

site_dir, source_font, work = (Path(p) for p in sys.argv[1:4])

# 收录范围与既有子集一致：只取中日韩表意文字与全角/中文标点，
# 拉丁字母与数字由 Manrope 承担（字体栈里 Manrope 在前）。
ranges = ((0x3000, 0x303F), (0x4E00, 0x9FFF), (0x3400, 0x4DBF), (0xFF00, 0xFFEF))
corpus = "\n".join(
    Path(p).read_text(encoding="utf-8")
    for p in [site_dir / "index.html", *sorted(glob.glob(str(site_dir / "src/*.js")))]
)
# 注释不渲染，别把它们的字带进子集
corpus = re.sub(r"<!--.*?-->", "", corpus, flags=re.S)
needed = sorted({c for c in corpus if any(lo <= ord(c) <= hi for lo, hi in ranges)})
text_file = work / "chars.txt"
text_file.write_text("".join(needed), encoding="utf-8")
print(f"页面用到的中文字符：{len(needed)} 个")

font = TTFont(source_font)
version = next((str(r) for r in font["name"].names if r.nameID == 5), "")
if "2.004" not in version:
    print(f"注意：源字体版本为 {version!r}，既有子集来自 Version 2.004-H2", file=sys.stderr)
instance = work / "NotoSansSC-Regular.ttf"
instancer.instantiateVariableFont(font, {"wght": 400}).save(instance)

out = site_dir / "public/fonts/noto-sc-400.woff2"
subprocess.run([
    "pyftsubset", str(instance), f"--text-file={text_file}", "--flavor=woff2",
    "--layout-features=*", f"--output-file={out}", "--name-IDs=*", "--drop-tables+=DSIG",
], check=True)

subset = TTFont(out)
cmap = subset.getBestCmap()
missing = [c for c in needed if ord(c) not in cmap]
if missing:
    sys.exit(f"子集缺字：{''.join(missing)}")
print(f"已写出 {out.relative_to(site_dir)}：{out.stat().st_size} 字节，覆盖 {len(cmap)} 个码位，无缺字")
PY
