#!/usr/bin/env node

// The WebGL 1 bridge has smooth varying interpolation and the exact
// NetDuke32 startup requests GL_SMOOTH plus GL_FOG_MODE/GL_LINEAR. Neither
// legacy fixed-function selector exists in WebGL's resolver. Treat only those
// already-effective defaults as guest-side no-ops; every other value keeps the
// original BoxedWine trap and therefore fails loudly instead of being silently
// misrendered.

import { createHash } from "node:crypto";
import { readFile, writeFile } from "node:fs/promises";

const [path] = process.argv.slice(2);
if (!path) {
  console.error("usage: patch-libgl-webgl1-fixed-defaults.mjs <libGL.so.1>");
  process.exit(2);
}

const acceptedSha256 = new Set([
  "f09030c9fc8cbfa71e04985629217159aa61332363d014e12d39d993ea850c2f",
  "e922274208b4216f4db1cb40c6618cbf50144d4971e6d81a0c4f9c3eb2606907",
]);
const patchedSha256 = new Set([
  "4d0fb67a6e073591d5043aa714482540af07b7726475652089f254b3a8668cb1",
  "e5e7b91961fa76d77e90b4b244ab58491cbe3d67db63d805cd4d74b9403e8045",
]);

const patches = [
  {
    name: "glShadeModel(GL_SMOOTH)",
    offset: 0x2213b,
    original: Buffer.from(
      "5589e5e879a5010005b18e0300ff750868d8000000cd9983c408905dc3",
      "hex",
    ),
    replacement: Buffer.from(
      "817c2404011d0000" + // cmp dword ptr [esp+4], GL_SMOOTH
      "7501" +             // anything else: original BoxedWine trap
      "c3" +               // GL_SMOOTH: WebGL already interpolates smoothly
      "5589e5" +           // push ebp; mov ebp,esp
      "ff7508" +           // mode
      "68d8000000" +       // BoxedWine glShadeModel selector
      "cd99" +             // int 0x99
      "83c408" +           // pop selector + argument
      "5d" +               // pop ebp
      "c3",
      "hex",
    ),
  },
  {
    name: "glFogi(GL_FOG_MODE, GL_LINEAR)",
    offset: 0x22e4f,
    original: Buffer.from(
      "5589e5e865980100059d810300ff750cff7508682f010000cd9983c40c905dc3",
      "hex",
    ),
    replacement: Buffer.from(
      "817c240801260000" + // cmp dword ptr [esp+8], GL_LINEAR
      "7501" +             // any other mode: original BoxedWine trap
      "c3" +               // GL_LINEAR: shader path already selects linear fog
      "5589e5" +           // push ebp; mov ebp,esp
      "ff750c" +           // param
      "ff7508" +           // pname
      "682f010000" +       // BoxedWine glFogi selector
      "cd99" +             // int 0x99
      "83c40c" +           // pop selector + arguments
      "5d" +               // pop ebp
      "c3",
      "hex",
    ),
  },
];

for (const patch of patches) {
  if (patch.original.length !== patch.replacement.length) {
    throw new Error(`${patch.name}: internal patch length mismatch`);
  }
}

const image = await readFile(path);
const inputSha256 = createHash("sha256").update(image).digest("hex");
if (patchedSha256.has(inputSha256)) {
  console.log(`${path}: WebGL 1 fixed-default guards already applied`);
  process.exit(0);
}
if (!acceptedSha256.has(inputSha256)) {
  throw new Error(`unexpected input SHA-256 ${inputSha256}`);
}

for (const patch of patches) {
  if (!image.subarray(patch.offset, patch.offset + patch.original.length).equals(patch.original)) {
    throw new Error(`unexpected ${patch.name} body at file offset 0x${patch.offset.toString(16)}`);
  }
  patch.replacement.copy(image, patch.offset);
}

const outputSha256 = createHash("sha256").update(image).digest("hex");
if (!patchedSha256.has(outputSha256)) {
  throw new Error(`unexpected patched output SHA-256 ${outputSha256}`);
}
await writeFile(path, image);

console.log(`${path}: installed WebGL 1 fixed-default guards`);
console.log(`SHA-256 ${outputSha256}`);
