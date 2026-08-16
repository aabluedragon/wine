import http from 'node:http';
import { createReadStream, statSync, writeFileSync } from 'node:fs';
import { resolve, normalize, extname } from 'node:path';
const root = resolve(process.argv[2] ?? 'build-gl');
const mime = { '.css': 'text/css', '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.zip': 'application/zip' };
http.createServer((req, res) => {
  if (req.method === 'POST' && req.url.split('?')[0] === '/savecache') {
    const chunks = []; req.on('data', c => chunks.push(c));
    req.on('end', () => { const buf = Buffer.concat(chunks); writeFileSync(resolve(root, 'netduke32-jit-modules.zip'), buf);
      console.log(`saved JIT cache: ${buf.length} bytes`); res.writeHead(200, {'Access-Control-Allow-Origin':'*'}).end('ok'); });
    return;
  }
  if (req.url === '/') { res.writeHead(302, { Location: '/boxedwine.html?root=tinycore-wine11-parent-inline-webgl-pci-glxshim-legacyctxattrib-fixeddefaults.zip&app=netduke32-v1.2.1-slotshim4.zip&p=cmd&args=%2Fc%20run.bat%20-cfg%20netduke32.cfg%20-nosetup%20-g%20DUKE3D.GRP%20-v1%20-l1%20-s3&resolution=640x480&storage=memory&env=%22WINEDLOVERRIDES:mscoree,mshtml=|WINEDEBUG:+seh,+wgl,+opengl,+debugstr%22&w=%2Fhome%2Fusername%2F.wine%2Fdosdevices%2Fc%3A%2Ffiles%2Fnetduke32' }); return res.end(); }
  const file = resolve(root, `.${normalize(req.url.split('?')[0])}`);
  if (!file.startsWith(root)) return res.writeHead(403).end();
  try { const st = statSync(file); if (!st.isFile()) throw 0;
    res.writeHead(200, { 'Content-Type': mime[extname(file)] ?? 'application/octet-stream', 'Content-Length': st.size,
      'Cache-Control': 'no-store', 'Cross-Origin-Embedder-Policy':'require-corp','Cross-Origin-Opener-Policy':'same-origin','Cross-Origin-Resource-Policy':'cross-origin' });
    createReadStream(file).pipe(res);
  } catch { res.writeHead(404).end(); }
}).listen(8082, () => console.log('Open http://localhost:8082/'));
