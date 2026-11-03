# MIDI Monitor search

Filter what the MIDI Monitor **shows**, by port, channel, type and data ranges. From your spec. **Built** (`src/app/monitor.c`, `src/app/monitor_search.c`), and tested on the PC against the real code (`tools/host_tests/monitor`). Items marked **Q** were questions; all nine are **settled** (the suggestions were accepted, 2026-11-02) and are listed at the end.

Screens: **Search Port (OP1)**, **Search Channel (OP2)**, **Search Type (OP3)**, **Search D1 (OP4)** and **Search D2 (OP5)** in `fm1-screens.json`.

Related: `decisions.md` (MIDI Monitor).

---

## Your spec

- The lower five black keys on the Monitor, printed **OP1–OP5** on the panel (F#3, G#3, A#3, C#4, D#4):
  - **Hold:** opens that key's search screen.
  - **Short press:** turns that search on or off.
- **inc** = include in the monitor, **exc** = exclude from it.

| Key | Searches by | Settings |
|---|---|---|
| OP1 | Port | TRS, USB, BLE: each INC or EXC |
| OP2 | Channel | 1–16: each INC or EXC |
| OP3 | Message type | Each type INC or EXC |
| OP4 | Data 1 | Range FROM–TO (0–127), INC or EXC |
| OP5 | Data 2 | Range FROM–TO (0–127), INC or EXC |

---

## Behaviour

### What a search does

- **It only changes what the monitor shows.** The pipeline, the output and the 100-message history are untouched. Every message is still recorded, so turning a search off brings back everything it hid (Q1).
- **Searches combine with AND** (Q2). A message is shown only if every search that is on lets it through. With port USB only and channel 10 only, you see USB channel 10.
- **An off search keeps its settings**, so a short press brings it back exactly as it was.

### Each search

- **Port, Channel, Type:** each item is INC or EXC, and all start as INC. A message is shown if its port, channel or type is INC. With everything INC the search has no effect; set the ones you don't want to EXC.
- **D1 and D2 ranges:**
  - **INC** shows only messages whose value is inside FROM–TO.
  - **EXC** hides messages whose value is inside FROM–TO.
  - The defaults are 0–127 and INC, which have no effect.
- **Messages without that field** (Q3):
  - **Channel search:** system messages (clock, SysEx and the rest) have no channel. They pass the channel search; use the Type search to hide them.
  - **D1 and D2 searches:** a message with no data 1 or data 2 (STA, CAT's D2, and so on) counts as **outside** the range. An INC range hides it and an EXC range keeps it. So "D1 060–060 INC" shows only messages with data 1 = 60.
- **Values:**
  - D1 and D2 are compared as received.
  - Note On with velocity 0 counts as NOF in the Type search, the same as the filters.
  - Pitch bend's D1 and D2 are its LSB and MSB bytes, not the combined value shown in the monitor (Q4).
  - SysEx has no D1 or D2.

### Message types for OP3

16 types, the same names the monitor shows:

| Channel | System |
|---|---|
| NON, NOF, PAT, CC, PC, CAT, PB | SYX, MTC, SPP, SS, TR, STA, CON, STP, RST |

Clock ticks aren't listed, because the monitor never shows them. Active Sensing is dropped on arrival.

### The monitor with a search on

```
MIDI MONITOR SRCH CLK
P CH TYP D1   D2  -3
...
```

- **SRCH** shows in amber in the header while any search is on (Q5).
- **Key LEDs:** OP1–OP5 are lit while their search is on, so you can see at a glance which ones are active. The shortcut keys (C#5, D#5, F#5) keep working as before.
- **Selecting and scrolling** move through the shown rows only. The selected row is the bottom shown row, and the shortcuts act on it. The amber scroll number counts shown rows.
- **Empty result:** if nothing matches, row 2 shows `NO MATCHES` in dim (Q6).

### Keys and timing

- **A hold is 800 ms** (Q7; first 400 ms, raised to 800 ms on 2026-11-02). The search screen opens as soon as the key has been held that long, and stays open after the key is released.
- **A short press** is released before 800 ms. It toggles that search on or off on release.
- **On a search screen:**
  - A short press of its own key toggles the search on or off, the same as on the monitor. The STATE row shows the result.
  - Holding another OP key switches to that search.
  - **HOME** goes back to the monitor (Q8). The search screens are sub-pages of the monitor.
- The keys only act this way on the Monitor and its search screens. Live Keys still plays notes.

### Storage

**Not stored** (Q9). Searches are for looking at what is happening now; they start off at power-up, with all items INC and ranges 0–127.

---

## Screens

All five share a header row, `STATE:ON` / `OFF` on row 2, and the hint `HOLD OP1-5 HOME:BACK` in dim on row 14.

### OP1: Port

```
SEARCH PORT  OP1

STATE:ON

TRS: INC
USB: INC
BLE: EXC
```

### OP2: Channel

```
SEARCH CHANNEL  OP2

STATE:ON

01: INC     09: EXC
02: INC     10: INC
...
08: INC     16: EXC
```

The lower 16 white keys toggle their channel, as on Channel Filter, and are lit while their channel is INC.

### OP3: Type

```
SEARCH TYPE  OP3

STATE:OFF

NON: INC    SYX: EXC
NOF: INC    MTC: EXC
PAT: EXC    SPP: INC
 CC: INC     SS: INC
 PC: INC     TR: INC
CAT: EXC    STA: INC
 PB: INC    CON: INC
            STP: INC
            RST: INC
```

### OP4 / OP5: D1 / D2 range

```
SEARCH D1  OP4

STATE:ON

FROM:060  TO:072
     C4       C5        (note names, dim)
MODE:INC
```

- OP5 is the same for D2, without note names.
- FROM can't pass TO: raising FROM pushes TO, as on Edit Trigger.

### Controls on every search screen

| Control | Action |
|---|---|
| SELECT | Moves the focus through STATE and the fields |
| ALGORITHM, OCT± | Change the focused field: INC/EXC flips, ON/OFF flips, numbers step |
| White keys | OP2 only: toggle channels |
| Its OP key, short press | Search on/off |
| Another OP key, hold | Switch to that search |
| HOME | Back to the monitor |

---

## Implementation sketch

- **`monitor.c`:**
  - The search state: port and type masks, a channel mask, two ranges with modes, and five on/off flags.
  - `shown(entry)` tests one stored entry.
  - Scrolling and the selection walk the ring and skip entries that `shown()` rejects. At most 100 tests per redraw (every 50 ms), each a few comparisons, and only while a search is on.
  - Nothing changes in the pipeline, or on the MIDI path at all.
- **`monitor_search.c` (new):** the five pages, sharing one `update` with a per-page table of fields.
- **Keys:** hold/short-press timing per OP key, from `ui_input.keys`, `keys_pressed` and `now`.

---

## Questions (all settled)

All suggestions accepted on 2026-11-02.

| # | Question | Decision |
|---|---|---|
| Q1 | Does a search only change what the monitor shows, keeping the full history so turning it off restores everything? | **Settled:** **Yes, view only.** |
| Q2 | How do several active searches combine? | **Settled:** **AND:** shown only if every active search lets it through. |
| Q3 | Messages without the searched field? | **Settled:** **Channel:** system messages pass. **D1/D2:** no value counts as outside the range. |
| Q4 | Pitch bend's D1/D2: raw LSB/MSB bytes, or the combined value? | **Settled:** **Raw bytes**, like every other type. |
| Q5 | Show that a search is on? | **Settled:** **`SRCH`** in amber in the header, and OP1–OP5 LEDs lit while their search is on. |
| Q6 | Nothing matches? | **Settled:** **`NO MATCHES`** in dim. |
| Q7 | Hold time? | **Settled:** **800 ms** (first 400 ms, then raised); the screen opens while the key is still held. |
| Q8 | How do you leave a search screen? | **Settled:** **HOME** back to the monitor. Hold another OP key to switch searches. |
| Q9 | Are searches stored? | **Settled:** **No:** off at power-up, not part of a patch. |
