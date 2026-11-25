// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Shan-Sherif
// the Studio's own code (app/web/studio.html's script, without the DOM) on test lines: its syllables, and the document
// it would send; studio_check.c then reads the same lines through the C firmware with that document
import fs from "node:fs";
import zlib from "node:zlib";
const [, , page, dictGz, dictH, outDir] = process.argv;
let src = fs.readFileSync(page, "utf8");
src = src.slice(src.indexOf("<script>") + 8, src.indexOf("</script>"));
src = src.slice(0, src.indexOf("// ---------------------------------------------------------------- the page"));
src = src.replace(/\(async \(\) => \{[\s\S]*?\}\)\(\);/, "");          // the dictionary's fetch
src = src.replace(/^\$\("ver"\).*$/m, "");
const builtin = Object.fromEntries([...fs.readFileSync(dictH, "utf8").matchAll(/\{"([^"]+)", "([^"]+)"\}/g)].map((m) => [m[1], m[2]]));
src = src.replace(/\/\*BUILTIN\*\/.*?\/\*END\*\//, JSON.stringify(builtin));
const lines = ["Harder, better, faster, stronger", "Twinkle, twinkle, little star, how I wonder what you are!",
  "Row, row, row your boat, gently down the stream.", "Mary had a little lamb, its fleece was white as snow.",
  "London Bridge is falling down, falling down, falling down.", "Hello Chimera, sing for me - ~ tonight",
  "Ti-ger, ti-ger, burning bright in the forests of the night", "Fire and water, everywhere; strengths of the world"];
const run = new Function("document", "window", src + `
  dict = new Map();
  for (const l of DICT.split("\\n")) { const t = l.indexOf("\\t"); if (t > 0) dict.set(l.slice(0, t), l.slice(t + 1)); }
  LINES.forEach((t, i) => { slots[i].text = t; });
  slots[5].fixes = { chimera: "kai mirror" };
  return { doc: documentText(), syl: slots.map((s) => analyse(s).syl.filter((y) => !y.brk).map((y) => y.text)) };`);
globalThis.DICT = zlib.gunzipSync(fs.readFileSync(dictGz)).toString();
globalThis.LINES = lines;
const r = run.call(null, {}, {});
fs.writeFileSync(outDir + "/doc.txt", r.doc);
fs.writeFileSync(outDir + "/studio_syl.txt", r.syl.map((s) => s.join(" ")).join("\n") + "\n");
console.log(r.doc.length + " bytes; document:\n" + r.doc);
