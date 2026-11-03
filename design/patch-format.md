# Patch data and SysEx format

This is the format for patches, global settings, Flash storage and SysEx dumps. All of it is built: the patch body and Flash storage in `src/app/patch.c`, SysEx in `src/app/sysex.c`. `tools/patch_sysex.py` requests and sends dumps from a computer. All its open items are answered (`open-questions.md`, "Sysex Open Questions").

Related: `decisions.md` (behaviour of each page), `open-questions.md` 22–26 (answers this is based on).

---

## What is stored

| Kept in | Contents |
|---|---|
| **Patch** (16 of them) | Name, Channel Filter, Message Filter, Message and Channel Remap rules, Event Triggers, Knobs Config, Live Keys settings |
| **Global settings** (one) | Device ID, Allow Dump Request, last patch, Metronome settings |
| **Not stored** | Knob values (they restart at 0), the MIDI monitor, the page you were on |

- Patches are numbered **1–16** on screen and **0–15** in SysEx and in Flash.
- The **edit buffer** is the patch in use, held in RAM. Editing any page changes the edit buffer only.
  - PLAY on Patch Select loads the selected patch into the edit buffer (Q23).
  - REC on Patch Select (two presses) writes the edit buffer to the selected patch.
  - The title shows `*` while the edit buffer has unsaved changes.
- At power-up the edit buffer is loaded from the last patch used: the last one loaded with PLAY or saved with REC. A SysEx dump doesn't change it.

---

## Patch body, version 2

Version 2 is version 1 with an event-trigger section added at the end. The firmware writes version 2 and still loads version 1 bodies, as patches with no triggers.

A patch is one byte string, the **patch body**. The same bytes go into Flash and, after 7-bit packing, into SysEx. Multi-byte numbers are little-endian. Channels are 0–15 (shown as 1–16), and ports are 0 = TRS, 1 = USB, 2 = BLE.

### Fixed part (91 bytes)

| Offset | Size | Field | Values |
|---:|---:|---|---|
| 0 | 1 | Format version | `2` (`1` accepted on load) |
| 1 | 12 | Name | ASCII `A`–`Z`, `0`–`9`, `-` and space, padded with spaces |
| 13 | 6 | Channel Filter: 3 × uint16, one per port | bit *n* set = channel *n*+1 passes |
| 19 | 6 | Message Filter, system types: 3 × uint16, one per port | bits 0–9: SYX, MTC, SPP, STA, CON, SS, TR, RST, STP, TCK; set = passes |
| 25 | 48 | Message Filter, channel types: 3 ports × 16 channels × uint8, port-major | bits 0–6: NON, NOF, CC, PC, PAT, CAT, PB; set = passes |
| 73 | 8 | Knobs Config: 4 × {channel, CC number} | channel 0–15, CC 0–119 |
| 81 | 7 | Live Keys: channel, velocity min, velocity max, octave, program, bank MSB, bank LSB | channel 0–15; velocities 1–127, min ≤ max; octave −3…+3 as int8; program 0–127 (shown 1–128); MSB/LSB 0–127 or `FF` = OFF |
| 88 | 1 | Channel rule count, *c* | 0–32 |
| 89 | 2 | Message rule count, *m* | 0–256 |

### Rules (variable)

| Offset | Size | Field |
|---:|---:|---|
| 91 | 3 × *c* | Channel rules: {port, input channel, output channel} |
| 91 + 3*c* | 6 × *m* | Message rules: {port, channel, type, input value, output value, output channel}. Type 0 = NOTE, 1 = CC, 2 = PC. Values 0–127. |

### Event triggers (version 2 only, variable)

Straight after the rules. Behaviour: `event-triggers.md`.

| Offset | Size | Field |
|---:|---:|---|
| end of rules | 1 | Trigger count *t* (0–32) |
| +1 | 2 | Trigger message count *n* (0–256) |
| +3 | 10 × *t* | Triggers, in firing order: {port, channel, type, D1 from, D1 to, D2 from, D2 to, flags, message count, reserved `0`}. Type 0 = NON, 1 = NOF, 2 = CC, 3 = PC. Ranges 0–127 with from ≤ to. Flags: bit 0 = Pass Input, bit 1 = Fire ENTER. Message count 0–32. |
| … | 4 × *n* | Messages, each trigger's in order: {type, channel, D1, D2}. D1/D2 are 0–127, or `80` = IN (copy the triggering message's value). D2 is `0` for PC. |

- Size: a version 1 body is 91 bytes with no rules, at most 91 + 96 + 1,536 = 1,723 bytes. Version 2 adds 3 bytes plus the triggers: 94 bytes with nothing in it, at most 1,723 + 3 + 320 + 1,024 = **3,070 bytes**. That fits one 4 KB Flash sector with its record header.
- The trigger message counts must add up to *n*.
- Message rules are written sorted by port, channel, type and input value, so the same rules always give the same bytes.
- These are the firmware's own structures (`struct mp_filters`, `mp_msg_rule`, `mp_ch_rule` in `src/app/midiproc.h`, plus the knob and Live Keys settings), so loading and saving is a copy.
- On load, every field is range-checked. A body with an unknown version, a bad count or an out-of-range value is rejected as a whole, and the edit buffer is left unchanged.
- **Defaults** (an empty slot, or a rejected body): name `PATCH nn`, every filter passes, no rules, no triggers, knobs on channel 1 with CC 20–23, Live Keys on channel 1 with velocity 100–127, octave 0, program 1 and bank OFF.

A later version adds fields at the end and increases the version byte. The firmware keeps reading older versions and fills new fields with their defaults.

---

## Global settings

| Offset | Size | Field | Values |
|---:|---:|---|---|
| 0 | 1 | Format version | `2` (`1` accepted on load) |
| 1 | 1 | Device ID | 0–127 (Sysex Dump page) |
| 2 | 1 | Allow Dump Request | 0 = NO, 1 = YES |
| 3 | 1 | Last patch | 0–15, for power-up |
| 4 | 1 | Metronome enabled (version 2) | 0 = OFF, 1 = ON |
| 5 | 1 | Metronome time base | 0–3 = 1/4, 1/8, 1/16, 1/32 |
| 6 | 1 | Metronome beats per bar | 1–16 |
| 7 | 2 | Click frequency, Hz | 200–4000, a multiple of 10 |
| 9 | 2 | First-beat frequency, Hz | 200–4000, a multiple of 10 |

- Version 1 is the first 4 bytes. It still loads, with the metronome defaults (OFF, 1/4, 4 beats, 1000 Hz, 1500 Hz), and is rewritten as version 2 the next time a global setting changes.
- Global settings are never sent in a dump. They describe this FM-1, not a patch.

---

## Flash layout

The SDK gives raw access to the user Flash partition in 4 KB sectors, with no transactions. Its own notes advise "CRC/versioned records and two sectors if previous data must survive". Writing it erases the stock projects, voices and samples stored there (Q25: accepted).

| Sector | Offset | Contents |
|---:|---:|---|
| 0, 1 | `0x0000`, `0x1000` | Global settings, A and B copies |
| 2–17 | `0x2000`–`0x11000` | Patch slots 0–15, one per sector |

Each sector holds one **record**:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | Magic `FMPX` |
| 4 | 1 | Record type: 1 = global settings, 2 = patch |
| 5 | 1 | Slot (patch 0–15; 0 for globals) |
| 6 | 2 | Body length |
| 8 | 4 | Sequence number (globals: the higher valid copy wins) |
| 12 | *n* | Body |
| 12 + *n* | 4 | CRC-32 of bytes 0 to 11 + *n* |

- A sector without the magic, or with a bad CRC, counts as empty and gives defaults. That includes the stock data still in the partition on first use. Nothing is written until you save.
- Saving a patch erases and rewrites only its own sector. If power fails mid-write, only that patch is lost and comes back as defaults.
- Global settings alternate between their two sectors, so the previous copy survives an interrupted write.
- 18 sectors (72 KB) in all. The firmware checks `fm1_flash_size()` before writing.

---

## SysEx

### Header (your format, Q26)

| Byte | Value |
|---|---|
| Start | `F0` |
| Manufacturer ID | `7D` (reserved for non-commercial use) |
| Device ID | 0–127, from the Sysex Dump page |
| Model ID | `11` |
| Command | see below |
| Data | per command |
| End | `F7` |

| Command | Meaning | Data |
|---|---|---|
| `00` | Single patch dump | Dump packet |
| `01` | All patch dump | Dump packets for patches 0–15, in order |
| `10` | Single patch request | Patch number, 0–15 or `7F` |
| `11` | All patch request | none |

**Patch number `7F` means the edit buffer.** A single dump to `7F` replaces the patch in use without touching Flash (for auditioning from an editor), and a request for `7F` returns the patch in use, including unsaved changes. `7F` isn't valid in an all-patch dump.

The FM-1 answers a message only if its Device ID matches exactly. `7F` is not treated as "any device".

### Dump packets

The SDK limits each outgoing message to **128 bytes**, and a patch can be 3,070 bytes. A dump is therefore a series of packets, each a complete SysEx message with the header above:

| Byte | Field |
|---|---|
| 0 | Patch number, 0–15, or `7F` = edit buffer |
| 1 | Packet index, 0 … count − 1 |
| 2 | Packet count, 1–32 |
| 3 … | Payload: up to 112 bytes, the next ≤ 98 bytes of the patch body, 7-bit packed |
| last | Checksum |

- **Size:** 5 header bytes (`F0 7D` device, model, command) + 3 + 112 + 1 checksum + `F7` = 122 bytes at most. A patch with no rules or triggers fits in one packet (94 bytes → 108 packed). A full patch takes 32.
- **7-bit packing:** each group of up to 7 body bytes becomes one byte holding their top bits (bit *i* = top bit of byte *i*), followed by the 7 bytes with their top bit cleared. The last group may be shorter.
- **Checksum:** the sum of bytes 0 to the end of the payload, then `(128 − sum % 128) % 128`. The sum of those bytes plus the checksum is a multiple of 128 (Roland style).
- **All patch dump (`01`):** the packets for patch 0, then patch 1, and so on to 15. Each patch's packets carry their own patch number, index and count, so an all-patch dump is 16 single dumps under command `01`.
- **Pacing:** the FM-1 sends the next packet only when the USB output has room for it, so normal MIDI is never dropped to make way for a dump.

### Receiving

- Packets are collected per patch. When all *count* packets have arrived with good checksums, the body is unpacked and checked as under *Patch body*.
- A missing, repeated or bad packet discards that patch. Nothing partial is ever applied.
- **Requests** (`10`, `11`) are answered only when Allow Dump Request is YES.
- **Dumps overwrite without confirmation**, so a web-based patch editor can write patches directly:
  - **Single patch dump (`00`) to 0–15:** written to that slot in Flash as soon as the patch is complete. If that slot is the patch in use, the edit buffer is reloaded from it too, and any unsaved changes are lost.
  - **Single patch dump to `7F`:** replaces the edit buffer only. The title shows `*`, as for any unsaved change.
  - **All patch dump (`01`):** each patch is written to its slot as it completes. If one patch is bad, that slot keeps its old contents and the rest are still written.
- **Flash writes wait for the dump to finish.** Received patches are held in RAM and written to Flash once no dump packet has arrived for 100 ms, one slot per control iteration. A patch whose packets stop for 1 s is discarded. So a host can send an all-patch dump without gaps; tested with 18-packet patches sent back to back, and a 32-packet (3,070-byte) patch round-trips intact.
- **Flash writes pause MIDI.** Erasing and writing a sector runs on the control task, so MIDI processing waits while it happens (expected to be a few tens of ms per patch, to be measured). This only happens on a save, a dump to a slot, or a change of patch. A dump to `7F` never writes Flash.
- The FM-1's own SysEx bypasses the Message Filter's SYX setting (Q9), and isn't forwarded to the output.

### Diagnostics (separate)

`F0 7D 4D 50 01 F7` (read) and `F0 7D 4D 50 02 F7` (reset) are the latency diagnostics used by `tools/latency_test.py`. They also use `7D`, but their third and fourth bytes (`4D 50`) sit where a patch message has the Device ID and model `11`. Their model byte is `50`, so the two never clash, even with Device ID `4D`. They could be removed in a release build.

---

## Size and limits summary

| Item | Limit | Why |
|---|---|---|
| SysEx out, per message | 128 bytes | `fm1_midi_send_port` |
| SysEx in, per message | 1,024 bytes | SDK receive buffer |
| Patch body | 3,070 bytes | 32 channel rules, 256 message rules, 32 triggers with 256 messages |
| Dump packets per patch | 1–32 | 98 body bytes per packet |
| Flash used | 72 KB | 18 × 4 KB sectors |
