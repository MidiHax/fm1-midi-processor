# Automated test plan

A Python test suite that checks the FM-1's MIDI pipeline on the real device, over USB. Each test uploads a patch designed for it, sends MIDI in, and compares what comes out with what a reference model of the pipeline predicts. Nothing here is built yet.

Related: `patch-format.md` (patch body and SysEx), `decisions.md` (filter and remap behaviour), `open-questions.md` 6–21.

---

## How a test works

1. **Load a test patch into the edit buffer.** The suite sends the patch to patch number `7F`, so nothing is written to Flash and the user's stored patches are untouched.
2. **Check it loaded.** It requests `7F` back and compares the body with what it sent.
3. **Send a batch of MIDI messages** to the FM-1's USB input.
4. **Send a fence:** a single patch request for `7F`. Requests go through the same input queue as MIDI, and the reply is only sent once the output queue is empty. So when the reply arrives, everything the batch produced has already come out.
5. **Compare** the messages received before the fence with the model's prediction for the batch, message by message and in order. Missing, extra, changed and reordered messages all fail the test.

The fence makes the "nothing comes out" checks reliable. Without it, a test could only wait and hope that a blocked message wasn't merely late.

### The reference model

`model.py` is a plain Python version of the pipeline:

> Channel Filter → Message Filter → message rules → channel rules

It works from the same patch description the test uploads. Given a patch and an input message, it returns the expected output: nothing, the message unchanged, or the remapped message. It follows the rules in `decisions.md`:

- Note On with velocity 0 counts as Note Off (NOF) for filtering.
- A NOTE rule covers Note On, Note Off and Poly Aftertouch.
- A message rule sets data 1 and the output channel. The channel rule then applies to the resulting channel.
- Active Sensing (FE) and the undefined F4, F5, F9 and FD are always dropped.
- SysEx of up to 128 bytes is forwarded if SYX passes; longer SysEx is never forwarded.
- The FM-1's own patch SysEx is never filtered or forwarded.

Two kinds of test use the model:

- **Explicit cases** with hand-written expectations, one per rule in `decisions.md`. The model is checked against these too, so a wrong model can't hide a wrong device.
- **Generated cases:** random patches and random message streams, with the expected output taken from the model. These find combinations nobody thought to write down. A fixed random seed makes every failure repeatable.

---

## Layout

```
tools/
  fm1lib/                 shared library (patch_sysex.py moves its code here)
    port.py               open the FM-1 ports (waits 50 ms after opening), send, collect
    sysex.py              header, 7-bit packing, checksums, packets, reassembly
    patch.py              Patch class: fields and rules -> body bytes, and back
    model.py              reference pipeline
  tests/
    conftest.py           device fixture; backs up and restores the edit buffer
    test_codec.py         offline: packing, checksums, body encoding (no device)
    test_model.py         offline: the model against the explicit cases
    test_sysex.py         dumps, requests, rejection
    test_channel_filter.py
    test_message_filter.py
    test_remap.py
    test_pipeline.py      generated cases, filters and remaps together
    test_storage.py       Flash slots (opt-in)
    test_latency.py       latency limits (opt-in)
```

- The suite runs with pytest: `python -m pytest tools/tests`. The offline tests run without the FM-1 connected.
- **Session setup:** `conftest.py` opens the ports once, checks the device answers a `7F` request, and saves the edit buffer. At the end it restores the edit buffer, so a test run leaves the FM-1 as it found it.
- **Settings:** `--device N` sets the Device ID. Allow Dump Req must be YES, because the fence and the load check depend on requests. If no request is answered, the suite stops at once and says why.

---

## Test patches

A `Patch` object is built in Python and turned into body bytes by `patch.py`, for example:

```python
p = Patch.default()
p.channel_filter(USB, pass_channels={1, 2, 10})
p.voice_filter(USB, channel=10, block={NON, NOF})
p.message_rule(USB, channel=1, type=NOTE, inp=60, out=72, out_channel=2)
p.channel_rule(USB, inp=2, out=16)
```

Channels are written 1–16 in tests, as on screen. The library converts them.

Most tests need only a few rules. Each test module builds its own patches, so a failure points to one feature. A few fixed patches cover the limits:

- **Full lists:** 32 message rules in one list, and 256 rules in total.
- **Full channel rules:** 32 channel rules.
- **Largest body:** 3,070 bytes (version 2, with 32 triggers and 256 trigger messages), sent as 32 packets.

These also check that a full patch loads and comes back unchanged.

---

## What is tested

### SysEx and patches (`test_sysex.py`)

| Case | Expected |
|---|---|
| Edit-buffer dump, then request it back | The same body, byte for byte (rules are returned in canonical order, so the test sends them that way) |
| Largest body (32 packets), with and without gaps between packets | Loaded intact |
| Each packet damaged in turn: bad checksum, wrong index, missing packet, repeated packet, wrong count, bad 7-bit packing | Rejected; the edit buffer is unchanged |
| A body with each field out of range in turn: version, name character, filter bits, knob CC 120, velocity min above max, octave ±4, bank 128, rule port 3, rule count too high, length mismatch, 33 rules in one list | Rejected; the edit buffer is unchanged |
| Packets stop half way, then a new dump starts after 1 s | The partial patch is dropped and the new one loads |
| A patch message with another Device ID | Not acted on. If SYX passes, it is forwarded like any other SysEx |
| Our own request or dump with SYX blocked | Still answered (it bypasses the filter) and not forwarded |
| Request for patch 17, unknown command | No reply, nothing forwarded |

### Channel Filter (`test_channel_filter.py`)

- For each of the 16 channels, a patch passing only that channel. Each voice type is sent on all 16 channels, and exactly one channel's messages come out.
- System messages pass whatever the channel settings are.
- Channel filters set for TRS or BLE don't affect USB input.

### Message Filter (`test_message_filter.py`)

- **System types:** each of the 10 types blocked on its own, with all 10 sent. Exactly the blocked one is missing. The types and the messages that test them are SYX, MTC (F1), SPP (F2), STA (FA), CON (FB), SS (F3), TR (F6), RST (FF), STP (FC) and TCK (F8).
- **Voice types:** each of the 7 types blocked on one channel, with every type sent on that channel and on a neighbouring channel. Only the blocked type, on that channel only, is missing.
- **Velocity 0:** Note On with velocity 0 follows the NOF setting, not NON.
- **Always dropped:** FE, F4, F5, F9 and FD, with every filter passing.
- **SysEx size:** 128 bytes is forwarded, and 129 and 300 bytes are not.
- **Order:** the Channel Filter applies before the Message Filter. A blocked channel stays blocked whatever its type settings.

### Remaps (`test_remap.py`)

- **NOTE rules:** Note On, Note Off and Poly Aftertouch are remapped. Note On with velocity 0 is remapped too.
- **CC and PC rules:** each remaps only its own type. Channel Aftertouch and Pitch Bend are never remapped by a message rule.
- **Output channel:** a rule's output channel is applied, and data 2 is kept.
- **Lookup key:** a rule applies only to its own port, channel, type and input value. The same rule on TRS doesn't touch USB input.
- **Channel rules:** applied after message rules, to the channel the message rule produced. A channel rule alone moves every voice type, CAT and PB included.
- **Untouched messages:** system messages pass both stages unchanged.
- **Hanging notes:** a Note On and its Note Off go through the same rule, so a note always gets released.
- **Full lists:** with 32 rules in one list, every input value maps correctly.

### Generated cases (`test_pipeline.py`)

- A few hundred random patches: random filters, up to 256 message rules and up to 32 channel rules.
- Each gets a stream of a few thousand random messages: every voice type on every channel with random data, system messages, and the always-dropped bytes.
- Messages go in batches of at most 64, with a fence after each batch, to stay inside the FM-1's 256-message input queue.
- The output must match the model exactly.
- **Run time:** about 1–2 ms per message, so roughly a minute for the default run. `--long` runs more.

### Storage (`test_storage.py`, opt-in with `--flash`)

These write Flash, so they don't run by default.

- **Back up first.** The test saves all 16 slots before it starts and writes them back at the end.
- **Single dump to a slot:** a request returns the same body. If it's the slot in use, the edit buffer changes too.
- **All-patch dump of 16 patches without gaps:** all 16 are stored.
- **A slot that isn't in use:** a dump to it leaves the edit buffer as it was.

### Latency (`test_latency.py`, opt-in with `--latency`)

- The existing `latency_test.py` measurement, run as a test. It fails if the FM-1's own figures go over set limits: mean under 200 µs and max under 1 ms (today about 125 µs and 430 µs).
- It runs with an empty patch and again with a full one (256 rules), so rule lookups can't slow MIDI unnoticed.

---

## What these tests can't reach

| Not covered | Why | Instead |
|---|---|---|
| TRS input | The tests can only send on USB | Optional: a USB MIDI interface's DIN out into the FM-1's TRS in, selected with `--trs-port`. The same tests then run on TRS too, including running status, which USB input doesn't use. |
| BLE input | Not connected yet (Q28) | — |
| Screens, LEDs, monitor colours | Not visible over MIDI | A manual checklist per page in this document, later |
| Knobs and Live Keys | Sent by the panel, not received | Manual |
| Allow Dump Req = NO, Device ID changes | Only set on the panel | Manual, or the optional test command below |
| Settings surviving a power-cycle | Needs a power-cycle | Manual: power-cycle the FM-1, then run `get all` |

---

## Optional firmware support

Two small diagnostic commands would close most of the gaps above. Both would use the existing diagnostic header `F0 7D 4D 50`, which is separate from the patch format.

- **`03` Read monitor:** returns the last *n* monitor entries, with port, status, data and fate (passed, remapped or blocked). This tests the monitor's dark yellow and dark grey marking, and gives a second check of each pipeline decision as the FM-1 saw it.
- **`04` Set global settings:** sets Device ID and Allow Dump Req in RAM only, never writing Flash. This tests request refusal and Device ID matching.

Like the latency command, both could be left out of a release build.

---

## Decisions for you

1. Build the optional diagnostic commands `03` and `04`?
2. Is a USB MIDI interface available for the TRS loop, or should TRS stay manual?
3. Should `pytest` and `hypothesis` be added as tools? The random patches can be generated without `hypothesis`; it only makes failing cases shrink to the smallest example.
4. Are the latency limits above (mean under 200 µs, max under 1 ms) right?
