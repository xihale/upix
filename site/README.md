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

获取入口：Android 指向 GitHub 发布 `android-v0.1.1`（arm64-v8a APK），CLI 指向 GitHub 发布 `cli-v0.1.0`。

## 部署

推送 main（改动 `site/**`）触发 `.github/workflows/deploy-pages.yml`：lint、构建并发布 `site/dist` 到 GitHub Pages，并自动启用 Pages（`configure-pages`）。构建时从 GitHub API 解析最新的 `android-*` / `cli-*` 发布 tag 自动替换下载链接（见 `vite.config.js`，API 不可用时回退到内置固定 tag）。自定义域名 `upix.xihale.top`：DNS CNAME → `xihale.github.io`，`public/CNAME` 绑定，HTTPS 证书由 GitHub 在首次部署后自动签发。

## 素材

- 图片：《重返未来：1999》官网壁纸（[来源](https://re.bluepoch.com/home/detail.html#wallpaper)，© 广州深蓝互动网络科技有限公司）。图注已标注来源；官网《许可及服务协议》禁止未经书面同意的商业性使用与信息网络传播，公开部署前需获授权或替换为自有/开放授权素材。
- 字体：自托管 Manrope 400/600 与 Noto Sans SC 400（WOFF2，OFL 许可证在 `public/fonts/`）。中文字体按页面文案提取子集，新增文案需同步扩充字形。

设计与评审记录在 `design/`。
