// Tests of the web editor's patch codec (html/patch-codec.js). Also writes
// test vectors for firmware_check.c, which runs them through the real patch.c:
//   node codec_test.js [vectors-file]
'use strict';
const fs = require('fs');
const path = require('path');
const C = require(path.join(__dirname, '..', '..', '..', 'html', 'patch-codec.js'));

let checks = 0, failures = 0;
function check(ok, text) {
  checks++;
  if (!ok) { failures++; console.log('FAIL: ' + text); }
}
function rejects(bytes, text) {
  try { C.decode(bytes); check(false, 'should reject: ' + text); } catch (e) { check(e instanceof C.CodecError, 'rejects with CodecError: ' + text); }
}

/* Small repeatable random numbers. */
let seed = 0x2f6e2b1;
function rnd(n) {
  seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5; seed >>>= 0;
  return seed % n;
}
const range = () => { const a = rnd(128), b = rnd(128); return [Math.min(a, b), Math.max(a, b)]; };

function randomPatch() {
  const p = C.blankPatch(rnd(16));
  const chars = 'ABCXYZ0189- ';
  p.name = Array.from({ length: 12 }, () => chars[rnd(chars.length)]).join('');
  for (let i = 0; i < 3; i++) {
    p.ch[i] = rnd(65536); p.sys[i] = rnd(1024);
    p.voice[i] = Array.from({ length: 16 }, () => rnd(128));
  }
  p.knobs = p.knobs.map(() => ({ ch: rnd(16), cc: rnd(120) }));
  const vel = [1 + rnd(127), 1 + rnd(127)].sort((a, b) => a - b);
  p.live = { ch: rnd(16), vmin: vel[0], vmax: vel[1], oct: rnd(7) - 3, prog: rnd(128), msb: rnd(4) ? rnd(128) : -1, lsb: rnd(4) ? rnd(128) : -1 };
  const chSeen = new Set();
  for (let i = rnd(33); i > 0; i--) {
    const port = rnd(3), input = rnd(16);
    if (chSeen.has(port << 4 | input)) continue;
    chSeen.add(port << 4 | input);
    p.chRules.push({ port, in: input, out: rnd(16) });
  }
  const seen = new Set(), lists = {};
  for (let i = rnd(120); i > 0; i--) {
    const r = { port: rnd(3), ch: rnd(16), type: rnd(3), in: rnd(128), out: rnd(128), outCh: rnd(16) };
    const list = r.port << 8 | r.ch << 4 | r.type;
    if (seen.has(C.msgKey(r)) || (lists[list] || 0) >= C.LIM.perList) continue;
    seen.add(C.msgKey(r)); lists[list] = (lists[list] || 0) + 1;
    p.msgRules.push(r);
  }
  let total = 0;
  for (let i = rnd(C.LIM.trig + 1); i > 0; i--) {
    const type = rnd(4), msgs = [];
    for (let k = rnd(9); k > 0 && total < C.LIM.allMsgs; k--, total++) {
      const t = rnd(4), val = () => (rnd(5) ? rnd(128) : C.IN);
      msgs.push({ type: t, ch: rnd(16), d1: val(), d2: t === 3 ? 0 : val() });
    }
    p.trig.push({ port: rnd(3), ch: rnd(16), type, d1: range(), d2: range(), pass: !rnd(2), enter: !rnd(2), msgs });
  }
  return p;
}

function fullPatch() {
  const p = C.blankPatch(0);
  for (let i = 0; i < C.LIM.chRules; i++) p.chRules.push({ port: i % 3, in: i % 16, out: 1 });
  for (let i = 0; i < C.LIM.msgRules; i++) p.msgRules.push({ port: i % 3, ch: i % 16, type: 0, in: i % 128, out: 1, outCh: 0 });
  for (let i = 0; i < C.LIM.trig; i++) p.trig.push({ port: 0, ch: 0, type: 3, d1: [i, i], d2: [0, 127], pass: true, enter: true, msgs: Array.from({ length: 8 }, () => ({ type: 2, ch: 0, d1: C.IN, d2: 1 })) });
  return p;
}

/* ---- Sizes from design/patch-format.md ---- */
const empty = C.encode(C.blankPatch(0));
check(empty.length === 94, 'empty body is 94 bytes');
check(C.pack(empty).length === 108, 'empty body packs to 108 bytes');
check(C.packets(0, empty, 0).length === 1, 'empty body is one packet');
const fullBody = C.encode(fullPatch());
const fullPackets = C.packets(3, fullBody, 5);
check(fullBody.length === C.LIM.body, 'full body is 3070 bytes');
check(fullPackets.length === C.LIM.packets, 'full body is 32 packets');
check(Math.max(...fullPackets.map(m => m.length)) === 122, 'largest packet is 122 bytes');
check(fullPackets.every(m => m.slice(1, -1).every(v => v < 128)), 'packets are all 7-bit');
check(fullPackets.every(m => m.slice(5, -1).reduce((a, b) => a + b, 0) % 128 === 0), 'checksums sum to a multiple of 128');
check(JSON.stringify(C.request(7, C.EDIT)) === '[240,125,7,17,16,127,247]', 'single request bytes');
check(JSON.stringify(C.requestAll(7)) === '[240,125,7,17,17,247]', 'all request bytes');

/* ---- Round trips ---- */
const vectors = [];
for (let i = 0; i < 400; i++) {
  const p = randomPatch();
  const body = C.encode(p);
  const again = C.encode(C.decode(body).patch);
  check(C.sameBytes(body, again), `round trip ${i}`);
  check(!C.validate(p).some(x => x.sev === 'red'), `random patch ${i} has no errors`);
  const assembler = new C.Assembler();
  let done = null;
  for (const m of C.packets(i % 16, body, 9)) done = assembler.feed(m) || done;
  check(done && done.slot === i % 16 && done.device === 9 && C.sameBytes(done.body, body), `packets round trip ${i}`);
  vectors.push([1, body]);
}
vectors.push([1, empty], [1, fullBody]);

/* The encoder sorts, so added order doesn't matter. */
const a = C.blankPatch(0), b = C.blankPatch(0);
a.msgRules = [{ port: 1, ch: 0, type: 0, in: 9, out: 1, outCh: 0 }, { port: 0, ch: 2, type: 1, in: 3, out: 1, outCh: 0 }];
b.msgRules = [a.msgRules[1], a.msgRules[0]];
a.chRules = [{ port: 2, in: 1, out: 1 }, { port: 0, in: 5, out: 1 }];
b.chRules = [a.chRules[1], a.chRules[0]];
check(C.sameBytes(C.encode(a), C.encode(b)), 'rules are written in canonical order');

/* A PC message's D2 is always written as 0. */
const pc = C.blankPatch(0);
pc.trig = [{ port: 0, ch: 0, type: 0, d1: [0, 1], d2: [0, 1], pass: true, enter: false, msgs: [{ type: 3, ch: 0, d1: 1, d2: C.IN }] }];
check(C.decode(C.encode(pc)).patch.trig[0].msgs[0].d2 === 0, 'PC message D2 is written as 0');

/* ---- Rejections: each is one wrong byte in a valid body ---- */
const withAll = C.blankPatch(0);
withAll.chRules = [{ port: 0, in: 0, out: 1 }];
withAll.msgRules = [{ port: 0, ch: 0, type: 0, in: 1, out: 2, outCh: 3 }];
withAll.trig = [{ port: 0, ch: 0, type: 2, d1: [1, 2], d2: [3, 4], pass: true, enter: true, msgs: [{ type: 2, ch: 0, d1: 1, d2: 2 }, { type: 3, ch: 1, d1: 5, d2: 0 }] }];
const base = C.encode(withAll);
vectors.push([1, base]);
const rulesEnd = 91 + 3 + 6, trigAt = rulesEnd + 3, msgAt = trigAt + 10;
const bad = [
  ['version 0', 0, 0], ['name lower case', 1, 97], ['name symbol', 12, 33],
  ['system bit 10', 20, 4], ['voice bit 7', 25, 128],
  ['knob channel', 73, 16], ['knob CC 120', 74, 120],
  ['live channel', 81, 16], ['velocity 0', 82, 0], ['min over max', 82, 127 + 1], ['octave +4', 84, 4], ['octave -4', 84, 252], ['program 128', 85, 128], ['bank 128', 86, 128],
  ['channel rule port', 91, 3], ['channel rule in', 92, 16], ['channel rule out', 93, 16],
  ['message rule port', 94, 3], ['message rule channel', 95, 16], ['message rule type', 96, 3], ['message rule in', 97, 128], ['message rule out', 98, 128], ['message rule out channel', 99, 16],
  ['trigger port', trigAt, 3], ['trigger channel', trigAt + 1, 16], ['trigger type', trigAt + 2, 4], ['D1 from over to', trigAt + 3, 3], ['D1 to 128', trigAt + 4, 128], ['D2 from over to', trigAt + 5, 5], ['flag bit 2', trigAt + 7, 7], ['reserved byte', trigAt + 9, 1],
  ['message type', msgAt, 4], ['message channel', msgAt + 1, 16], ['message D1 129', msgAt + 2, 129], ['message D2 129', msgAt + 3, 129], ['PC message with D2', msgAt + 7, 5],
];
for (const [text, at, value] of bad) {
  const body = base.slice();
  body[at] = value;
  rejects(body, text);
  vectors.push([0, body]);
}
const counts = [
  ['channel rule count over', 88, 33], ['message rule count over', 89, 1 + 0, 90, 1],
  ['trigger count over', rulesEnd, 33], ['trigger total over', rulesEnd + 1, 1, rulesEnd + 2, 1], ['message count sum wrong', trigAt + 8, 1],
];
for (const [text, at, value, at2, value2] of counts) {
  const body = base.slice();
  body[at] = value;
  if (at2 !== undefined) body[at2] = value2;
  rejects(body, text);
  vectors.push([0, body]);
}
rejects(base.slice(0, 90), 'too short');
rejects([...base, 0], 'one byte too long');
rejects(base.slice(0, -1), 'one byte too short');
vectors.push([0, base.slice(0, 90)], [0, [...base, 0]], [0, base.slice(0, -1)]);

const overList = C.blankPatch(0);
for (let i = 0; i <= C.LIM.perList; i++) overList.msgRules.push({ port: 0, ch: 0, type: 0, in: i, out: 0, outCh: 0 });
const overBody = C.encode(overList);
rejects(overBody, '33 rules in one list');
vectors.push([0, overBody]);

/* ---- Versions ---- */
const v1 = C.encode(C.blankPatch(0)).slice(0, -3);
v1[0] = 1;
check(C.decode(v1).version === 1 && C.decode(v1).patch.trig.length === 0, 'version 1 opens with no triggers');
check(C.encode(C.decode(v1).patch)[0] === 2, 'version 1 is written as version 2');
vectors.push([2, v1]);
const v3 = [...C.encode(C.blankPatch(0)), 1, 2, 3];
v3[0] = 3;
check(C.decode(v3).readOnly === true, 'a newer version opens read-only');
check(C.decode(empty).readOnly === false, 'version 2 is editable');
vectors.push([0, v3]); // the current firmware refuses a newer version

/* ---- Validation ---- */
const dup = C.blankPatch(0);
dup.msgRules = [{ port: 1, ch: 0, type: 0, in: 5, out: 1, outCh: 0 }, { port: 1, ch: 0, type: 0, in: 5, out: 2, outCh: 0 }, { port: 1, ch: 0, type: 0, in: 6, out: 2, outCh: 0 }];
check(C.duplicateRules(dup).size === 2, 'duplicate message rules are found');
check(C.validate(dup).some(x => x.sev === 'red'), 'duplicates are an error');
const hang = C.blankPatch(0);
hang.trig = [{ port: 0, ch: 0, type: 0, d1: [0, 1], d2: [0, 1], pass: true, enter: false, msgs: [{ type: 0, ch: 0, d1: 1, d2: 1 }] }];
check(C.validate(hang).some(x => x.sev === 'amber' && /NON WITHOUT NOF/.test(x.text)), 'NON WITHOUT NOF warning');
check(!C.validate(hang).some(x => x.sev === 'red'), 'a warning is not an error');
const cap = C.blankPatch(0);
for (let i = 0; i < 3; i++) cap.trig.push({ port: 0, ch: 0, type: 3, d1: [0, 127], d2: [0, 127], pass: true, enter: false, msgs: Array.from({ length: 30 }, () => ({ type: 2, ch: 0, d1: 1, d2: 1 })) });
check(C.validate(cap).some(x => x.sev === 'amber' && /stops at 64/.test(x.text)), 'overlap over the 64-message cap warns');
check(C.cleanName('ab-c!d') === 'AB-CD       ', 'names are cleaned and padded');

/* ---- Packets and files ---- */
const body = C.encode(randomPatch());
const asm = () => new C.Assembler();
const throwsOn = (fn, text) => { try { fn(); check(false, text); } catch (e) { check(e instanceof C.CodecError, text); } };
const pk = C.packets(2, fullBody, 0);
throwsOn(() => { const x = pk[0].slice(); x[8] ^= 1; asm().feed(x); }, 'bad checksum is refused');
throwsOn(() => asm().feed(pk[1]), 'a patch starting mid-way is refused');
throwsOn(() => { const s = asm(); s.feed(pk[0]); s.feed(pk[2]); }, 'a missing packet is refused');
check(C.unpack([0x7f, 1]) === null, 'unpack refuses top bits for absent bytes');
check(C.unpack([0]) === null, 'unpack refuses a group with no data');
check(C.isDump(pk[0], 0) && !C.isDump(pk[0], 1), 'isDump matches the Device ID');
check(!C.isDump(C.request(0, 0)), 'an echoed request is not a dump');

const one = C.parseSyx(C.buildSyx([{ slot: 4, body }], 3));
check(one.errors.length === 0 && one.patches.length === 1 && one.patches[0].slot === 4 && C.sameBytes(one.patches[0].body, body), 'single .syx round trip');
const sixteen = Array.from({ length: 16 }, (_, i) => ({ slot: i, body: C.encode(C.blankPatch(i)) }));
const file = C.buildSyx(sixteen, 0);
check(file[4] === 1, 'a 16-patch file uses the all-patch command');
check(C.parseSyx(file).patches.length === 16, '16-patch .syx round trip');
check(C.parseSyx(Uint8Array.from([0xf0, 0x41, 0xf7])).errors.length === 1, 'a file with no dump is reported');

/* Golden file: a real device dump decodes and re-encodes to the same bytes. */
const goldenArg = process.argv[3];
if (goldenArg) {
  const golden = C.parseSyx(fs.readFileSync(goldenArg));
  check(golden.errors.length === 0 && golden.patches.length > 0, 'device dump parses');
  for (const { slot, body: b } of golden.patches) check(C.sameBytes(C.encode(C.decode(b).patch), b), `device patch ${slot} re-encodes identically`);
  console.log(`device dump: ${golden.patches.length} patches`);
}

if (process.argv[2]) fs.writeFileSync(process.argv[2], vectors.map(([want, b]) => want + ' ' + b.map(v => v.toString(16).padStart(2, '0')).join('')).join('\n') + '\n');
console.log(`${checks - failures}/${checks} passed`);
process.exit(failures ? 1 : 0);
