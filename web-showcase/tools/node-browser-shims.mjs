// Minimal browser-ish shims so the emscripten SDL build can run headless in node.
export function installShims() {
  const listeners = {};
  const fakeEl = {
    addEventListener(){}, removeEventListener(){}, appendChild(){}, removeChild(){},
    style:{}, getContext(){ return null; }, width:640, height:480,
    getBoundingClientRect(){ return {left:0,top:0,width:640,height:480}; },
    parentElement:null, requestPointerLock(){}, focus(){},
  };
  globalThis.document = {
    getElementById(){ return fakeEl; }, querySelector(){ return fakeEl; },
    querySelectorAll(){ return []; }, createElement(){ return fakeEl; },
    addEventListener(){}, removeEventListener(){}, body: fakeEl,
    documentElement: fakeEl, hidden:false, visibilityState:'visible',
  };
  globalThis.window = globalThis;
  /* navigator exists in node */
  globalThis.addEventListener = ()=>{};
  globalThis.removeEventListener = ()=>{};
  if (typeof globalThis.requestAnimationFrame === 'undefined')
    globalThis.requestAnimationFrame = (cb)=>setTimeout(()=>cb(Date.now()),16);
  // AudioContext whose ScriptProcessor onaudioprocess fires periodically to
  // drive BoxedWine's audio-based emulation timing.
  class FakeParam { constructor(v){this.value=v;} setValueAtTime(){} }
  class FakeNode { constructor(){this.onaudioprocess=null;} connect(){return this;} disconnect(){} }
  globalThis.AudioContext = class {
    constructor(){ this.sampleRate=44100; this.state='running'; this.destination={};
      this._t0=Date.now(); }
    get currentTime(){ return (Date.now()-this._t0)/1000; }
    createScriptProcessor(bufferSize, inCh, outCh){
      const node = new FakeNode(); node.bufferSize = bufferSize||1024;
      const ms = Math.max(1, Math.round((node.bufferSize/this.sampleRate)*1000));
      const buf = { getChannelData: ()=> new Float32Array(node.bufferSize), sampleRate:this.sampleRate, length:node.bufferSize, numberOfChannels:2 };
      setInterval(()=>{ if(node.onaudioprocess){ try{ node.onaudioprocess({ outputBuffer: buf, inputBuffer: buf, playbackTime:this.currentTime }); }catch(e){} } }, ms);
      return node;
    }
    createBuffer(ch,len,rate){ return { getChannelData:()=>new Float32Array(len), sampleRate:rate, length:len, numberOfChannels:ch }; }
    createBufferSource(){ const n=new FakeNode(); n.start=()=>{}; n.stop=()=>{}; n.buffer=null; return n; }
    createGain(){ const n=new FakeNode(); n.gain=new FakeParam(1); return n; }
    createMediaStreamSource(){ return new FakeNode(); }
    resume(){ this.state='running'; return Promise.resolve(); }
    suspend(){ return Promise.resolve(); }
    close(){ return Promise.resolve(); }
  };
  globalThis.webkitAudioContext = globalThis.AudioContext;
}
