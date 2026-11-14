// Pure-JS + Web Streams (`DecompressionStream('deflate-raw')`) ZIP reader & writer.
// Parses from the Central Directory (EOCD) so archives with streaming data
// descriptors (flag bit 3) extract cleanly, and writes standard UTF-8 Stored
// ZIP archives for instant zero-recompression batch image export.

import { t } from '../i18n/index.js';

const CRC32_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let i = 0; i < 256; i += 1) {
    let c = i;
    for (let k = 0; k < 8; k += 1) {
      c = (c & 1) !== 0 ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
    }
    table[i] = c >>> 0;
  }
  return table;
})();

export function crc32(bytes) {
  let crc = 0xffffffff;
  for (let i = 0; i < bytes.length; i += 1) {
    crc = CRC32_TABLE[(crc ^ bytes[i]) & 0xff] ^ (crc >>> 8);
  }
  return (crc ^ 0xffffffff) >>> 0;
}

async function inflateRaw(compressedBytes, expectedSize) {
  if (typeof DecompressionStream === 'undefined') {
    throw new Error(t('runtime.zipDecompressionUnsupported'));
  }
  const ds = new DecompressionStream('deflate-raw');
  const writer = ds.writable.getWriter();
  writer.write(compressedBytes).catch(() => {});
  writer.close().catch(() => {});

  const reader = ds.readable.getReader();
  const chunks = [];
  let total = 0;
  while (true) {
    const { value, done } = await reader.read();
    if (done) break;
    chunks.push(value);
    total += value.byteLength;
  }
  const out = new Uint8Array(expectedSize > 0 && expectedSize === total ? expectedSize : total);
  let offset = 0;
  for (const ch of chunks) {
    out.set(ch, offset);
    offset += ch.byteLength;
  }
  return out;
}

/**
 * Extract files from a `.zip` buffer into a `Map<filename, Uint8Array>`.
 * @param {Uint8Array | ArrayBuffer} input
 * @returns {Promise<Map<string, Uint8Array>>}
 */
export async function extractZipEntries(input) {
  const bytes = input instanceof Uint8Array ? input : new Uint8Array(input);
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);

  // Locate End of Central Directory (signature 0x06054b50) within last 65KB.
  let eocdOffset = -1;
  const minSearch = Math.max(0, bytes.byteLength - 65557);
  for (let pos = bytes.byteLength - 22; pos >= minSearch; pos -= 1) {
    if (view.getUint32(pos, true) === 0x06054b50) {
      eocdOffset = pos;
      break;
    }
  }
  if (eocdOffset < 0) {
    throw new Error(t('runtime.zipInvalidEocd'));
  }

  const totalEntries = view.getUint16(eocdOffset + 10, true);
  const cdOffset = view.getUint32(eocdOffset + 16, true);
  const decoder = new TextDecoder('utf-8');
  const result = new Map();

  let ptr = cdOffset;
  for (let i = 0; i < totalEntries; i += 1) {
    if (ptr + 46 > bytes.byteLength || view.getUint32(ptr, true) !== 0x02014b50) {
      throw new Error(t('runtime.zipCorruptCentralDir'));
    }
    const method = view.getUint16(ptr + 10, true);
    const compSize = view.getUint32(ptr + 20, true);
    const uncompSize = view.getUint32(ptr + 24, true);
    const nameLen = view.getUint16(ptr + 28, true);
    const extraLen = view.getUint16(ptr + 30, true);
    const commentLen = view.getUint16(ptr + 32, true);
    const localHeaderOffset = view.getUint32(ptr + 42, true);

    const rawName = decoder.decode(bytes.subarray(ptr + 46, ptr + 46 + nameLen));
    ptr += 46 + nameLen + extraLen + commentLen;

    if (rawName.endsWith('/')) {
      continue; // directory entry
    }

    if (
      localHeaderOffset + 30 > bytes.byteLength ||
      view.getUint32(localHeaderOffset, true) !== 0x04034b50
    ) {
      throw new Error(t('runtime.zipCorruptLocalHeader', { name: rawName }));
    }
    const localNameLen = view.getUint16(localHeaderOffset + 26, true);
    const localExtraLen = view.getUint16(localHeaderOffset + 28, true);
    const dataStart = localHeaderOffset + 30 + localNameLen + localExtraLen;
    const payload = bytes.subarray(dataStart, dataStart + compSize);

    let fileBytes;
    if (method === 0) {
      fileBytes = payload.slice();
    } else if (method === 8) {
      fileBytes = await inflateRaw(payload, uncompSize);
    } else {
      throw new Error(t('runtime.zipUnsupportedMethod', { method, name: rawName }));
    }

    const baseName = rawName.split('/').pop() || rawName;
    result.set(baseName, fileBytes);
  }

  return result;
}

function dosDateTime(date = new Date()) {
  const year = Math.max(1980, date.getFullYear());
  const dosDate = ((year - 1980) << 9) | ((date.getMonth() + 1) << 5) | date.getDate();
  const dosTime = (date.getHours() << 11) | (date.getMinutes() << 5) | (date.getSeconds() >> 1);
  return { dosDate, dosTime };
}

/**
 * Build a standard `.zip` `Blob` from `{ name, bytes }` entries (Stored method 0).
 * @param {Array<{ name: string, bytes: Uint8Array | ArrayBuffer | Blob }>} entries
 * @returns {Promise<Blob>}
 */
export async function createZipBlob(entries) {
  const encoder = new TextEncoder();
  const { dosDate, dosTime } = dosDateTime();
  const localParts = [];
  const centralParts = [];
  const usedNames = new Set();

  let offset = 0;
  for (const item of entries) {
    const payload = item.bytes ?? item.blob ?? item.data;
    let rawBytes;
    if (payload instanceof Uint8Array) {
      rawBytes = payload;
    } else if (payload instanceof ArrayBuffer) {
      rawBytes = new Uint8Array(payload);
    } else if (payload && typeof payload.arrayBuffer === 'function') {
      rawBytes = new Uint8Array(await payload.arrayBuffer());
    } else {
      throw new Error(t('runtime.zipCorruptEntry', { name: item.name }));
    }

    // Deduplicate colliding archive filenames gracefully.
    let safeName = (item.name || 'image.png').replace(/^[/\\]+/, '');
    if (usedNames.has(safeName)) {
      const dot = safeName.lastIndexOf('.');
      const stem = dot > 0 ? safeName.slice(0, dot) : safeName;
      const ext = dot > 0 ? safeName.slice(dot) : '';
      let n = 2;
      while (usedNames.has(`${stem}_${n}${ext}`)) n += 1;
      safeName = `${stem}_${n}${ext}`;
    }
    usedNames.add(safeName);

    const nameBytes = encoder.encode(safeName);
    const checksum = crc32(rawBytes);
    const size = rawBytes.byteLength;

    // Local file header (30 bytes + name)
    const lfh = new Uint8Array(30 + nameBytes.byteLength);
    const lv = new DataView(lfh.buffer);
    lv.setUint32(0, 0x04034b50, true);
    lv.setUint16(4, 20, true); // version needed 2.0
    lv.setUint16(6, 0x0800, true); // UTF-8 filename flag
    lv.setUint16(8, 0, true); // method 0 = Store
    lv.setUint16(10, dosTime, true);
    lv.setUint16(12, dosDate, true);
    lv.setUint32(14, checksum, true);
    lv.setUint32(18, size, true);
    lv.setUint32(22, size, true);
    lv.setUint16(26, nameBytes.byteLength, true);
    lv.setUint16(28, 0, true);
    lfh.set(nameBytes, 30);

    // Central directory header (46 bytes + name)
    const cdh = new Uint8Array(46 + nameBytes.byteLength);
    const cv = new DataView(cdh.buffer);
    cv.setUint32(0, 0x02014b50, true);
    cv.setUint16(4, 20, true); // version made by
    cv.setUint16(6, 20, true); // version needed
    cv.setUint16(8, 0x0800, true); // UTF-8 filename flag
    cv.setUint16(10, 0, true); // method 0 = Store
    cv.setUint16(12, dosTime, true);
    cv.setUint16(14, dosDate, true);
    cv.setUint32(16, checksum, true);
    cv.setUint32(20, size, true);
    cv.setUint32(24, size, true);
    cv.setUint16(28, nameBytes.byteLength, true);
    cv.setUint16(30, 0, true);
    cv.setUint16(32, 0, true);
    cv.setUint16(34, 0, true);
    cv.setUint16(36, 0, true);
    cv.setUint32(38, 0, true);
    cv.setUint32(42, offset, true);
    cdh.set(nameBytes, 46);

    localParts.push(lfh, rawBytes);
    centralParts.push(cdh);
    offset += lfh.byteLength + size;
  }

  let cdSize = 0;
  for (const ch of centralParts) cdSize += ch.byteLength;

  const eocd = new Uint8Array(22);
  const ev = new DataView(eocd.buffer);
  ev.setUint32(0, 0x06054b50, true);
  ev.setUint16(4, 0, true);
  ev.setUint16(6, 0, true);
  ev.setUint16(8, centralParts.length, true);
  ev.setUint16(10, centralParts.length, true);
  ev.setUint32(12, cdSize, true);
  ev.setUint32(16, offset, true);
  ev.setUint16(20, 0, true);

  return new Blob([...localParts, ...centralParts, eocd], { type: 'application/zip' });
}
