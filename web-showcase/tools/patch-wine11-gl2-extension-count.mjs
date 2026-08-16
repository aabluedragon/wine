#!/usr/bin/env node

// Wine 11.0 synthesizes GL_NUM_EXTENSIONS from its filtered extension array
// before calling the Unix OpenGL driver.  On a legacy GL 2 context it also
// rejects glGetStringi, leaving loaders with a positive count and a NULL
// indexed-string function.  Let GL_NUM_EXTENSIONS fall through to the driver;
// BoxedWine's WebGL 1 / GL 2 bridge then returns the spec-correct value zero.
//
// This patch is deliberately pinned to the stripped Wine 11.0 opengl32.so in
// the showcase's canonical glxshim root.  It changes the JE that selects
// Wine's synthetic count to NOPs.  The following JBE dispatches through the
// normal driver path for enum 0x821d.

import { createHash } from 'node:crypto';
import { readFile, writeFile } from 'node:fs/promises';

const path = process.argv[2];
if (!path) {
  throw new Error('usage: patch-wine11-gl2-extension-count.mjs <opengl32.so>');
}

const inputSha256 =
  '60bb64157afc62067ee2298fac0a96943deaf63783efd30c91b8ad8c305eaea0';
const outputSha256 =
  'a3dceaa1f93019a76da43c9b5019c9847b25abc385af2e20c6da47821649287b';
const offset = 0x6aac0;
const before = Buffer.from('7465', 'hex');
const after = Buffer.from('9090', 'hex');
const context = Buffer.from('81fa1d8200007465762c81faa68c', 'hex');

const sha256 = (data) => createHash('sha256').update(data).digest('hex');
const data = await readFile(path);
const currentHash = sha256(data);

if (currentHash === outputSha256) process.exit(0);
if (currentHash !== inputSha256) {
  throw new Error(`unexpected Wine opengl32.so SHA-256: ${currentHash}`);
}
if (!data.subarray(offset - 6, offset + 8).equals(context)) {
  throw new Error(`unexpected get_integer code at file offset 0x${offset.toString(16)}`);
}
if (!data.subarray(offset, offset + before.length).equals(before)) {
  throw new Error(`unexpected branch bytes at file offset 0x${offset.toString(16)}`);
}

after.copy(data, offset);
const patchedHash = sha256(data);
if (patchedHash !== outputSha256) {
  throw new Error(`patched Wine opengl32.so SHA-256 mismatch: ${patchedHash}`);
}
await writeFile(path, data);
