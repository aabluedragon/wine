// Late-bind the GL entries of a boxedwine.js wasm import object.
//
// Emscripten's LEGACY_GL_EMULATION installs its bookkeeping wrappers by
// reassigning the JS variables (`_glAttachShader = _emscripten_glAttachShader =
// wrapped`), but the wasm import object literal (`{Ke:_emscripten_glAttachShader,
// ...}`) snapshots whatever the variables held when it was evaluated. In the
// packaged runtime that snapshot is taken before the wrappers land, so every GL
// call the guest makes through wasm bypasses the emulation layer entirely:
// GL.programShaders stays empty and the first fixed-function draw dies in
// Renderer.init reading `.type` of undefined.
//
// Rewriting each `Xx:_emscripten_glName` entry into `Xx:(...a)=>_emscripten_glName(...a)`
// makes the import resolve the variable at call time, after the wrappers exist.
//
//   node web-showcase/tools/patch-boxedwine-legacy-gl-imports.mjs <boxedwine.js>

import { readFileSync, writeFileSync } from 'node:fs';

const [target] = process.argv.slice(2);
if (!target) {
  console.error('usage: patch-boxedwine-legacy-gl-imports.mjs <boxedwine.js>');
  process.exit(2);
}

const MARKER = '/* boxedwine-legacy-gl-imports: late-bound */';
let source = readFileSync(target, 'utf8');
if (source.includes(MARKER)) {
  console.log('already patched');
  process.exit(0);
}

// Only entries inside an object literal: `,Xx:_emscripten_glName` / `{Xx:_emscripten_glName`.
// The value must be the bare identifier (followed by , or }), so wrapper
// definitions like `var _glX = _emscripten_glX;` are untouched.
// Lookahead for the closing delimiter: consuming it would swallow the next
// entry's leading comma and skip every other pair.
const entry = /([{,])([A-Za-z_$][\w$]*):(_emscripten_gl[A-Za-z0-9_]+)(?=[,}])/g;
let count = 0;
source = source.replace(entry, (whole, open, key, fn) => {
  count++;
  return `${open}${key}:(...a)=>${fn}(...a)`;
});
if (count < 50) {
  console.error(`only ${count} import entries matched; refusing (wrong file or format changed)`);
  process.exit(1);
}
// Draw-time bookkeeping repair. Renderer.init in the legacy-GL emulation reads
// GL.programShaders[GL.currProgram] and GL.shaderInfos[...] to introspect the
// user's program; when a shader was created or attached through a path its
// wrappers did not see, that read throws. The attached shaders are recoverable
// from WebGL itself, so rebuild the records before the draw runs.
const HELPER = `function __bwEnsureGLBookkeeping(){try{
  if(typeof GL==="undefined"||!GL.currProgram)return;
  GL.programShaders=GL.programShaders||{};GL.shaderInfos=GL.shaderInfos||{};
  var ids=GL.programShaders[GL.currProgram];
  if(ids&&ids.length>=2&&GL.shaderInfos[ids[0]])return;
  var prog=GL.programs[GL.currProgram];if(!prog)return;
  var shs=GLctx.getAttachedShaders(prog)||[];
  if(shs.length>=2){
    ids=[];
    for(var i=0;i<shs.length;i++){var id=shs[i].name|0;ids.push(id);
      if(!GL.shaders[id])GL.shaders[id]=shs[i];
      if(!GL.shaderInfos[id])GL.shaderInfos[id]={type:GLctx.getShaderParameter(shs[i],35663),ftransform:false};}
    GL.programShaders[GL.currProgram]=ids;
    return;
  }
  /* Shaders detached after linking: the user-program branch of Renderer.init
     only reads shaderInfos[ids[0]].type to order the pair and never touches
     the shader objects again, so placeholder records are sufficient. */
  var vs=6000000+GL.currProgram*2,fs=vs+1;
  GL.shaderInfos[vs]={type:35633,ftransform:false};
  GL.shaderInfos[fs]={type:35632,ftransform:false};
  GL.programShaders[GL.currProgram]=[vs,fs];
}catch(e){}}
`;
let repaired = 0;
source = source.replace(/([{,])([A-Za-z_$][\w$]*):\(\.\.\.a\)=>(_emscripten_gl(?:DrawArrays|DrawElements|DrawRangeElements)[A-Za-z0-9_]*)\(\.\.\.a\)(?=[,}])/g,
  (whole, open, key, fn) => {
    repaired++;
    return `${open}${key}:(...a)=>{__bwEnsureGLBookkeeping();return ${fn}(...a)}`;
  });

// The emulation's own glDetachShader/glDeleteShader wrappers erase the very
// records Renderer.init later needs: NetDuke32 detaches and deletes its shaders
// right after linking (standard GL hygiene), which left the renderer without
// the translation metadata (ftransform) and so without matrix uploads — every
// vertex transformed by nothing and the scene rasterized to black. Perform the
// real GL operation but keep the records.
let preserved = 0;
source = source.replace(/([{,])([A-Za-z_$][\w$]*):\(\.\.\.a\)=>(_emscripten_glDetachShader)\(\.\.\.a\)(?=[,}])/g,
  (whole, open, key) => {
    preserved++;
    return `${open}${key}:(p,sh)=>{try{GLctx.detachShader(GL.programs[p],GL.shaders[sh])}catch(e){}}`;
  });
source = source.replace(/([{,])([A-Za-z_$][\w$]*):\(\.\.\.a\)=>(_emscripten_glDeleteShader)\(\.\.\.a\)(?=[,}])/g,
  (whole, open, key) => {
    preserved++;
    return `${open}${key}:(sh)=>{try{var o=GL.shaders[sh];if(o)GLctx.deleteShader(o)}catch(e){}}`;
  });

writeFileSync(target, MARKER + '\n' + HELPER + source);
console.log(`late-bound ${count} GL import entries, draw-repair on ${repaired}, record-preserving on ${preserved}, in ${target}`);
