import http from 'node:http';
import { createReadStream, statSync, writeFileSync, readFileSync } from 'node:fs';
import { resolve, normalize, extname } from 'node:path';
import { gzipSync } from 'node:zlib';

const root = resolve(process.argv[2] ?? 'build');
// Allow a second showcase root to be served next to the default one.
const port = Number(process.env.PORT ?? 8080);
const mime = { '.css': 'text/css', '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.zip': 'application/zip' };

// gzip the big, compressible assets on the wire (the 5.7 MB wasm and the
// 60+ MB app/root zips) so a page load — especially a phone on the LAN — pulls
// far fewer bytes. The zips are stored (deflate level 0) inside so the guest FS
// never re-inflates them at runtime; HTTP gzip is transparent to the browser and
// to BoxedWine's fetch, so this is a pure transfer win with no runtime cost.
// Each file is compressed once and cached in memory, keyed by path + mtime + size.
const GZIP_EXT = new Set(['.wasm', '.zip', '.js', '.css', '.html']);
const gzCache = new Map();
// Returns a gzip buffer worth sending, or null if the file barely compresses
// (e.g. the root zip, whose contents are already packed) — in which case we
// stream it raw and never waste CPU re-compressing it.
function gzipped(file, stat) {
  const key = `${file}:${stat.mtimeMs}:${stat.size}`;
  const hit = gzCache.get(file);
  if (hit && hit.key === key) return hit.buf; // may be null (known not worth it)
  const buf = gzipSync(readFileSync(file), { level: 6 });
  const worthwhile = buf.length < stat.size * 0.9 ? buf : null;
  gzCache.set(file, { key, buf: worthwhile });
  return worthwhile;
}

http.createServer((request, response) => {
  // Capture endpoint: the page POSTs a recorded JIT cache zip here so it can be
  // saved without going through a sandboxed browser download.
  if (request.method === 'POST' && request.url.split('?')[0] === '/savecache') {
    const chunks = [];
    request.on('data', c => chunks.push(c));
    request.on('end', () => {
      const buf = Buffer.concat(chunks);
      const out = resolve(root, 'netduke32-jit-modules.zip');
      writeFileSync(out, buf);
      console.log(`saved JIT cache: ${out} (${buf.length} bytes)`);
      response.writeHead(200, { 'Access-Control-Allow-Origin': '*' }).end('ok');
    });
    return;
  }
  if (request.url === '/') {
    // Keep all Wine DLL overrides in one value. `|` separates independent
    // environment assignments in the shell, leaving semicolons available to
    // WINEDLLOVERRIDES itself.
    response.writeHead(302, { Location: '/boxedwine.html?root=tinycore-wine11.zip&app=netduke32.zip&p=run.bat&args=-cfg%20netduke32.cfg%20-nosetup%20-g%20DUKE3D.GRP%20-v1%20-l1%20-s3&resolution=640x480&storage=memory&env=%22WINEDLLOVERRIDES:mscoree,mshtml=%22&runtime=2' });
    return response.end();
  }
  const pathname = request.url.split('?')[0];
  const file = resolve(root, `.${normalize(pathname)}`);
  if (!file.startsWith(root)) return response.writeHead(403).end();
  try {
    const stat = statSync(file);
    if (!stat.isFile()) throw new Error('not a file');
    const headers = {
      'Content-Type': mime[extname(file)] ?? 'application/octet-stream',
      // Development server: always serve the current runtime and packaged app.
      'Cache-Control': 'no-store',
      'Cross-Origin-Embedder-Policy': 'require-corp',
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Resource-Policy': 'cross-origin',
    };
    const wantsGzip = /\bgzip\b/.test(request.headers['accept-encoding'] ?? '');
    const gz = wantsGzip && GZIP_EXT.has(extname(file)) && stat.size > 4096 ? gzipped(file, stat) : null;
    if (gz) {
      headers['Content-Encoding'] = 'gzip';
      headers['Content-Length'] = gz.length;
      headers['Vary'] = 'Accept-Encoding';
      response.writeHead(200, headers);
      response.end(gz);
    } else {
      headers['Content-Length'] = stat.size;
      response.writeHead(200, headers);
      createReadStream(file).pipe(response);
    }
  } catch {
    response.writeHead(404).end();
  }
}).listen(port, () => console.log(`Open http://localhost:${port}/`));
