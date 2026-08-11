import http from 'node:http';
import { createReadStream, statSync } from 'node:fs';
import { resolve, normalize, extname } from 'node:path';

const root = resolve(process.argv[2] ?? 'build');
const mime = { '.css': 'text/css', '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.zip': 'application/zip' };

http.createServer((request, response) => {
  if (request.url === '/') {
    response.writeHead(302, { Location: '/boxedwine.html?root=tinycore-wine11.zip&app=netduke32.zip&p=netduke32.exe&args=-nosetup%20-g%20DUKE3D.GRP&resolution=648x434&env=%22WINEDLLOVERRIDES:mscoree,mshtml=%22&runtime=2' });
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
      'Cross-Origin-Embedder-Policy': 'require-corp',
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Resource-Policy': 'cross-origin',
    });
    createReadStream(file).pipe(response);
  } catch {
    response.writeHead(404).end();
  }
}).listen(8080, () => console.log('Open http://localhost:8080/'));
