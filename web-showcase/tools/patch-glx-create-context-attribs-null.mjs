#!/usr/bin/env node

import { createHash } from "node:crypto";
import { readFile, writeFile } from "node:fs/promises";

const [path] = process.argv.slice(2);
if (!path) {
  console.error("usage: patch-glx-create-context-attribs-null.mjs <libGL.so.1>");
  process.exit(2);
}

const expectedSha256 = "f09030c9fc8cbfa71e04985629217159aa61332363d014e12d39d993ea850c2f";
const functionOffset = 0x3c29f;
const original = Buffer.from([0x55, 0x89, 0xe5]); // push ebp; mov ebp,esp
const replacement = Buffer.from([0x31, 0xc0, 0xc3]); // xor eax,eax; ret

const image = await readFile(path);
const inputSha256 = createHash("sha256").update(image).digest("hex");
if (inputSha256 !== expectedSha256) {
  throw new Error(`unexpected input SHA-256 ${inputSha256}; expected ${expectedSha256}`);
}
if (!image.subarray(functionOffset, functionOffset + original.length).equals(original)) {
  throw new Error(`unexpected bytes at file offset 0x${functionOffset.toString(16)}`);
}

replacement.copy(image, functionOffset);
await writeFile(path, image);

const outputSha256 = createHash("sha256").update(image).digest("hex");
console.log(`${path}: glXCreateContextAttribsARB now returns NULL`);
console.log(`SHA-256 ${outputSha256}`);
