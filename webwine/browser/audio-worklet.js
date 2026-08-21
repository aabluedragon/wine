/* Drains the SharedArrayBuffer PCM ring the interpreter fills (see
 * webwine/wasm_ipc.c webwine_audio_push) and plays it.
 *
 * The producer is bursty - it refills whenever the interpreter reaches a
 * housekeeping tick with the SDL audio lock free - so this keeps a cushion
 * before starting and, on underrun, outputs silence and re-warms rather than
 * repeating stale samples (which is what makes underruns sound like clicks).
 * Also resamples linearly if the AudioContext could not be opened at the game's
 * own rate. */
class WWAudio extends AudioWorkletProcessor {
  constructor(opts) {
    super();
    const o = opts.processorOptions;
    this.idx = new Int32Array(o.sab, 0, 2);
    this.data = new Float32Array(o.sab, 8);
    this.cap = o.cap;
    this.ratio = o.srcRate / sampleRate;   // input frames consumed per output frame
    this.warmFrames = Math.max(1024, Math.ceil(o.srcRate * 0.06));  // ~60ms cushion
    this.pos = 0;
    this.warm = false;
    this.underruns = 0;
  }
  process(_inputs, outputs) {
    const out = outputs[0];
    const L = out[0], R = out[1] || out[0];
    const n = L.length;
    const w = Atomics.load(this.idx, 0);
    const r = Atomics.load(this.idx, 1);
    const avail = (w - r + this.cap) % this.cap;

    if (!this.warm) {
      if (avail < this.warmFrames) { L.fill(0); if (R !== L) R.fill(0); return true; }
      this.warm = true;
      this.pos = 0;
    }
    const need = Math.ceil(n * this.ratio) + 2;
    if (avail < need) {
      L.fill(0); if (R !== L) R.fill(0);
      this.warm = false;                    // re-cushion before resuming
      this.underruns++;
      if ((this.underruns & 31) === 1) this.port.postMessage({ underruns: this.underruns });
      return true;
    }
    let frac = this.pos;
    for (let i = 0; i < n; i++) {
      const base = frac | 0, t = frac - base;
      const a0 = ((r + base) % this.cap) * 2;
      const a1 = ((r + base + 1) % this.cap) * 2;
      L[i] = this.data[a0] * (1 - t) + this.data[a1] * t;
      const rv = this.data[a0 + 1] * (1 - t) + this.data[a1 + 1] * t;
      if (R !== L) R[i] = rv;
      frac += this.ratio;
    }
    const consumed = frac | 0;
    Atomics.store(this.idx, 1, (r + consumed) % this.cap);
    this.pos = frac - consumed;
    return true;
  }
}
registerProcessor('ww-audio', WWAudio);
