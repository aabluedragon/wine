#!/usr/bin/env node

// Make Wine's X11 driver use its GLX 1.0 make-current fallback. BoxedWine's
// browser bridge supports one drawable for draw/read, so the GLX 1.3 symbol
// is unnecessary and the native driver already has the compatible fallback.
import { readFile, writeFile } from 'node:fs/promises';

const path = process.argv[2];
if (!path) throw new Error('usage: disable-glx-make-context-current.mjs <winex11.drv.so>');
const data = await readFile(path);
const name = Buffer.from('glXMakeContextCurrent\0');
const replacement = Buffer.from('xlXMakeContextCurrent\0');
const offset = data.indexOf(name);
if (offset < 0) {
  if (data.includes(replacement)) process.exit(0);
  throw new Error(`GLX resolver name not found in ${path}`);
}
replacement.copy(data, offset);
await writeFile(path, data);
