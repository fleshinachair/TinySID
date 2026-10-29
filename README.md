# TinySID

Turn classic Commodore 64 music into a live instrument on your Ableton Move.

TinySID is a Schwung sound generator that runs `.sid` tunes on a built-in C64 chip, live, locked to your Move's tempo.

- **Send the SID voices to your Move tracks.** Each of the three voices can go out as MIDI on its own channel and play any
  instrument on another track, with the tune's own tuning carried on pitch bend. See [Notes Out](#notes-out).
- **Borrow one SID's sound for another tune.** Lock a tune's instruments with **Timbre Lock**, load a different `.sid`, and it
  plays its own notes with the first tune's sound.
- **Play it from the pads.** Mute voices, transpose, capture a loop on the bar you're on, fire the tune's own sound effects, or
  hand a voice to the pads and play the song's real instruments.

Version 0.9.0. See the [release notes](RELEASE_NOTES.md).

## Install

1. Install `TinySID.tar.gz` from the releases page with Schwung's module manager.
2. Copy your own `.sid` files into the module's `sids` folder.
3. Load TinySID in a track slot and pick a tune with **SID File** on the Tune + Loop page.

A built-in demo tune plays until you choose one. No commercial tunes are included.

## The pages

- **Mixer.** The level of each voice (0 mutes it), the tempo mode, and the tune's estimated BPM and key (they read **0** and
  **--** until known). Its first screen is a piano roll: the note sounding now, its tuning error in cents, and a meter per voice.
- **Tune + Loop.** **SID File** (hold its knob and click the jog wheel to open the list, turn to pick, click), **Subtune**,
  **Timbre Lock**, **AutoTune**, **Master Tune**, and the loop controls.
- **Mix + Glue.** Stereo **Spread**, **Duck** and **DuckRel** (the tune pumps on every beat), **Filter Pump**, **Lo-Fi**,
  **Transpose** (up to an octave), and **FX Duck**. Duck and Filter Pump follow the Move's clock, or the tune's own beat when the
  Move is stopped.
- **Pad + Filter.** **Play On Voice** hands one voice to the pads, played with the song's own instruments (**Pad Sound**) and
  kept in tune with the song. **Filter**, **Cutoff** and **Reso** replace or offset the tune's filter. **Chip** picks the 6581
  or 8580. **Pad Mode** switches the pads between playing and Control.
- **Notes Out.** Send each voice as MIDI, its channel, and **SID Delay**. See below.

## Control mode

Set **Pad Mode** to Control and the pads become buttons, lit in their own colours:

| Pads | What they do |
|---|---|
| Bottom row, 1 to 3 | Mute / unmute voice 1, 2, 3 |
| Bottom row, 4 to 7 | Unmute all, transpose down, transpose up, transpose back to 0 |
| Bottom row, 8 | **Loop capture** (below) |
| Second row, 1 to 3 | Send voice 1, 2, 3 as MIDI (green is on) |
| Top row | The tune's own **sound effects**, purple, louder the harder you hit |

**Loop capture:** press to start a loop on the bar that's playing (the pad flashes red), and again to end it on the bar playing
then (steady red). **Loop Start** and **Loop Len** are set to match, up to 8 bars, and the song goes round that stretch. Press it
again for a new loop, or set Loop Len to Off to run on. Bars are counted from Play, so the Move's transport must be running.

**Sound effects:** many tunes carry jingles and effects as extra subtunes. TinySID finds the short ones (up to eight) and plays
them over the song, three at a time at most. The song dips as one hits (**FX Duck**). Tunes that fire effects from inside the song
have nothing to find.

## Notes Out

Each SID voice can play an instrument on another Move track:

1. Put TinySID on its own track (say track 4) and turn that track's **MIDI In off**, or it hears its own notes coming back.
2. Give tracks 1 to 3 an instrument and set their MIDI In to channels 1, 2 and 3. Those are the channels voices 1, 2 and 3 send on
   to start with; change them on the Notes Out page.
3. Select TinySID's own track on the Move before opening it. The Move lights a pad green for every note on its selected track,
   which would flicker over TinySID's colours.
4. Switch voices on with the second row of pads or the Notes Out page, then set **SID Delay** by ear so the SID lines up with the
   tracks (0 to 200 ms, 50 to start with, only applied while a voice is being sent).

The note sent is the nearest semitone to the pitch you hear, and the rest goes out as pitch bend (a ±2 semitone range is
assumed), so a tune that sits 37 cents sharp is played 37 cents sharp on the other track too. Each voice has its own channel
because pitch bend is per channel. The switches start off on a new module and are saved with the set. The mute pads only affect
the SID's own sound, so you can mute a voice and still send it.

## Timbre Lock

Turn it on while a tune plays and the sound is frozen: each voice's waveform, pulse width and envelope, and the filter. The
tune's notes keep playing. Change subtune or load another `.sid` and it plays through the old sound. Drum hits pass through
untouched, so drums stay drums.

## AutoTune

Many tunes sit a constant 20 to 60 cents off A = 440 Hz. TinySID learns the offset from the tune's own notes and takes it off
every note, so melodies, slides, drum sweeps and vibrato keep their shape. **A440 Chromatic** also draws held notes onto the exact
note, and **In-Key** onto the tune's key. **Master Tune** (-100 to +100 cents) shifts the whole grid. It needs a few seconds of the
tune to learn the offset.

## Loops

**Loop Len**, **Loop Start** and **Loop Nudge** repeat a stretch of the song. With Loop Start 5 and Loop Len 4 Bars, the tune
is caught at the top of bar 5 and rewound to it when bar 9 arrives, over and over. A bar is the tune's own 16 rows. If a tune
starts with a pickup and the loop sounds early or late, set **Loop Nudge** (0 to 15 sixteenths) until it starts on the real
downbeat. **Host Lock** keeps the repeats exact.

## Keeping time

- **Free** modes play the tune at its natural speed.
- **Host Lock** follows the Move's clock, so the tune stays in step with your clips however long they loop and starts on Step 1
  when you press play. **0.5x** and **2x** play it at half or double speed.
- **Speed** nudges the tempo without changing pitch (this takes the tune out of the lock).

The tune follows the Move's transport: play starts it from the top, stop silences it, and continue picks up where it left off.

## Limits

- Single-SID `.sid` files only (PSID format). RSID files and tunes that need two SID chips are not supported.
- Sampled "digi" sounds don't play.
- Key and BPM are estimates, and some tunes have no clear key (shown as `--`).
- Notes Out assumes the receiving instrument's pitch-bend range is ±2 semitones, and SID Delay is one value for all three voices.

## For developers

Build with `scripts/build.sh` (use `RELEASE=1` for anything you publish, so no copyrighted tune is bundled) and test with
`scripts/test.sh`. How it works inside is in [docs/TECHNICAL.md](docs/TECHNICAL.md).

SID tunes are copyrighted by their authors. Keep your own `.sid` files out of the repo; they are git-ignored.
