import http from 'node:http';
import { createReadStream, statSync, writeFileSync } from 'node:fs';
import { resolve, normalize, extname } from 'node:path';

const root = resolve(process.argv[2] ?? 'build');
// Allow a second showcase root to be served next to the default one.
const port = Number(process.env.PORT ?? 8080);
const mime = { '.css': 'text/css', '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.zip': 'application/zip' };

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
    response.writeHead(200, {
      'Content-Type': mime[extname(file)] ?? 'application/octet-stream',
      'Content-Length': stat.size,
      // Development server: always serve the current runtime and packaged app.
      'Cache-Control': 'no-store',
      'Cross-Origin-Embedder-Policy': 'require-corp',
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Resource-Policy': 'cross-origin',
    });
    createReadStream(file).pipe(response);
  } catch {
    response.writeHead(404).end();
  }
}).listen(port, () => console.log(`Open http://localhost:${port}/`));
