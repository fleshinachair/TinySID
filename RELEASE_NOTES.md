# TinySID release notes

## v0.9.0

First public candidate. TinySID plays C64 `.sid` (PSID) music on a built-in 6502 + SID core inside a Move track slot.

### What it does

- **Live 6502 + SID:** the tune's own player code runs on every frame (illegal opcodes included). The SID has 6581 and 8580
  filter models, ADSR, sync and ring modulation. PSID v1-v4, single-SID, PAL or NTSC; RSID is rejected.
- **Mixer:** per-voice level (0 mutes a voice), output trim, tempo sync and speed.
- **Song page:** file browser for `sids/`, subtune, estimated BPM and key/scale readouts (`D Min`, `C Maj`, or `--`).
- **Snoop:** pick a voice and see the note it is sounding. The first page of the Song level is a **piano roll**: key banner,
  note name, frequency and tuning error, and a two-octave keyboard with the sounding key lit.
- **Pad + Filter:** hand one voice to the pads and play it with the song's own instruments, including their pulse-width
  sweeps and vibrato. Override the tune's filter (low / band / high pass), with cutoff and resonance, and choose the chip model.
- **Transport:** follows MIDI Start / Stop / Continue (or the host's clock status). Stopping fades the voices out so nothing
  keeps ringing.
- **Host Lock:** the tune is phase-locked to the Move's MIDI clock. Details below.
- **Background scan:** a silent second engine plays each tune for 5 minutes of music (a few frames per audio block) to find
  the key, the tempo and the per-voice instrument lists.

### Host Lock phase lock

Host Lock used to scale the frame rate by `host tempo / song tempo`, which drifts against a clip that loops for a long time.
It now counts the tune's frames from MIDI clock ticks (24 per quarter note): a 16th-note row is 6 ticks, and after K ticks
exactly `floor(K * frames_per_row / 6) + 1` frames have played, in integer arithmetic. Start resets the count and the first
tick plays frame 1, so the tune's first downbeat lands on Step 1 of the clip. The Move's tempo never enters the count.

- The tune's row length comes from a built-in table (Commando subtune 1 = 6 frames per row = exactly 125 BPM) or from
  the scan, and the BPM readout is a whole number.
- Stop holds the tune and Continue resumes in place. If the clock disappears for 2 seconds the tune waits for it. With no clock
  at all it free-runs after 1 second so it is never silent.
- A Speed (`tempo_trim`) other than 1.0, or a typed BPM override, leaves the tick grid and scales the rate instead.
- Tested over 64 bars at 120 and 125 BPM with jittered ticks, a mid-run tempo change, Half and 2x, Start, Stop and Continue:
  zero frame drift at bar 64.

### Changes since v0.5.0

- **Output level:** TinySID was about 10 dB hotter than Schwung's other generators (Commando peaked at -0.1 dBFS). A fixed
  -9 dB base gain now puts a tune near -22 dBFS RMS, level with the others. `out_trim` still goes up to +6 dB on top.
- **Piano roll:** it is now a canvas *page* that is the first page of the Song level, so it shows as soon as you open the
  level (before it needed a click and looked inert).
- **Layout:** the Snoop dials moved to the second line of the Song page, Subtune moved from the Mixer to the Song page,
  and Pad and Filter share one page. The Pitch and Cents dials are gone (the roll still shows them).
- **Tempo:** a loaded file picks Free PAL or Free NTSC from its header. Loading an NTSC tune used to play it at PAL speed.
- **BPM:** one value from the whole-song scan, so it no longer wanders on tunes like Cybernoid II. Catalogued tunes use
  the table.
- **Pad sounds:** the list is the instruments the song really uses on the voice, merged when they differ only in sustain
  level or pulse width. Late instruments (such as Commando's sax lead) are found by the 5-minute scan. The built-in
  Saw / Pulse / Triangle sounds are gone.
- **DC offset:** an 8 Hz output high-pass removes the SID's unipolar DC.
- **Host warnings:** the `bad param preset=0` warning is fixed.

### Known limits

- RSID files are rejected; only single-SID PSID v1-v4.
- Combined waveforms are the AND of their parts. No digi / `$D418` sample timing, because the CPU runs one whole `play` call
  per tick rather than cycle-interleaved with the SID.
- Key and tempo are estimates: tunes that modulate show the key most windows agree on, and an unknown tune's BPM assumes
  16th-note rows.
- The piano roll and the page layout were checked against the Schwung UI's own page planner, contract validator and drawing
  code, and the module was run on a Move, but the OLED was only looked at on the device by the author.

### Install

Download `TinySID.tar.gz` from the release and install it with Schwung's module manager, or unpack it so the files sit in
`/data/UserData/schwung/modules/sound_generators/tinysid/`. Put your own `.sid` files in its `sids/` folder. The release
package contains no copyrighted tunes: the default tune is a bundled original demo.

### Files in the package

`dsp.so`, `module.json`, `canvas.js` (piano roll), `help.json`, `release.json`.
