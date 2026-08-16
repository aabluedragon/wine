#!/usr/bin/env node

// The 32-bit BoxedWine guest libGL implements glXMakeContextCurrent with
// selector 0xb62. Wine only supplies identical draw/read drawables here, so
// use the stable GLX 1.0 selector (0x153) and discard the redundant read
// drawable. The function body is identical in the TinyCore libGL variants.
import { readFile, writeFile } from 'node:fs/promises';

const path = process.argv[2];
if (!path) throw new Error('usage: patch-glx-make-current.mjs <libGL.so>');
const data = await readFile(path);
const offset = 0x3c1fa;
const before = Buffer.from('ff7514ff7510ff750cff750868620b0000cd9983c414905dc3', 'hex');
const after = Buffer.from('ff7514ff750cff75086853010000cd9983c4109090905dc3', 'hex');
if (data.subarray(offset, offset + after.length).equals(after)) process.exit(0);
if (data.subarray(offset, offset + before.length).equals(before)) {
  after.copy(data, offset);
} else {
  // Mesa's libGL.so.1.2.0 dispatcher: overwrite its GLX 1.3 entry with a
  // stack-compatible tail jump to glXMakeCurrent(dpy, draw, ctx).
  const mesaOffset = 0x2bb08;
  const mesaBefore = Buffer.from('5589e58b4d148b45088b550cc7450c1a000000894d088b4d105de924feffff', 'hex');
  const mesaAfter = Buffer.from('5589e58b45148945105de9cfffffff90909090909090909090909090909090', 'hex');
  if (data.subarray(mesaOffset, mesaOffset + mesaAfter.length).equals(mesaAfter)) process.exit(0);
  if (!data.subarray(mesaOffset, mesaOffset + mesaBefore.length).equals(mesaBefore)) {
    throw new Error(`unexpected glXMakeContextCurrent implementation in ${path}`);
  }
  mesaAfter.copy(data, mesaOffset);
}
await writeFile(path, data);
