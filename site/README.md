# Upix · 一幅作品的展览

Upix 落地页。原生 HTML、CSS、JavaScript；Vite 开发与发布，GSAP 滚动动效。

## 运行

```sh
npm install
npm run dev -- --port 5174
```

`npm run lint` 检查 JavaScript，`npm run build` 生成静态发布文件，`npm run preview` 预览发布文件。

## 页面

首屏拖动对比原图与修复图，支持鼠标、触摸和键盘方向键、Home/End；放大/还原同步改变两图尺寸与位置；开启「减少动态效果」显示静态内容；加载失败可重试并保留状态。仅展示固定作品的对比，不含上传或在线推理。

首屏之后是「作品之外」特性区：五条展签（局域网协作、海量社区模型、不重复处理、全程本地、三端同源），左列短名、右列一句话，沿用获取区的发丝线与字级（`src/style.css` 的 `.feature-list`），不引入卡片、图标或第二强调色。文案只写仓库 README 的 Features 与已发布能力，新特性先上线再上页面；口径是不写具体数量、协议名、文件格式与实现术语（用户要求），条目本身也不求多。

主题跟随 `prefers-color-scheme`：亮色是银白展台配晴空那幅，暗色是深底配夜色那幅，两幅各自 4×、原图尺寸分别是 640 × 360 与 640 × 427（图注随主题切换）。选图用 `<picture>` 的 `media`，首帧就不会先画错一张；`<meta name="theme-color">` 按主题分两条、`color-scheme` 声明 `light dark`。系统在会话中途切主题时，`matchMedia` 的 change 事件负责重新等待解码并换 alt。

键盘仍可用方向键 / Home / End 操作对比，但**图像与把手都不做焦点描边**（用户明确要求），这是对 WCAG 2.4.7 的有意偏离。

`?state=zoom` 深链以放大态直接打开页面，供证据截图与回归脚本复用（`src/main.js`），不是用户功能。

获取入口：Android 指向 GitHub 发布 `android-v0.1.1`（arm64-v8a APK），CLI 指向 GitHub 发布 `cli-v0.1.0`。手机布局隐藏首屏导航里的 GitHub 入口，仓库仍可从获取区的发布页进入。

## 部署

推送 main（改动 `site/**`）触发 `.github/workflows/deploy-pages.yml`：lint、构建并发布 `site/dist` 到 GitHub Pages，并自动启用 Pages（`configure-pages`）。构建时从 GitHub API 解析最新的 `android-*` / `cli-*` 发布 tag 自动替换下载链接（见 `vite.config.js`，API 不可用时回退到内置固定 tag）。自定义域名 `upix.xihale.top`：DNS CNAME → `xihale.github.io`，`public/CNAME` 绑定，HTTPS 证书由 GitHub 在首次部署后自动签发。

维护这些链接与元数据时需要同时改两处，改动后跑一次 `npm run build` 并检查 `dist/index.html`：

- `index.html` 里两个 `releases/tag/<平台>-…` 是替换锚点，`vite.config.js` 的 `FALLBACK_TAGS` 必须与它们取同值；否则 API 不可用时会构建出与配置不一致的固定链接。
- API 返回的 tag 只接受 `[A-Za-z0-9._-]`（git 允许 ref 名含 `"`/`<`/`&`，会注入 HTML 属性），不合格的 tag 被跳过并保留固定 tag。
- `index.html` 的 `canonical` / `og:*` 绝对地址必须与 `public/CNAME` 的域名一致；相对地址会被抓取器丢弃（链接预览无图）。

## 素材

- 图片：两幅作品都取自《重返未来：1999》官网壁纸（[来源](https://re.bluepoch.com/home/detail.html#wallpaper)，© 广州深蓝互动网络科技有限公司）。图注已标注来源；官网《许可及服务协议》禁止未经书面同意的商业性使用与信息网络传播，公开部署前需获授权或替换为自有/开放授权素材。
- 两幅各是一对 `source-*.webp`（低分辨率原图）与 `restored-*.webp`（Upix 4× 结果）。暗色那对是既有素材（640 × 427 → 2560 × 1708）；亮色那对现场生成：2560 × 1440 的壁纸用 Lanczos 降到 640 × 360 存成 WebP q82 当原图，再喂给 Upix 自己放大——`upix-cli public/images/source-light.webp -o public/images/restored-light.webp -m Remacri-x4 --output-format webp --output-quality 85 --webp-method 6`（2560 × 1440、249 KB）。选型是量化比较的结果：Remacri-x4 对真值的 SSIM 0.8882 / PSNR 27.97 dB、纹理能量 2.95（真值 3.43），优于 AnimeSharp-x4（0.8843 / 28.09 / 2.55）与 HFA2k-x4（0.8784 / 28.48 / 2.25）；q85 相对 q90 的 PSNR 42.2 dB，肉眼无差，省 71 KB。
- `restored-*.webp` 保持 2560 宽（4×）全尺寸，不要加响应式小图变体：手机放大 2.1× 在 DPR 3 下要 ~2450 设备像素，降采样会让「放大细节」变糊。移动端省下的是 ~250 KB 传输，换来的是核心交互失真，不划算。
- 换素材时记得同步三处：`<picture>` 的 dark `<source>`、`<img>` 的亮色 `src`、以及图注里的 `640 × 360` / `640 × 427`；`og:image` 指向亮色那幅（抓取器按亮色渲染），`preload` 两条都带 `media`。
- 字体：自托管 Manrope 400/600 与 Noto Sans SC 400（WOFF2，OFL 许可证在 `public/fonts/`）。中文字体是按页面文案切出的子集，改动任何中文文案后都要重跑 `./scripts/subset-font.sh`（需要 `fonttools` + `brotli`，默认从 Google Fonts 取 Noto Sans SC 可变字体并实例化到 wght=400）；脚本在缺字时非零退出，别让新文案落到系统字体上。Pages 对这些静态资源 `max-age=600`，改字后最迟十分钟全量生效。
- 图标：favicon 与 apple-touch-icon 取自 Android 启动图标（adaptive icon 浅色配色，`public/images/icon.svg` 复刻 foreground 矢量）。

设计与评审记录在 `design/`。
