# TinySID

Turn classic Commodore 64 music into a live instrument on your Ableton Move.

TinySID is a Schwung sound generator. It runs `.sid` tunes on a built-in 6502 + SID
chip emulation, live and locked to your Move's tempo.

- **Send SID voices to other tracks.** Each voice can go out as MIDI and play any Move instrument, tuning included.
- **Borrow one tune's sound for another.** Lock a tune's instruments with **Timbre Lock**, then load a different `.sid`.
- **Play it from the pads.** Mute voices, transpose, capture loops, fire the tune's sound effects, or play its real instruments.

Version 0.9.0 · [Release notes](RELEASE_NOTES.md)

## Install

1. Install `TinySID.tar.gz` from the [releases page](https://github.com/fleshinachair/TinySID/releases) with Schwung's module
   manager (or unpack it into `/data/UserData/schwung/modules/sound_generators/tinysid/`).
2. Copy your `.sid` files into the module's `sids` folder (Schwung's web manager or `scp`).
3. Load TinySID in a track slot and pick a tune with **SID File** on the Tune + Loop page.

A built-in demo tune plays until you choose one. No commercial tunes are included.

## Pages

| Page | Controls |
|---|---|
| **Mixer** | Opens on a piano roll (current note, tuning in cents, a meter per voice). Voice 1–3 level (0 mutes), **Tempo Sync**, **Speed**, and read-outs for estimated **BPM** and **Key** (`0` / `--` until known). |
| **Tune + Loop** | **SID File** (hold its knob and click the jog wheel to browse), **Subtune**, **Timbre Lock**, **AutoTune**, **Master Tune**, **Loop Len / Start / Nudge**. |
| **Mix + Glue** | **Spread**, **Duck** + **DuckRel** (pump on every beat), **Filter Pump**, **Lo-Fi**, **Transpose** (±12), **FX Duck**. |
| **Pad + Filter** | **Filter**, **Cutoff**, **Reso**, **Chip** (6581 / 8580), **Play On Voice**, **Pad Sound**, **Pad Mode** (Play / Control). |
| **Notes Out** | Send voice 1–3 as MIDI, each voice's channel, **SID Delay**. |

Duck and Filter Pump follow the Move's clock, or the tune's own beat when the Move is stopped.

## Playing the pads

**Play mode.** Set **Play On Voice** to hand one voice to the pads. It plays your notes with one of the song's own instruments
(**Pad Sound**), in tune with the song. One note at a time; velocity sets level.

**Control mode.** Set **Pad Mode** to Control and the pads become lit buttons:

| Pads | Action |
|---|---|
| Bottom row 1–3 | Mute / unmute voice 1, 2, 3 |
| Bottom row 4–7 | Unmute all · transpose down · up · reset |
| Bottom row 8 | **Loop capture**: press to start a loop on the current bar (flashing), again to end it (steady red). Up to 8 bars; needs the Move's transport running. |
| Second row 1–3 | Send voice 1, 2, 3 as MIDI (green = on) |
| Top row | The tune's **sound effects** (purple), louder the harder you hit |

Sound effects are short extra subtunes TinySID finds in the file (up to eight, three at once). The song dips as one hits
(**FX Duck**). Tunes that fire effects from inside the song have none to find.

## Notes Out

Play another Move track from each SID voice:

1. Put TinySID on its own track and turn that track's **MIDI In off** (or it hears its own notes).
2. Give tracks 1–3 instruments with MIDI In on channels 1, 2, 3 (the default send channels).
3. Select TinySID's own track before opening it, or the Move's note lights flicker over its pads.
4. Switch voices on (second pad row or Notes Out page) and set **SID Delay** by ear (0–200 ms, default 50).

Each note goes out as the nearest semitone plus pitch bend (±2 semitone range assumed), so detuned tunes stay detuned. Sending is
separate from muting: you can mute a voice and still send it. Send switches start off and are saved with the set.

## Sound and tuning

- **Timbre Lock** freezes each voice's waveform, pulse width and envelope, plus the filter. Load another tune or subtune and it
  plays through the old sound. Drums pass through untouched.
- **AutoTune** learns the tune's offset from A = 440 Hz (often 20–60 cents) over a few seconds and removes it, keeping vibrato and
  slides. **A440 Chromatic** also pulls held notes onto exact pitches; **In-Key** onto the tune's key. **Master Tune** shifts
  everything ±100 cents.
- **Filter** = Song keeps the tune's filter, with Cutoff and Reso as offsets. Low / Band / High Pass runs all voices through that
  filter, with Cutoff and Reso absolute.

## Loops and timing

- **Loops.** Loop Start 5 + Loop Len 4 Bars repeats bars 5–8. A bar is 16 of the tune's rows. If the loop sounds early or late
  (tunes with a pickup), adjust **Loop Nudge** (0–15 sixteenths).
- **Free (PAL / NTSC)** plays the tune at its natural speed.
- **Host Lock 1x / 0.5x / 2x** follows the Move's clock without drift and starts on Step 1. Use it for exact loops.
- **Speed** changes tempo without changing pitch (and leaves Host Lock).
- Transport: play starts from the top, stop silences, continue resumes.

## Limits

- Single-SID PSID files only. No RSID or multi-SID tunes.
- Sampled "digi" sounds don't play; combined waveforms are approximate.
- Key and BPM are estimates; some tunes have no clear key (`--`).
- Notes Out assumes a ±2 semitone pitch-bend range, and SID Delay is shared by all three voices.

## For developers

```bash
scripts/build.sh              # cross-build build/TinySID.tar.gz (needs python3 + aarch64-linux-gnu-gcc or zig)
RELEASE=1 scripts/build.sh    # use for anything you publish: never bundles a copyrighted tune
scripts/test.sh               # native unit tests (ASan/UBSan where available), UI tests if node is installed
```

Internals are in [docs/TECHNICAL.md](docs/TECHNICAL.md). SID tunes are copyrighted by their authors: keep your `.sid` files out of
the repo (they're git-ignored).
