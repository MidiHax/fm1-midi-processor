/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * FM-1 MIDI Processor patches: the body format and SysEx packets, as specified
 * in design/patch-format.md. Mirrors src/app/patch.c and src/app/sysex.c.
 * No DOM, so tools/host_tests/codec can test it in Node.
 *
 * A patch object:
 *   name       12 characters
 *   ch         [3] uint16, bit n = channel n+1 passes
 *   sys        [3] uint16, one bit per SYS type
 *   voice      [3][16] uint8, one bit per VOICE type
 *   knobs      [4] {ch, cc}
 *   live       {ch, vmin, vmax, oct, prog, msb, lsb}; msb/lsb -1 = OFF
 *   chRules    [] {port, in, out}
 *   msgRules   [] {port, ch, type, in, out, outCh}
 *   trig       [] {port, ch, type, d1:[from,to], d2:[from,to], pass, enter,
 *                  msgs:[{type, ch, d1, d2}]}; d1/d2 128 = IN
 */
(function (root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.PatchCodec = api;
}(globalThis, function () {
  'use strict';

  const PORTS = ['TRS', 'USB', 'BLE'];
  const SYS = ['SYX', 'MTC', 'SPP', 'STA', 'CON', 'SS', 'TR', 'RST', 'STP', 'TCK'];
  const VOICE = ['NON', 'NOF', 'CC', 'PC', 'PAT', 'CAT', 'PB'];
  const REMAP = ['NOTE', 'CC', 'PC'];
  const TRIG = ['NON', 'NOF', 'CC', 'PC'];
  const TRIG_PC = 3, IN = 128, BANK_OFF = 255;
  const LIM = { body: 3070, chRules: 32, msgRules: 256, perList: 32, trig: 32, trigMsgs: 32, allMsgs: 256, packets: 32, emit: 64 };

  const VERSION = 2, FIXED = 91, TRIG_HEAD = 3, TRIG_BYTES = 10, TRIG_MSG_BYTES = 4, CH_RULE_BYTES = 3, MSG_RULE_BYTES = 6;
  const OFF = { name: 1, channels: 13, system: 19, voice: 25, knobs: 73, live: 81, chCount: 88, msgCount: 89 };
  const SYS_MASK = (1 << SYS.length) - 1, VOICE_MASK = (1 << VOICE.length) - 1;

  const MANUFACTURER = 0x7d, MODEL = 0x11;
  const CMD = { dump: 0x00, dumpAll: 0x01, request: 0x10, requestAll: 0x11 };
  const EDIT = 0x7f, PATCHES = 16, HEADER = 5, BODY_PER_PACKET = 98, PAYLOAD_MAX = 112;

  class CodecError extends Error {}
  const fail = text => { throw new CodecError(text); };

  /* ---- Patch objects ---------------------------------------------------- */
  function defaultName(slot) {
    return ('PATCH ' + String(slot + 1).padStart(2, '0')).padEnd(12, ' ');
  }

  /* The firmware's default patch (patch.c, mp_patch_default_body). */
  function blankPatch(slot) {
    return {
      name: defaultName(slot),
      ch: [0xffff, 0xffff, 0xffff],
      sys: [SYS_MASK, SYS_MASK, SYS_MASK],
      voice: PORTS.map(() => Array(16).fill(VOICE_MASK)),
      knobs: [20, 21, 22, 23].map(cc => ({ ch: 0, cc })),
      live: { ch: 0, vmin: 100, vmax: 127, oct: 0, prog: 0, msb: -1, lsb: -1 },
      chRules: [], msgRules: [], trig: [],
    };
  }

  /* Uppercases and keeps only the characters a patch name allows. */
  function cleanName(text) {
    return String(text).toUpperCase().replace(/[^A-Z0-9 -]/g, '').slice(0, 12).padEnd(12, ' ');
  }

  const msgKey = r => r.port << 13 | r.ch << 9 | r.type << 7 | r.in;

  /* ---- Encoding --------------------------------------------------------- */
  /* Always version 2, in the firmware's canonical order, so the same patch
   * gives the same bytes as mp_patch_encode. */
  function encode(p) {
    const b = [VERSION];
    const u16 = v => b.push(v & 255, v >> 8 & 255);
    for (let i = 0; i < 12; i++) b.push((p.name[i] || ' ').charCodeAt(0));
    p.ch.forEach(u16);
    p.sys.forEach(u16);
    p.voice.forEach(v => b.push(...v));
    p.knobs.forEach(k => b.push(k.ch, k.cc));
    const l = p.live;
    b.push(l.ch, l.vmin, l.vmax, l.oct & 255, l.prog, l.msb < 0 ? BANK_OFF : l.msb, l.lsb < 0 ? BANK_OFF : l.lsb);
    b.push(p.chRules.length);
    u16(p.msgRules.length);
    [...p.chRules].sort((x, y) => x.port - y.port || x.in - y.in).forEach(r => b.push(r.port, r.in, r.out));
    [...p.msgRules].sort((x, y) => msgKey(x) - msgKey(y)).forEach(r => b.push(r.port, r.ch, r.type, r.in, r.out, r.outCh));
    b.push(p.trig.length);
    u16(p.trig.reduce((n, t) => n + t.msgs.length, 0));
    p.trig.forEach(t => b.push(t.port, t.ch, t.type, t.d1[0], t.d1[1], t.d2[0], t.d2[1], (t.pass ? 1 : 0) | (t.enter ? 2 : 0), t.msgs.length, 0));
    p.trig.forEach(t => t.msgs.forEach(m => b.push(m.type, m.ch, m.d1, m.type === TRIG_PC ? 0 : m.d2)));
    return b;
  }

  /* ---- Decoding: the checks of body_valid in patch.c ---------------------- */
  const get16 = (b, at) => b[at] | b[at + 1] << 8;
  const rangeOk = (lo, hi) => lo <= hi && hi <= 127;
  const valueOk = v => v <= 127 || v === IN;
  const bankOk = v => v <= 127 || v === BANK_OFF;

  /* Returns {patch, version, readOnly}, or throws CodecError. A body newer
   * than this page opens read-only: writing it back would drop what it adds. */
  function decode(bytes) {
    const b = Array.from(bytes), n = b.length;
    if (n < FIXED) fail(`Patch is too short (${n} bytes).`);
    const version = b[0];
    if (version < 1) fail('Patch has an invalid format version.');
    const c = b[OFF.chCount], m = get16(b, OFF.msgCount);
    if (c > LIM.chRules) fail('Patch has too many channel rules.');
    if (m > LIM.msgRules) fail('Patch has too many message rules.');
    const rulesEnd = FIXED + CH_RULE_BYTES * c + MSG_RULE_BYTES * m;
    if (version === 1 ? n !== rulesEnd : n < rulesEnd) fail('Patch length does not match its rule counts.');

    const p = blankPatch(0);
    let name = '';
    for (let i = 0; i < 12; i++) {
      const ch = b[OFF.name + i];
      if (!((ch >= 65 && ch <= 90) || (ch >= 48 && ch <= 57) || ch === 45 || ch === 32)) fail('Patch name has an invalid character.');
      name += String.fromCharCode(ch);
    }
    p.name = name;
    for (let port = 0; port < 3; port++) {
      p.ch[port] = get16(b, OFF.channels + 2 * port);
      p.sys[port] = get16(b, OFF.system + 2 * port);
      if (p.sys[port] & ~SYS_MASK) fail('Message Filter has invalid system bits.');
      p.voice[port] = b.slice(OFF.voice + 16 * port, OFF.voice + 16 * port + 16);
      if (p.voice[port].some(v => v & ~VOICE_MASK)) fail('Message Filter has invalid channel bits.');
    }
    p.knobs = [0, 1, 2, 3].map(k => ({ ch: b[OFF.knobs + 2 * k], cc: b[OFF.knobs + 2 * k + 1] }));
    if (p.knobs.some(k => k.ch > 15 || k.cc > 119)) fail('Knobs Config is out of range.');
    const q = b.slice(OFF.live, OFF.live + 7);
    const octave = q[3] > 127 ? q[3] - 256 : q[3];
    if (q[0] > 15 || q[1] < 1 || q[1] > 127 || q[2] < 1 || q[2] > 127 || q[1] > q[2] || octave < -3 || octave > 3 || q[4] > 127 || !bankOk(q[5]) || !bankOk(q[6])) fail('Live Keys is out of range.');
    p.live = { ch: q[0], vmin: q[1], vmax: q[2], oct: octave, prog: q[4], msb: q[5] === BANK_OFF ? -1 : q[5], lsb: q[6] === BANK_OFF ? -1 : q[6] };

    let r = FIXED;
    p.chRules = [];
    for (let i = 0; i < c; i++, r += CH_RULE_BYTES) {
      if (b[r] >= 3 || b[r + 1] > 15 || b[r + 2] > 15) fail('A channel rule is out of range.');
      p.chRules.push({ port: b[r], in: b[r + 1], out: b[r + 2] });
    }
    p.msgRules = [];
    const perList = {};
    for (let i = 0; i < m; i++, r += MSG_RULE_BYTES) {
      if (b[r] >= 3 || b[r + 1] > 15 || b[r + 2] >= REMAP.length || b[r + 3] > 127 || b[r + 4] > 127 || b[r + 5] > 15) fail('A message rule is out of range.');
      const list = b[r] << 8 | b[r + 1] << 4 | b[r + 2];
      perList[list] = (perList[list] || 0) + 1;
      if (perList[list] > LIM.perList) fail('A message rule list has too many rules.');
      p.msgRules.push({ port: b[r], ch: b[r + 1], type: b[r + 2], in: b[r + 3], out: b[r + 4], outCh: b[r + 5] });
    }

    p.trig = [];
    if (version >= 2) {
      if (n - r < TRIG_HEAD) fail('Patch is missing its trigger section.');
      const t = b[r], total = get16(b, r + 1);
      if (t > LIM.trig) fail('Patch has too many triggers.');
      if (total > LIM.allMsgs) fail('Patch has too many trigger messages.');
      const end = r + TRIG_HEAD + TRIG_BYTES * t + TRIG_MSG_BYTES * total;
      if (version === VERSION ? n !== end : n < end) fail('Patch length does not match its trigger counts.');
      r += TRIG_HEAD;
      let at = r + TRIG_BYTES * t, sum = 0;
      for (let i = 0; i < t; i++, r += TRIG_BYTES) {
        if (b[r] >= 3 || b[r + 1] > 15 || b[r + 2] >= TRIG.length || !rangeOk(b[r + 3], b[r + 4]) || !rangeOk(b[r + 5], b[r + 6]) || (b[r + 7] & ~3) || b[r + 8] > LIM.trigMsgs || b[r + 9]) fail('A trigger is out of range.');
        sum += b[r + 8];
        if (sum > total) fail('Trigger message counts do not add up.');
        const msgs = [];
        for (let k = 0; k < b[r + 8]; k++, at += TRIG_MSG_BYTES) {
          if (b[at] >= TRIG.length || b[at + 1] > 15 || !valueOk(b[at + 2]) || !valueOk(b[at + 3]) || (b[at] === TRIG_PC && b[at + 3] !== 0)) fail('A trigger message is out of range.');
          msgs.push({ type: b[at], ch: b[at + 1], d1: b[at + 2], d2: b[at + 3] });
        }
        p.trig.push({ port: b[r], ch: b[r + 1], type: b[r + 2], d1: [b[r + 3], b[r + 4]], d2: [b[r + 5], b[r + 6]], pass: !!(b[r + 7] & 1), enter: !!(b[r + 7] & 2), msgs });
      }
      if (sum !== total) fail('Trigger message counts do not add up.');
    }
    return { patch: p, version, readOnly: version > VERSION };
  }

  /* ---- Checks before sending --------------------------------------------- */
  /* Indexes of message rules that share an input. The firmware's validator
   * accepts these, then keeps only one on load. */
  function duplicateRules(p) {
    const seen = new Map(), bad = new Set();
    p.msgRules.forEach((r, i) => {
      const k = msgKey(r);
      if (seen.has(k)) { bad.add(i); bad.add(seen.get(k)); } else seen.set(k, i);
    });
    return bad;
  }

  /* Problems with a patch: sev 'red' blocks sending, 'amber' is a warning. */
  function validate(p) {
    const out = [];
    const red = text => out.push({ sev: 'red', text });
    const amber = text => out.push({ sev: 'amber', text });
    const label = i => String(i + 1).padStart(2, '0');
    const dups = duplicateRules(p);
    if (dups.size) red(`${dups.size} message rules share an input. The FM-1 would keep only one.`);
    const chSeen = new Set();
    let chDup = false;
    p.chRules.forEach(r => { const k = r.port << 4 | r.in; if (chSeen.has(k)) chDup = true; chSeen.add(k); });
    if (chDup) red('Two channel rules share an input. The FM-1 would keep only one.');
    const lists = {};
    p.msgRules.forEach(r => { const k = r.port << 8 | r.ch << 4 | r.type; lists[k] = (lists[k] || 0) + 1; });
    if (Object.values(lists).some(n => n > LIM.perList)) red(`A message rule list is over ${LIM.perList} rules.`);
    if (p.msgRules.length > LIM.msgRules) red(`Over ${LIM.msgRules} message rules.`);
    if (p.chRules.length > LIM.chRules) red(`Over ${LIM.chRules} channel rules.`);
    if (p.trig.length > LIM.trig) red(`Over ${LIM.trig} triggers.`);
    if (p.trig.reduce((n, t) => n + t.msgs.length, 0) > LIM.allMsgs) red(`Over ${LIM.allMsgs} trigger messages.`);
    p.trig.forEach((t, i) => {
      if (t.msgs.length > LIM.trigMsgs) red(`Trigger ${label(i)} has over ${LIM.trigMsgs} messages.`);
      if (t.msgs.filter(m => m.type === 0).length > t.msgs.filter(m => m.type === 1).length) amber(`Trigger ${label(i)}: NON WITHOUT NOF. A note may hang.`);
      const overlap = p.trig.filter(o => o.port === t.port && o.ch === t.ch && o.type === t.type && o.d1[0] <= t.d1[1] && t.d1[0] <= o.d1[1] &&
        (t.type === TRIG_PC || (o.d2[0] <= t.d2[1] && t.d2[0] <= o.d2[1])));
      const sum = overlap.reduce((n, o) => n + o.msgs.length, 0);
      if (sum > LIM.emit) amber(`Trigger ${label(i)}: overlapping triggers can send ${sum} messages. The FM-1 stops at ${LIM.emit}.`);
    });
    return out;
  }

  /* ---- 7-bit packing and packets ----------------------------------------- */
  function pack(data) {
    const out = [];
    for (let g = 0; g < data.length; g += 7) {
      const part = data.slice(g, g + 7);
      out.push(part.reduce((t, v, i) => t | (v >> 7 & 1) << i, 0), ...part.map(v => v & 127));
    }
    return out;
  }

  /* The checks of unpack in sysex.c. Returns null if malformed. */
  function unpack(data) {
    const out = [];
    for (let g = 0; g < data.length; g += 8) {
      const len = Math.min(data.length - g, 8);
      if (len < 2) return null;
      const top = data[g];
      if (top >> (len - 1)) return null;
      for (let i = 1; i < len; i++) out.push(data[g + i] | (top >> (i - 1) & 1) << 7);
    }
    return out;
  }

  const checksum = d => (128 - d.reduce((s, v) => s + v, 0) % 128) % 128;

  function packets(slot, body, device, command = CMD.dump) {
    const count = Math.max(1, Math.ceil(body.length / BODY_PER_PACKET)), out = [];
    for (let i = 0; i < count; i++) {
      const d = [slot, i, count, ...pack(body.slice(i * BODY_PER_PACKET, (i + 1) * BODY_PER_PACKET))];
      out.push([0xf0, MANUFACTURER, device, MODEL, command, ...d, checksum(d), 0xf7]);
    }
    return out;
  }

  const request = (device, slot) => [0xf0, MANUFACTURER, device, MODEL, CMD.request, slot, 0xf7];
  const requestAll = device => [0xf0, MANUFACTURER, device, MODEL, CMD.requestAll, 0xf7];

  /* Is this one of our dump packets? Requests echoed by the pipeline are not. */
  function isDump(m, device) {
    return m.length >= 10 && m[0] === 0xf0 && m[1] === MANUFACTURER && m[3] === MODEL && m[m.length - 1] === 0xf7 &&
      (m[4] === CMD.dump || m[4] === CMD.dumpAll) && (device === undefined || m[2] === device);
  }

  /* Collects dump packets into whole patches. feed() returns
   * {slot, body, device} when a patch completes, else null, and throws
   * CodecError on a bad packet. As on the FM-1, nothing partial is kept. */
  class Assembler {
    constructor() { this.parts = new Map(); }
    feed(m) {
      const d = Array.from(m).slice(HEADER, -1);
      const payload = d.length - 4;
      if (d.length < 5 || payload > PAYLOAD_MAX) fail('A dump packet has a bad length.');
      const slot = d[0], index = d[1], count = d[2];
      if (!(slot < PATCHES || slot === EDIT) || count < 1 || count > LIM.packets || index >= count) fail('A dump packet has a bad header.');
      if (checksum(d.slice(0, -1)) !== d[d.length - 1]) { this.parts.delete(slot); fail(`A dump packet for patch ${slot} has a bad checksum.`); }
      if (index === 0) this.parts.set(slot, { count, next: 0, body: [] });
      const part = this.parts.get(slot);
      if (!part || part.count !== count || part.next !== index) { this.parts.delete(slot); fail(`Packets for patch ${slot} are missing or out of order.`); }
      const bytes = unpack(d.slice(3, -1));
      const last = index + 1 === count;
      if (!bytes || !bytes.length || bytes.length > BODY_PER_PACKET || (!last && bytes.length !== BODY_PER_PACKET)) { this.parts.delete(slot); fail(`A dump packet for patch ${slot} is malformed.`); }
      part.body.push(...bytes);
      part.next = index + 1;
      if (!last) return null;
      this.parts.delete(slot);
      return { slot, body: part.body, device: m[2] };
    }
  }

  /* A .syx file: returns {patches: [{slot, body}], errors: [text]}. A later
   * patch for the same slot replaces an earlier one. */
  function parseSyx(bytes) {
    const assembler = new Assembler(), found = new Map(), errors = [];
    let start = -1, messages = 0;
    bytes.forEach((v, i) => {
      if (v === 0xf0) start = i;
      else if (v === 0xf7 && start >= 0) {
        const m = Array.from(bytes.slice(start, i + 1));
        start = -1;
        if (!isDump(m)) return;
        messages++;
        try {
          const done = assembler.feed(m);
          if (done) found.set(done.slot, done.body);
        } catch (e) { errors.push(e.message); }
      }
    });
    if (!messages) errors.push('No FM-1 patch dump in this file.');
    return { patches: [...found].sort((a, b) => a[0] - b[0]).map(([slot, body]) => ({ slot, body })), errors };
  }

  /* A .syx file's bytes for the given patches, as a device dump would be. */
  function buildSyx(items, device) {
    const command = items.length === PATCHES ? CMD.dumpAll : CMD.dump;
    return Uint8Array.from(items.flatMap(({ slot, body }) => packets(slot, body, device, command).flat()));
  }

  const sameBytes = (a, b) => a.length === b.length && Array.from(a).every((v, i) => v === b[i]);

  return {
    PORTS, SYS, VOICE, REMAP, TRIG, LIM, IN, EDIT, PATCHES, CodecError,
    blankPatch, cleanName, msgKey, encode, decode, duplicateRules, validate,
    pack, unpack, checksum, packets, request, requestAll, isDump, Assembler, parseSyx, buildSyx, sameBytes,
  };
}));
