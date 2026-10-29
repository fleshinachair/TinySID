# TinySID release notes

## v0.9.0: first public release

**Turn any C64 `.sid` into a live instrument on your Ableton Move.** TinySID runs the tune's own 6502 player code on a SID chip
emulation inside a Schwung track slot. Play it, loop it, borrow its sounds for another tune, and send its voices to your other
Move tracks.

### Headline features

**Borrow one SID's sound for another tune.** Play a tune whose instruments you love, turn **Timbre Lock** on (Tune + Loop page),
then load a different `.sid`. The new tune plays its own notes with the first tune's instruments: waveforms, pulse widths,
envelopes and filter. Drums and the driver's hard restarts still come through, so it keeps its groove. Mix Hubbard's bass with
Galway's melody, or dress a quiet tune in Commando's sound.

**Send the SID voices to your Move tracks (Notes Out).** Each of the three voices can go out as MIDI on its own channel and play
any instrument on another Move track: a Drift pad doubling the lead, a drum rack following the bass. The tune's tuning travels with
the notes on pitch bend, so a tune that sits 37 cents sharp of A = 440 Hz is played 37 cents sharp on the other track too. Switch each voice on with
the pads (second row in Control mode) or the Notes Out page. The switches are separate from the audio mutes, so you can hear only
the Move instruments, only the SID, or both. **SID Delay** holds TinySID's own sound back (50 ms to start with, up to 200) so it
lands with the tracks it is playing. It only applies while a voice is being sent.

### Also in this release

- **Control mode turns the pads into a performance surface.** Bottom row: mute voices 1 to 3, unmute all, transpose down / up / reset,
  and **loop capture**. Press it to start a loop on the bar that's playing, and again to end it there. Second row: Notes Out per
  voice. Top row: a drum kit of the tune's own **sound effects**, found automatically among its subtunes and playable over the song,
  louder the harder you hit.
- **Play the tune's own instruments on the pads.** Hand any voice to the pads and play it with the song's real instruments,
  pulse-width sweeps and vibrato included. The pad stays in tune with the song, even on tunes that aren't tuned to A = 440 Hz.
- **AutoTune** learns each tune's own tuning and brings it onto the grid (or the tune's key) without flattening vibrato, slides or
  drum sweeps. **Master Tune** shifts the grid by up to 100 cents.
- **Locked to the Move.** Host Lock follows the Move's clock without drift, measured at zero frames out after 64 bars. The bar looper repeats any
  1 to 8 bars note for note.
- **Mix + Glue:** stereo Spread, sidechain Duck and Filter Pump (they keep the tune's own beat when the Move is stopped), Lo-Fi,
  Transpose and FX Duck.
- **Piano roll** on the first page: the key, the note each voice is playing and its tuning, and a keyboard.
- Louder output, and the tune keeps running even when every voice is muted.

### Setting up Notes Out

1. Put TinySID on its own track (for example track 4) and turn that track's **MIDI In off**. Otherwise it hears its own notes
   coming back.
2. Give tracks 1 to 3 an instrument each and set their MIDI In to channels 1, 2 and 3 (the defaults Notes Out sends on).
3. Select TinySID's own track on the Move before opening its screen. The Move lights a pad for every note on its selected track,
   and that would flicker over TinySID's pad colours.
4. Switch voices on with the second row of pads, and adjust **SID Delay** by ear until everything lines up.

### Known limits

- Single-SID PSID files only. RSID files (tunes that drive their own interrupts) aren't supported.
- No sample playback through the volume register (digi drums), and combined waveforms are approximate.
- Notes Out assumes the receiving instrument's pitch-bend range is ±2 semitones (the usual default).
- SID Delay is one value for all three voices. If your instruments respond at different speeds, set it for the slowest.
- Key and tempo readouts are estimates.

### Install

Download `TinySID.tar.gz` from the release and install it with Schwung's module manager, or unpack it into
`/data/UserData/schwung/modules/sound_generators/tinysid/`. Put your own `.sid` files in its `sids/` folder. The package contains
no copyrighted tunes: the built-in tune is an original demo.

Files: `dsp.so`, `module.json`, `ui_chain.js` (the module's own screen), `canvas.js` (piano roll), `help.json`, `release.json`.
Delete `ui_chain.js` to fall back to Schwung's standard editor.

More detail: [README](README.md) and [docs/TECHNICAL.md](docs/TECHNICAL.md).
