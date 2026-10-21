# TinySID

Play classic Commodore 64 music on your Ableton Move.

TinySID is a Schwung sound generator that runs `.sid` tunes on a built-in
C64 chip, live. Mute voices, reshape the filter, jam on the pads with the song's own instruments, and lock the tune to
your Move's tempo so it stays in time with your clips.

Version 0.9.0. See the [release notes](RELEASE_NOTES.md).

## Install

1. Install `TinySID.tar.gz` from the releases page with Schwung's module manager.
2. Copy your own `.sid` files into the module's `sids` folder.
3. Load TinySID in a track slot and pick a tune with **SID File** on the Song page.

A built-in demo tune plays until you choose one. No commercial tunes are included.

## Using it

**Mixer.** Set the level of each of the three SID voices (0 mutes it), the output trim, and the tempo mode.

**Song.** Pick a tune and subtune. TinySID listens to it in the background for a few seconds and shows the
estimated **BPM** and **key**. The second line is the **Snoop**: choose a voice and see the note it's playing.
Open the Song level and the first screen is a **piano roll** showing that voice on a keyboard.

**Pad + Filter.** Hand one SID voice to the pads and play it with the song's own instruments. **Pad Sound** lists
them. The filter section lets you replace the tune's filter with low, band or high pass, and switch between the two
classic chip models (6581 and 8580).

## Keeping time

- **Free** modes play the tune at its natural speed.
- **Host Lock** follows the Move's clock. The tune stays in step with your clips however long they loop, and when you
  press play the tune starts on Step 1. **0.5x** and **2x** play it at half or double speed.
- **Speed** nudges the tempo up or down without changing pitch (this takes the tune out of the lock).

The tune follows the Move's transport: play starts it from the top, stop silences it, and continue picks up where it left off.

## Piano roll

The filled key is the note sounding now. Small bars along the bottom of a key are notes from the last half second.
Black keys are always drawn solid; a hollow black key is the one sounding.

## Limits

- Single-SID `.sid` files only (PSID format). RSID files and tunes that need two SID chips are not supported.
- Sampled "digi" sounds don't play.
- Key and BPM are estimates, and some tunes have no clear key (shown as `--`).

## For developers

Build with `scripts/build.sh` (use `RELEASE=1` for anything you publish, so no copyrighted tune is bundled) and test with
`scripts/test.sh`. How it works inside is in [docs/TECHNICAL.md](docs/TECHNICAL.md).

SID tunes are copyrighted by their authors. Keep your own `.sid` files out of the repo; they are git-ignored.
