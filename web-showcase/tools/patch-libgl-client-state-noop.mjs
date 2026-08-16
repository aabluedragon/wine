#!/usr/bin/env node
import fs from "node:fs";

const file = process.argv[2];
if (!file) {
  console.error("usage: patch-libgl-client-state-noop.mjs <libGL.so.1>");
  process.exit(2);
}
const b = fs.readFileSync(file);
const expected = {
  glEnableClientState: { offset: 0x20462, bytes: [0x55, 0x89, 0xe5] },
  glDisableClientState: { offset: 0x2047c, bytes: [0x55, 0x89, 0xe5] },
  glVertexPointer: { offset: 0x23163, bytes: [0x55, 0x89, 0xe5] },
  glTexCoordPointer: { offset: 0x231f5, bytes: [0x55, 0x89, 0xe5] },
};
for (const [name, spec] of Object.entries(expected)) {
  for (let i = 0; i < spec.bytes.length; i++) {
    if (b[spec.offset + i] !== spec.bytes[i])
      throw new Error(`${name}: unexpected bytes at 0x${(spec.offset + i).toString(16)}`);
  }
  // void-returning wrappers: xor eax,eax; ret. Leave the following wrapper
  // intact; these functions are adjacent in the ELF text section.
  b[spec.offset] = 0x31; b[spec.offset + 1] = 0xc0; b[spec.offset + 2] = 0xc3;
}
fs.writeFileSync(file, b);
console.log(`${file}: client-state wrappers replaced with no-ops`);
