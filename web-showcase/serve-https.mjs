import https from 'node:https';
import { createReadStream, statSync, readFileSync } from 'node:fs';
import { resolve, normalize, extname } from 'node:path';
import { gzipSync } from 'node:zlib';

// HTTPS variant of serve.mjs for LAN testing (phones/tablets). The threaded
// WASM build needs crossOriginIsolated (SharedArrayBuffer), which browsers only
// grant in a "secure context" — https, or localhost. A plain http://<LAN-IP>
// URL is NOT a secure context, so SharedArrayBuffer is disabled and the
// emulator never starts. Serving over https (even self-signed) fixes that.
//
//   CERT=<cert.pem> KEY=<key.pem> PORT=8443 node serve-https.mjs build-jitgl
//
// The cert is self-signed, so the phone's browser shows a one-time warning —
// tap "Advanced -> proceed". Generate one bound to your LAN IP with:
//   openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
//     -keyout key.pem -out cert.pem -subj "/CN=<LAN-IP>" \
//     -addext "subjectAltName=IP:<LAN-IP>,IP:127.0.0.1,DNS:localhost"

const root = resolve(process.argv[2] ?? 'build-jitgl');
const port = Number(process.env.PORT ?? 8443);
const certPath = process.env.CERT;
const keyPath = process.env.KEY;
if (!certPath || !keyPath) {
  console.error('set CERT=<cert.pem> and KEY=<key.pem> (self-signed, LAN IP in SAN)');
  process.exit(2);
}
const mime = { '.css': 'text/css', '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.zip': 'application/zip' };

// gzip the big compressible assets on the wire (wasm ~4.8x, app zip ~2.3x) so a
// phone on the LAN pulls far fewer bytes; transparent to the browser and to
// BoxedWine's fetch, no runtime cost. Compressed once, cached; files that don't
// shrink (the already-packed root zip) are streamed raw. See serve.mjs.
const GZIP_EXT = new Set(['.wasm', '.zip', '.js', '.css', '.html']);
const gzCache = new Map();
function gzipped(file, stat) {
  const key = `${file}:${stat.mtimeMs}:${stat.size}`;
  const hit = gzCache.get(file);
  if (hit && hit.key === key) return hit.buf;
  const buf = gzipSync(readFileSync(file), { level: 6 });
  const worthwhile = buf.length < stat.size * 0.9 ? buf : null;
  gzCache.set(file, { key, buf: worthwhile });
  return worthwhile;
}

https.createServer({ cert: readFileSync(certPath), key: readFileSync(keyPath) }, (request, response) => {
  const pathname = request.url.split('?')[0];
  const file = resolve(root, `.${normalize(pathname)}`);
  if (!file.startsWith(root)) return response.writeHead(403).end();
  try {
    const stat = statSync(file);
    if (!stat.isFile()) throw new Error('not a file');
    const headers = {
      'Content-Type': mime[extname(file)] ?? 'application/octet-stream',
      'Cache-Control': 'no-store',
      // Required for SharedArrayBuffer / pthreads.
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
// Bind all interfaces so the LAN can reach it.
}).listen(port, '0.0.0.0', () => console.log(`HTTPS on :${port} (all interfaces)`));
