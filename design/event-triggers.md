# Event triggers

An event trigger watches the input for a matching message and, when one arrives, sends a list of other messages. Example: a foot controller sends Program Change 5, and the FM-1 sends a program change, with bank select if needed, to each synth in a rack.

**Built** (`src/app/triggers.c`, `src/app/trigger_pages.c`) and tested on the device. Items marked **Q** were questions, listed at the end with the answers. Q1–Q5, Q8 and Q9 are decided; Q6, Q7, Q10 and Q11 are still open, and the design assumes my suggestion for each. The screens are in `fm1-screens.json`: Event Triggers, Edit Trigger and Trigger Messages.

Related: `patch-format.md` (patch body, SysEx), `decisions.md` (filters and remaps), `test-plan.md`.

---

## What a trigger is

**Trigger (the header):**

| Field | Values | Notes |
|---|---|---|
| Input port | TRS, USB, BLE | |
| Input channel | 1–16 | |
| Type | NON, NOF, CC, PC | NON is Note On with velocity 1–127. NOF is Note Off, including Note On with velocity 0, as in the filters. |
| D1 range | from–to, 0–127 | Note number, CC number or program number |
| D2 range | from–to, 0–127 | Velocity or CC value. Not used for PC. |
| Pass input | YES / NO | NO removes the triggering message from the output. Default YES. **Q2** |
| Fire | EVERY / ENTER | ENTER fires only when the value moves into the D2 range from outside it. Default ENTER. **Q3** |
| Messages | 0–32 | The list below, sent in order |

Your spec listed "D1 range lower" twice. I've taken the second to mean the upper end.

**Message (one row):**

| Field | Values | Notes |
|---|---|---|
| Type | NON, NOF, CC, PC | **Q6**: more types later |
| Channel | 1–16 | |
| D1 | 0–127, or `IN` | `IN` copies the triggering message's data 1. **Q4** |
| D2 | 0–127, or `IN` | Not used for PC. `IN` copies the triggering message's data 2 (for PC, its program). |

### Limits

| Item | Limit |
|---|---|
| Triggers per patch | 32 |
| Messages per trigger | 32 |
| Messages in all triggers | 256 |

These are sized for the rack example: 16 synths, each with bank MSB, LSB and program, is 48 messages. That needs two triggers, or a smaller rack. Each trigger is one "scene", so a foot controller with 32 scenes fits. **Q5:** enough for now.

---

## Behaviour

### Where it sits in the pipeline

```
input -> Channel Filter -> Message Filter -> [triggers match here] -> message remap -> channel remap -> output
                                                       |
                                                       +-> trigger messages ---------------------------> output
```

- **Triggers match after the filters,** so a blocked message never fires a trigger. They match the values as received, before any remap, which are the values the monitor shows.
- **The triggering message** continues through the remaps as usual if Pass Input is YES. With NO, it goes no further.
- **Trigger messages go straight to the output.** They aren't filtered or remapped and can't fire other triggers, so there are no loops. They are sent exactly as listed on the Trigger Messages page.
- **Order:** the triggering message first (if passed), then each matching trigger's messages, in row order.
- **Several triggers can match one message.** All of them fire, in list order. A foot controller PC can fire one trigger for the synths and another for the lighting desk. The firmware caps the output of one input message at 64 messages; anything beyond the cap is not sent, and the trigger's list shows it in red (**Q7**).

### Matching

A message matches a trigger when all of these hold:
- Port and channel are equal.
- The type matches. NON is Note On with velocity above 0. NOF is Note Off, or Note On with velocity 0.
- Data 1 is within the D1 range (inclusive).
- Data 2 is within the D2 range (inclusive). PC has no data 2, so this check is skipped for it.

A range whose "from" is above its "to" never matches. The editor stops you entering one; it's mentioned only so that SysEx uploads behave predictably.

**Fire mode ENTER** (**Q3**) is for continuous controllers. Take a pedal on CC 11 with the range 100–127: EVERY fires on each value in that range as the pedal moves, perhaps 30 times in one press. ENTER fires once as the pedal crosses into the range, and again only after it has left. The FM-1 remembers the last value it saw for each trigger to do this, and the memory resets when a patch loads. For Note On, Note Off and PC, ENTER behaves like EVERY, so ENTER is the default for new triggers.

### Timing

- Trigger messages go out right after the triggering message, in the same pass, so they arrive within about a millisecond.
- **Q8: no gap for now.** Some older synths miss messages that arrive too close together. A per-trigger gap (0–100 ms between messages) can be added later if needed. It would be the biggest change to this design, because the messages would then go out over several control iterations instead of at once. The reserved byte in each trigger record leaves room for it.

### Hanging notes

A trigger can send a Note On with no Note Off. The FM-1 doesn't track these. The Trigger Messages page shows a hint, `NON WITHOUT NOF`, when a list has more Note Ons than Note Offs.

### Editing

- Triggers are part of the patch, like filters and remaps. Changes apply at once and are stored when you save the patch on Patch Select.
- **There is no draft and save step, unlike the Add Rule pages.** A trigger can hold 32 messages, and losing that much work to an accidental button press would hurt. Edits therefore apply as you make them. A half-edited trigger fires while you work on it, the same as a half-edited filter. **Q9: decided.**
- **New triggers** start as USB, channel 1, PC, D1 0–127, Pass Input YES, Fire ENTER and no messages. Without messages, a trigger sends nothing.

### Monitor

- **Q10:** input messages that fired a trigger could be shown in their own colour, e.g. teal `0x4EAE` from the palette, the same way remapped and blocked rows are now. The colour would be recorded once in the pipeline, at no cost to the monitor.
- Trigger messages aren't shown, because the monitor lists input.

---

## Screens

All three pages are in `fm1-screens.json`. Their button is **SEL (Q1)**: it opens Event Triggers, and pressing it again from a sub-page goes back one level. Its LED is lit on every page, like the other page buttons.

### Event Triggers (list)

```
EVENT TRIGGERS
P CH TYP D1      #
U 01 PC  000-127 04
U 01 PC  005-005 12
T 10 NON 036-036 02
U 01 CC  064-064 01
+ NEW TRIGGER
```

- One row per trigger: port letter, channel, type, D1 range and number of messages. The rows are in firing order.
- 12 rows are visible, and the list scrolls.
- **Controls:**
  - SELECT, ALGORITHM and OCT± move the selection.
  - PLAY opens the selected trigger. On + NEW TRIGGER, it adds a trigger and opens it.
  - REC deletes the selected trigger, with a second press to confirm.
- Footer: `TRIGS:04 MSGS:019`, the triggers in the patch and the messages in all of them.

### Edit Trigger

```
EDIT TRIGGER 02

PORT:USB   CH:01
TYPE:PC

D1 FROM:005 TO:005
                        (note names here for NON/NOF)
D2 FROM:--- TO:---

PASS INPUT:YES
FIRE:ENTER

MESSAGES:12

PLAY:MSGS  REC:TEST
```

- **Fields:** SELECT moves through PORT → CH → TYPE → D1 FROM → D1 TO → D2 FROM → D2 TO → PASS INPUT → FIRE. The D2 fields are skipped and shown as `---` for PC.
- **Changing values:**
  - ALGORITHM and OCT± change the focused field.
  - Keys set CH.
  - FROM can't go above TO: raising FROM pushes TO up, and lowering TO pushes FROM down.
- **Note names:** for NON and NOF, a dim line under each D1 field shows the note names, e.g. `C4-C4`.
- **PLAY** opens Trigger Messages.
- **REC sends the trigger's messages now, as a test,** without needing the foot controller. `IN` values use the trigger's lower range ends. This matches REC on Live Keys and Sysex Dump, which also send. The LED blinks off when it sends.
- **SEL** returns to the list.

### Trigger Messages

```
TRIGGER 02 MESSAGES
## TYP CH D1  D2
01 CC  01 000 002
02 CC  01 032 000
03 PC  01 005 ---
04 PC  02 IN  ---
+ ADD MESSAGE
...
MSGS:04/32 ALL:019
```

- One row per message, sent top to bottom. 11 rows are visible; row 13 holds the hint or a red LIST FULL / PATCH FULL.
- **Moving and editing:**
  - SELECT moves cell by cell: TYP → CH → D1 → D2 of each row, then on to the next row, and finally + ADD MESSAGE.
  - ALGORITHM and OCT± change the focused cell.
  - Keys set the channel when a CH cell is focused.
  - D1 and D2 step `IN`, 0, 1, …, 127, with `IN` below 0.
- **PLAY** inserts a copy of the focused row below it, with the focus moved to the copy, so a series is quick to build: copy, then change one value. On + ADD MESSAGE, PLAY adds a CC 0 on channel 1.
- **REC** deletes the focused row, with a second press to confirm.
- **Footer:** this trigger's count and the patch-wide count, red when the cap could cut messages off (**Q7**). `NON WITHOUT NOF` appears in amber on row 13 when it applies.
- **SEL** returns to Edit Trigger.

---

## Data

### In RAM

```c
struct mp_trigger {
    uint8_t port, channel, type;       /* type: TRIG_NON, TRIG_NOF, TRIG_CC, TRIG_PC */
    uint8_t d1_lo, d1_hi, d2_lo, d2_hi;
    uint8_t flags;                     /* bit 0 pass input, bit 1 fire on enter */
    uint8_t first, count;              /* messages: pool[first .. first+count-1] */
    uint8_t last_d2;                   /* ENTER mode: last value seen, 0xFF none */
};
struct mp_trigger_msg {
    uint8_t type, channel, d1, d2;     /* d1/d2: 0..127, or 0x80 = IN */
};
```

- The messages are one pool of 256, stored per trigger in order. Inserting a message moves the later ones up. That is at most 256 × 4 bytes, and happens only while editing.
- **Fast skip:** a 16-bit mask per port and type records which channels have any trigger. Messages with no possible trigger cost one bit test, so the latency of normal traffic doesn't change.
- **Output queue:** the pipeline reserves room for one input's worst case before processing it. That is now 4 messages and becomes 64 (`MP_MAX_EMIT`) only when triggers exist. The queue holds 256.

### Patch body, version 2

Version 2 appends a trigger section after the version 1 rules. A version 1 body loads with no triggers.

| Offset | Size | Field |
|---:|---:|---|
| end of v1 | 1 | Trigger count *t* (0–32) |
| +1 | 2 | Message count *n* (0–256), uint16 |
| +3 | 10 × *t* | Triggers: port, channel, type, d1 from, d1 to, d2 from, d2 to, flags, message count, reserved 0 |
| … | 4 × *n* | Messages: type, channel, d1, d2 (`80` = IN), each trigger's in order |

- The byte at offset 0 becomes `2`.
- **Size:** at most 1,723 + 3 + 320 + 1,024 = **3,070 bytes**. That still fits one 4 KB Flash sector with its record header, and takes at most 32 SysEx packets (`count` is still one byte).
- **Checks on load:**
  - Ports, channels, types and ranges are valid; ranges have from ≤ to.
  - Each trigger has at most 32 messages, and the messages add up to *n*.
  - Message values are 0–127 or `80`.
  - Flag bits other than 0–1 are 0.
  - D2 bytes in PC messages are 0.
- **7-bit SysEx:** `80` is packed the same as any other byte value. No change is needed.
- **Received-patch buffer:** the FM-1 holds up to 16 received patches in RAM until it writes them to Flash. This grows from 27 KB to 48 KB. The firmware has about 175 KB of RAM to spare above the SDK's minimum.

---

## Testing

The test plan (`test-plan.md`) extends naturally:
- The reference model gains triggers: matching, Pass Input, ENTER memory, `IN` values and output order.
- Hand-written cases cover:
  - Range edges: from, to, one outside each.
  - Velocity-0 Note On matching NOF and not NON.
  - PC ignoring D2.
  - Several triggers matching one message.
  - A filtered message not firing.
  - Pass Input NO.
  - ENTER on a CC sweep: up, down and up again.
  - `IN` copies.
  - The 64-message cap.
- Generated cases add random triggers to the random patches.
- **Fence:** the fence (a request for the edit buffer) still works, because trigger output comes out before the request's reply.

---

## Questions

| # | Question | Answer |
|---|---|---|
| Q1 | Which button opens Event Triggers? | **Decided: SEL.** |
| Q2 | Should a trigger be able to stop the triggering message (Pass Input NO)? | **Decided: yes,** default YES. |
| Q3 | Add the ENTER fire mode? | **Decided: yes,** and it is the default for new triggers. |
| Q4 | Add the `IN` value, which copies the trigger's data? | **Decided: yes.** |
| Q5 | Are 32 triggers, 32 messages each and 256 in total enough? | **Decided: enough for now.** |
| Q6 | Row types: only NON, NOF, CC and PC? | *Open.* Suggestion: yes for now. Pitch Bend, Aftertouch and SysEx rows could come later. SysEx is the hard one, because each row would need up to 128 bytes. |
| Q7 | Cap on what one input message can send? | *Open.* Suggestion: **64.** It keeps a mistaken setup, such as 10 triggers matching one message, from flooding the output. |
| Q8 | A gap between trigger messages, for slow synths? | **Decided: no gap for now.** |
| Q9 | Edits apply live, with no save step on the trigger pages? | **Decided: yes.** Saving the patch is the save step. |
| Q10 | Colour input rows that fired a trigger in the monitor? | *Open.* Suggestion: yes, teal `0x4EAE`, recorded like the remapped and blocked colours. |
| Q11 | Copy a trigger? | *Open.* Suggestion: not in the first version. PLAY on + NEW TRIGGER while a trigger is selected could start from a copy of it. |
