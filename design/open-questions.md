# Open questions: MVP screens

These questions remain from the review of `fm1-screens.json`. Settled items are in `decisions.md`.

Each question has my suggestion. Write your answer after **Answer:**; "ok" accepts the suggestion. Questions 1–5 come first because the monitor is the first screen I'll build.

---

## MIDI Monitor

**1. Before or after processing?** Does the monitor show messages as they arrive (before the filters and remaps), or as they're sent (after)?
*Suggestion:* as they arrive. That shows what the FM-1 receives, which is what you need when a filter seems to be dropping something.
**Answer:** OK

**2. Clock and Active Sensing.** MIDI clock arrives 24 times per beat and Active Sensing every 300 ms. Either one would push everything else out of the 100-event buffer within seconds.
*Suggestion:* don't add them to the list. Show a small indicator in the header that blinks while clock is arriving. Start/Stop/Continue are still listed.
**Answer:** OK. ActiveSensing is ignored and is never passed through the pipeline.

**3. Note names.** Is note 60 shown as C4 (Roland and most DAWs) or C3 (Yamaha)?
*Suggestion:* C4. The keyboard's own labels (F3–G5) use this convention.
**Answer:** OK

**4. Pitch bend.** Show the two raw bytes (`64 0`), or one value from −8192 to +8191?
*Suggestion:* one signed value, for example `PB  +0` or `PB  -512`. It fits in the D1/D2 columns.
**Answer:** OK

**5. SysEx.** How should SysEx appear in the list?
*Suggestion:* `SYX` with the byte count in the data columns, for example `U -- SYX 24B`.
**Answer:** OK

---

## Message Filter

**6. "EFFECTIVE CH".** It sits between the system messages (SYX…TCK) and the channel messages (NON…PB). Does that mean channel messages are filtered per channel, while system messages are filtered per port only?
*Suggestion:* yes. That gives one system filter per port, plus one channel-message filter per port and channel (3 ports × 16 channels). The keys pick the channel.
**Answer:** System messages are per-port (input). Changing the port updates all the system message fields to the settings for the select port. Change a system message filter updates the filter only for the selected input port. Same
for the channel messages, each message is per-port, per-channel. Changing the port or channel fields updates the channel message values for that key pair. 

**7. Note On with velocity 0.** Many devices send this instead of Note Off. Should it be treated as Note Off by the NOF filter?
*Suggestion:* yes. Otherwise blocking Note On while allowing Note Off would still let these through, and notes would hang.
**Answer:** OK

**8. Active Sensing (FE).** It isn't on the screen. Add it?
*Suggestion:* add `ACT` to the right column below TCK (one row is free there).
**Answer:** No, Active Sensing is ignored on receipt and never sent out via mapping.

**9. SysEx filter and our own SysEx.** The SDK handles firmware-update SysEx before our code, so the filter can't block re-flashing. Should our own patch-control SysEx (question 26) also bypass the filter?
*Suggestion:* yes. Otherwise setting SYX to NO would lock you out of remote control.
**Answer:** OK

---

## Knobs

**10. What the knobs send.** KNOB 1–4 are endless encoders, not potentiometers. Should each keep its own value from 0 to 127 and send it as an absolute CC? Or should they send relative CC (+1/−1), which some DAWs support?
*Suggestion:* absolute values for the MVP. Relative mode can be added later as an option per knob.
**Answer:** OK

**11. Show the values?** Should Knobs Config show each knob's current value?
*Suggestion:* yes, as a bar under each knob's row, so you can see a knob move.
**Answer:** OK

**12. CC 120–127.** These are channel mode messages (All Sound Off, Reset All Controllers, All Notes Off, and so on). Allow them as knob targets?
*Suggestion:* limit knobs to CC 0–119.
**Answer:** OK

**13. Knob routing.** Do knob messages go through the pipeline (filters, remaps), or merge straight into the output?
*Suggestion:* straight into the output. The knobs are configured with their final channel and CC, so filtering them again would be confusing.
**Answer:** OK, straight to the output

---

## Live Keys

**14. Key routing.** Same question for notes played on the keys: through the pipeline, or straight to the output?
*Suggestion:* straight to the output, like the knobs.
**Answer:** OK

**15. Octave 0.** With octave 0, which note does the lowest key play?
*Suggestion:* F3 (note 53), matching the label printed on the panel. OCT± shifts by 12 semitones within −3 to +3.
**Answer:** OK

**16. Program Change numbers.** Show 0–127 or 1–128?
*Suggestion:* 1–128, as most synths display it. The message itself sends 0–127.
**Answer:** OK 1-128

---

## Remaps

**17. What does a message rule change?** This is the least clear part of the design. For Note On, does a rule map the note number (data 1) or the velocity (data 2)? For CC, the CC number or its value? The example `100 → 127, ch 10` could mean either.
*Suggestion:* data 1 (note number, CC number, program number). Value mapping (velocity curves, CC scaling) fits better with the later LFO and envelope features.
**Answer:** yes data 1 (note number, CC number, program number) are the values that remap rules mutate.

**18. Rules or tables?** The spec says "Remapper **Tables**". Do you mean individual rules, as designed, or full 128-entry lookup tables?
*Suggestion:* rules, as designed. They're easier to edit with an encoder and use far less storage.
**Answer:**  individual rules

**19. Which output channel wins?** A message rule has its own output channel, and the Channel Remap stage comes after it. Which applies?
*Suggestion:* the message rule sets the channel, and Channel Remap then still applies to the result, because it's the last stage in the spec's flow. The rule's channel defaults to its input channel, so most rules won't change it.
**Answer:** OK

**20. List limits and duplicates.** How many rules can each list hold? What happens if you add a rule whose source is already in the list?
*Suggestion:* 32 rules per list. Adding a duplicate source replaces the old rule, after a confirmation.
**Answer:** OK

**21. Editing rules.** Can existing rules be edited, or only added and deleted?
*Suggestion:* PLAY on a selected rule opens it in the Add page for editing. PLAY with nothing selected adds a new one.
**Answer:** OK

---

## Patches and storage

**22. What a patch holds.** I assume a patch holds the filters, remaps, knobs and Live Keys settings, while Device ID and "Allow Dump Req" are global. Correct?
*Suggestion:* yes. Global settings are saved separately and survive patch changes.
**Answer:** OK

**23. When a patch loads.** Does changing Patch Number load that patch at once, or only when you press PLAY?
*Suggestion:* only on PLAY, so you can browse names without losing unsaved changes. Show a marker such as `*` in the title while there are unsaved changes.
**Answer:** Only on Play

**25. Erasing the stock data.** Saving patches means writing to the user Flash partition. That erases the stock projects, voices and samples stored there. Reinstalling stock V15 restores the factory voices. Is that OK?
*Suggestion:* yes. Back up with the M-VAVE workbench first if you want to keep anything.
**Answer:** OK

**26. SysEx format.** SysEx messages need a manufacturer ID. Should an incoming patch dump load immediately or ask first?
*Suggestion:* use `7D` (the ID reserved for non-commercial use) with your Device ID. An incoming dump replaces the edit buffer immediately, but is only written to Flash when you save, so a stray dump can't overwrite a stored patch.
**Answer:** 

Sysex Header Format
===================

F0

sysex manufacturer id: ~~00h 30h 0Ah~~ 7Dh (non-commercial; changed 2026-11-01)

Device ID (From UI)

Model ID: 11h

Command ID: 00h = single patch dump, 01h = all patch dump, 10h = single patch request, 11h = All patch request

*Written up, with the data structures, in `patch-format.md`. Dumps are split into packets of at most 128 bytes there, because of an SDK limit.*

<requested patch data>

F7

---

Sysex Open Questions
---

1. When an all-patch dump arrives, should it write all 16 patches to Flash straight away, or ask for confirmation on the FM-1 first? For a single-patch dump I suggest loading it into the current, unsaved settings so a stray dump can't overwrite a stored patch. This is the unanswered half of question 26. *ANSWER:* No confirmation, a sysex dump overwrites. Single patch dumps also overwrite without confirmation (this is to support a future web-based patch editor). We should reserve a preset number to signal dumping to the edit buffer instead of overwriting a preset.
2. Should the FM-1 also answer Device ID 7F ("all devices"), as many synths do? *ANSWER:* No
3. At power-up, should it load the last patch you used, or always patch 1? *ANSWER:* Last patch used


## Scope

**27. Note Transposer.** It's in the spec's flow but has no screen. Is it in the MVP?
*Suggestion:* add a small screen after Message Remap in the FX cycle, with a semitone offset from −24 to +24 per port and channel.
**Answer:** After MVP

**28. Later features.** LFOs, envelopes, Variable Capture, MIDI learn and BLE aren't in these designs. Should they wait until after the MVP?
*Suggestion:* yes for all except BLE. BLE is already one of the three ports on the filter screens, so I'd include BLE input and output in the MVP.
**Answer:** After MVP

**29. Unused encoders.** The PRESETS encoder, and KNOB 1–4 on the editing screens, aren't used. Should PRESETS step through patches from any screen?
*Suggestion:* yes. Turning PRESETS shows the patch number and name in the header, and PLAY loads it, as on Patch Select.
**Answer:** Knobs 1-4 are _always_ used for sending CC data as per Knobs Config. Presets encoder, on first tick, will jump to the "Patch Select" page, but that hasn't been built yet, so hold on Presets encoder implementation for now.

---

*Question 24 (name entry) is settled: names are entered with the encoders only. See `decisions.md`.*
