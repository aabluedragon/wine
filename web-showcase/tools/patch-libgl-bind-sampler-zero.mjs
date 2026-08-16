#!/usr/bin/env node

// WebGL 1 has no sampler objects. Binding sampler 0 means restoring the
// default texture-unit sampling state, which is already the only state exposed
// by this bridge. Return directly for sampler 0. Any unexpected nonzero sampler
// still follows the original BoxedWine trap so unsupported real sampler-object
// use remains a loud failure instead of being silently misrendered.

import { createHash } from "node:crypto";
import { readFile, writeFile } from "node:fs/promises";

const [path] = process.argv.slice(2);
if (!path) {
  console.error("usage: patch-libgl-bind-sampler-zero.mjs <libGL.so.1>");
  process.exit(2);
}

const acceptedSha256 = new Set([
  // Canonical GLX shim.
  "f09030c9fc8cbfa71e04985629217159aa61332363d014e12d39d993ea850c2f",
  // Canonical shim after glXCreateContextAttribsARB is routed through the
  // legacy GLX context path required by the exact NetDuke32 executable.
  "e922274208b4216f4db1cb40c6618cbf50144d4971e6d81a0c4f9c3eb2606907",
]);
const patchedSha256 = new Set([
  // Canonical GLX shim plus this glBindSampler patch.
  "ddf41970ae1b347a8fb9b034e9c79cff311e207d4523a25f81cb9113b3391a61",
  // Legacy-context GLX shim plus this glBindSampler patch.
  "89c09000b4cf9675c6e122acd5c6976e5c00c4aae6bd637e9f155cb33cafe736",
]);
const functionOffset = 0x23c1b;
const original = Buffer.from(
  "5589e5e8998a010005d1730300ff750cff75086899010000cd9983c40c905dc3",
  "hex",
);
const replacement = Buffer.from(
  "837c240800" +   // cmp dword ptr [esp+8], 0 (sampler)
  "7501" +         // nonzero: continue into the original trap body
  "c3" +           // sampler 0: return (default sampler remains active)
  "5589e5" +       // push ebp; mov ebp,esp
  "ff750c" +       // sampler
  "ff7508" +       // unit
  "6899010000" +   // BoxedWine glBindSampler selector
  "cd99" +         // int 0x99
  "83c40c" +       // pop selector + two arguments
  "5d" +           // pop ebp
  "c3" +           // ret
  "909090",
  "hex",
);

if (replacement.length !== original.length) {
  throw new Error(`internal patch length mismatch: ${replacement.length} != ${original.length}`);
}

const image = await readFile(path);
const inputSha256 = createHash("sha256").update(image).digest("hex");
if (patchedSha256.has(inputSha256)) {
  console.log(`${path}: sampler-0 patch already applied`);
  process.exit(0);
}
if (!acceptedSha256.has(inputSha256)) {
  throw new Error(`unexpected input SHA-256 ${inputSha256}`);
}
if (!image.subarray(functionOffset, functionOffset + original.length).equals(original)) {
  throw new Error(`unexpected glBindSampler body at file offset 0x${functionOffset.toString(16)}`);
}

replacement.copy(image, functionOffset);
const outputSha256 = createHash("sha256").update(image).digest("hex");
if (!patchedSha256.has(outputSha256)) {
  throw new Error(`unexpected patched output SHA-256 ${outputSha256}`);
}
await writeFile(path, image);

console.log(`${path}: glBindSampler returns directly for sampler 0`);
console.log(`SHA-256 ${outputSha256}`);
