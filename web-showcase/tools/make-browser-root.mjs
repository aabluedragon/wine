// Build a browser-only derivative of a BoxedWine root ZIP.
//
// Wine's normal bootstrap (wineboot -> services.exe -> winedevice.exe ->
// hidclass/winehid) cannot complete inside the browser guest: winedevice hangs
// while holding the loader lock, services time out, and the application never
// starts. The packed prefix already carries a complete static registry and
// drive layout, so the only thing still required is the per-session readiness
// event each of those processes would have signalled.
//
// This replaces C:\windows\system32\{wineboot,services}.exe with the minimal
// PE32 stubs in tools/, leaving every other byte of the root untouched.
//
// The prefix copy alone is not enough: for a module in the system directory the
// default load order is builtin-first, so Wine would keep picking the builtin
// out of /opt/wine/lib/wine/i386-windows. The builtin PE and its i386-unix half
// are therefore dropped from the root as well, which leaves the native stub as
// the only remaining candidate.
//
//   node web-showcase/tools/make-browser-root.mjs <in.zip> <out.zip>

import { execFileSync } from 'node:child_process';
import { copyFileSync, mkdtempSync, mkdirSync, rmSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const [input, output] = process.argv.slice(2);
if (!input || !output) {
  console.error('usage: make-browser-root.mjs <in.zip> <out.zip>');
  process.exit(2);
}

const CC = process.env.WIN32_CC ?? 'i686-w64-mingw32-gcc';
const PREFIX = 'home/username/.wine/drive_c/windows/system32';
const STUBS = [
  { src: 'wineboot-browser.c', entry: 'wineboot.exe' },
  { src: 'services-browser.c', entry: 'services.exe' },
];

const work = mkdtempSync(resolve(tmpdir(), 'browser-root-'));
try {
  for (const stub of STUBS) {
    const dir = resolve(work, PREFIX);
    mkdirSync(dir, { recursive: true });
    // -nostdlib keeps wineboot.exe free of a CRT whose process-attach work is
    // itself part of what stalls the guest.
    execFileSync(CC, [
      '-Os', '-s', '-Wall', '-Wextra', '-nostdlib', '-Wl,--no-insert-timestamp',
      '-Wl,-e,mainCRTStartup', '-mwindows',
      '-o', resolve(dir, stub.entry), resolve(here, stub.src), '-lntdll', '-lkernel32',
    ], { stdio: 'inherit' });
  }

  const out = resolve(output);
  if (existsSync(out)) rmSync(out);
  copyFileSync(resolve(input), out);
  // `zip` replaces matching entries in place, so every unrelated entry keeps
  // its original bytes, timestamps, and external attributes.
  for (const stub of STUBS) {
    execFileSync('zip', ['-q', out, `${PREFIX}/${stub.entry}`], { cwd: work, stdio: 'inherit' });
  }
  const builtins = STUBS.flatMap((stub) => [
    `opt/wine/lib/wine/i386-windows/${stub.entry}`,
    `opt/wine/lib/wine/i386-unix/${stub.entry}.so`,
  ]);
  execFileSync('zip', ['-q', '-d', out, ...builtins], { stdio: 'inherit' });
  const sha = execFileSync('shasum', ['-a', '256', out]).toString().trim();
  console.log(`wrote ${sha}`);
} finally {
  rmSync(work, { recursive: true, force: true });
}
