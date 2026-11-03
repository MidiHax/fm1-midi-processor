# MIDI clock lockout, Metronome and Clock Monitor

Three related features from your spec: one input owns the MIDI clock at a time, a metronome clicks along with it, and a read-only page shows the clock. **Built** (`src/app/clock.c`, `src/app/clock_pages.c`, `src/app/audio_output.c`). The lockout and timing logic is tested on the PC (`tools/host_tests`), and USB clock pass-through is tested on the device. Items marked **Q** were questions; all thirteen are **settled** (the suggestions were accepted, 2026-11-02) and are listed at the end.

Screens: **Metronome** and **Clock Monitor** in `fm1-screens.json`.

Related: `decisions.md` (filters, navigation), `patch-format.md` (storage).

---

## 1. MIDI clock lockout

### Your rules

- Inputs: TRS, USB, BLE.
- The first input that sends **START** takes the lock.
- While locked, clock messages (START, STOP, CONTINUE, tick) from other inputs are dropped.
- **STOP** from the locking input releases the lock; any input can then take it.
- Runs **after** the Message Filter.
- Clocks are never merged at the output.

### Where it sits

```
input -> Channel Filter -> Message Filter -> [clock lockout] -> triggers -> remaps -> output
                                                    |
                                                    +-> clock state (Metronome, Clock Monitor)
```

- Real-time messages have no channel, so the Channel Filter never touches them. The Message Filter's STA, CON, STP and TCK settings apply first. A port whose STA is set to NO can never take the lock.
- Triggers and remaps don't act on clock messages, so their position doesn't matter. The lockout is placed before them to keep the order simple.
- **Monitor:** clock messages dropped by the lockout are shown dark grey, like filtered ones. Only START, STOP and CONTINUE rows are listed, because ticks never are. The `CLK` blink in the header follows the owner's ticks only.

### Gaps in the rules, and the behaviour decided

| Situation | Decided behaviour | Q |
|---|---|---|
| Ticks arrive before any START: at power-up, or from a device that runs its clock while stopped (many drum machines and DAWs do) | The input whose clock passed last is the **owner**. While unlocked, only the owner's ticks pass. The first input to send any clock message becomes the owner. So clocks are never merged, as your summary says. | Q1 |
| CONTINUE from another input while unlocked | Takes the lock, like START. | Q2 |
| The owner disappears without STOP: TRS cable pulled, or USB host closed | After **2 s** with no clock messages from the owner, the lock is released and the owner is forgotten (the run stops, elapsed time pauses). The next input to send any clock message becomes the owner. | Q3 |
| Song Position Pointer (F2) from a non-owner while locked | Dropped too. It belongs to the transport. | Q4 |
| STOP from the owner | Passes, and releases the lock. The owner stays owner for ticks until another input sends START or CONTINUE. | Q1 |
| START from the owner while it already holds the lock | Passes; the lock stays. Elapsed time restarts. | |
| BLE | BLE input isn't connected to the pipeline yet (open question 28). The lockout handles three ports, so BLE works as soon as it is. | |

- **The lockout is always on, not a patch setting** (Q5). It never changes anything unless two inputs send clock.

### Cost

One port number and a timestamp, checked only for status F8–FC and F2. Nothing on the note path.

---

## 2. Metronome (EDIT, after Knobs Config)

EDIT now cycles **Knobs Config → Metronome**.

```
METRONOME

ENABLED:ON

TIME BASE:1/4
BEATS/BAR:4

CLICK FREQ:  1000HZ
1ST BEAT:    1500HZ

VOLUME
==========------          (the MASTER level, on its own row)
CLOCK:USB RUN
BAR:0012 BEAT:3
REC:ON/OFF
```

### Fields

| Field | Values | Default | Notes |
|---|---|---|---|
| ENABLED | ON / OFF | OFF | |
| TIME BASE | 1/4, 1/8, 1/16, 1/32 | 1/4 | One click per note of this length: every 24, 12, 6 or 3 ticks. |
| BEATS/BAR | 1–16 | 4 | **Added to your spec (Q6):** "1st beat" needs a bar length. The first click of each bar uses the 1ST BEAT tone. Counted in time-base clicks: at 1/8 with 8 beats per bar, a 4/4 bar is accented once. |
| CLICK FREQ | 200–4000 Hz, 10 Hz steps | 1000 Hz | ALGORITHM steps 10 Hz; OCT± step 100 Hz. (Q7) |
| 1ST BEAT | 200–4000 Hz, 10 Hz steps | 1500 Hz | Same steps. |

- **Read-only rows:**
  - VOLUME shows the MASTER pot as a bar.
  - CLOCK shows the source and state from the lockout.
  - BAR and BEAT count from START.
- **Controls:** SELECT moves through the five fields, ALGORITHM changes the value and OCT± step it. REC toggles ENABLED from any field, as a quick mute; its LED is lit while enabled. The keys aren't used.
- Settings are written to Flash when you leave the page, and only if they changed.
- **Runs on every page** while ENABLED is on, as your spec says.

### When it clicks

- **Only while the clock runs** (Q8): from START or CONTINUE until STOP. When stopped it is silent, even if ticks keep arriving.
- **Counting:**
  - START resets the count, so the next tick is bar 1, beat 1.
  - CONTINUE resumes from where STOP left off.
  - If a Song Position Pointer arrived while stopped, CONTINUE resumes from that position instead, so the accent stays on the downbeat when a DAW starts mid-song.

### The sound

- **The click:** a short tone at the chosen frequency, about 25 ms with a fast decay so it doesn't thump. The first beat of each bar uses the 1ST BEAT frequency. It's generated in the audio interrupt, which already mixes USB and Bluetooth audio for the headphone/line output (`audio_output.c`).
- **Volume:** set by the MASTER pot (Q9). Today MASTER already scales the USB and Bluetooth audio passing through to the headphone output. MASTER controls the click only, and pass-through audio stays at the host's volume.
- **USB recording:** the click is not sent to the computer's USB audio input (Q10). The FM-1 currently also sends its output mix to the host as a recording input, and a click there would end up in recordings.
- **Timing:** ticks are timestamped in microseconds as they arrive. Each click is scheduled 4 ms after its tick arrived, at an exact sample, so its timing doesn't depend on when the control task got to it. The control task maps microseconds to the audio interrupt's sample count (see *Implementation*). In the PC tests, clicks land within about 80 µs of target, including with the audio clock 100 ppm fast or slow.

### Storage

Metronome settings are **global settings**, not part of a patch (Q11). They describe your setup, not a MIDI routing. Changing patches during a gig shouldn't change the click. The global record goes to version 2 with the new fields; version 1 records load with the defaults.

---

## 3. Clock Monitor (SEQ)

SEQ opens it; its LED is lit on every page, like the other page buttons. **Read-only:** no field takes focus, and SELECT, ALGORITHM, PLAY and REC do nothing.

```
CLOCK MONITOR

SOURCE:USB
STATE:RUN

BPM
120.0                    (4x size)

ELAPSED
01:23:45                 (4x size)
```

| Row | Shows |
|---|---|
| SOURCE | The clock owner: TRS, USB, BLE, or `NONE` before any clock has arrived. |
| STATE | `RUN` in green from START/CONTINUE, `STOP` in dim after STOP. RUN is also when the lock is held (Q12). |
| BPM | From the owner's tick stream, one decimal. `---.-` when no tick has arrived for 1 s. |
| ELAPSED | Hours:minutes:seconds since the last START. Starts from 0 on START, pauses on STOP, carries on from where it paused on CONTINUE. Up to 99:59:59, then it stays there. |

### BPM

- Computed only while this page is shown, as you asked. On entering the page it shows `---.-` until enough ticks have arrived.
- Uses the microsecond tick timestamps. BPM is the average over the last 48 ticks (two beats): 60 000 000 × 24 ÷ (average tick interval in µs). At 120 BPM that is one new reading per beat, settling in about one second. Smooth enough for one decimal, quick enough to follow a tempo change. (Q13)
- It works whether or not the clock is running, because a stopped DAW often still sends ticks.

### Elapsed time

- Tracked all the time, not just while the page is shown, so it is right when you open the page.
- Wall-clock time, not musical time.

---

## Implementation

- **`clock.c`:**
  - Owner port, lock flag and run state; the owner's last clock time for the 2 s release.
  - Tick counter and bar/beat position for the metronome; the elapsed-time accumulator.
  - A ring of the last 48 tick timestamps, filled only while the Clock Monitor is shown.
- **`processor.c`:** after the filters, `if (mp_is_clock_message(status) && !mp_clock_accept(in)) { fate = MP_BLOCKED; return; }`.
- **Tick timestamps:** the input queue already timestamps every message on arrival (`mp_time_us()`, the latency-test timer). `struct mp_event` now carries that `stamp`.
- **Audio time base:**
  - The audio interrupt only counts frames (`mp_audio_frames`, +128 per block). It can't read the time itself, because `fm1_millis()` runs from Flash.
  - `mp_service()` samples (time, frames) many times a second. A sample is never earlier than its block's start, so the estimate follows the earliest-looking samples and creeps later by 1/4096 of each later one, to track any clock drift.
  - The anchor frame is moved only once a minute, so rounding doesn't build up.
  - A click due at time *t* is posted as a frame number.
- **Click synthesis (`audio_output.c`, in RAM):**
  - The sound is a sine from a 256-entry table (filled at start-up, not stored in Flash), with a 32-sample attack and an exponential decay over about 26 ms.
  - It is added after the copy sent to USB recording.
  - Its level comes from `fm1_engine_output_level()` (MASTER), squared for a more even loudness curve.
  - Pass-through audio now uses only the host's playback volume.
- **Pages:** `metronome_page` (EDIT group, after Knobs Config) and `clock_monitor_page` (new SEQ group), in `clock_pages.c`. The Clock Monitor's 4× digits use `ui_big()` in `screen.c`.
- **Storage:** global record version 2, adding enabled, time base, beats per bar and the two frequencies (7 bytes; `patch-format.md`). Version 1 records still load, with the metronome defaults.

---

## Questions (all settled)

All suggestions accepted on 2026-11-02.

| # | Question | Decision |
|---|---|---|
| Q1 | Ticks before any START, or while unlocked: what passes? | **Settled:** Only the **owner's**: the input whose clock passed last, or the first input to send any clock message. Clocks are never merged. |
| Q2 | Does CONTINUE take the lock like START? | **Settled:** **Yes.** |
| Q3 | Release the lock if the owner goes silent without STOP? | **Settled:** **Yes, after 2 s** with no clock messages from it. |
| Q4 | Is Song Position Pointer locked out too? | **Settled:** **Yes**, from non-owners while locked. |
| Q5 | Is the lockout a patch setting, or always on? | **Settled:** **Always on.** |
| Q6 | Add BEATS/BAR, so the 1st-beat tone knows where bars start? | **Settled:** **Yes**, 1–16 in time-base clicks, default 4. |
| Q7 | Frequency range and step? | **Settled:** **200–4000 Hz, 10 Hz steps**; OCT± step 100 Hz. Defaults 1000 Hz / 1500 Hz. |
| Q8 | Click only while the clock runs (START/CONTINUE to STOP)? | **Settled:** **Yes.** |
| Q9 | MASTER today also sets the level of USB and Bluetooth audio passing through to the headphones. Should it set the click only? | **Settled:** **Click only.** Pass-through audio follows the host's own volume. |
| Q10 | Keep the click out of the USB audio sent to the computer? | **Settled:** **Yes**, headphone/line output only. |
| Q11 | Metronome settings: global, or per patch? | **Settled:** **Global.** |
| Q12 | Should the Clock Monitor also say whether the lock is held (between START and STOP) as well as RUN/STOP? | **Settled:** **No.** They're the same thing: RUN means locked. |
| Q13 | BPM averaging window? | **Settled:** **48 ticks (2 beats).** |
