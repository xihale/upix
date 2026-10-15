import { gsap } from 'gsap';
import { ScrollTrigger } from 'gsap/ScrollTrigger';

gsap.registerPlugin(ScrollTrigger);

const artwork = document.querySelector('.artwork');
const comparison = document.querySelector('.comparison-input');
const zoomButton = document.querySelector('.zoom-button');
const retryButton = document.querySelector('.retry-button');
const message = document.querySelector('.status-message');
const images = [...artwork.querySelectorAll('img')];
const pictures = [...artwork.querySelectorAll('picture')];
const originalUrls = images.map(image => image.getAttribute('src'));
// 明暗两幅构图不同，同一个 <img> 的 alt 得跟着主题走；文案在 index.html 的 data-alt-dark 里
const originalSourceSets = pictures.map(picture => [...picture.querySelectorAll('source')].map(source => source.getAttribute('srcset')));
const darkScheme = matchMedia('(prefers-color-scheme: dark)');
const alts = images.map(image => ({ light: image.alt, dark: image.dataset.altDark || image.alt }));
let loadingGeneration = 0;
let split = 50;
let zoomed = false;

function applyScheme() {
  const dark = darkScheme.matches;
  images.forEach((image, index) => { image.alt = dark ? alts[index].dark : alts[index].light; });
}

function updateSplit(value) {
  split = Math.min(100, Math.max(0, Number(value)));
  comparison.value = String(split);
  comparison.setAttribute('aria-valuetext', `原图 ${split}%，修复图 ${100 - split}%`);
  artwork.style.setProperty('--split', `${split}%`);
}

function setZoom(value) {
  zoomed = value;
  artwork.style.setProperty('--image-scale', value ? '2.1' : '1');
  zoomButton.setAttribute('aria-pressed', String(value));
  zoomButton.innerHTML = `${value ? '还原画面' : '放大细节'}<span aria-hidden="true">${value ? '−' : '＋'}</span>`;
}

async function loadImages(retry = false) {
  const generation = ++loadingGeneration;
  artwork.dataset.status = 'loading';
  artwork.setAttribute('aria-busy', 'true');
  comparison.disabled = true;
  zoomButton.disabled = true;
  retryButton.hidden = true;
  message.textContent = '正在载入图片';
  if (retry) {
    images.forEach((image, index) => {
      image.src = `${originalUrls[index]}?retry=${generation}`;
    });
    // <picture> 命中 <source> 时 img.src 不参与选择，重试参数得同时写到 source 上
    pictures.forEach((picture, index) => {
      picture.querySelectorAll('source').forEach((source, sourceIndex) => {
        source.srcset = `${originalSourceSets[index][sourceIndex]}?retry=${generation}`;
      });
    });
  }
  try {
    await Promise.all(images.map(image => image.decode()));
    if (generation !== loadingGeneration) return;
    artwork.dataset.status = 'ready';
    artwork.setAttribute('aria-busy', 'false');
    comparison.disabled = false;
    zoomButton.disabled = false;
    ScrollTrigger.refresh();
  } catch {
    if (generation !== loadingGeneration) return;
    artwork.dataset.status = 'error';
    artwork.setAttribute('aria-busy', 'false');
    message.textContent = '图片加载失败';
    retryButton.hidden = false;
  }
}

comparison.addEventListener('input', () => updateSplit(comparison.value));
comparison.addEventListener('pointerdown', event => {
  if (comparison.disabled || !event.isPrimary || event.button !== 0) return;
  event.preventDefault();
  comparison.focus({ preventScroll: true });
  comparison.setPointerCapture(event.pointerId);
  artwork.dataset.dragging = 'true';
  updateFromPointer(event);
});
comparison.addEventListener('pointermove', event => {
  if (comparison.hasPointerCapture(event.pointerId)) updateFromPointer(event);
});
for (const eventName of ['pointerup', 'pointercancel', 'lostpointercapture']) {
  comparison.addEventListener(eventName, event => {
    delete artwork.dataset.dragging;
    if (comparison.hasPointerCapture(event.pointerId)) comparison.releasePointerCapture(event.pointerId);
  });
}

function updateFromPointer(event) {
  const bounds = artwork.getBoundingClientRect();
  updateSplit(Math.round(((event.clientX - bounds.left) / bounds.width) * 100));
}

zoomButton.addEventListener('click', () => setZoom(!zoomed));
retryButton.addEventListener('click', () => loadImages(true));
// 系统/浏览器切主题：<picture> 会自己换图，这里重新等待解码并同步 alt
darkScheme.addEventListener('change', () => {
  applyScheme();
  loadImages();
});
if (new URLSearchParams(location.search).get('state') === 'zoom') setZoom(true);
applyScheme();
loadImages();

const media = gsap.matchMedia();
media.add('(prefers-reduced-motion: no-preference)', () => {
  gsap.from('.hero-heading', { y: 14, opacity: 0, duration: .5, ease: 'power3.out' });
  gsap.from('.artwork-figure', { y: 20, opacity: 0, duration: .6, delay: .06, ease: 'power3.out' });
  gsap.from('.feature-section h2, .feature-section > p', {
    y: 18, stagger: .06, duration: .5, ease: 'power3.out',
    scrollTrigger: { trigger: '.feature-section', start: 'top 85%', once: true },
  });
  gsap.from('.feature-list > li', {
    y: 14, stagger: .05, duration: .5, ease: 'power3.out',
    scrollTrigger: { trigger: '.feature-list', start: 'top 88%', once: true },
  });
  gsap.from('.get-section h2, .get-section p', {
    y: 18, stagger: .06, duration: .5, ease: 'power3.out',
    scrollTrigger: { trigger: '.get-section', start: 'top 85%', once: true },
  });
  gsap.from('.platform-links a', {
    y: 12, stagger: .06, duration: .5, ease: 'power3.out',
    scrollTrigger: { trigger: '.platform-links', start: 'top 90%', once: true },
  });
});

if (import.meta.hot) import.meta.hot.dispose(() => media.revert());
