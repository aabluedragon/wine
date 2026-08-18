// Headless Chrome driver for the BoxedWine/Wine WASM showcase.
//
// Launches Chrome with a real (SwiftShader) WebGL stack, opens the showcase
// URL, streams guest/console output, and samples the canvas so a run can be
// judged on evidence: internal canvas size, whether pixels are non-black, and
// whether they change over time. Optionally sends keyboard input.
//
//   node web-showcase/tools/cdp-run.mjs --url <url> [--seconds 90]
//        [--out <dir>] [--shot 10,30,60] [--key <ch>@<sec> ...]
//
// Everything it writes goes under --out (default: a timestamped scratch dir).

import { spawn } from 'node:child_process';
import { mkdirSync, writeFileSync, appendFileSync, rmSync } from 'node:fs';
import { resolve } from 'node:path';

const argv = process.argv.slice(2);
const arg = (name, fallback) => {
  const i = argv.indexOf(`--${name}`);
  return i === -1 ? fallback : argv[i + 1];
};
const all = (name) => argv.reduce((acc, v, i) => (v === `--${name}` ? [...acc, argv[i + 1]] : acc), []);

const url = arg('url');
if (!url) {
  console.error('usage: cdp-run.mjs --url <url> [--seconds N] [--out DIR] [--shot 10,30] [--key w@20]');
  process.exit(2);
}
const seconds = Number(arg('seconds', 90));
const outDir = resolve(arg('out', `/tmp/cdp-run-${process.pid}`));
const shotAt = String(arg('shot', '')).split(',').filter(Boolean).map(Number);
// --key <name>@<sec> sends one key; --type <text>@<sec> types a whole string.
const keys = [
  ...all('key').map((k) => {
    const [ch, at] = k.split('@');
    return { ch, at: Number(at) };
  }),
  ...all('type').map((k) => {
    const at = k.slice(k.lastIndexOf('@') + 1);
    return { text: k.slice(0, k.lastIndexOf('@')), at: Number(at) };
  }),
].sort((a, b) => a.at - b.at);
// --hold <name>@<startSec>:<endSec> — keyDown at start, keyUp at end (movement).
const holds = all('hold').map((k) => {
  const [name, span] = k.split('@');
  const [from, to] = span.split(':').map(Number);
  return { name, from, to, down: false };
});

const port = Number(arg('port', 9222));
const chrome = arg('chrome', '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome');

mkdirSync(outDir, { recursive: true });
const logPath = resolve(outDir, 'console.log');
writeFileSync(logPath, '');
const log = (line) => {
  appendFileSync(logPath, line + '\n');
};

// --profile-dir <path>: reuse a persistent Chrome profile (IndexedDB survives
// across runs — needed to test the emulator's persisted JIT cache). Without
// it, each run gets a fresh wiped profile as before.
const profile = arg('profile-dir', null) ? resolve(arg('profile-dir')) : resolve(outDir, 'profile');
if (!arg('profile-dir', null)) rmSync(profile, { recursive: true, force: true });

const headful = argv.includes('--headful'); // real window: Pointer Lock works
const child = spawn(chrome, [
  ...(headful ? [] : ['--headless=new']),
  `--remote-debugging-port=${port}`,
  `--user-data-dir=${profile}`,
  '--no-first-run',
  '--no-default-browser-check',
  '--disable-gpu-sandbox',
  '--use-angle=swiftshader',
  '--use-gl=angle',
  '--enable-unsafe-swiftshader',
  '--enable-features=SharedArrayBuffer',
  '--ignore-certificate-errors',
  '--window-size=1280,900',
  '--mute-audio',
  'about:blank',
], { stdio: ['ignore', 'pipe', 'pipe'] });
child.stderr.on('data', (d) => log('[chrome] ' + d.toString().trimEnd()));

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function findTarget() {
  for (let i = 0; i < 60; i++) {
    try {
      const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
      const page = list.find((t) => t.type === 'page');
      if (page) return page;
    } catch {}
    await sleep(500);
  }
  throw new Error('no Chrome page target');
}

const page = await findTarget();
const ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((r, j) => {
  ws.onopen = r;
  ws.onerror = j;
});

let nextId = 1;
const pending = new Map();
const send = (method, params = {}) =>
  new Promise((resolveCall, rejectCall) => {
    const id = nextId++;
    pending.set(id, { resolveCall, rejectCall });
    ws.send(JSON.stringify({ id, method, params }));
  });

ws.onmessage = (ev) => {
  const msg = JSON.parse(ev.data);
  if (msg.id && pending.has(msg.id)) {
    const { resolveCall, rejectCall } = pending.get(msg.id);
    pending.delete(msg.id);
    return msg.error ? rejectCall(new Error(JSON.stringify(msg.error))) : resolveCall(msg.result);
  }
  if (msg.method === 'Runtime.consoleAPICalled') {
    const text = (msg.params.args ?? []).map((a) => a.value ?? a.description ?? a.type).join(' ');
    log(`[console.${msg.params.type}] ${text}`);
  } else if (msg.method === 'Runtime.exceptionThrown') {
    const d = msg.params.exceptionDetails;
    log(`[exception] ${d.text} ${d.exception?.description ?? ''}`);
    console.log(`!! exception: ${d.text} ${(d.exception?.description ?? '').split('\n')[0]}`);
  } else if (msg.method === 'Log.entryAdded') {
    log(`[log.${msg.params.entry.level}] ${msg.params.entry.text}`);
  }
};

await send('Runtime.enable');
await send('Log.enable');
await send('Page.enable');

// Frame counter. The emulator presents each guest frame by uploading the
// framebuffer as a texture and drawing it, so counting texture uploads on the
// WebGL context counts presented frames exactly, with none of the cost or the
// sampling ceiling of screenshotting. This has to be installed before the page
// creates its context, hence addScriptToEvaluateOnNewDocument.
const FRAME_HOOK = `(() => {
  const counters = { upload: 0, draw: 0, bind: 0 };
  window.__bwCounters = counters;
  window.__bwBinds = [];
  window.__bwShaders = [];
  // Capture the exact source each shader is given, plus its compile result, so
  // a failing translation chain can be read instead of inferred.
  for (const proto of [self.WebGLRenderingContext, self.WebGL2RenderingContext]) {
    if (!proto || !proto.prototype.shaderSource) continue;
    const origSource = proto.prototype.shaderSource;
    proto.prototype.shaderSource = function (shader, src) {
      // GLSL ES repair: emscripten's legacy-GL emulation prepends its fog and
      // texcoord declarations in front of whatever precision header the source
      // had, and GLSL ES rejects any declaration before a float precision is
      // set (and #extension lines that follow declarations). Hoist #extension
      // lines and a precision block to the top; later precision statements
      // still override for the declarations that follow them.
      // NOTE: this code lives inside a backtick template, so every backslash
      // needs doubling to survive into the injected page script.
      let text = String(src);
      if (!/^\\s*#version/.test(text)) {
        const lines = text.split('\\n');
        const extensions = lines.filter((l) => /^\\s*#extension/.test(l));
        const rest = lines.filter((l) => !/^\\s*#extension/.test(l));
        text = extensions.join('\\n') + (extensions.length ? '\\n' : '') +
          '#ifdef GL_ES\\nprecision highp float;\\n#endif\\n' + rest.join('\\n');
      }
      if (window.__bwShaders.length < 24) {
        window.__bwShaders.push({ head: text.slice(0, 400) });
      }
      return origSource.call(this, shader, text);
    };
  }
  window.__bwCompiles = [];
  // Capture at compile time via getShaderSource: this reads whatever source the
  // context actually holds, regardless of how it was delivered, plus the result.
  for (const proto of [self.WebGLRenderingContext, self.WebGL2RenderingContext]) {
    if (!proto || !proto.prototype.compileShader) continue;
    const origCompile = proto.prototype.compileShader;
    proto.prototype.compileShader = function (shader) {
      const r = origCompile.call(this, shader);
      if (window.__bwCompiles.length < 40) {
        window.__bwCompiles.push({
          ok: !!this.getShaderParameter(shader, 0x8B81),
          log: String(this.getShaderInfoLog(shader) || '').slice(0, 160),
          src: String(this.getShaderSource(shader) || '').slice(0, 320),
        });
      }
      return r;
    };
  }
  // Record who binds textures. The guest's binds come from wasm; anything with
  // JS frames above the wasm boundary is the runtime's own presentation code.
  for (const proto of [self.WebGLRenderingContext, self.WebGL2RenderingContext]) {
    if (!proto || !proto.prototype.bindTexture) continue;
    const originalBind = proto.prototype.bindTexture;
    proto.prototype.bindTexture = function (target, texture) {
      counters.bind++;
      if (window.__bwBinds.length < 40) {
        const stack = (new Error()).stack || '';
        window.__bwBinds.push({
          name: texture && texture.name !== undefined ? texture.name : null,
          caller: stack.split('\\n').slice(1, 4).join(' | ').slice(0, 300),
        });
      }
      return originalBind.call(this, target, texture);
    };
  }
  window.__bwDrawProbes = [];
  for (const proto of [self.WebGLRenderingContext, self.WebGL2RenderingContext]) {
    if (!proto) continue;
    for (const [name, bucket] of [['texImage2D', 'upload'], ['texSubImage2D', 'upload'],
                                  ['drawArrays', 'draw'], ['drawElements', 'draw']]) {
      const original = proto.prototype[name];
      if (!original) continue;
      proto.prototype[name] = function (...args) {
        counters[bucket]++;
        // Per-texture upload journal: which texture got which data. LUMINANCE
        // textures cannot be attached to an FBO in WebGL 1, so upload-side
        // accounting is the only way to know their content.
        if (bucket === 'upload') {
          try {
            const tex = this.getParameter(this.TEXTURE_BINDING_2D);
            const id = tex && tex.name !== undefined ? tex.name : -1;
            const pixels = args[args.length - 1];
            let sum = -1;
            if (pixels && pixels.length !== undefined) {
              sum = 0;
              const n = Math.min(pixels.length, 65536);
              for (let i = 0; i < n; i++) sum += pixels[i];
            }
            const journal = (window.__bwTexLog = window.__bwTexLog || {});
            journal[id] = { w: args.length >= 9 ? args[3 + (name === 'texSubImage2D' ? 1 : 0)] : -1,
              h: args.length >= 9 ? args[4 + (name === 'texSubImage2D' ? 1 : 0)] : -1,
              fmt: args.length >= 9 ? args[args.length - 3] : -1, sum, uploads: (journal[id]?.uploads || 0) + 1 };
          } catch (e) {}
        }
        const r = original.apply(this, args);
        // Sample the draw stream: where do draws land, and do they produce
        // color? Reading right after the call sees the true render target.
        if (bucket === 'draw' && counters.draw % 977 === 0 && window.__bwDrawProbes.length < 30) {
          try {
            const px = new Uint8Array(32 * 32 * 4);
            this.readPixels(0, 0, 32, 32, this.RGBA, this.UNSIGNED_BYTE, px);
            let sum = 0; for (let i = 0; i < px.length; i++) sum += px[i];
            // Content of the textures bound to the first units: attach each to
            // a scratch FBO and read a few texels. An all-zero palette texture
            // fully explains a black multiply chain.
            const active = this.getParameter(this.ACTIVE_TEXTURE);
            const savedFb = this.getParameter(this.FRAMEBUFFER_BINDING);
            const units = [];
            for (let u = 0; u < 4; u++) {
              this.activeTexture(this.TEXTURE0 + u);
              const tex = this.getParameter(this.TEXTURE_BINDING_2D);
              units.push(tex && tex.name !== undefined ? tex.name : null);
            }
            this.activeTexture(active);
            window.__bwDrawProbes.push({
              n: counters.draw,
              fbo: !!savedFb,
              texlog: window.__bwTexLog || {},
              vp: Array.from(this.getParameter(this.VIEWPORT) || []),
              sum,
              units,
            });
          } catch (e) {
            window.__bwDrawProbes.push({ n: counters.draw, err: String(e).slice(0, 80) });
          }
        }
        return r;
      };
    }
  }
})()`;
await send('Page.addScriptToEvaluateOnNewDocument', { source: FRAME_HOOK });

await send('Page.navigate', { url });

// Report the canvas geometry and where it sits on the page. Pixels are NOT read
// back through drawImage: the emulator presents through a WebGL context without
// preserveDrawingBuffer, so the backing buffer is already cleared by the time a
// later task copies it, and every sample reads as pure black even while the page
// is visibly rendering. Sampling goes through Page.captureScreenshot instead,
// which reads the composited frame.
const PROBE = `(() => {
  const c = document.querySelector('canvas');
  if (!c) return JSON.stringify({ canvas: false });
  const r = c.getBoundingClientRect();
  const n = window.__bwCounters || { upload: 0, draw: 0, bind: 0 };
  return JSON.stringify({ canvas: true, w: c.width, h: c.height,
    cssW: c.clientWidth, cssH: c.clientHeight,
    x: Math.round(r.x), y: Math.round(r.y),
    upload: n.upload, draw: n.draw, bind: n.bind,
    binds: (window.__bwBinds || []).slice(0, 12),
    shaders: (window.__bwShaders || []),
    compiles: (window.__bwCompiles || []),
    drawProbes: (window.__bwDrawProbes || []) });
})()`;

// Mean luminance and a hash of the composited canvas region, decoded from an
// uncompressed-enough JPEG. Successive hashes prove pixels actually change.
// --light: skip the per-second screenshot sampling (Page.captureScreenshot
// forces a GPU readback — measured as multi-second readPixels stalls under
// SwiftShader). Upload/draw counting stays, so fps numbers remain valid.
const light = argv.includes('--light');

async function sampleCanvas(info) {
  if (light) return {};
  if (!info.canvas || !info.cssW || !info.cssH) return {};
  const shot = await send('Page.captureScreenshot', {
    format: 'jpeg',
    quality: 80,
    clip: { x: info.x, y: info.y, width: info.cssW, height: info.cssH, scale: 0.25 },
  });
  const bytes = Buffer.from(shot.data, 'base64');
  let hash = 2166136261;
  for (const b of bytes) hash = ((hash ^ b) * 16777619) >>> 0;
  // A flat black region compresses to almost nothing; real content does not.
  return { shotBytes: bytes.length, hash: hash.toString(16) };
}

const samples = [];
const shots = new Set(shotAt);
const keyQueue = [...keys];
let focused = false;

// The emulator only receives keyboard events once its canvas has focus, so a
// real click has to land on it before any key is sent.
async function focusCanvas() {
  const r = await send('Runtime.evaluate', { expression: PROBE, returnByValue: true });
  const c = JSON.parse(r.result.value ?? '{}');
  if (!c.canvas) return false;
  const x = c.x + Math.floor(c.cssW / 2);
  const y = c.y + Math.floor(c.cssH / 2);
  const common = { x, y, button: 'left', buttons: 1, clickCount: 1 };
  await send('Input.dispatchMouseEvent', { type: 'mousePressed', ...common });
  await sleep(60);
  await send('Input.dispatchMouseEvent', { type: 'mouseReleased', ...common, buttons: 0 });
  await send('Runtime.evaluate', { expression: `document.querySelector('canvas').focus()` });
  return true;
}

// A single printable character, or one of the named keys the OSD needs.
const NAMED_KEYS = {
  Enter:     { key: 'Enter',  code: 'Enter',      vk: 13, text: '\r' },
  Backquote: { key: '`',      code: 'Backquote',  vk: 192, text: '`' },
  Escape:    { key: 'Escape', code: 'Escape',     vk: 27 },
  Space:     { key: ' ',      code: 'Space',      vk: 32, text: ' ' },
  Control:   { key: 'Control',    code: 'ControlLeft', vk: 17 },
  ArrowUp:   { key: 'ArrowUp',    code: 'ArrowUp',    vk: 38 },
  ArrowDown: { key: 'ArrowDown',  code: 'ArrowDown',  vk: 40 },
  ArrowLeft: { key: 'ArrowLeft',  code: 'ArrowLeft',  vk: 37 },
  ArrowRight:{ key: 'ArrowRight', code: 'ArrowRight', vk: 39 },
};

function keyDescriptor(name) {
  if (NAMED_KEYS[name]) {
    const k = NAMED_KEYS[name];
    return { key: k.key, code: k.code, windowsVirtualKeyCode: k.vk, nativeVirtualKeyCode: k.vk, text: k.text };
  }
  const ch = name;
  const upper = ch.toUpperCase();
  const vk = upper.charCodeAt(0);
  const code = /[0-9]/.test(ch) ? `Digit${ch}` : /[a-z]/i.test(ch) ? `Key${upper}` : 'Unidentified';
  return { key: ch, code, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk, text: ch };
}

async function typeKey(name) {
  const { text, ...common } = keyDescriptor(name);
  await send('Input.dispatchKeyEvent', { type: text ? 'keyDown' : 'rawKeyDown', text, ...common });
  await sleep(60);
  await send('Input.dispatchKeyEvent', { type: 'keyUp', ...common });
}

// Type a whole string, one character at a time.
async function typeText(text) {
  for (const ch of text) {
    await typeKey(ch === ' ' ? 'Space' : ch);
    await sleep(40);
  }
}

// --mouselook <sec>: at <sec>, click the canvas (grants Pointer Lock) then
// dispatch a burst of horizontal mouse moves so the diagnostics show what the
// guest does with relative motion.
const mouselookAt = arg('mouselook', null) !== null ? Number(arg('mouselook')) : null;
let mouselookDone = false;
async function mouselookTick(t) {
  if (mouselookAt === null || mouselookDone || t < mouselookAt) return;
  mouselookDone = true;
  const r = await send('Runtime.evaluate', { expression: PROBE, returnByValue: true });
  const c = JSON.parse(r.result.value ?? '{}');
  if (!c.canvas) { console.log(`t=${t}s  mouselook: no canvas`); return; }
  const cx = c.x + Math.floor(c.cssW / 2);
  const cy = c.y + Math.floor(c.cssH / 2);
  await send('Input.dispatchMouseEvent', { type: 'mousePressed', x: cx, y: cy, button: 'left', buttons: 1, clickCount: 1 });
  await sleep(30);
  await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x: cx, y: cy, button: 'left', buttons: 0, clickCount: 1 });
  await sleep(200);
  const locked = await send('Runtime.evaluate', { expression: `(document.pointerLockElement === document.querySelector('canvas'))`, returnByValue: true });
  console.log(`t=${t}s  mouselook: clicked canvas, pointerLocked=${locked.result.value}`);
  // Drag right in steps: the browser derives movementX from position deltas.
  let px = cx;
  for (let i = 0; i < 20; i++) {
    px += 25;
    await send('Input.dispatchMouseEvent', { type: 'mouseMoved', x: px, y: cy, button: 'none', buttons: 0 });
    await sleep(50);
  }
  console.log(`t=${t}s  mouselook: dispatched 20 right-moves (+25px each)`);
}

// --readfile <emulator path>@<sec>: dump the tail of a file from the
// emscripten FS (the guest's disk) into the run log — e.g. the game's own
// netduke32.log, which never reaches stdout.
const readfileQueue = all('readfile').map((k) => {
  const at = k.slice(k.lastIndexOf('@') + 1);
  return { path: k.slice(0, k.lastIndexOf('@')), at: Number(at) };
}).sort((a, b) => a.at - b.at);

async function readEmuFile(path) {
  const expr = `(function(p){try{
    var fs=(typeof Module!=="undefined"&&Module.FS)?Module.FS:FS;
    var d=fs.readFile(p);var s='';
    for(var i=0;i<d.length;i++)s+=String.fromCharCode(d[i]);
    return s.slice(-12000);
  }catch(e){return 'READFILE-ERR: '+e}})(${JSON.stringify(path)})`;
  const r = await send('Runtime.evaluate', { expression: expr, returnByValue: true });
  return r.result.value;
}

// --cpuprofile <startSec>:<durationSec> — collect a V8 CPU profile of the page
// (JS + wasm, with wasm function names) and write it next to the samples.
const cpuprofileArg = arg('cpuprofile', null);
let cpuprofile = null;
if (cpuprofileArg) {
  const [at, dur] = cpuprofileArg.split(':').map(Number);
  cpuprofile = { at, dur, state: 'idle' };
}

async function cpuProfileTick(t) {
  if (!cpuprofile) return;
  if (cpuprofile.state === 'idle' && t >= cpuprofile.at) {
    cpuprofile.state = 'running';
    await send('Profiler.enable');
    await send('Profiler.setSamplingInterval', { interval: 500 });
    await send('Profiler.start');
    console.log(`t=${t}s  cpu profile started (${cpuprofile.dur}s)`);
  } else if (cpuprofile.state === 'running' && t >= cpuprofile.at + cpuprofile.dur) {
    cpuprofile.state = 'done';
    const r = await send('Profiler.stop');
    const profile = r.profile;
    const { writeFileSync } = await import('node:fs');
    writeFileSync(`${outDir}/page.cpuprofile`, JSON.stringify(profile));
    // Self-time summary: hits per node, resolved to function names.
    const nodes = new Map(profile.nodes.map((n) => [n.id, n]));
    const hits = new Map();
    for (const n of profile.nodes) {
      if (!n.hitCount) continue;
      const f = n.callFrame;
      const name = (f.functionName || '(anonymous)') + ' @ ' + (f.url || '').split('/').pop();
      hits.set(name, (hits.get(name) || 0) + n.hitCount);
    }
    const total = [...hits.values()].reduce((a, b) => a + b, 0) || 1;
    const top = [...hits.entries()].sort((a, b) => b[1] - a[1]).slice(0, 25);
    console.log(`t=${t}s  cpu profile done, ${total} samples; top self-time:`);
    for (const [name, h] of top) {
      console.log(`  ${(100 * h / total).toFixed(1)}%  ${name}`);
    }
  }
}

for (let t = 1; t <= seconds; t++) {
  await sleep(1000);
  await cpuProfileTick(t).catch((e) => console.log('cpuprofile error: ' + e));
  await mouselookTick(t).catch((e) => console.log('mouselook error: ' + e));
  while (readfileQueue.length && readfileQueue[0].at <= t) {
    const f = readfileQueue.shift();
    const content = await readEmuFile(f.path).catch((e) => `READFILE-ERR: ${e}`);
    console.log(`t=${t}s  readfile ${f.path} >>>\n${content}\n<<< end readfile`);
  }
  for (const h of holds) {
    if (!h.down && t >= h.from && t < h.to) {
      if (!focused) {
        focused = await focusCanvas().catch(() => false);
        console.log(`t=${t}s  focused canvas: ${focused}`);
      }
      h.down = true;
      const { text, ...common } = keyDescriptor(h.name);
      await send('Input.dispatchKeyEvent', { type: text ? 'keyDown' : 'rawKeyDown', text, ...common }).catch(() => {});
      console.log(`t=${t}s  hold-down '${h.name}'`);
    } else if (h.down && t >= h.to) {
      h.down = false;
      const { text, ...common } = keyDescriptor(h.name);
      await send('Input.dispatchKeyEvent', { type: 'keyUp', ...common }).catch(() => {});
      console.log(`t=${t}s  hold-up '${h.name}'`);
    }
  }
  while (keyQueue.length && keyQueue[0].at <= t) {
    const k = keyQueue.shift();
    if (!focused) {
      focused = await focusCanvas().catch(() => false);
      console.log(`t=${t}s  focused canvas: ${focused}`);
    }
    if (k.text !== undefined) {
      await typeText(k.text).catch(() => {});
      console.log(`t=${t}s  typed '${k.text}'`);
    } else {
      await typeKey(k.ch).catch(() => {});
      console.log(`t=${t}s  sent key '${k.ch}'`);
    }
  }
  let info = {};
  try {
    const r = await send('Runtime.evaluate', { expression: PROBE, returnByValue: true, awaitPromise: false });
    info = JSON.parse(r.result.value ?? '{}');
    Object.assign(info, await sampleCanvas(info));
  } catch (e) {
    info = { evalError: String(e) };
  }
  // Presented frames since the previous sample; samples are one second apart.
  const previous = samples[samples.length - 1];
  if (previous && info.upload !== undefined && previous.upload !== undefined) {
    info.fps = info.upload - previous.upload;
  }
  samples.push({ t, ...info });
  if (t % 5 === 0 || shots.has(t)) {
    console.log(`t=${t}s  ${JSON.stringify(info)}`);
  }
  if (shots.has(t)) {
    try {
      const s = await send('Page.captureScreenshot', { format: 'png' });
      writeFileSync(resolve(outDir, `shot-${String(t).padStart(3, '0')}s.png`), Buffer.from(s.data, 'base64'));
    } catch (e) {
      console.log(`screenshot at ${t}s failed: ${e.message}`);
    }
  }
}

writeFileSync(resolve(outDir, 'samples.json'), JSON.stringify(samples, null, 2));

// 2 KiB of JPEG for a quarter-scale frame is far more than a flat fill encodes to.
const drawn = samples.filter((s) => s.shotBytes > 2048);
const distinct = new Set(drawn.map((s) => s.hash));
console.log('---');
console.log(`out dir:        ${outDir}`);
console.log(`samples:        ${samples.length}`);
console.log(`non-black:      ${drawn.length}`);
console.log(`distinct frames:${distinct.size}`);
const last = samples[samples.length - 1];
console.log(`final sample:   ${JSON.stringify(last)}`);

// Frame rate over the tail of the run, once the game is past its slow startup.
const tail = samples.slice(-Math.min(60, Math.floor(samples.length / 3))).filter((s) => s.fps !== undefined);
if (tail.length) {
  const rates = tail.map((s) => s.fps).sort((a, b) => a - b);
  const mean = rates.reduce((a, b) => a + b, 0) / rates.length;
  console.log(`fps (last ${tail.length}s): mean ${mean.toFixed(2)}  median ${rates[rates.length >> 1]}  min ${rates[0]}  max ${rates[rates.length - 1]}`);
}

ws.close();
child.kill('SIGTERM');
process.exit(0);
