// Derive an application package that differs only in netduke32.cfg settings.
//
// The package is dominated by DUKE3D.GRP, so the variant is produced by copying
// the zip and replacing the single config entry in place. Every other entry
// keeps its original bytes, and a variant costs a second instead of a minute.
//
//   node web-showcase/tools/make-app-variant.mjs <in.zip> <out.zip> Key=Value ...
//
// Keys are matched case-insensitively against existing `Key = Value` lines
// anywhere in the file; a key that is not already present is an error, because
// silently adding one to the wrong section would not take effect.

import { execFileSync } from 'node:child_process';
import { copyFileSync, mkdtempSync, readFileSync, writeFileSync, rmSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { resolve } from 'node:path';

const [input, output, ...assignments] = process.argv.slice(2);
if (!input || !output || !assignments.length) {
  console.error('usage: make-app-variant.mjs <in.zip> <out.zip> Key=Value ...');
  process.exit(2);
}

const ENTRY = 'netduke32/netduke32.cfg';
const work = mkdtempSync(resolve(tmpdir(), 'app-variant-'));
try {
  execFileSync('unzip', ['-q', resolve(input), ENTRY, '-d', work], { stdio: 'inherit' });
  const path = resolve(work, ENTRY);
  // The config is CRLF; keep it that way so the game's parser stays happy.
  let text = readFileSync(path, 'latin1');

  for (const assignment of assignments) {
    const index = assignment.indexOf('=');
    if (index < 1) throw new Error(`bad assignment: ${assignment}`);
    const key = assignment.slice(0, index).trim();
    const value = assignment.slice(index + 1).trim();
    const line = new RegExp(`^(\\s*${key}\\s*=\\s*).*$`, 'im');
    if (!line.test(text)) throw new Error(`key not present in config: ${key}`);
    text = text.replace(line, `$1${value}`);
    console.log(`  ${key} = ${value}`);
  }

  writeFileSync(path, text, 'latin1');
  const out = resolve(output);
  if (existsSync(out)) rmSync(out);
  copyFileSync(resolve(input), out);
  execFileSync('zip', ['-q', out, ENTRY], { cwd: work, stdio: 'inherit' });
  console.log(`wrote ${execFileSync('shasum', ['-a', '256', out]).toString().trim()}`);
} finally {
  rmSync(work, { recursive: true, force: true });
}
