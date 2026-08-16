#!/usr/bin/env node

import { createHash } from "node:crypto";
import { readFile, writeFile } from "node:fs/promises";

const [path] = process.argv.slice(2);
if (!path) {
  console.error("usage: patch-glx-create-context-attribs-legacy.mjs <libGL.so.1>");
  process.exit(2);
}

const expectedSha256 = "f09030c9fc8cbfa71e04985629217159aa61332363d014e12d39d993ea850c2f";
const functionOffset = 0x3c29f;
const original = Buffer.from(
  "5589e5e815040000054ded0100ff7518ff7514ff7510ff750cff750868660b0000cd9983c418905dc3",
  "hex",
);

// The browser bridge provides one WebGL/OpenGL-ES 2 context and cannot honor
// WGL/GLX flags for a second attributed desktop context. Preserve the FBConfig
// and share context, but route the <=2.1 request through glXCreateNewContext
// with GLX_RGBA_TYPE. This returns another Wine-visible logical context backed
// by the existing browser context.
const replacement = Buffer.from(
  "5589e5" +       // push ebp; mov ebp,esp
  "ff7514" +       // direct
  "ff7510" +       // share context
  "6814800000" +   // GLX_RGBA_TYPE
  "ff750c" +       // GLXFBConfig
  "ff7508" +       // Display *
  "68610b0000" +   // BoxedWine glXCreateNewContext selector
  "cd99" +         // int 0x99
  "83c418" +       // pop selector + five arguments
  "5d" +           // pop ebp
  "c3" +           // ret
  "909090909090909090",
  "hex",
);

if (replacement.length !== original.length) {
  throw new Error(`internal patch length mismatch: ${replacement.length} != ${original.length}`);
}

const image = await readFile(path);
const inputSha256 = createHash("sha256").update(image).digest("hex");
if (inputSha256 !== expectedSha256) {
  throw new Error(`unexpected input SHA-256 ${inputSha256}; expected ${expectedSha256}`);
}
if (!image.subarray(functionOffset, functionOffset + original.length).equals(original)) {
  throw new Error(`unexpected glXCreateContextAttribsARB body at file offset 0x${functionOffset.toString(16)}`);
}

replacement.copy(image, functionOffset);
await writeFile(path, image);

const outputSha256 = createHash("sha256").update(image).digest("hex");
console.log(`${path}: glXCreateContextAttribsARB routes to glXCreateNewContext`);
console.log(`SHA-256 ${outputSha256}`);
