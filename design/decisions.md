# UI decisions

Agreed changes and clarifications to `fm1-screens.json`. Where this file and the JSON disagree, this file wins.

## Port names

- Use TRS / USB / BLE everywhere. The one-letter forms are T / U / B.
- Port fields cycle in the order TRS → USB → BLE.

## Screen navigation

At power-up a splash screen (name, "BY MIDIHAX", version from `MIDIPROC_VERSION` in `src/app/version.h`) shows for 2 s, then the monitor. MIDI is processed meanwhile; the panel buttons wait for the monitor.

Pressing a screen button that owns several screens cycles through them. The first press from another group opens that group's first screen, so HOME always lands on the monitor.

| Button | Cycle order |
|---|---|
| HOME | Midi Monitor |
| EDIT | Knobs Config → Metronome |
| FX | Channel Filter → Message Filter → Message Remap → Channel Remap (pipeline order) |
| SAVE | Patch Select → Sysex Dump |
| ARP | Live Keys |
| SEL | Event Triggers (Edit Trigger and Trigger Messages are opened from it with PLAY; SEL goes back one level) |
| SEQ | Clock Monitor |

- OCT− and OCT+ are lit on every page that uses them: MIDI Monitor (scroll), Knobs Config and the filter pages (step the value), and Live Keys (octave).
- Buttons that open pages have their LEDs lit on every page: HOME, EDIT, FX, ARP, SAVE, SEL, SEQ and GLO (Build Message Rule; GLO again returns).
- The FX order follows the spec's flow: Channel Filter → Message Filter → Remapper Tables → Note Transposer → Channel Remap. If a Note Transposer screen is added, it goes between Message Remap and Channel Remap.
## Colours

- Field labels are ORANGE (`0xFBA5`) and editable values are BLUE (`0x3C9F`) on every screen.
- **Focused field:** inverted, black text (`0x0000`) on a BLUE (`0x3C9F`) background over the field's full width. Only one field is focused at a time. A focused list row uses the same style.
- On Add Message Rule, the header row (P / CH / TYP) uses cyan labels with white values on purpose. It shows the port, channel and type inherited from the Message Remap list, which can't be edited on this page. SELECT skips them and only cycles the blue fields: input value, output value and output channel. To change them, return to the list with FX.

## MIDI Monitor (built)

- Shows input as received, before the pipeline. Newest at the bottom, 100-event buffer.
- MIDI clock is not listed: `CLK` in the header blinks on the first half of each beat while clock arrives. Start resets the blink to the beat.
- Active Sensing (FE) is dropped on arrival: never shown and never passed through. The Message Filter therefore has no ACT entry.
- Notes are shown with 60 = C4. Pitch bend is one signed value (−8192…+8191), and Song Position is one value. SysEx is shown as its byte count, e.g. `SYX 6B`.
- Data 2 starts at column 14 (heading `P CH TYP D1   D2`), not 13 as first designed. Notes such as C#-1 take 4 characters and ran into it.
- REC clears the list in two steps. Its LED is lit while the monitor is shown. The first press makes it flash rapidly; a second press within 5 seconds clears the list, otherwise it returns to steadily lit.
- While scrolled back, the view stays on the same events as new ones arrive, and the amber number in row 1 shows how many rows back you are.
- Rows whose message matched a remap rule (message or channel rule) are dark yellow (`0x8C60`). The pipeline records the match while it processes the message, so the monitor never looks rules up again. A row keeps its colour even if the rule is later deleted.
- Rows a filter blocked (Channel Filter or Message Filter, SysEx included) are dark grey (`0x4A49`), recorded the same way. A blocked message never reaches the remaps, so a row is never both.
- Rows whose message fired an event trigger are teal (`0x4EAE`), recorded the same way. Teal wins over dark yellow when a message both fired a trigger and was remapped.
- **Shortcuts:** the bottom row shown is selected (black on blue, so its fate colour is hidden while selected). Scroll back to select an older message; the view stays put as new MIDI arrives. Three black keys act on it at once, with no menu, and each is lit only when it applies:
  - **C#5 message rule:** notes (NON, NOF, PAT), CC and PC. Opens Add Message Rule with port, channel, type and input from the message and output = input, or the existing rule for that input. Nothing changes until REC saves it.
  - **D#5 channel rule:** any channel message. Opens Add Channel Rule with port and input channel, output = the same channel, or the existing rule.
  - **F5 / G5 rule builder:** put the message (NON, NOF, PAT, CC, PC) into Build Message Rule's IN / OUT and open it (`rule-builder.md`).
  - **F#5 trigger:** NON, NOF, CC and PC. Adds a trigger at once (trigger edits are live): D1 range = the value, D2 0–127 (NON 1–127), Pass Input YES, Fire ENTER, no messages, and opens Edit Trigger. If 32 triggers exist, the list opens showing TRIGGERS FULL.
  - PLAY was the first idea; the keys replaced it, so PLAY still does nothing on the monitor.
- **Search (built, `monitor-search.md`):**
  - OP1–OP5 (F#3, G#3, A#3, C#4, D#4) control the port, channel, type, D1 and D2 searches. A short press turns that search on or off; an 800 ms hold opens its page (a sub-page of the monitor; HOME returns).
  - Searches only hide rows and combine with AND. While any is on, SRCH shows in amber and the OP key LEDs show which are active.
  - Scrolling, the selected row and the shortcuts use the shown rows. `NO MATCHES` shows when nothing passes.
  - Not stored: every search is off at power-up.

## Channel Filter and Message Filter (built)

- FX cycles Channel Filter → Message Filter, then the remaps. Both pages edit one input port at a time; the selected IN PORT is shared, so it stays the same when moving between them.
- **Channel Filter:** fields are IN PORT, then channels 1–16. The lower 16 white keys toggle their channel from any field, and are lit while their channel passes.
- **Message Filter:** system types are filtered per input port; channel types per input port and channel. Field order for SELECT: IN PORT, SYX, MTC, SPP, STA, CON, SS, TR, RST, STP, TCK, EFFECTIVE CH, NON, NOF, CC, PC, PAT, CAT, PB (each column top to bottom). The lower 16 white keys pick the effective channel from any field; its key is lit.
- An OK/NO field flips on each ALGORITHM detent or OCT± press. IN PORT cycles TRS → USB → BLE; EFFECTIVE CH cycles 16 → 1.
- Note On with velocity 0 counts as Note Off (NOF). Active Sensing has no entry: it is dropped on arrival. The FM-1's own control SysEx bypasses the SYX filter.
- Defaults: everything passes.
- The undefined system statuses F4, F5, F9 and FD are dropped on arrival, like Active Sensing: never shown, filtered or passed through.
- **SysEx pass-through:** SysEx up to 128 bytes is forwarded, in order with other messages. Longer SysEx is shown in the monitor but not forwarded: the SDK sends at most 128 bytes per output message.
- BLE settings can be edited, but BLE input isn't connected to the pipeline yet (open question 28).

## Remaps (built)

- FX cycles Channel Filter → Message Filter → Message Remap → Channel Remap. In the pipeline, the message rules run after the filters, then the channel rules.
- **Message rules** change data 1 (note, CC or program number) and set the output channel. A rule's key is input port + channel + type + input value. TYP is **NOTE, CC or PC**. A NOTE rule applies to Note On, Note Off and Poly Aftertouch for that note number, so a remapped note is always released. CAT and PB have no data-1 number, so they aren't offered.
- **Channel rules** move one input port's channel to another channel. They apply to the channel after any message rule, so a message rule's output channel can be remapped again.
- Up to 32 rules per Message Remap list (port + channel + type), from a pool of 256; up to 32 channel rules.
- Lists end with a **+ NEW RULE** row. PLAY on a rule opens it for editing (title EDIT … RULE); PLAY on + NEW RULE adds one. On Message Remap, PLAY with the header focused also adds one. Lists are sorted: message rules by input value, channel rules by port then input channel.
- REC deletes the selected rule (list) or saves (Add Rule page), with the same two-press confirmation as the monitor: the REC LED flashes after the first press; a second press within 5 s confirms. Any other control cancels it. The Add Rule pages also show "REC AGAIN TO SAVE".
- Saving a rule whose input is already in the list replaces that rule; the Add Rule page shows "REPLACES A RULE" in amber first. A full list shows "LIST FULL (32)" in red and REC does nothing.
- FX on an Add Rule page returns to its list without saving.
- Message Remap: SELECT steps P → CH → TYP → each rule → + NEW RULE, then wraps to P. ALGORITHM and OCT± change the focused header field, or move through the list when it is focused (OCT+ moves down). The white keys set CH from any field; its key is lit.
- Add Message Rule: new rules start at input = output = 60 for NOTE (0 for CC and PC), with the list's channel as output channel. For NOTE rules the note names are shown under the values. The white keys set the output channel.
- Channel Remap: SELECT, ALGORITHM and OCT± all move through the list.
- Add Channel Rule: new rules start at USB, channel 1 → 1. The white keys set the focused channel field; its key is lit.
- PLAY is lit on both lists; REC is lit when it would do something.

## Knobs Config (built)

- KNOB 1–4 each keep a value from 0 to 127 and send it as an absolute CC on their own channel whenever turned, from any page. They start at 0, so the first turn sends 1. Defaults: channel 1, CC 20–23 (undefined in the MIDI spec).
- Knob messages go straight to the output, not through the pipeline. CC numbers are limited to 0–119.
- Each knob's value is shown as a cyan bar on the empty row under its fields, and as a number in dim at the right of its KNOB row.
- Controls: SELECT moves the focus through the 8 fields. ALGORITHM and OCT± change the value. Channels cycle 16 → 1; CC numbers stop at 0 and 119. On a channel field the lower 16 white keys pick the channel, and the key for the current channel is lit.

## Live Keys (built)

- Keys play notes on the selected channel with a random velocity from MIN to MAX, straight to the output. At octave 0 the lowest key is F3 (note 53). OCT± shift by 12 within −3…+3; both their LEDs are lit on this page. Held keys light their LEDs.
- Program Change is shown 1–128 and sent 0–127. REC sends Bank Select MSB (CC 0) and LSB (CC 32) when they aren't OFF, then Program Change. The REC LED is lit on this page and blinks off when it sends.
- SELECT moves the focus through CH, MIN, MAX, PROGRAM CHANGE, MSB, LSB. ALGORITHM changes the value; OCT± are the octave here, not value steps. MIN and MAX push each other so MIN ≤ MAX. MSB and LSB go below 0 to OFF.
- Leaving the page sends Note Off for any keys still held.
- Defaults: channel 1, velocity 100–127, octave 0, program 1, bank OFF.

## Patch Select (built)

- Rows: PATCH NUMBER (the slot PLAY and REC act on), STORED (that slot's saved name, or EMPTY), NAME (the edit buffer's name), IN USE (the patch the edit buffer came from, with UNSAVED in amber), a status line and a `PLAY LOAD  REC SAVE` hint. The title shows `*` while there are unsaved changes.
- STORED and IN USE were added to the design: browsing numbers doesn't load anything (Q23), so the page shows both what is stored in the selected slot and what is in use.
- SELECT steps PATCH NUMBER → each of the 12 name characters → back. ALGORITHM and OCT± change the number or the character (space, A–Z, 0–9, dash). The lower 16 white keys pick the patch, except while the name is focused; the selected patch's key is lit.
- PLAY loads at once, without confirmation; unsaved changes are dropped. An empty slot loads the defaults, named `PATCH nn`.
- REC saves in two presses (flashing LED, 5 s), like the other REC actions.
- After a load or save the status line shows the time Flash took, e.g. `SAVED TO 03 (45MS)`, so the MIDI pause can be measured. Errors are red (`FLASH ERROR -n`, or `NO STORAGE`).
- The "unsaved" check compares the edit buffer with a copy of the patch as loaded or saved. It runs on entering the page and after each change there, never per MIDI message.

## Sysex Dump (built)

- PATCH NUMBER is 1–16 or ALL; it starts on the patch in use. With it focused, the lower 16 white keys pick the patch and the selected key is lit.
- REC sends at once, with no confirmation (sending only reads). Its LED flashes while a dump is going out, and the status row shows `SENDING PATCH nn`, then `SENT nn` or `SENT ALL`.
- A dump of a patch number sends the stored patch, not unsaved edits. An empty slot is sent as the defaults.
- ALLOW DUMP REQ and DEVICE ID take effect at once. They are written to Flash when you leave the page, and only if they changed.
- Added to the design: row 12 counts patches received, patches rejected and requests answered (`RX OK:3 BAD:0 REQ:1`), and row 13 shows a Flash error from storing a received patch. This is for checking a patch editor or librarian.
- Dumps go to USB MIDI OUT, the only output.

## Event Triggers (built)

- Design and answers: `event-triggers.md`. Three pages on SEL: Event Triggers (list), Edit Trigger, Trigger Messages.
- Triggers match after the filters and before the remaps. Their messages go straight to the output, unfiltered and unremapped, after the triggering message. All matching triggers fire in list order, at most 64 messages per input.
- Edits apply at once; triggers are stored with the patch (format version 2, `patch-format.md`).
- New triggers: USB, channel 1, PC 0–127, Pass Input YES, Fire ENTER, no messages.
- Trigger Messages shows 11 rows, not 12 as first drawn: row 13 holds `NON WITHOUT NOF` (amber) or `LIST FULL (32)` / `PATCH FULL (256)` (red).
- A message count in red (list, Edit Trigger, Trigger Messages footer) means overlapping triggers before it could use up the 64-message cap, so some of its messages may not be sent.

## Clock lockout, Metronome and Clock Monitor (built)

- Design and settled answers: `clock-and-metronome.md`.
- **Lockout** (always on, after the filters): one input owns the clock and clocks are never merged. START or CONTINUE from another input takes over only while the owner hasn't locked it (between its START/CONTINUE and STOP). An owner silent for 2 s is forgotten. Clock messages it drops show dark grey in the monitor, and the `CLK` blink follows the owner only.
- **Metronome** (EDIT, after Knobs Config): clicks on the headphone/line output while enabled and the clock runs, from any page. Settings are global, saved when leaving the page.
- **MASTER now sets the click volume only.** USB and Bluetooth audio pass through at the computer's or phone's own volume. The click is not sent back to the computer's USB audio input.
- **Clock Monitor** (SEQ): read-only source, RUN/STOP, BPM (4× digits, measured only while shown) and elapsed time.

## Rule Builder (built)

- Design and settled answers Q1–Q10: `rule-builder.md`.
- **Build Message Rule** is a scratchpad (RAM only): GLO opens it from any page and returns to the page it came from. GLO is lit on every page.
- **F5 / G5** on the monitor put the selected row (NON, NOF, PAT, CC, PC) into IN / OUT and open it. **ENV / LFO** do the same on Live Keys (last note or Program Change sent) and Knobs Config (the focused knob's CC), with port USB.
- OUT must have IN's type (`TYPE MISMATCH`). REC (two presses) adds the rule to its Message Remap list, replacing a rule with the same input, then clears the scratchpad.

## Settings storage

- **Built.** A patch holds the filters, remaps, event triggers, knob channels and CCs, Live Keys settings and the name; knob values are not stored. Format and Flash layout: `patch-format.md`.
- Format agreed in `patch-format.md`: power-up loads the last patch used; SysEx dumps overwrite without confirmation; patch number `7F` addresses the edit buffer; the Device ID must match exactly (no `7F` broadcast).

## Controls

- **Channel Remap list:** the keys do nothing. Channels are picked on Add Channel Rule.
- **Message Remap list:** SELECT steps P → CH → TYP → each rule → + NEW RULE. ALGORITHM and OCT± edit the focused header field, or scroll the rules when the list is focused. Keys set the channel. REC deletes the selected rule only while the list is focused.
- **Patch names:** entered with the encoders only. SELECT moves the cursor, ALGORITHM (or OCT±) picks the character from A–Z, 0–9, dash and space, up to 12 characters. Keys select patches 1–16 and are ignored while the name is focused.

## Add Rule pages

- The Add Rule pages are not part of a cycle. PLAY opens them from their list, and FX returns to that list.
- The trigger sub-pages work the same way with SEL: from Trigger Messages it returns to Edit Trigger, and from Edit Trigger to the list.
- When the edit buffer is replaced (a patch load or a received dump), any sub-page returns to its top-level list.
