# TinySID

Play classic Commodore 64 music on your Ableton Move.

TinySID is a Schwung sound generator that runs `.sid` tunes on a built-in
C64 chip, live. Mute voices, reshape the filter, jam on the pads with the song's own instruments, and lock the tune to
your Move's tempo so it stays in time with your clips.

Version 0.9.0. See the [release notes](RELEASE_NOTES.md).

## Install

1. Install `TinySID.tar.gz` from the releases page with Schwung's module manager.
2. Copy your own `.sid` files into the module's `sids` folder.
3. Load TinySID in a track slot and pick a tune with **SID File** on the Tune + Loop page.

A built-in demo tune plays until you choose one. No commercial tunes are included.

## Using it

**Mixer.** Set the level of each of the three SID voices (0 mutes it), the output trim, and the tempo mode. The
estimated **BPM** and **key** of the tune are read out here too (they show **0** and **--** until they are known).

**Tune + Loop.** Pick a tune (**SID File**) and subtune. TinySID listens to the tune in the background for a few seconds to
find its BPM and key, shown on the Mixer. The loop controls (see below), **Timbre Lock** and **AutoTune** are here too.

**AutoTune** brings the tune into tune with the Move. Many tunes sit a constant 20 to 60 cents off A = 440 Hz (they were tuned
for the other video standard's clock, or by ear). TinySID learns that offset from the tune's own notes and takes it off every
note, so melodies, arpeggios, slides, drum sweeps and vibrato all keep their shape and nothing is moved to the semitone beside
it. Notes that are held are also drawn onto the exact note (**A440 Chromatic**) or onto the nearest note of the tune's key
(**In-Key**), and a vibrato stays a vibrato. **Master Tune** (-100 to +100 cents) shifts the whole grid, for tunes you want at
A = 435 or A = 444. It needs a few seconds of the tune to learn the offset.

**Piano roll.** The first screen of the module is a **piano roll** showing the note the song's most recent voice is playing, with
its tuning error in cents. Three small meters in its banner show the level of each voice (a dotted line is a muted voice), and
the key shows as the song is now transposed. Its encoders are the Mixer's, so you can ride the voice levels while you watch it.
**Transpose** (Mix + Glue page) moves every voice up or down by up to an octave, on top of AutoTune, and the key readouts follow
it. A pitch-bend message bends the whole song by up to two semitones.

**Mix + Glue.** **Filter Pump** is a low-pass on the output that closes on every beat and opens again, with the same triggers and
release as **Duck** (the Move's quarter notes, counted from Play, and MIDI note 36). Turn it up (try 0.6) and set **DuckRel** to
100 to 250 ms. It works on any tune and at any Filter setting: it is its own filter, not the SID's, so it needs no setup and does
nothing to the sound between beats. **Lo-Fi** brings the bit depth and the sample rate down together,
from clean to crunchy. **Spread** sets the stereo picture: 0 is the old mono sound, at 1 voice 2 sits on the left and voice 3 on the
right (voice 1 stays in the middle, and a voice is no louder for moving). **Duck** dips the whole output on every quarter note, so the tune pumps with a kick on the beat; **DuckRel** is how
long it takes to come back. The beat is the Move's clock, counted from Play, while the Move is playing (and MIDI note 36 is a kick
too). When no clock is arriving, which is the case whenever you are just playing the SID with the Move stopped, Duck and Filter Pump keep the
tune's own beat instead: a quarter note of its tempo, from the top of the tune (Est BPM on the Mixer page). **FX Duck** is how far
the song dips when a sound effect hits (below), 0 to 1. The output also runs through a 35 Hz high-pass to leave the bottom end to the Move's kicks and bass.

**Sound effects.** Many tunes carry more than the song: their other subtunes are jingles and effects, a shot, a pickup, a death
sting. TinySID listens to every subtune in the background and keeps the ones that sound and then stop within about six seconds (a few seconds after loading a
tune; up to eight, in subtune order; a subtune that runs longer is a second tune, not an effect). In Pad Mode Control the top row of pads (the eight on the far side) plays them: purple where there
is an effect, white as you hit it, dark where there is none, and the harder you hit, the louder. Each effect plays on a spare player
of its own over the song, so the song carries on, and up to three can sound at once. They use the song's chip, filter, level, spread,
tuning and transpose, but not its mutes. The song dips for a moment as one hits (**FX Duck**, on the Mix + Glue page; 0 turns it off). Not
every tune has effects as subtunes: some fire them from inside the song, and those cannot be played this way.

**Notes Out.** Sends the SID voices' notes as MIDI, so other Move tracks can play what the SID plays. In Pad Mode Control the
**row above the controls** has three pads, one per voice: green while that voice is being sent, grey when it is not; press to switch.
They are separate from the mute pads, so you can mute a voice in the audio and still send it, or the other way round. The **Notes
Out** page has the same three switches (**Send 1, 2, 3**) and a channel for each (**Chan 1, 2, 3**): voice 1 goes to MIDI channel 1,
voice 2 to channel 2 and voice 3 to channel 3 until you change them, which are the Move's first three tracks, so put TinySID on
track 4 and set those tracks' MIDI input to match. Each voice has a channel of its own because pitch bend is per channel: voices
sharing one would bend each other. The switches start off on a new module, and what you set is saved with the set. The note is the
nearest semitone to the pitch you hear, and the rest (up to half a semitone, and any vibrato or glide) goes out as pitch bend,
assuming the receiver's bend range is two semitones (the usual default), so a tune that sits 37 cents sharp is played 37 cents sharp
downstream as well. Noise and the pad's voice send nothing, and a note is let go when you stop, switch a voice off, change its
channel, or remove the module. The notes also leave by USB-A on cable 2, which is how Schwung delivers them. If TinySID is on one of the
channels it sends on, that channel is skipped (the notes would only come back in), and notes arriving on a channel in use are ignored.
The notes Notes Out sends come back: to the screen, looking exactly like pad presses, and into TinySID's own track if its MIDI input
is on (turn MIDI In off on TinySID's track; it does not need it to send). The module knows every note it sent and expects each back once:
the first press or incoming note that matches one, within two seconds, is ignored, so neither presses a button nor plays the pad voice.
A pad pressed at the very moment its own note is on the way back may need a second press. While TinySID uses its own screen, incoming
notes are never buttons. The Move also lights a pad green for every note that plays on its **selected** track, over TinySID's colours;
the module paints its own colours back on the next frame, but a busy tune still flickers. So while you use Control mode, select
TinySID's own track on the Move (the one with MIDI In off) before opening TinySID: then no notes play on the selected track and nothing
flashes. The other tracks play the notes a little after TinySID's own sound, because they go through the Move's MIDI input and their own
instruments. **SID Delay** (Notes Out page, 0 to 200 ms, 50 to start with) holds TinySID's sound back by that much, but only while a
voice is being sent, so with Notes Out off there is no delay at all. Adjust it until they sound together. All three voices wait together
(delaying only the sent ones would put the tune out of time with itself), and switching a first voice on or the last one off leaves a
short gap, as long as the delay, while it changes.

**Pad + Filter.** Hand one SID voice to the pads and play it with the song's own instruments. **Pad Sound** lists
them. The filter section lets you replace the tune's filter with low, band or high pass. **Pad Mode** is Play, where the pads play the voice, or Control, where the pads become buttons. In Control mode the **bottom row**
of pads (left to right) is: mute / unmute voice 1, voice 2, voice 3; unmute all; transpose down; transpose up; transpose back to
zero. They light in their own colours (the three voices all blue, grey while muted; unmute white; transpose orange, yellow and
red). The **eighth pad** captures a loop: press it to start a loop on the bar that is playing (it flashes red), and again to end it
on the bar playing then (steady red while it loops); **Loop Start** and **Loop Len** are set to match, up to 8 bars, and the song
carries on round that stretch. Press it again to capture a new one, or turn Loop Len to Off to let the song run on. It needs the Move's
transport running, since bars are counted from Play. The row above holds the three **Notes Out** switches (below). The **top row** is a little drum set of the tune's own sound effects. The rest of the grid goes dark. Back in Play mode the Move's own pad lights return. Switching Pad Mode lets go of any note
you are holding. The module has its own screen to do this (`ui_chain.js`), which is also why the **SID File** cell opens a small
list of its own: hold the SID File knob and click the jog wheel to open it, turn the jog wheel to pick a file and click, and Back closes it. The pad notes are in tune with the song as it sounds: with
AutoTune off the pad shares the song's own tuning (a tune 37 cents sharp gets a pad 37 cents sharp), and with it on both sit on
A = 440 Hz. A pad note rings out for 300 ms at most after you let go (the songs' own releases run to 24 seconds), and pitch movement
wider than a semitone in a Pad Sound (an arpeggio or a swoop) is not carried onto the notes you play; vibrato and trills are, and
so is the pulse-width movement. **Chip** (6581 or 8580) is on this page too. The pad voice is yours alone: the
song cannot change its pitch, waveform or envelope, whatever its other voices do. On a tune that doesn't use the filter,
**Cutoff** and **Reso** act on the pad (it starts fully open: turn Cutoff down to darken it). On a tune that does use its
filter, they offset the song's filter for the voices the song routes, and the pad stays out of it; pick Low, Band or High
Pass in **Filter** to put Cutoff and Reso in charge of everything including the pad.

**Timbre Lock** freezes the sound: each voice's last real instrument (waveform, pulse width and envelope) and the filter are
held, while the tune's notes and gates keep playing. Change subtune or load another tune and it plays through the old sound.
Drum hits (pure noise) pass through untouched, so drums stay drums, and so do the tune's own "hard restart" writes that let
short notes start.

**Loop Len**, **Loop Start** and **Loop Nudge** (Tune + Loop page) repeat a stretch of the song. With Loop Start 5 and Loop Len 4 Bars the tune is caught at
the top of bar 5 and, when bar 9 arrives, rewound to bar 5, over and over. Bars are counted from Play (bar 1 is the
first), so it needs the Move's clock running, and **Host Lock** keeps the repeats exact. Set Loop Len to Off and the song
carries on; switch it back on later and it jumps back to the loop at the next bar line. A Loop Start the song has already
passed waits until the next time you press Play. A bar is the tune's own 16 rows, so at Host Lock 0.5x or 2x it is two bars
or half a bar of the Move. If a tune starts with a pickup and the loop sounds a few steps early or late, set **Loop Nudge**
(0 to 15 sixteenths) until the loop starts on the real downbeat. When the loop turns round, a note
still sounding rings out instead of cutting off.

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
