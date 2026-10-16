// SPDX-License-Identifier: MIT
// The Squatch Tuner's AudioWorklet processor: runs the WebAssembly core (tuner_wasm.cpp) on the
// audio thread and posts the newest reading to the page about 30 times a second.
//
// processorOptions: { wasm: ArrayBuffer (tuner.wasm), a4, channel (0-based), minHz, rateHz }
// Messages in: { a4 } or { channel }. Messages out: { type: 'error', message } once, and after
// that { type: 'frame', frame, samples, rate } per reading: frame is a Float32Array laid out as
// tuner_frame() (see tuner_wasm.cpp), samples the input samples the core had heard when it was
// taken (its audio position, the clock the page's hold runs on: hosts/web/hold.js).
//
// process() does not allocate: the block is copied into the module's own input buffer and the
// frame into a Float32Array made once. The one allocation is postMessage's copy of that
// 13-float frame, 30 times a second; a SharedArrayBuffer would avoid it, but that needs
// cross-origin isolation headers, which a static host such as GitHub Pages cannot send.

class SquatchTunerProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const o = options.processorOptions;
    this.channel = o.channel ?? 0;
    this.every = Math.max(1, Math.round(sampleRate / (o.rateHz ?? 30)));
    this.count = 0;
    this.samples = 0;
    this.port.onmessage = (e) => this.command(e.data);
    try {
      this.load(o);
    } catch (err) {
      this.wasm = null;
      this.port.postMessage({ type: 'error', message: String(err && err.message ? err.message : err) });
    }
  }

  // Compiling a 31 kB module synchronously is well within what browsers allow off the main thread.
  load(o) {
    const { exports } = new WebAssembly.Instance(new WebAssembly.Module(o.wasm), {});
    exports._initialize();
    if (exports.tuner_configure(sampleRate, o.minHz ?? 38) !== 0) throw new Error(`sample rate ${sampleRate} not supported`);
    exports.tuner_set_a4(o.a4 ?? 440);
    const mem = exports.memory.buffer;
    this.maxBlock = exports.tuner_max_block();
    this.input = new Float32Array(mem, exports.tuner_input(), this.maxBlock);
    this.frame = new Float32Array(mem, exports.tuner_frame(), exports.tuner_frame_size());
    this.out = new Float32Array(this.frame.length);
    this.wasm = exports;
  }

  command(m) {
    if (!this.wasm) return;
    if (typeof m.a4 === 'number') this.wasm.tuner_set_a4(m.a4);
    if (typeof m.channel === 'number') this.channel = m.channel;
  }

  tune(x) {
    if (x.length <= this.maxBlock) {
      this.input.set(x);
      this.wasm.tuner_process(x.length);
      return;
    }
    for (let i = 0; i < x.length; i += this.maxBlock) {  // only if a browser ever sends big blocks
      const part = x.subarray(i, Math.min(x.length, i + this.maxBlock));
      this.input.set(part);
      this.wasm.tuner_process(part.length);
    }
  }

  process(inputs) {
    if (!this.wasm) return false;
    const channels = inputs[0];
    const x = channels.length ? channels[Math.min(this.channel, channels.length - 1)] : null;
    if (x) {
      this.tune(x);
      this.samples += x.length;
    }
    this.count += x ? x.length : 128;
    if (this.count >= this.every) {
      this.count -= this.every;
      this.wasm.tuner_frame();
      this.out.set(this.frame);
      this.port.postMessage({ type: 'frame', frame: this.out, samples: this.samples, rate: sampleRate });
    }
    return true;
  }
}

registerProcessor('squatch-tuner', SquatchTunerProcessor);
