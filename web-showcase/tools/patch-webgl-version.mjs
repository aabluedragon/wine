#!/usr/bin/env node

// Wine's wined3d parser accepts desktop GL version strings beginning with a
// number, but Emscripten deliberately reports "OpenGL ES 2.0 (WebGL …)".
// BoxedWine's Unix-side GL driver calls that Emscripten import directly, so
// present an equivalent, parser-compatible version string at this boundary.
import { readFile, writeFile } from 'node:fs/promises';

const path = process.argv[2];
if (!path) throw new Error('usage: patch-webgl-version.mjs <boxedwine.js>');

const source = await readFile(path, 'utf8');
const before = '`OpenGL ES 2.0 (${webGLVersion})`';
const after = '`2.0 BoxedWine WebGL (${webGLVersion})`';

if (source.includes(after)) process.exit(0);
if (!source.includes(before)) {
  throw new Error(`did not find Emscripten GL_VERSION implementation in ${path}`);
}

await writeFile(path, source.replace(before, after));
