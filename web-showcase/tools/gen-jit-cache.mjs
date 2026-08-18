// Generate the prebuilt WASM-JIT cache for the NetDuke32 web showcase, headless.
//
// The showcase runs netduke32.exe under BoxedWine's x86->WASM JIT. Cold, the JIT
// compiles the game's code at runtime, which floods the browser main thread and
// causes a multi-minute boot freeze plus "runs 1s then hangs" on new gameplay
// code. A prebuilt cache makes those blocks LOAD instead of COMPILE (the shell
// auto-fetches "<app>-jit-modules.zip" next to the app zip), eliminating the
// freeze and the hangs.
//
// We generate it here instead of in the browser because a headless Node run has
// no rendering to contend with, so it boots the game fast and compiles cleanly.
// It drives boxedwine.js (the `jit` target) directly, records the compiled
// blocks (record/persistence mode -> Module.wasmJitCache), and serializes them
// to the cache zip the browser shell consumes.
//
// Requirements:
//   * The `jit` boxedwine build must export FS (project/emscripten/makefile jit
//     target: EXPORTED_RUNTIME_METHODS must include FS). Force a relink after a
//     flag change: `rm Build/Jit/boxedwine.html boxedwine.js && make jit`.
//   * The showcase build/ must contain tinycore-wine11.zip and netduke32.zip.
//
// Usage:
//   BOXEDWINE_BUILD=/path/to/Build/Jit node tools/gen-jit-cache.mjs [build_dir] [record_seconds]
//   (defaults: build dir = ../build, record = 45s)
//
// Output: <build_dir>/netduke32-jit-modules.zip

import { readFileSync, writeFileSync, existsSync } from 'node:fs';
import { deflateRawSync } from 'node:zlib';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';
import { installShims } from './node-browser-shims.mjs';

const require = createRequire(import.meta.url);
const here = path.dirname(fileURLToPath(import.meta.url));

const BUILD_DIR = process.argv[2] || path.resolve(here, '..', 'build');
const RECORD_MS = (parseInt(process.argv[3] || '45', 10)) * 1000;
const BOXEDWINE_BUILD = process.env.BOXEDWINE_BUILD ||
  '/Users/alonamir/dev/boxedwine/project/emscripten/Build/Jit';
// The root/app packages and the DLL overrides have to match the browser run
// exactly: a block cache recorded against a different Wine root or a different
// renderer path is keyed on addresses that will not be hit, so it silently
// degrades to compiling everything again.
const ROOT_ZIP = process.env.ROOT_ZIP || 'tinycore-wine11.zip';
const APP_ZIP = process.env.APP_ZIP || 'netduke32.zip';
const DLL_OVERRIDES = process.env.DLL_OVERRIDES || 'mscoree,mshtml=';
const OUT = path.join(BUILD_DIR, process.env.OUT_NAME || 'netduke32-jit-modules.zip');

const boxedwineJs = path.join(BOXEDWINE_BUILD, 'boxedwine.js');
if (!existsSync(boxedwineJs)) { console.error('boxedwine.js not found at ' + boxedwineJs + ' (set BOXEDWINE_BUILD)'); process.exit(1); }
for (const z of [ROOT_ZIP, APP_ZIP])
  if (!existsSync(path.join(BUILD_DIR, z))) { console.error('missing ' + z + ' in ' + BUILD_DIR); process.exit(1); }

installShims();

const args = ['-root', '/root', '-zip', ROOT_ZIP, '-mount', APP_ZIP,
  '/home/username/.wine/dosdevices/c:/files', '-mount_drive', '/d_drive', 'd',
  '-resolution', '640x480', '-novideo', '-disableHideCursor',
  '-env', 'WINEDLLOVERRIDES=' + DLL_OVERRIDES,
  '-w', '/home/username/.wine/dosdevices/c:/files/netduke32',
  // RUNBAT=1 mirrors the browser's launch: cmd -> run.bat -> Wine virtual desktop
  // (explorer), so the record captures the ~4000 desktop/cmd/explorer blocks the
  // direct `/bin/wine netduke32.exe` launch never touches.
  ...(process.env.RUNBAT
    ? ['/bin/wine', 'cmd', '/c', 'run.bat', '-nosetup', '-g', 'DUKE3D.GRP', '-v1', '-l1', '-s3']
    : ['/bin/wine', 'netduke32.exe', '-nosetup', '-g', 'DUKE3D.GRP', '-v1', '-l1', '-s3'])];

const t0 = Date.now();
function pre() {
  for (const z of [ROOT_ZIP, APP_ZIP]) {
    const b = readFileSync(path.join(BUILD_DIR, z));
    cfg.FS.writeFile('/' + z, new Uint8Array(b.buffer, b.byteOffset, b.byteLength));
  }
  process.stderr.write('[gen] zips loaded\n');
}
function onRuntimeInitialized() {
  // Mirror the shell's initWasmJitCache() map setup, then activate record mode
  // so the JIT emits relocatable (exportable) blocks into Module.wasmJitCache.
  cfg.wasmJitCache = new Map();
  cfg.wasmJitCompiledCache = new Map();
  cfg.wasmJitGroupModules = new Map();
  cfg.wasmJitGroupInstances = new Map();
  cfg.wasmJitGroupEntryMap = new Map();
  cfg.wasmJitInstalledCache = new Map();
  cfg.wasmJitInstalledByTableIndex = new Map();
  cfg.wasmJitBlockMeta = new Map();
  cfg.wasmJitGroupedManifest = null;
  cfg.wasmJitPersistenceWanted = true;
  cfg._wasm_jit_set_persistence_active();
  cfg._wasm_jit_set_record_active();
  process.stderr.write('[gen] record mode active\n');
}
const cfg = {
  arguments: args, locateFile: (p) => path.join(BOXEDWINE_BUILD, p),
  preRun: [pre], onRuntimeInitialized,
  print: () => {}, printErr: (t) => { if (!/deprecat|No audio/i.test(t)) process.stderr.write(t + '\n'); },
};
process.argv = [process.argv[0], process.argv[1], ...args];

// Load boxedwine.js so it actually uses our Module: a plain require() fails
// because `var Module = typeof Module...` hoists and shadows globalThis.Module,
// so wrap the source in a Function with Module (and node globals) as parameters.
const src = readFileSync(boxedwineJs, 'utf8');
const fakeModule = { exports: cfg };
new Function('Module', 'require', '__dirname', '__filename', 'module', 'exports', 'process', 'global', 'globalThis', src)
  (cfg, require, BOXEDWINE_BUILD, boxedwineJs, fakeModule, fakeModule.exports, process, globalThis, globalThis);

// ---- flat v6-*.wasm cache zip writer (matches the shell's saveJitModules) ----
function crc32(d) { let c = 0xFFFFFFFF; for (let i = 0; i < d.length; i++) { c ^= d[i]; for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xEDB88320 & -(c & 1)); } return (c ^ 0xFFFFFFFF) >>> 0; }
function buildZip(entries) {
  const parts = [], cd = []; let off = 0; const enc = new TextEncoder();
  for (const e of entries) {
    const nb = enc.encode(e.name);
    const comp = new Uint8Array(deflateRawSync(Buffer.from(e.data.buffer, e.data.byteOffset, e.data.byteLength), { level: 6 }));
    const crc = crc32(e.data);
    const lh = new DataView(new ArrayBuffer(30 + nb.length));
    lh.setUint32(0, 0x04034B50, true); lh.setUint16(4, 20, true); lh.setUint16(8, 8, true); lh.setUint32(14, crc, true);
    lh.setUint32(18, comp.length, true); lh.setUint32(22, e.data.length, true); lh.setUint16(26, nb.length, true);
    new Uint8Array(lh.buffer, 30).set(nb);
    const c = new DataView(new ArrayBuffer(46 + nb.length));
    c.setUint32(0, 0x02014B50, true); c.setUint16(4, 20, true); c.setUint16(6, 20, true); c.setUint16(10, 8, true);
    c.setUint32(16, crc, true); c.setUint32(20, comp.length, true); c.setUint32(24, e.data.length, true);
    c.setUint16(28, nb.length, true); c.setUint32(42, off, true); new Uint8Array(c.buffer, 46).set(nb);
    parts.push(new Uint8Array(lh.buffer)); parts.push(comp); cd.push(new Uint8Array(c.buffer)); off += 30 + nb.length + comp.length;
  }
  const cdOff = off; let cdSize = 0; for (const c of cd) { parts.push(c); cdSize += c.length; }
  const eo = new DataView(new ArrayBuffer(22));
  eo.setUint32(0, 0x06054B50, true); eo.setUint16(8, entries.length, true); eo.setUint16(10, entries.length, true);
  eo.setUint32(12, cdSize, true); eo.setUint32(16, cdOff, true); parts.push(new Uint8Array(eo.buffer));
  let total = 0; for (const p of parts) total += p.length; const out = new Uint8Array(total); let o = 0; for (const p of parts) { out.set(p, o); o += p.length; } return out;
}

setInterval(() => process.stderr.write('[gen] ' + ((Date.now() - t0) / 1000 | 0) + 's blocks=' + (cfg.wasmJitCache ? cfg.wasmJitCache.size : 0) + '\n'), 15000);
setTimeout(() => {
  const KEY = /^v6-[0-9a-f]{8}-[0-9a-f]{8}$/i;
  const entries = [];
  const exportedKeys = new Set();
  if (cfg.wasmJitCache) for (const [k, v] of cfg.wasmJitCache) if (KEY.test(String(k))) { entries.push({ name: String(k) + '.wasm', data: v }); exportedKeys.add(String(k)); }
  if (!entries.length) { console.error('[gen] no blocks captured'); process.exit(1); }
  // Emit boxedwine-jit-manifest.json (block exit metadata + runtime constants) so
  // the output can be fed to boxedwine-wasm-jit-cache-pipeline.mjs for grouping /
  // direct-call rewriting. Mirrors the browser shell's saveJitModules() manifest.
  if (cfg.wasmJitBlockMeta && cfg.wasmJitBlockMeta.size > 0) {
    const manifestEntries = Array.from(cfg.wasmJitBlockMeta.values())
      .filter((e) => e && exportedKeys.has(e.key))
      .sort((a, b) => (a.key < b.key ? -1 : (a.key > b.key ? 1 : 0)));
    const manifest = { version: 1, cacheVersion: 'v6', generatedAt: new Date().toISOString(),
      entryCount: manifestEntries.length, runtime: cfg.wasmJitRuntimeConstants || null, entries: manifestEntries };
    entries.push({ name: 'boxedwine-jit-manifest.json', data: new TextEncoder().encode(JSON.stringify(manifest, null, 2)) });
    console.error('[gen] manifest: ' + manifestEntries.length + ' entries, runtime=' + (cfg.wasmJitRuntimeConstants ? 'yes' : 'NULL'));
  } else {
    console.error('[gen] WARNING: wasmJitBlockMeta empty — output will NOT be groupable');
  }
  // Interior-transition profile sidecar (profile-guided split hints), if present.
  if (cfg.wasmJitInteriorProfileLines && cfg.wasmJitInteriorProfileLines.length > 0) {
    entries.push({ name: 'boxedwine-jit-profile.txt', data: new TextEncoder().encode(cfg.wasmJitInteriorProfileLines.join('\n') + '\n') });
  }
  const zip = buildZip(entries);
  writeFileSync(OUT, Buffer.from(zip));
  console.log('[gen] wrote ' + OUT + ' (' + zip.length + ' bytes, ' + entries.length + ' entries incl. manifest)');
  process.exit(0);
}, RECORD_MS);
