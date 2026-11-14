#!/usr/bin/env node
import { execFileSync } from 'node:child_process';
import { copyFileSync, existsSync, mkdirSync, statSync } from 'node:fs';
import { homedir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(__dirname, '../..');
const wasmDir = resolve(repoRoot, 'wasm');
const zigOutBin = resolve(wasmDir, 'zig-out/bin');
const outDir = resolve(__dirname, '../public/wasm');

function resolveZigBinary() {
  if (process.env.ZIG) {
    return process.env.ZIG;
  }
  try {
    execFileSync('zig', ['version'], { stdio: 'ignore' });
    return 'zig';
  } catch {
    const zvmZig = join(homedir(), '.local/share/zvm/bin/zig');
    if (existsSync(zvmZig)) {
      return zvmZig;
    }
    throw new Error('Zig compiler not found on PATH or at ~/.local/share/zvm/bin/zig');
  }
}

const zigBin = resolveZigBinary();
const simdOnly = process.argv.includes('--simd-only');

mkdirSync(outDir, { recursive: true });

const args = ['build', 'wasm', '-Doptimize=fast'];
if (simdOnly) {
  args.push('-Dsimd=true');
}

console.log(`[upix-wasm] Running: ${zigBin} ${args.join(' ')} (cwd=${wasmDir})`);
execFileSync(zigBin, args, { cwd: wasmDir, stdio: 'inherit' });

const outputs = simdOnly
  ? ['upix_wasm.wasm']
  : ['upix_wasm_scalar.wasm', 'upix_wasm.wasm'];

for (const name of outputs) {
  const src = resolve(zigOutBin, name);
  if (!existsSync(src)) {
    throw new Error(`Expected Zig WASM output not found: ${src}`);
  }
  const dest = resolve(outDir, name);
  copyFileSync(src, dest);
  const kb = (statSync(dest).size / 1024).toFixed(1);
  console.log(`[upix-wasm] Wrote ${dest} (${kb} KB)`);
}
