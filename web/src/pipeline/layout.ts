/**
 * Tile layout — byte-for-byte port of upix::TileProcessor::compute_tile_layout
 * (engine/core/tile_processor.cpp). Tile starts step over the input with
 * step = max(1, tileSize - overlap); edge tiles SHRINK to image bounds (they
 * are never padded), which is what guarantees full weight coverage downstream.
 */

export interface TileConfig {
  tileSize: number;
  overlap: number;
}

export interface TileRegion {
  x: number;
  y: number;
  width: number;
  height: number;
  /** echo of config overlap, mirrors TileRegion::overlap in engine */
  overlap: number;
}

export function computeTileLayout(
  imageWidth: number,
  imageHeight: number,
  config: TileConfig,
): TileRegion[] {
  const tileSize = Math.max(1, Math.trunc(config.tileSize));
  const overlap = Math.min(Math.max(Math.trunc(config.overlap), 0), tileSize - 1);
  const step = Math.max(1, tileSize - overlap);

  const startsX: number[] = [];
  const startsY: number[] = [];
  for (let x = 0; x < imageWidth; x += step) {
    startsX.push(x);
    if (x + tileSize >= imageWidth) break;
  }
  for (let y = 0; y < imageHeight; y += step) {
    startsY.push(y);
    if (y + tileSize >= imageHeight) break;
  }
  if (startsX.length === 0) startsX.push(0);
  if (startsY.length === 0) startsY.push(0);

  const regions: TileRegion[] = [];
  for (const y of startsY) {
    for (const x of startsX) {
      regions.push({
        x,
        y,
        width: Math.min(tileSize, imageWidth - x),
        height: Math.min(tileSize, imageHeight - y),
        overlap,
      });
    }
  }
  return regions;
}
