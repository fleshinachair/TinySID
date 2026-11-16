# TinySID v0.9.1

**v0.9.1 adds an MIT licence** (`LICENSE`, also included in the download). There are no code or feature changes from v0.9.0.

**Turn any C64 `.sid` into a live instrument on your Ableton Move.** TinySID runs the tune's own 6502 player on a SID chip emulation
inside a Schwung track slot, locked to the Move's clock.

## Headline features

- **Borrow one SID's sound for another tune.** Turn on **Timbre Lock** while a tune plays, then load a different `.sid`: it plays
  its own notes with the first tune's instruments (waveforms, pulse widths, envelopes, filter). Drums still come through.
- **Send the SID voices to your Move tracks.** **Notes Out** sends each of the three voices as MIDI on its own channel to any
  instrument on another track. The tune's tuning rides on pitch bend, so an off-key tune stays off-key the same way on the other
  track. Per-voice switches on the pads or the Notes Out page, and a **SID Delay** to line the SID up with the tracks.

## Also

- **Control mode pads:** mute voices, transpose, **loop capture** (start and end a loop on the bar you're on), and the tune's own
  **sound effects** as a drum kit.
- **Play the song's own instruments** from the pads, kept in tune with the song.
- **AutoTune**, **Host Lock** (zero drift over 64 bars), a 1 to 8 bar **looper**, Mix + Glue (Spread, Duck, Filter Pump, Lo-Fi,
  Transpose), a piano roll, and a louder output that keeps running when every voice is muted.

## Notes Out setup

1. Put TinySID on its own track and turn that track's **MIDI In off**.
2. Tracks 1 to 3 get an instrument each, with MIDI In on channels 1, 2 and 3.
3. Select TinySID's own track on the Move before opening it, or the Move flickers pads green for notes on the selected track.

## Limits

Single-SID PSID files only (no RSID, no digi drums). Notes Out assumes a ±2 semitone pitch-bend range on the receiver, and SID
Delay is one value for all three voices. Key and tempo are estimates.

## Install

Install `TinySID.tar.gz` with Schwung's module manager, or unpack it into
`/data/UserData/schwung/modules/sound_generators/tinysid/`. Put your own `.sid` files in its `sids/` folder. No copyrighted tunes
are included; the built-in tune is an original demo. More in the [README](README.md) and [docs/TECHNICAL.md](docs/TECHNICAL.md).
