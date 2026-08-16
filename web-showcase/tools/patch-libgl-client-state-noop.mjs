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

// Translate the two legacy array-pointer calls that the game uses into the
// GLES2 generic-attribute entry point supported by the WebGL bridge. At the
// wrapper entry, the original arguments are [esp+4..16]. The repeated
// [esp+16] operands account for each preceding push while preserving those
// original arguments.
const pointerWrappers = [
  ["glVertexPointer", 0x23163, 0],
  ["glTexCoordPointer", 0x231f5, 1],
];
for (const [name, offset, index] of pointerWrappers) {
  if (b[offset] !== 0x55 || b[offset + 1] !== 0x89 || b[offset + 2] !== 0xe5)
    throw new Error(`${name}: unexpected wrapper bytes at 0x${offset.toString(16)}`);
  const code = Buffer.from([
    0xff, 0x74, 0x24, 0x10, // push pointer
    0xff, 0x74, 0x24, 0x10, // push stride
    0x6a, 0x00,              // normalized = GL_FALSE
    0xff, 0x74, 0x24, 0x10, // push type
    0xff, 0x74, 0x24, 0x10, // push size
    0x6a, index,              // attribute index
    0x68, 0xbe, 0x0a, 0x00, 0x00, // glVertexAttribPointer selector
    0xcd, 0x99,
    0x83, 0xc4, 0x18,        // discard six bridge arguments
    0xc3,
  ]);
  code.copy(b, offset);
  // The original wrappers are longer than this translation; pad the tail
  // without touching the adjacent exported function.
  const next = name === "glVertexPointer" ? 0x23189 : 0x2321b;
  for (let i = offset + code.length; i < next; i++) b[i] = 0x90;
}
fs.writeFileSync(file, b);
console.log(`${file}: client-state and legacy pointer wrappers translated`);
