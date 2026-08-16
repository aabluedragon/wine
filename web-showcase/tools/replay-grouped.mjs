// Headless grouped-cache REPLAY harness (Phase 3 M2b verification).
//
// Loads a GROUPED jit-modules zip (groups/group-*.wasm + grouped manifest) into
// the Module.wasmJitGroup* maps exactly as boxedwine-shell.js's
// importJitModulesFromBuffer does, then boots netduke32 headless under Node and
// runs it. If the register-parameter tail calls (M2b) pass the wrong values, the
// game's control flow diverges and traps within a few blocks; a clean run with
// groupHits>0 / groupInstallFail=0 and climbing MIPS confirms the grouped
// register-passing ABI is correct.
//
// Usage:
//   BOXEDWINE_BUILD=/path/to/Build/Jit node tools/replay-grouped.mjs <build_dir> <grouped.zip> <run_seconds>

import { readFileSync, existsSync } from 'node:fs';
import { inflateRawSync } from 'node:zlib';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';
import { installShims } from './node-browser-shims.mjs';

const require = createRequire(import.meta.url);
const here = path.dirname(fileURLToPath(import.meta.url));
const BUILD_DIR = process.argv[2] || path.resolve(here, '..', 'build');
const GROUPED_ZIP = process.argv[3] || path.join(BUILD_DIR, 'netduke32-jit-modules-grouped.zip');
const RUN_MS = (parseInt(process.argv[4] || '30', 10)) * 1000;
const BOXEDWINE_BUILD = process.env.BOXEDWINE_BUILD ||
  '/Users/alonamir/dev/boxedwine/project/emscripten/Build/Jit';
const boxedwineJs = path.join(BOXEDWINE_BUILD, 'boxedwine.js');
for (const f of [boxedwineJs, GROUPED_ZIP]) if (!existsSync(f)) { console.error('missing ' + f); process.exit(1); }
for (const z of ['tinycore-wine11.zip', 'netduke32.zip'])
  if (!existsSync(path.join(BUILD_DIR, z))) { console.error('missing ' + z + ' in ' + BUILD_DIR); process.exit(1); }

installShims();

const CACHE_KEY_RE = /^v6-([0-9a-f]{8})-([0-9a-f]{8})$/i;
const CACHE_VERSION = 'v6';

// --- minimal zip reader (local file headers only; method 0/8) ---------------
function readZipEntries(bytes) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const dec = new TextDecoder();
  const out = [];
  let pos = 0;
  while (pos + 30 <= bytes.length) {
    if (view.getUint32(pos, true) !== 0x04034B50) break;
    const method = view.getUint16(pos + 8, true);
    const cSize = view.getUint32(pos + 18, true);
    const uSize = view.getUint32(pos + 22, true);
    const fnLen = view.getUint16(pos + 26, true);
    const exLen = view.getUint16(pos + 28, true);
    const dataStart = pos + 30 + fnLen + exLen;
    if ((method === 0 || method === 8) && dataStart + cSize <= bytes.length) {
      const name = dec.decode(bytes.subarray(pos + 30, pos + 30 + fnLen));
      const raw = bytes.subarray(dataStart, dataStart + cSize);
      const data = method === 8 ? new Uint8Array(inflateRawSync(raw)) : new Uint8Array(raw);
      out.push({ name, data });
    }
    pos = dataStart + cSize;
  }
  return out;
}

const args = ['-root', '/root', '-zip', 'tinycore-wine11.zip', '-mount', 'netduke32.zip',
  '/home/username/.wine/dosdevices/c:/files', '-mount_drive', '/d_drive', 'd',
  '-resolution', '640x480', '-novideo', '-disableHideCursor',
  '-env', 'WINEDLLOVERRIDES=mscoree,mshtml=',
  '-w', '/home/username/.wine/dosdevices/c:/files/netduke32',
  ...(process.env.RUNBAT
    ? ['/bin/wine', 'cmd', '/c', 'run.bat', '-nosetup', '-g', 'DUKE3D.GRP', '-v1', '-l1', '-s3']
    : ['/bin/wine', 'netduke32.exe', '-nosetup', '-g', 'DUKE3D.GRP', '-v1', '-l1', '-s3'])];

const t0 = Date.now();
function pre() {
  for (const z of ['tinycore-wine11.zip', 'netduke32.zip']) {
    const b = readFileSync(path.join(BUILD_DIR, z));
    cfg.FS.writeFile('/' + z, new Uint8Array(b.buffer, b.byteOffset, b.byteLength));
  }
  process.stderr.write('[replay] zips loaded\n');
}

async function onRuntimeInitialized() {
  // Mirror the shell's initWasmJitCache() map set.
  cfg.wasmJitCache = new Map();
  cfg.wasmJitCompiledCache = new Map();
  cfg.wasmJitGroupModules = new Map();
  cfg.wasmJitGroupInstances = new Map();
  cfg.wasmJitGroupEntryMap = new Map();
  cfg.wasmJitInstalledCache = new Map();
  cfg.wasmJitInstalledByTableIndex = new Map();
  cfg.wasmJitBlockMeta = new Map();
  cfg.wasmJitGroupRelocArrays = new Map();
  cfg.wasmJitGroupUnpatched = new Map();
  cfg.wasmJitProfileSplitTargets = new Map();
  cfg.wasmJitProfileSplitTargetSources = new Map();
  cfg.wasmJitGroupedManifest = null;

  // Load the grouped zip and populate the maps (importJitModulesFromBuffer,
  // single-threaded path, node-adapted).
  const zbytes = new Uint8Array(readFileSync(GROUPED_ZIP));
  const files = readZipEntries(zbytes);
  let manifest = null;
  const groupBytesByPath = new Map();
  for (const f of files) {
    if (f.name === 'boxedwine-jit-grouped-manifest.json') manifest = JSON.parse(new TextDecoder().decode(f.data));
    else if (f.name.indexOf('groups/') === 0 && f.name.toLowerCase().endsWith('.wasm')) groupBytesByPath.set(f.name, f.data);
    else if (CACHE_KEY_RE.test(f.name.replace(/\.wasm$/i, ''))) cfg.wasmJitCache.set(f.name.replace(/\.wasm$/i, ''), f.data);
  }
  if (!manifest || manifest.format !== 'boxedwine-wasm-jit-grouped-cache') { console.error('[replay] not a grouped cache'); process.exit(1); }
  if (manifest.cacheVersion !== CACHE_VERSION) { console.error('[replay] version mismatch ' + manifest.cacheVersion); process.exit(1); }
  if (!!manifest.mt) { console.error('[replay] MT-piped cache; this is an ST build'); process.exit(1); }

  cfg.wasmJitGroupedManifest = manifest;
  // Stage reloc groups unpatched (compiled per-process on first touch by the
  // C++ lookup path); precompile reloc-free groups shared.
  let relocPending = 0, precompiled = 0;
  for (const group of manifest.groups) {
    const relocEntries = (group.entries || []).filter((e) => (e.relocCount >>> 0) > 0);
    const patches = group.directCallPatches || [];
    const bytes = groupBytesByPath.get(group.path);
    if (!bytes) { console.error('[replay] missing group bytes ' + group.path); continue; }
    if (relocEntries.length || patches.length) {
      cfg.wasmJitGroupUnpatched.set(group.path, { bytes, entries: relocEntries, patches });
      relocPending++;
    } else {
      cfg.wasmJitGroupModules.set(group.path, await WebAssembly.compile(bytes));
      precompiled++;
    }
    for (const entry of (group.entries || [])) {
      cfg.wasmJitGroupEntryMap.set(entry.key, { groupPath: entry.path || group.path, exportName: entry.exportName || entry.key });
    }
  }
  process.stderr.write('[replay] grouped cache staged: groups=' + manifest.groups.length +
    ' entryMap=' + cfg.wasmJitGroupEntryMap.size + ' relocGroups=' + relocPending +
    ' precompiledGroups=' + precompiled + ' flatFallback=' + cfg.wasmJitCache.size + '\n');

  // Activate persistence (relocatable blocks + grouped install path). NOT record.
  cfg._wasm_jit_set_persistence_active();
  process.stderr.write('[replay] persistence active (replay mode)\n');
}

const cfg = {
  arguments: args, locateFile: (p) => path.join(BOXEDWINE_BUILD, p),
  preRun: [pre], onRuntimeInitialized,
  print: (t) => { if (process.env.REPLAY_STDOUT) process.stderr.write(t + '\n'); },
  printErr: (t) => { if (!/deprecat|No audio/i.test(t)) process.stderr.write(t + '\n'); },
};
process.argv = [process.argv[0], process.argv[1], ...args];

const src = readFileSync(boxedwineJs, 'utf8');
const fakeModule = { exports: cfg };
new Function('Module', 'require', '__dirname', '__filename', 'module', 'exports', 'process', 'global', 'globalThis', src)
  (cfg, require, BOXEDWINE_BUILD, boxedwineJs, fakeModule, fakeModule.exports, process, globalThis, globalThis);

setInterval(() => process.stderr.write('[replay] ' + ((Date.now() - t0) / 1000 | 0) + 's alive\n'), 10000);
setTimeout(() => { process.stderr.write('[replay] run window elapsed, exiting\n'); process.exit(0); }, RUN_MS);
