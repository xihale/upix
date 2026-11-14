// Generates the "示例图" on the fly: a 192×128 Geist resolution plate on the
// near-white canvas. Fine 1px hairlines, concentric arcs, diagonal vectors and
// micro-type expose aliasing and ringing immediately when upscaled 2× or 4×.
// Zero asset files, works completely offline.

export async function createSampleImage() {
  const w = 192;
  const h = 128;
  const canvas = document.createElement('canvas');
  canvas.width = w;
  canvas.height = h;
  const ctx = canvas.getContext('2d');

  // Near-white canvas background
  ctx.fillStyle = '#fafafa';
  ctx.fillRect(0, 0, w, h);

  // Outer border & 3-column modular grid hairlines
  ctx.strokeStyle = '#d4d4d4';
  ctx.lineWidth = 1;
  for (const x of [64.5, 128.5]) {
    ctx.beginPath();
    ctx.moveTo(x, 8);
    ctx.lineTo(x, h - 8);
    ctx.stroke();
  }
  for (const y of [44.5, 88.5]) {
    ctx.beginPath();
    ctx.moveTo(8, y);
    ctx.lineTo(w - 8, y);
    ctx.stroke();
  }

  // Vercel blue registration disc — the plate's only chromatic element
  ctx.fillStyle = '#0070f3';
  ctx.beginPath();
  ctx.arc(154, 44, 18, 0, Math.PI * 2);
  ctx.fill();

  // Concentric resolution arcs (high-frequency curve test)
  ctx.strokeStyle = '#171717';
  ctx.lineWidth = 1.2;
  for (const r of [10, 17, 24, 31]) {
    ctx.beginPath();
    ctx.arc(128.5, 88.5, r, Math.PI, Math.PI * 1.5);
    ctx.stroke();
  }

  // Diagonal line-pair resolution wedge
  ctx.strokeStyle = '#171717';
  ctx.lineWidth = 1;
  for (let i = 0; i < 5; i++) {
    ctx.beginPath();
    ctx.moveTo(72 + i * 7, 38);
    ctx.lineTo(90 + i * 7, 12);
    ctx.stroke();
  }

  // Typography specimen
  ctx.fillStyle = '#171717';
  ctx.font = '600 22px "Geist", "Helvetica Neue", Arial, sans-serif';
  ctx.textBaseline = 'alphabetic';
  ctx.fillText('Upix', 12, 34);

  ctx.fillStyle = '#8f8f8f';
  ctx.font = '400 8px "Geist Mono", ui-monospace, Menlo, Consolas, monospace';
  ctx.fillText('GEIST TEST PLATE', 12, 58);
  ctx.fillText('192 × 128 PX · 1:1', 12, 70);

  // Contrast step bar at bottom-left
  const greys = ['#171717', '#4d4d4d', '#8f8f8f', '#a1a1a1', '#d4d4d4'];
  greys.forEach((c, i) => {
    ctx.fillStyle = c;
    ctx.fillRect(12 + i * 18, 98, 16, 14);
  });

  const blob = await new Promise(resolve => canvas.toBlob(resolve, 'image/png'));
  return new File([blob], 'geist-plate-192x128.png', { type: 'image/png' });
}
