// Drop WGL_CONTEXT_OPENGL_NO_ERROR_ARB from the packaged Wine 11.0 X11 driver.
//
// dlls/winex11.drv/opengl.c translates WGL_CONTEXT_OPENGL_NO_ERROR_ARB (0x31b3)
// straight into GLX_CONTEXT_OPENGL_NO_ERROR_ARB without checking that the GLX
// implementation advertises GLX_ARB_create_context_no_error. BoxedWine's GLX
// shim does not implement it: it reports
//
//   gl_common_XCreateContextAttribsARB unhandled attribute 31b3
//
// and the emulator then traps with "memory access out of bounds" while
// NetDuke32 is bringing up its 8-bpp video mode.
//
// Wine 11.0 is only present here as a prebuilt i386 module, so the equivalent
// of removing that switch case is applied to the binary: the `cmp eax, 0x31b3`
// that selects the case is retargeted to a token no caller can pass, so the
// attribute falls through to the driver's `default:` branch and is simply not
// forwarded to GLX. The dead `mov dword [ebp], 0x31b3` that would have written
// the GLX token is left untouched.
//
//   node web-showcase/tools/patch-wine11-no-error-attrib.mjs <in.zip> <out.zip>

import { execFileSync } from 'node:child_process';
import { copyFileSync, mkdtempSync, readFileSync, writeFileSync, rmSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { resolve } from 'node:path';

const [input, output] = process.argv.slice(2);
if (!input || !output) {
  console.error('usage: patch-wine11-no-error-attrib.mjs <in.zip> <out.zip>');
  process.exit(2);
}

const ENTRY = 'opt/wine/lib/wine/i386-unix/winex11.so';
// `cmp eax, 0x31b3` followed by the `je` that enters the case.
const MATCH = Buffer.from('3db3310000 0f84'.replace(/ /g, ''), 'hex');
// 0x7fffffff is not a WGL token, so the comparison can never succeed.
const PATCH = Buffer.from('3dffffff7f 0f84'.replace(/ /g, ''), 'hex');

const work = mkdtempSync(resolve(tmpdir(), 'no-error-attrib-'));
try {
  execFileSync('unzip', ['-q', resolve(input), ENTRY, '-d', work], { stdio: 'inherit' });
  const module = resolve(work, ENTRY);
  const bytes = readFileSync(module);

  const hits = [];
  for (let i = 0; i + MATCH.length <= bytes.length; i++) {
    if (bytes.compare(MATCH, 0, MATCH.length, i, i + MATCH.length) === 0) hits.push(i);
  }
  if (hits.length !== 1) throw new Error(`expected exactly one switch case, found ${hits.length}: ${hits}`);

  PATCH.copy(bytes, hits[0]);
  writeFileSync(module, bytes);
  console.log(`patched ${ENTRY} at 0x${hits[0].toString(16)}`);

  const out = resolve(output);
  if (existsSync(out)) rmSync(out);
  copyFileSync(resolve(input), out);
  // In-place entry replacement: every other entry keeps its original bytes.
  execFileSync('zip', ['-q', out, ENTRY], { cwd: work, stdio: 'inherit' });
  console.log(`wrote ${execFileSync('shasum', ['-a', '256', out]).toString().trim()}`);
} finally {
  rmSync(work, { recursive: true, force: true });
}
