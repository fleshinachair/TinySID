# TinySID: technical notes

How TinySID works inside, for anyone building or changing it. The user guide is the [README](../README.md).

## Building

```
scripts/build.sh   cross-builds build/TinySID.tar.gz (aarch64; aarch64-linux-gnu-gcc or `zig cc`)
                   RELEASE=1 never embeds assets/Commando.sid: use it for anything you publish
scripts/test.sh    native unit tests under ASan/UBSan (where available) + manifest check
src/dsp/           cpu6502, sid, psid, tinysid (engine), plugin.c + schwung_api.h (host adapter)
canvas.js          the piano roll page (runs inside the Schwung UI; also inlined into ui_chain.js)
src/ui/            ui_chain.template.js: the module's own editor, see "The module's own editor" below
help.json          in-module help; release.json: version + download URL for Schwung's updater
tools/             make_demo_sid.py, embed_sid.py, check_manifest.py, gen_ui_json.py, gen_ui_chain.py, fetch_host_ref.sh
```

The Schwung UI asks the DSP for its pages through `get_param("ui_hierarchy")` and
`get_param("chain_params")`, so `tools/gen_ui_json.py` derives both from `module.json`
at build time. Change the manifest and rebuild; do not edit the generated header.

## Pages and parameters

**Mixer** (the top level, with links to the three pages below)

| Key | Range | Notes |
|---|---|---|
| `v1_vol` `v2_vol` `v3_vol` | 0..1 | scales each voice before summing / filter routing; 0 = mute |
| `out_trim` | -18..+6 dB | on top of a fixed -5 dB base gain; still a parameter of the engine, but on no page and not saved (the dial was dropped) |
| `sync_mode` | Free (PAL 50Hz) / Free (NTSC 60Hz) / Host Lock 1x / 0.5x / 2x | see Tempo |
| `tempo_trim` | 0.75..1.5 | scales the play() rate in every mode; pitch is untouched. Off 1.0, Host Lock leaves the tick grid (see Tempo) |
| `song_bpm` `song_key` | read-only | the tempo (0 until it is known) and key (`--` until the scan has chosen) of the tune, beside the tempo controls |

**The piano roll** is `piano_roll`, a canvas *page* declared on the Mixer (root) level with `as_page` and `page_first`, so the module
opens on it (see below). The host gives a canvas page its level's knob list as encoders and always adds the level's knob grid after
it, so the roll and the Mixer grid carry the same eight knobs: the voice levels move while you watch the meters. (A separate Song
level only existed to carry knobs for the roll; with the roll on the Mixer it is gone.) `snoop_voice` is no longer a knob: the
plugin sets it to All Voices, so the roll follows whichever voice started last. The engine still answers `snoop_voice`, `last_note`
and `snoop_note` etc. for tests and tools.

**Tune + Loop**, one page (level `tune`): `loop_length` (Off / 1 / 2 / 4 / 8 Bars), `loop_start` (1..32), `loop_nudge` (0..15 rows) and
`sid_file` (file browser, starts in the module's `sids/` folder) on the first line; `subtune` (1..N, re-inits the 6502),
`timbre_lock`, `pitch_snap` (AutoTune: Off / A440 Chromatic / In-Key) and `master_tune` (-100..100 cents) on the second.

**Mix + Glue** (level `glue`): `stereo_spread` (0..1, default 0.5), `ducking_depth` (0..1, default 0 = off), `ducking_release`
(20..300 ms, default 80), `fx_duck` (0..1, default 0.4), `filter_pump` (0..1), `lofi` (0..1) and `transpose` (-12..12 semitones).
**Notes Out** (level `midi`): `midi_out_v1`..`v3` (Off / On, default Off) and `midi_out_ch1`..`ch3` (1..16, default 1, 2, 3); all six are saved with the set. The host's page planner leaves this order alone and
raises no warnings (checked by `tests/test_host_pages.mjs`).

The piano roll's page carries the same encoders. The note name, frequency, tuning error and level are drawn on the roll itself from
the `snoop_feed` string; the engine still answers `snoop_note`, `snoop_level`, `snoop_hz` and `snoop_cents`, but they are no longer knobs.

**Pad + Filter**, one page: `pad_target` (Off / Voice 1-3), `pad_sound` (the song's own instruments on that voice),
`filter_mode` (Song / Low Pass / Band Pass / High Pass), `cutoff_bias`, `res_bias` and `chip_model` (6581 / 8580) on the first line,
then `pad_target`, `pad_sound` and `pad_mode` (Play / Control) on the second. Keep Filter, Cutoff and Reso together inside one row: the host treats them as one
filter graphic and moves them to the second row if they straddle the break.

**Pad voice isolation.** While a pad voice is engaged (`pad_target` set and the voice taken over) every 6502 write to its
frequency, pulse width, control and AD/SR registers is replaced by the pad's own, and the frame routine's writes are put back
right after each play() (`pad_frame_intercept`). The pad's control byte is only waveform + gate: sync, ring mod and test are
masked out (`& 0xF0`), so no neighbouring voice can reset or modulate its oscillator. The song's other voices and its filter
carry on untouched.

**Pad filter route.** The one thing a pad voice shares with the song is the SID's single filter. When the tune does not use
it (Song mode, the tune's $D418 mode bits all clear) the pad voice is routed through a low-pass held fully open (cutoff 255 plus
the Cutoff knob, resonance from the tune plus Reso), so Cutoff and Reso act on the pad alone and it sounds as before. A voice
routed through the filter with no mode selected is silent on a real SID, hence the open low-pass. When the tune does use the
filter, the pad is NOT forced into it: the song's cutoff sweeps (closing on every drum hit, say) would shut the pad each time.
Cutoff and Reso then offset the song's filter for the voices the song routes; to put the knobs in charge of the whole filter,
including the pad, pick a Filter mode (Low / Band / High Pass). The rule is re-evaluated whenever the tune writes $D418.

**Timbre Lock.** Switching it on holds, for each voice, its last real instrument: the pulse width, waveform (with ring and sync,
not the test bit) and AD/SR it last sounded with a gate, a tonal waveform and an envelope (`timbre_track`, updated after every
play()), plus the filter registers $D415-$D418 as the tune last wrote them (`raw[]`, not the chip, so the cutoff / reso knobs are
not baked in). `apply_reg` substitutes the held values for the tune's writes. Pitch, the gate bit and the master volume nibble
still follow the tune. Two things are let through: a voice on a pure noise waveform (a drum hit; `timbre_passes`), and a "hard
restart", the tune writing AD and SR both as 0 just before a note (`timbre_hr`). That second one matters: the SID's ADSR delay
bug stalls an attack for up to 30 ms if the rate counter is large when the gate comes, which is why drivers zero the envelope
first; holding AD/SR through it swallowed short notes and left a track at a fraction of its loudness. A voice that has no
instrument yet is not held. `cutoff_bias` / `res_bias` / `filter_mode` and the pad voice apply on top. The held values survive
`init_tune`, so a subtune change or a new file plays through the held sound. Switching it off replays the tune's own registers.
Not saved with the state.

**AutoTune.** `pitch_snap` and `master_tune` act on each voice's frequency word, per play() frame (`snap_frame`) and on every
register write (`snapped_reg`), as a correction in cents applied as a ratio, so the pitch's movement stays the tune's own.
Pitches are measured on the file's own clock (PAL 985248 Hz or NTSC 1022727 Hz from the header; `sync_mode` only changes the play()
rate), against the grid A = 440 Hz + `master_tune`.
- **Global tuning offset.** Every tonal, gated note adds a unit vector at angle 2 pi (error from its nearest semitone) / 100 to a
  running sum; once 24 notes agree (resultant length at least 0.5) the circular mean is the tune's own offset, never more than half
  a semitone, and it is taken off every frame of every voice. This is what puts a tune that sits 40 cents sharp (Boulder Dash:
  NTSC header, +30..+50 cents on every note) in tune with the Move without a single note moving to the semitone beside it.
  Measuring against both clocks was tried and dropped: a table tuned for the other clock and one that is merely sharp look the
  same, and picking wrongly moves the whole tune a half-step.
- **Held notes.** A voice that moves less than 40 cents a frame for 3 frames is also drawn onto the nearest note of the grid
  (or of `song_key`, major or natural minor, once the scan has found a key). The smoothed centre of the pitch (12% per frame) is
  what is moved, so a vibrato stays a vibrato; the pull ramps in at 60% per frame and is exact within half a cent. A leap, a
  sweep or a slide (40 cents from the centre) starts afresh, so arpeggios and drum sweeps get only the global offset. A held
  note stays on its note until it is 65 cents from it, so a pitch between two notes does not flip between them.
- Noise voices and the pad voice are never touched. Off puts the tune's pitch back at once. `tinysid_snap_word` is the pure rule
  for the held-note pull and is what the tests check it against.

**Audio glue.** Each voice has a left and a right gain in the SID core (`sid_set_voice_pan`). `stereo_spread` puts voice 1 in the
middle and voices 2 and 3 at -/+0.7 x spread, with an equal-power law (the squares of the two gains add to 2, so spread 0 is the
old mono sound exactly and a voice is no louder for moving). The filter is one mono filter, so the voices routed through it share
the average pan of the routed voices. The output then passes a 35 Hz one-pole DC blocker per channel
(y = x - x[-1] + R y[-1], R = exp(-2 pi 35 / fs), about 0.995), then the ducking gain, then trim and the soft limiter.
Ducking: a trigger sets the target gain to `1 - ducking_depth` over a 2 ms attack, then the gain returns as
E = E a + (1 - a), a = exp(-1 / (release in samples)). Triggers are the host's own MIDI clock, every 24 ticks counted from Start
(`duck_ticks`, separate from the tune's tick count so a subtune change does not move the beat, and in every sync mode, not only
Host Lock), and MIDI note 36. A depth of 0 does nothing.
With no host clock arriving (the last tick more than 0.25 s ago, or none yet) the same trigger also fires from the song's own beat
(`song_beat`, once per `do_play`): a quarter note is `4 * song_fpr` frames when the tempo ratio is known and `frame_hz * 60 / song_bpm`
otherwise, counted from the first frame of the tune (`beat_left`, reset in `init_tune`). The Filter Pump closes on the same trigger.
A live clock hands the beat back to the host at once, so the two never double up.

**Performance.**
- **Transpose and bend.** `transpose * 100 + bend` cents (`pitch_offset`) are added to every song voice's correction after AutoTune,
  with AutoTune on or off (`pitch_active`), as the same frequency ratio, so the pad voice (which plays its own notes) is the only
  voice left alone. MIDI pitch bend is +-200 cents. `song_key` and the snoop feed show the key moved by the transpose
  (`shifted_key`); AutoTune's In-Key still uses the tune's own key, since it works before the transpose.
- **Pad Mode Control.** This is the DSP's own, note-based control, used by the stock editor (see below for the module's own one). In
  Control mode `tinysid_midi` sends note-ons from `CONTROL_BASE_NOTE` (36) up to `control_action` and plays nothing (and a kick on 36
  does not duck): 0-2 toggle `mute_mask` bits, 3 clears it, 4-6 step or reset the transpose. A mute is a level of 0 (`voice_vol`, which every place that sets a voice's gain
  uses) so the existing gain smoothing fades it; the knob level is untouched. `last_note` records every note-on in either mode.
- **Pad tuning.** `pad_note_word` moves each pad note by `pad_tuning_cents`: the tune's own learned offset from A = 440 Hz
  (`tuning_offset`) when AutoTune is off, so the pad shares the song's tuning, or the master tune when AutoTune has put the song on the
  grid. A held pad note follows the estimate as it is learned (`pad_retune`, after every play() frame).
- **Pad notes.** A pad note's release is capped at 8 (300 ms, `PAD_MAX_RELEASE`): the instruments the scan finds have releases up to
  15 (24 s) that the song's own driver cuts short with a hard restart, and the pad has no driver. Recorded pitch movement whose
  range is more than 100 cents (`PAD_MAX_BEND_CENTS`) is dropped when a patch is chosen (Commando's lead swoops +224 cents,
  Cybernoid II voice 2 has arpeggios of +800 cents); pulse-width and waveform movement, and smaller pitch movement (vibrato,
  trills), are kept. Switching Pad Mode lets go of the notes held (their note-offs would be ignored in Control mode). No scale
  lock: the Move's own scale layout already keeps pad notes in a scale, and a "Song Key" option duplicated it.
- **Filter pump.** `duck_trigger` (the clock's quarter notes and note 36) sets `pump_env` to `filter_pump`. The output stage then runs
  a state-variable low-pass per channel (`PUMP_Q` 1.2) whose cutoff is `20 kHz * 2^(-pump_env * 6)`, from open to about 310 Hz at full
  depth, with `pump_env` decaying by the ducker's per-sample release factor. It is the output's own filter: an earlier version
  moved the SID's cutoff register, which does nothing unless the tune routes voices through the filter, and routing them
  through the 6581 model's filter costs 40 to 50% of the level on real tunes (Commando 0.48, Boulder Dash 0.60 of plain) because
  the model soft-saturates what it filters. The filter starts from the signal when it is switched on (no click), runs while the
  depth is above 0 or the envelope has not finished, and leaves the SID registers alone.
- **Lo-Fi.** After the ducking gain and trim, per channel: sample-and-hold of 1 to 8 samples and quantisation from 16 down to 4 bits
  as `lofi` goes 0 to 1; 0 skips it entirely.
- **Mix feed.** `mix_feed` is `M,<l1>,<l2>,<l3>,<mute mask>,<transpose>` (levels 0..100, 0 while muted); the piano roll reads it
  next to `snoop_feed`.
- Saved with the set: `transpose`, `filter_pump`, `lofi`. Not saved: `pad_mode` (a mode, and restored it would leave
  the pads silent) and `timbre_lock` (restored, it would lock whatever happens to be playing at that moment).

**Bar looper.** `loop_start` (bar, 1..32), `loop_length` (Off / 1 / 2 / 4 / 8 bars) and `loop_nudge` (0..15 rows). A bar is the
TUNE's 16 rows: a row is 6 host ticks at Host Lock 1x, 12 at 0.5x and 3 at 2x, so a bar is 96, 192 or 48 ticks and always holds
the same frames of the tune (it is 96 ticks whenever the lock is not engaged: a free mode, an unknown row length, Speed off
1.0). Song time (`song_pos`) counts those ticks; bar 1 begins `loop_nudge` rows after the first tick after Start, which
moves the grid onto the tune's own bar lines when it has an intro pickup. A change of sync mode or nudge drops the snapshot.
Song time is separate from `clk_ticks`, which drives Host Lock and never goes back. At the top of bar `loop_start` the engine snapshots 64 KB of RAM, the shadow SID registers and A/X/Y/SP/P/PC
(whether or not a length is set, so a loop can be switched on later). With a length set, the top of bar `loop_start +
loop_length` (or any later bar line) restores the snapshot, re-syncs the chip to it (`apply_all`) and sets song time back to
the start bar. The frame counter, cycle count and SID voices are not rewound, so timing and envelopes carry on through the
join; in Host Lock with a known row length the repeats are frame-exact. A voice still sounding at the seam, which the loop's
first bar has not gated on, is not cut: its gate drops so it dies by its own release, and its control byte and sustain /
release stay as they were (`seam_hold`) until it has, or until the song writes them again. Length Off keeps the snapshot. A new `loop_start`
drops it and, if the song has already passed that bar, nothing is caught until a new run (Start, subtune, file). Without MIDI
clock there are no bar lines and nothing loops. Not saved with the state.

**Pending readouts.** `song_key` reads `--` until the scan has voted, and `song_bpm` reads 0 until the tempo is known: from the
title table at load, from the scan's gate cadence, or typed. The internal 116 BPM fallback only drives Host Lock until then.

## Transport

The tune always follows the host transport. MIDI Start restarts it from beat 1, Stop halts it,
Continue resumes. When the host reports its transport state (`get_clock_status`) but sends no
Start/Stop bytes, that is followed instead; a host that reports nothing leaves the tune running.
A module loaded while the host is stopped stays silent until the host starts.

Stopping ramps the song's voices out over ~12 ms and then cuts their envelopes, so a held note
with a long release does not keep ringing, and it does not come back on Continue. A stopped tune
stays silent across a new subtune or file. The pad voice is yours and keeps working while stopped.

## Tempo

`play()` is driven by a sample accumulator, so tempo changes never touch SID registers or pitch.
Calls land on their exact sample, not on block boundaries.

- **Free (PAL 50Hz) / Free (NTSC 60Hz):** 50 / 59.826 Hz times `tempo_trim` (CIA-timed tunes use
  their own timer rate). **Loading a file picks the matching mode from its header's video standard**,
  so an NTSC tune plays at 60 Hz as its author intended; you can still change it, and a subtune change
  leaves your choice alone. A Host Lock mode is never overridden by a load.
- **Host Lock** is phase-locked to the MIDI clock (24 per quarter note). The tune's frames are counted from the clock
  *ticks*, not from elapsed time, so nothing can drift however long a clip loops. A 16th-note row is 6 ticks (12 at
  Half, 3 at 2x) and the tune spends `fpr = num / den` frames on a row, so after K ticks exactly
  `floor(K * num / (den * 6)) + 1` frames have played: integer arithmetic only. Frame 1 plays on the first tick after
  MIDI Start (tick 0), so the tune's first downbeat lands on Step 1 of the clip; Start always resets the phase.
  Between ticks the next frame is placed by interpolating with the measured tempo, held just short of the next tick,
  so a frame is never early. The host's tempo only places frames between ticks; it never changes the count.
  A Stop holds the tune and Continue resumes it at the same tick count. If the clock disappears for 2 s without a
  Stop, the tune pauses and picks up in place when ticks return. With no clock at all for 1 s after a start the tune
  free-runs at the old ratio (native rate x `host_bpm / song_bpm`, `host_bpm` 120 until a clock arrives) so it is
  never silent, and the lock takes over, keeping the tune's place, when ticks begin. `tempo_trim` other than 1.0, a
  typed `song_bpm_override`, or an unknown row length also use that ratio instead of the tick grid.

## Song info (BPM, key, scale, instruments)

When a file or subtune is chosen, a second engine plays it silently (8 frames per audio block, about
5 seconds of wall time for 5 minutes of music) and listens to what it plays:

- **Key / scale** (shown together as `song_key`): pitch classes of the pitched notes (noise excluded) are matched against the
  Krumhansl-Schmuckler major and minor profiles in 30 s windows, and the windows vote (ties go to
  the clearer tonal centre, then the earlier one). Tunes that modulate, like Commando, have no single
  key: the display shows the one most windows agree on. Atonal or sparse material shows `--`.
- **Instruments:** every distinct waveform + AD/SR the tune plays on each voice, sampled once per
  frame the voice is sounding (so a held note counts for as long as it is held) and read at the end
  of the frame (drivers that hard-restart write the real envelope only after the gate). Sounds heard
  for under 4 frames are transitions and are dropped.
- **BPM and row length:** a title table (`k_known_tunes` in `tinysid.c`: title, subtune, BPM, frames per row) wins:
  Commando subtune 1 is 125 BPM with exactly 6 frames a row. Otherwise the most common gap between
  gate-ons on voices 1 and 3 over the whole scan (ties go to the shorter gap) is read as one 16th-note
  row (`frame rate x 15 / frames`, folded into 60..200 by doubling or halving the row length, which keeps it an exact
  ratio). The readout is snapped to a whole BPM. It is set once when the scan finishes and
  never moves while you play. It assumes the file's own video standard (an NTSC file counts 60 Hz
  frames) and 16th-note rows: other readings of the same rhythm differ by exact ratios (x2, x0.5, 50/60).

## The module's own editor (ui_chain.js)

The host's stock editor shows a module's pages from its `ui_hierarchy`, and gives it no way to light pads or to put a control on a
fixed pad: a pad press reaches the DSP as a note whose number depends on the Move's scale layout. A component that ships a
`ui_chain.js` owns the pads instead (`host_pad_block(1)`) and sees them as hardware pads 68..99, bottom-left to top-right, which is
how its Pad Mode Control puts the buttons on the bottom row and colours them (`setLED` from the shared `input_filter.mjs`, the call
the host's own UI uses). The mechanism is the one trance-gate (graebe, MIT) uses.

- **The load rule.** `enterComponentEdit` asks for the component's hierarchy first and loads its `ui_chain.js` only if there is none
  (`shadow_ui.js`: `getComponentHierarchy`, then `loadModuleUi`). So `plugin.c` answers `-1` to `get_param("ui_hierarchy")` when a file
  `ui_chain.js` sits in the module's directory, and the script supplies the same document itself. **Removing the file puts the stock
  editor straight back** (reload the module): the DSP checks for it when an instance is created, the pad controls go back to being
  notes 36-42, and nothing else changes. `module.json` names it (`"ui_chain": "ui_chain.js"`); the file is not required to exist.
- **Generated, not written twice.** `tools/gen_ui_chain.py` fills `src/ui/ui_chain.template.js` with `canvas.js` (inlined: the roll is
  drawn through the controller's `drawCanvasPage`) and the hierarchy `gen_ui_json.py` derives from `module.json`; `build.sh` stages
  the result. The script builds a page controller (`createController`) over that hierarchy and the DSP's `chain_params`, so the knob
  grid, widgets, "My Presets" and "Module" pages are the host's own.
- **Pads.** `pad_mode`, `voice_mutes` and `transpose` are read every sixth frame. In Control mode the script calls `host_pad_block(1)`
  every frame (restated, never edged: the shim drops it on its own at display-mode edges), receives pad presses as hardware pads and
  writes `voice_mutes` / `transpose`, and paints the grid (the first paint over a few frames, then a forced repaint of one pad a
  frame, which heals a clobber by Move). In Play mode it calls `host_pad_block(0)` and paints nothing; leaving Control, and Back,
  repaint the Move's own pad state from `shadow_get_pad_led_snapshot`.
- **The SID picker.** The stock editor opens the host's file browser for a filepath cell through host-internal calls a module cannot
  reach, so the SID File cell opens a list drawn by the script on the shared `filepath_browser.mjs` state machine (folders, the
  `.sid` filter, the start folder); a pick goes through `controller.commitValue`. Enums need no picker: their knob cycles the
  options and the controller shows the list while you turn.
- **Tests.** `tests/test_ui_chain.mjs` runs the generated file against the host's own libraries (`host_ref/`, see
  `fetch_host_ref.sh`) with a fake DSP, display, LED output and filesystem: page planning, the roll, knob writes, every pad action
  and colour, Play/Control hand-over, the picker, Back, and a cold start with nothing readable. What it cannot check is the host
  loading the file, which only a device does.

## Sound effects

The tune's other subtunes are often jingles and effects. A silent scan (`fxs_run`, FX_SLICE = 8 frames per 128 samples rendered, next
to the key scan's 8) plays each subtune on a second engine for FX_PROBE_FRAMES = 600 frames (12 s), noting the frames in which any
voice has its gate on with a waveform. A subtune is an effect if it sounded for at least 3 frames and was quiet again for the last
150 frames; it plays for its last sounding frame plus 50 (a second for the release). Subtunes are probed in order, up to 64, until 8
effects are found (`fx_count`, `fx_list`, `fx_ready`). Only a subtune whose last sounding frame is within FX_MAX_LAST_FRAME = 300
(6 s) counts: a longer one is a second tune (Cybernoid II's subtune 2 runs about 8 s). A new file clears the list (`fx_reset`, from `tinysid_load`).

A hit (`fx_trigger`, "<effect>,<velocity>") takes a free slot of FX_SLOTS = 3, or the one nearest to finishing, loads the same file into
a spare engine (`is_fx`: no scans of its own), copies the song's chip, filter, trim, spread, AutoTune mode, master tune and transpose,
restarts it on the subtune and copies the song's learned tuning (an effect is too short to learn one). The engine renders each active
slot after the song's own output stage, scaled by `0.25 + 0.75 * velocity / 127` with a 10 ms fade at the end, and adds it with a
clamp. The song dips by `fx_duck` through the ducker's envelope (only if that is deeper than the dip already running). The effect
scan is part of the same per-block budget as the key scan: `tests/bench_scan.c` measures both together (at most about 200 us a block).

## Notes out

`mout_frame` runs after every `do_play` of the song's engine, for each voice whose `midi_out_vN` is On. It reads the gate and waveform
the tune wrote and the frequency word the chip actually has (`sid.reg`, after AutoTune, transpose and bend), so what goes out is what
is heard. The audio mute mask is not consulted: the audio and the MIDI are muted apart. The pitch is `midi_cents(word)`, the note
its rounding to a semitone, the bend `(cents - note * 100) / 200 * 8192` (a receiver range of +-2 semitones). A new note sends the
bend first and then the note on (velocity 100), so it starts at the right pitch; a pitch that moves less than 200 cents from its
note is a bend (resent when it has moved 3 cents), a larger move or a new gate (`mout_rise`, set in `bus_write` even when the gate
is rewritten within a frame) is a note off and a new note. Noise, the pad's voice and a stopped tune release the note. Each voice
has its own channel (`mout_ch[v]`, default 1, 2, 3) because pitch bend is per channel; an event carries the channel in force when it
was queued, so a note off follows its note on even if the channel changes. The events wait in a 64-entry queue
(`tinysid_midi_out_peek` / `_drop`); `plugin.c` hands up to 8 a block to `midi_inject_to_move` (4-byte USB-MIDI packets on cable 2,
status 0x2E for a bend, 0x29 / 0x28 for note on and off) or, failing that, to `midi_send_external`, and keeps an event the host
refuses for the next block. With neither the events are dropped. An event for the channel this instance itself listens on
(`slot_recv_channel`) is dropped, and cable 2 also loops back into any slot listening on the channels in use, so `on_midi` ignores notes
and bends on a channel being sent on (`tinysid_midi_out_listens`). `host_api_v1_t` gained the two fields after `get_bpm`
(`midi_inject_to_move`, `slot_recv_channel`); the host's table ends in a run of NULLs, so a host without them reads NULL.
`tests/test_plugin.c` runs it against a mock host (packets, refusal and retry, echo, own channel, teardown, saved state).
While a module's screen owns the pads (`host_pad_block(1)`), the notes we inject come back to that screen as pad presses: a note in the
pad range (68..99), whatever channel it was sent on, arrives as a plain note-on, and the screen cannot tell it from a finger by its
bytes (the shim copies MIDI_IN, injected packets included, to the screen's feed, and only filters cable 0; the velocity does not
survive either). So the screen does not act on a press: it hands it to the engine (`pad_press`, "<pad note>,<velocity>"), which knows
which notes it handed to the host: every note-on taken in `tinysid_midi_out_drop` is owed back once on each path (`echo_pad`,
`echo_midi`, expiring after ECHO_EXPIRE_SEC = 2 s), and the first press on a note while it is owed is that note coming back and is
dropped (`echo_take`); otherwise the press is carried out (mutes, transpose, the Notes Out switches, an effect) and counted in
`pad_ack`, which the screen watches to flash an effect pad. The same notes come into the slot as MIDI when TinySID's track listens on
those channels: `tinysid_midi` drops the first note-on still owed on that path, and its note-off (`echo_held`, counted in
`midi_echoes`). With the module's own screen the plugin sets `note_controls` 0, so in Control mode incoming notes are never the stock
editor's note buttons (36..42) either. The Move's firmware also lights a pad for any note that reaches it, over our
colours; any note event the screen sees schedules a repaint of the whole grid, 8 pads a frame (ChordDex does the same).

## Always rendered

The shim stops calling `render_block` on a slot whose output has been near silence (every sample within +-4 of zero) for about a
second, and only probes it one block in 172 after that. For TinySID that would stop the tune itself: with all its voices muted the
song stops in mid-note, Notes Out leaves the last note hanging on the tracks it plays, and the tune falls out of time with the Move.
`module.json` declares `capabilities.requires_continuous_processing` (as linein and scratch do), so the slot is always rendered.

## The document budget

The host reads `chain_params` and `ui_hierarchy` through `get_param`, and a read that does not fit fails whole, which would leave the
screen with no contract at all. `tools/gen_ui_json.py` refuses a static document over 3,800 bytes; at run time `plugin.c` fills the
placeholders (the sids folder, the subtune count, the Pad Sound list) and gives the instrument list what is left of
CHAIN_PARAMS_BUDGET = 3,900, ending it after the last whole name that fits (`tinysid_pad_options`). `module.json` itself is capped at
8,192 bytes by the loader and is indented one space a level for that reason.

## Snoop and the piano roll

`snoop_note` follows the registers the chip actually has, so it shows the song, the pad and the pad's
recorded movement alike: the note is `round(69 + 12 log2(f / 440))` with `f = word x clock / 2^24` (the
file's own PAL or NTSC clock), `0` when the gate is low, the voice has no waveform, or it is noise only
(noise has no pitch). `All Voices` shows the voice that was gated on most recently.
`$1D45` is A4 (69); `$1168` is C4 (60), 261.6 Hz.

The `piano_roll` page (`canvas.js`) is a two-octave keyboard centred on the sounding octave: hollow
white keys, solid black keys, the sounding key inverted, the key banner (`[D Min]`), the voice, and the note
with its frequency (`D3 146.8Hz -3c`, C4 = MIDI 60). The manifest marks it `as_page` and `page_first`, which makes
the Schwung UI add it to the Mixer level's page rotation ahead of the knob page; the UI calls `drawPage` for it
continuously. It refreshes its extra keys only a few times a second, so the engine packs everything the page shows
into one string, `snoop_feed` (`K,<note>,<voice>,<key>,<cents>,<level>,<recent notes...>`), and the page reads that.

## Filter

`Song` keeps the tune's own filter and the knobs only offset it, so they do nothing on a tune that
never routes a voice through the filter (Commando never does). `Low Pass` / `Band Pass` / `High Pass`
force all three voices through the chosen filter; Cutoff and Reso then become absolute (centred
at 128 and 4).

## Pad

Choosing `Voice N` fades that voice out of the song (~20 ms), then it belongs to the keyboard
until you set Off or pick another voice: notes set its pitch and gate, the song's writes to it
are ignored, and voices 1-3 you did not choose are untouched. Held notes are monophonic with
last-note priority; velocity scales the voice (40-100%).

`pad_sound` is a list of the instruments this song actually plays on the voice you are using,
longest heard first, named like `1 Pul50 38 7A` (rank, waveform with pulse width in %, then AD and
SR in hex). There are no built-in sounds: if the voice never sounds, the song's other voices lend
theirs, and while nothing is known yet (the scan is still running, or the tune is silent) a single
`Lead` entry, a plain saw, keeps the keys working.

An instrument is more than a waveform and an envelope. Drivers move the voice while a note is held:
a fast pulse-width wobble, vibrato, trills, slides, arpeggios, waveform changes. The scan records
that movement from the longest note it hears with each instrument (up to 64 frames, about 1.3 s) and
the pad plays it back from the moment you press a key, at the song's frame rate. Pitch movement is
stored as a ratio to the note's first frame, so it follows the key you play; after the recording
ends, its second half loops (so a vibrato keeps going). Releasing the key freezes the movement while
the release plays out.

A chosen instrument is fixed: it does not change as the song moves between sounds. Sounds that
differ only in sustain level are one instrument (drivers fake a volume with the sustain nibble), at
the loudest sustain; pulse instruments with a sweeping width are one entry too. Drums (`Noi`) are
listed, faithfully.

The list comes from the background scan, so it is empty for a moment after a file or subtune
change; the host is told (via `is_loading`) to re-read it when it is ready, and again when you
change the pad voice. A saved set stores the list position, so it can name an instrument before the
scan has found it.

## Default tune

If `assets/Commando.sid` exists at build time it is embedded as the default tune; otherwise the
bundled original demo (`assets/demo.sid`) is used. `assets/Commando.sid` is git-ignored: it is
copyrighted, so keep it local and never commit or distribute a build that embeds it.

## Tests

`scripts/test.sh` runs the C suites under ASan/UBSan (including a 64-bar drift test of the Host Lock at 120 and
125 BPM, with jittered ticks, a mid-run tempo change, Half/2x, Start/Stop/Continue and clock loss), the piano roll
under Node against a mock 128 x 64 display, the Schwung UI's own page planner, contract validator and frame
context run against the manifest and `canvas.js` (needs `tools/fetch_host_ref.sh` once; skipped otherwise), a no-sanitizer benchmark of the headless pre-scan, and the manifest check. The scan costs very
different amounts on different tunes: 1,500 frames take under a millisecond on simple players but 9-23 ms on
players that use most of every frame, which is why it is spread over audio blocks (8 frames each; measured on a Move: up to about
0.9 ms per block for the heaviest tune tried, against 2.9 ms available, and about 0.09 ms once it is done) instead of run in one go.

## Known limits

- RSID files are rejected; only PSID v1-v4 single-SID tunes.
- Combined waveforms are the AND of their parts; no digi/`$D418` sample playback
  timing (CPU runs one whole `play` call per tick, not cycle-interleaved with the SID).
- The piano roll page has been checked against the Schwung UI's own planner, validator and frame context, not
  seen on a Move screen. The Host Lock is tested against a simulated 24 PPQN clock, block-quantised like the Move's.
- The ABI in `schwung_api.h` matches the header shipped on a Move; the file-browser and
  read-only parameter conventions follow the modules installed there.
