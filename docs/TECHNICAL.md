# TinySID: technical notes

How TinySID works inside, for anyone building or changing it. The user guide is the [README](../README.md).

## Building

```
scripts/build.sh   cross-builds build/TinySID.tar.gz (aarch64; aarch64-linux-gnu-gcc or `zig cc`)
                   RELEASE=1 never embeds assets/Commando.sid: use it for anything you publish
scripts/test.sh    native unit tests under ASan/UBSan (where available) + manifest check
src/dsp/           cpu6502, sid, psid, tinysid (engine), plugin.c + schwung_api.h (host adapter)
canvas.js          the Song level's piano roll page (runs inside the Schwung UI)
help.json          in-module help; release.json: version + download URL for Schwung's updater
tools/             make_demo_sid.py, embed_sid.py, check_manifest.py, gen_ui_json.py, fetch_host_ref.sh
```

The Schwung UI asks the DSP for its pages through `get_param("ui_hierarchy")` and
`get_param("chain_params")`, so `tools/gen_ui_json.py` derives both from `module.json`
at build time. Change the manifest and rebuild; do not edit the generated header.

## Pages and parameters

**Mixer** (the top level, with links to the two pages below)

| Key | Range | Notes |
|---|---|---|
| `v1_vol` `v2_vol` `v3_vol` | 0..1 | scales each voice before summing / filter routing; 0 = mute |
| `out_trim` | -18..+6 dB | on top of a fixed -9 dB base gain that puts the output level with Schwung's other generators |
| `sync_mode` | Free (PAL 50Hz) / Free (NTSC 60Hz) / Host Lock 1x / 0.5x / 2x | see Tempo |
| `tempo_trim` | 0.75..1.5 | scales the play() rate in every mode; pitch is untouched. Off 1.0, Host Lock leaves the tick grid (see Tempo) |

**Song** has two pages. The first is `piano_roll`, a canvas *page* (see below), so the keyboard is on the OLED as soon
as you open the level (no click needed). The second is the knob grid:

| First line | Second line (Snoop) |
|---|---|
| `sid_file` (file browser, starts in the module's `sids/` folder), `subtune` (1..N, re-inits the 6502), read-only `song_bpm`, read-only `song_key` (key and scale together, like `D Min`, or `--`) | `snoop_voice` (Voice 1 / 2 / 3 / All Voices), read-only `snoop_note` (MIDI note that voice is sounding, 0 when idle), read-only `snoop_level` |

The piano roll's page carries the same encoders. The note name, frequency and tuning error are drawn on the roll itself.

**Pad + Filter**, one page: `pad_target` (Off / Voice 1-3), `pad_sound` (the song's own instruments on that voice),
`chip_model` (6581 / 8580), `filter_mode` (Song / Low Pass / Band Pass / High Pass), then `cutoff_bias` and `res_bias`.

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

## Snoop and the piano roll

`snoop_note` follows the registers the chip actually has, so it shows the song, the pad and the pad's
recorded movement alike: the note is `round(69 + 12 log2(f / 440))` with `f = word x clock / 2^24` (the
file's own PAL or NTSC clock), `0` when the gate is low, the voice has no waveform, or it is noise only
(noise has no pitch). `All Voices` shows the voice that was gated on most recently.
`$1D45` is A4 (69); `$1168` is C4 (60), 261.6 Hz.

The `piano_roll` page (`canvas.js`) is a two-octave keyboard centred on the sounding octave: hollow
white keys, solid black keys, the sounding key inverted, the key banner (`[D Min]`), the voice, and the note
with its frequency (`D3 146.8Hz -3c`, C4 = MIDI 60). The manifest marks it `as_page` and `page_first`, which makes
the Schwung UI add it to the Song level's page rotation ahead of the knob page; the UI calls `drawPage` for it
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
