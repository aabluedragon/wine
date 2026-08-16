#!/usr/bin/env node

// The exact NetDuke32 v1.2.1 binary asks wglGetProcAddress for glBindSampler
// even on an OpenGL 2.0 context, then unconditionally calls the returned slot
// with sampler 0 while resetting state. Wine 11.0 knows the thunk and the X11
// driver resolves the guest libGL symbol, but opengl32 rejects it unless
// GL_ARB_sampler_objects or GL 3.3 is advertised.
//
// Keep GL_ARB_sampler_objects absent from the context so the application does
// not enable real sampler-object paths. Only relax the thunk registry's lookup
// requirement to the already-advertised GL 2.0 context. The companion guest
// libGL patch handles sampler 0 (the default/unbound sampler state) without
// dispatching an unavailable WebGL 1 sampler-object call.

import { createHash } from "node:crypto";
import { readFile, writeFile } from "node:fs/promises";

const [path] = process.argv.slice(2);
if (!path) {
  console.error("usage: patch-wine11-bind-sampler-lookup.mjs <opengl32.so>");
  process.exit(2);
}

const expectedSha256 =
  "60bb64157afc62067ee2298fac0a96943deaf63783efd30c91b8ad8c305eaea0";
const patchedSha256 =
  "8c51f6ac07fee4b978dd561c8fd3aa153342c7a067eb0cf9e10002dc6cbd059b";
const requirementOffset = 0x7daa4;
const original = Buffer.from("GL_ARB_sampler_objects\0", "ascii");
const replacement = Buffer.alloc(original.length);
Buffer.from("GL_VERSION_2_0\0", "ascii").copy(replacement);

const image = await readFile(path);
const inputSha256 = createHash("sha256").update(image).digest("hex");
if (inputSha256 === patchedSha256) {
  console.log(`${path}: sampler thunk lookup patch already applied`);
  process.exit(0);
}
if (inputSha256 !== expectedSha256) {
  throw new Error(`unexpected input SHA-256 ${inputSha256}; expected ${expectedSha256}`);
}
if (!image.subarray(requirementOffset, requirementOffset + original.length).equals(original)) {
  throw new Error(
    `unexpected glBindSampler registry requirement at file offset 0x${requirementOffset.toString(16)}`,
  );
}

replacement.copy(image, requirementOffset);
const outputSha256 = createHash("sha256").update(image).digest("hex");
if (outputSha256 !== patchedSha256) {
  throw new Error(`patched output SHA-256 ${outputSha256}; expected ${patchedSha256}`);
}
await writeFile(path, image);

console.log(`${path}: sampler thunk lookup allowed for the existing GL 2.0 context`);
console.log(`SHA-256 ${outputSha256}`);
