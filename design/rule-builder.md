# Rule Builder

A scratchpad for building **message remap rules** from messages you have seen or sent. You pick the input and the output from real messages, and REC adds the rule. From your spec. **Built** (`src/app/rule_builder.c`), and tested on the PC against the real code (`tools/host_tests/monitor`). Items marked **Q** were questions; all ten are **settled** (the suggestions were accepted, 2026-11-02) and are listed at the end.

Screen: **Build Message Rule** in `fm1-screens.json`.

Related: `decisions.md` (MIDI Monitor shortcuts, Message Remap), `patch-format.md` (message rules).

---

## Your spec

- **F5 on the MIDI Monitor:** opens Build Message Rule and puts the selected row in the **input**: P, CH, TYP and INPUT VALUE. Channel messages only.
- **G5 on the MIDI Monitor:** opens Build Message Rule and puts the selected row in the **output**: OUTPUT VALUE. Channel messages only.
- **GLO:** opens Build Message Rule.
- **The Build Message Rule screen:**
  - Is a scratchpad. It isn't saved with the patch and is lost at reboot.
  - Once both IN and OUT are set, REC adds the rule to its list, and the screen is cleared.
- **Live Keys and Knobs Config** get similar shortcuts, so the messages they send can be used to build rules.

---

## What a message rule can hold

A message rule today (`decisions.md`, Message Remap):

| Part | Fields |
|---|---|
| Input (the rule's key) | Port, channel, type (NOTE, CC or PC), input value (data 1) |
| Output | Output value (data 1), output channel |

- The output keeps the input's type. A NOTE rule changes the note number, a CC rule the CC number and a PC rule the program.
- A NOTE rule covers Note On, Note Off and Poly Aftertouch, so a remapped note is always released.
- CAT and PB have no data-1 number, so there are no rules for them.

This shapes two choices below. The shortcuts can only use NON, NOF, PAT, CC and PC (Q2). The output must have the same type as the input (Q1).

---

## Behaviour

### The scratchpad

- **Two halves, IN and OUT.** Each half is either **empty** (`---`) or **set**.
  - IN holds port, channel, type and value.
  - OUT holds value and channel. Its type always follows IN.
- **Setting a half from a message** fills all of its fields, and replaces whatever that half held.
  - Note On, Note Off and Poly Aftertouch become NOTE. CC is CC, and PC is PC.
  - OUT CH comes from the message's channel, so a G5 on a channel-2 message sends the rule's output to channel 2.
- **Order doesn't matter.** You can set OUT first, then IN.
- **The fields can be edited** on the screen with SELECT, ALGORITHM and OCT±, so you can fix a value or build a rule by hand (Q3). Changing an empty half sets it from defaults:
  - IN starts as USB, channel 1, NOTE 60.
  - OUT starts as IN's value and channel.
- **Kept until reboot** (Q9). Changing page, loading a patch or receiving a dump leaves it as it was.

### Saving with REC

- **REC is lit** when both halves are set and the types match. It saves in two presses, like every other REC save: the first press flashes the LED, and a second press within 5 s confirms. Any other control cancels.
- **The rule goes into its list:** the Message Remap list for IN's port + channel + type, the same list Add Message Rule saves to. It applies at once, like any rule edit, and is stored when you save the patch.
- **A rule with the same input is replaced.** `REPLACES A RULE` shows in amber first, the same as on Add Message Rule.
- **A full list stops the save.** `LIST FULL (32)` or `PATCH FULL (256)` shows in red and REC does nothing.
- **After a save** the screen is cleared (both halves empty) and stays open, and the status row shows `ADDED: USB 01 NOTE` in green for 3 s (Q4).

### Status row (row 9)

| Shows | When |
|---|---|
| Blank | Both halves empty (row 13 has the hint) |
| `NEEDS OUT` / `NEEDS IN` (dim) | One half set |
| `REC TO ADD` (dim) | Both set, ready to save |
| `TYPE MISMATCH` (red) | The halves came from different types. REC does nothing (Q1). |
| `REPLACES A RULE` (amber) | IN's list already has a rule for that input |
| `LIST FULL (32)` / `PATCH FULL (256)` (red) | No room |
| `ADDED: USB 01 NOTE` (green) | For 3 s after a save |

How a mismatch is tracked: OUT remembers the type it was set from. Setting IN, or editing it, re-checks the match. Editing OUT's fields keeps its type.

### Getting there and back

- **GLO** opens the screen from any page. GLO's LED is lit on every page, like the other buttons that open pages.
- **GLO on the screen** goes back to the page you came from (Q5). The other page buttons (HOME, FX, and so on) work as usual.
- The screen is not part of any button's cycle.

---

## Shortcuts

### MIDI Monitor: F5 and G5, or ENV and LFO

| Control | Puts the selected row into |
|---|---|
| **F5** or **ENV** | IN: port, channel, type, value |
| **G5** or **LFO** | OUT: value, channel |

ENV and LFO work as on Live Keys and Knobs Config, so they fill the builder on every page that can feed it.

- Like C#5, D#5 and F#5, each acts at once and is **lit only when it applies**: the selected row is NON, NOF, PAT, CC or PC (Q2).
- Both open the screen, as your spec says (Q6). GLO returns to the monitor with the same row selected and the scroll position unchanged, ready for the next key.
- With a monitor search on, they act on the selected shown row, the same as the other shortcuts.
- **C#5 stays.** It opens Add Message Rule for one message with output = input. F5 and G5 are for building a rule from two different messages (Q10).

### Live Keys and Knobs Config: ENV and LFO

F5 and G5 can't be used on these pages:
- On Live Keys they are played as notes.
- On Knobs Config, F5 and G5 are the channel 15 and 16 keys.

Instead: **ENV → IN** and **LFO → OUT** on both pages (Q7). Each is lit when there is a message to take (always on Knobs Config; on Live Keys after the first key or REC), and works like F5 and G5 on the monitor: set that half and open the screen.

| Page | Message taken |
|---|---|
| Live Keys | The last message the page sent: the last key played (NOTE, the Live Keys channel, the note number after the octave shift), or the last Program Change sent with REC (PC, its program). Nothing before the first key or REC press. |
| Knobs Config | The focused knob: CC, its channel and its CC number. You don't need to turn it. |

- **Port:** these messages don't come from an input port. IN takes **USB**, the common case. Change it on the screen if needed (Q8).
- A typical use: on the monitor, F5 on an incoming note. GLO back, then ARP to Live Keys, play the target note, and LFO. The rule is ready for REC.

---

## Screen

```
BUILD MESSAGE RULE
IN
P:USB CH:01 TYP:NOTE
VALUE:060
      C4
OUT
CH:01 VALUE:062
            D4

REPLACES A RULE

REC AGAIN TO SAVE

F5/ENV:IN G5/LFO:OUT
```

- Rows 1 and 5: the `IN` and `OUT` headings in cyan.
- Labels are orange and values blue, with the usual focus style. An empty half shows `---` in dim for every value.
- For NOTE, the note names are shown in dim under the values.
- OUT's type isn't shown. It always follows IN, and `TYPE MISMATCH` says when it doesn't.
- Row 11: `REC AGAIN TO SAVE` while the save is armed.
- Row 13: the hint, in dim.

### Controls

| Control | Action |
|---|---|
| SELECT | Moves the focus: IN P → CH → TYP → VALUE → OUT CH → VALUE |
| ALGORITHM, OCT± | Change the focused field. Port and type cycle, channel cycles 16 → 1, values 0–127. Changing an empty half sets it from defaults. |
| White keys | Set the focused CH field (IN or OUT). The current channel's key is lit. |
| REC | Adds the rule (two presses) |
| GLO | Back to the page you came from |

---

## Implementation

- **`rule_builder.c` (new):**
  - The scratchpad struct: `in_set`, `out_set`, a `struct mp_msg_rule`, and `out_type`. It lives in RAM and is zeroed at boot.
  - The page. It saves through `mp_msg_rule_save` (replace = the existing index from `mp_msg_rule_find`), with the shared two-press `ui_confirm`.
  - `mp_builder_set_in(port, channel, type, value)` and `mp_builder_set_out(type, channel, value)`.
- **`monitor.c`:** two more entries in the shortcut table (F5 = key 24, G5 = key 26), applying to NON/NOF/PAT/CC/PC.
- **`live_keys.c`, `knobs.c`:** remember the last message sent (Live Keys). ENV and LFO call the builder.
- **`screen.c`:** GLO opens the page from anywhere (`ui_open_builder`, which remembers the page) and returns from it, and its LED is lit. A patch load while on the builder makes GLO return to the top-level page instead of a sub-page.
- **`monitor.c`:** returning with GLO (`ui_returning_from_builder`) keeps the scroll position, and new events keep the same row selected.
- **Nothing on the MIDI path changes.** The builder only calls the existing rule save.
- **Host tests:** `tools/host_tests/monitor` covers F5/G5, ENV/LFO on the monitor, GLO back, mismatch, save and clear, ADDED, defaults, white keys, cancel, replace and LIST FULL (52 checks with the search tests).

---

## Questions (all settled)

All suggestions accepted on 2026-11-02.

| # | Question | Decision |
|---|---|---|
| Q1 | OUT from a different type than IN (e.g. IN a note, OUT a CC)? Rules can't change type today. | **Settled:** **Same type only for now.** Show `TYPE MISMATCH` and keep REC dark. Cross-type rules (note → CC and so on) would need a new rule format and decisions about velocity and Note Off, so they'd be a separate feature. |
| Q2 | Which messages do the shortcuts take? Your spec says channel messages. | **Settled:** **NON, NOF, PAT, CC and PC.** CAT and PB have no data-1 number, so rules can't use them. The keys stay dark on them. |
| Q3 | Can the fields be edited on the screen? | **Settled:** **Yes**, with the usual controls, so a value can be nudged or a rule built by hand from GLO. |
| Q4 | After REC, stay on the cleared screen? | **Settled:** **Yes**, with `ADDED: USB 01 NOTE` for 3 s. Go to the list with FX if you want to see it. |
| Q5 | How do you leave the screen? | **Settled:** **GLO again** returns to the page you came from. Other page buttons work as usual. |
| Q6 | Should F5/G5 open the screen, or stay on the monitor? | **Settled:** **Open it**, as your spec says. GLO brings you back with the selection unchanged. |
| Q7 | Which controls on Live Keys and Knobs Config? F5/G5 are notes or channel keys there. | **Settled:** **ENV → IN, LFO → OUT**, lit when there is a message to take. |
| Q8 | Port for Live Keys and Knobs messages, which have no input port? | **Settled:** **USB**, editable on the screen. |
| Q9 | Does the scratchpad survive page changes and patch loads? | **Settled:** **Yes.** Only a reboot or a save clears it. |
| Q10 | Keep the C#5 shortcut (Add Message Rule from one message)? | **Settled:** **Keep it.** It is quicker for an output that is just a different number. |
