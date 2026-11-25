// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shan-Sherif
// The emulated FM-1 (the site's emu/app.wasm: the firmware in WebAssembly) driven as its AudioWorklet drives it
// (base/web/emu/worklet.js, parts/midi_worklet.js), with the Studio's SysEx: hello, a words document, "sing and speak
// slot" (07), then the audio it renders must hold a voice. Checks the emulator and the protocol the Studio uses on it.
//   node port/emu_test.mjs ../app/build/site/emu/app.wasm
import fs from "node:fs";

const wasm = fs.readFileSync(process.argv[2]);
let mem, t = 0;
const wasi = {
  clock_time_get: (id, prec, out) => { new DataView(mem.buffer).setBigUint64(out, BigInt(Math.round(t * 1e9)), true); return 0; },
  fd_write: (fd, iov, n, out) => { new DataView(mem.buffer).setUint32(out, 0, true); return 0; },
};
const stub = new Proxy(wasi, { get: (o, k) => (k in o ? o[k] : () => 0) });
const env = new Proxy({}, { get: () => () => 0 });
const { instance } = await WebAssembly.instantiate(wasm, { wasi_snapshot_preview1: stub, env });
const ex = instance.exports;
mem = ex.memory;
if (ex._initialize) ex._initialize();
ex.web_master(2800);
ex.web_boot();

const HEAD = [0xf0, 0x7d, 0x56, 0x4f];
const num = (v, n) => Array.from({ length: n }, (_, i) => (v >> (7 * (n - 1 - i))) & 0x7f);
let peak = 0, sum2 = 0, count = 0;
function render(ms) {
  for (let k = 0; k < Math.ceil((44100 * ms) / 1000 / 256); k++) {
    ex.web_render(256);
    t += 256 / 44100;
    const l = new Float32Array(mem.buffer, ex.web_out_l(), 256);
    for (const v of l) { peak = Math.max(peak, Math.abs(v)); sum2 += v * v; count++; }
  }
}
function send(frame) {
  new Uint8Array(mem.buffer, ex.web_sx_in_buf(), frame.length).set(frame);
  ex.web_sx_in(frame.length);
  render(20);
  const out = [];
  const n = ex.web_sx_out_len();
  if (n) {
    const all = new Uint8Array(mem.buffer, ex.web_sx_out_buf(), n);
    for (let at = 0; at < all.length;) {
      let k = at + 1;
      while (k < all.length && all[k] !== 0xf7) k++;
      out.push(Array.from(all.slice(at, k + 1)));
      at = k + 1;
    }
    ex.web_sx_out_clear();
  }
  return out;
}
const sx = (cmd, payload = []) => send([...HEAD, cmd, ...payload, 0xf7]);
const ok = (r, cmd) => r.some((f) => f[4] === 0x7f && f[5] === cmd && f[6] === 0);
let fails = 0;
const check = (what, cond) => { console.log(`  ${cond ? "ok  " : "FAIL"} ${what}`); if (!cond) fails++; };

render(1500);                                   // the boot
const hello = sx(0x01);
check("hello answers", hello.length === 1 && hello[0][4] === 0x01);
console.log("    version:", String.fromCharCode(...hello[0].slice(12, -1)));
const doc = new TextEncoder().encode("VOCALWORDS 1\n1 Hello Chimera, sing for me\n2 Do re mi\n@chimera K AY1 M IH1 R ER0\n");
check("begin", ok(sx(0x02, num(doc.length, 3)), 0x02));
check("data", ok(sx(0x03, [...num(0, 3), ...doc]), 0x03));
check("commit", ok(sx(0x04, num(doc.reduce((a, b) => a + b, 0) & 0x1fffff, 3)), 0x04));
peak = sum2 = count = 0;
render(300);
const before = Math.sqrt(sum2 / count);
peak = sum2 = count = 0;
check("sing and speak slot 1", ok(sx(0x07, [0]), 0x07));
render(2500);
const rms = Math.sqrt(sum2 / count);
check(`a voice comes out (rms ${(20 * Math.log10(rms + 1e-12)).toFixed(1)} dB, peak ${peak.toFixed(2)}; before: ${(20 * Math.log10(before + 1e-12)).toFixed(1)} dB)`, rms > 0.003 && peak < 1.01);
check("an empty slot is refused", sx(0x07, [5]).some((f) => f[4] === 0x7f && f[5] === 0x07 && f[6] === 1));
process.exit(fails ? 1 : 0);
