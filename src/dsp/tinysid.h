/* TinySID engine: PSID player (6502 + SID), parameters, register intercepts
 * and pad jamming. Host-agnostic; plugin.c adapts it to the Schwung API. */
#ifndef TINYSID_H
#define TINYSID_H

#include <stddef.h>
#include <stdint.h>

#include "psid.h"

typedef struct tinysid tinysid_t;

/* Allocation hooks, so tests can count allocations. Default to malloc/free. */
extern void *(*tinysid_malloc)(size_t);
extern void (*tinysid_free)(void *);

tinysid_t *tinysid_create(double sample_rate);
void tinysid_destroy(tinysid_t *t);

/* Parses and loads a PSID image (copied), then (re)initialises the current
 * subtune. Returns a PSID_* code; on error the previously loaded tune stays. */
int tinysid_load(tinysid_t *t, const uint8_t *data, size_t len);
int tinysid_load_file(tinysid_t *t, const char *path);

/* Parameter keys: subtune, v1_vol, v2_vol, v3_vol, out_trim, sync_mode, tempo_trim, song_bpm,
 * song_key ("D Min", "C Maj", ... or "--"), song_scale, snoop_voice, snoop_note (readouts), chip_model, filter_mode, cutoff_bias, res_bias, pad_target, pad_sound
 * (a name or position in the list of the tune's own instruments for the pad voice, "Lead" while none are known),
 * load_path (set only), host_bpm (get only),
 * pitch_snap, master_tune (see AutoTune below), stereo_spread, ducking_depth, ducking_release (see Audio glue below),
 * timbre_lock ("Unlocked" / "Lock Timbre": the tune's pulse width, waveform, envelope and filter writes are held at what
 * they were when it was switched on; pitch, gates and master volume still follow the tune),
 * loop_nudge (0..15 rows) moves the loop's bar lines that many rows after the first tick, for a tune with an intro pickup.
 * A bar is the tune's 16 rows: 96 ticks at Host Lock 1x, 192 at 0.5x, 48 at 2x (96 whenever the lock is not engaged).
 * loop_capture ("start" / "stop" / "toggle" / "cancel"; reads "idle", "recording" or "looping"): start a loop on the bar playing, end it
 * on the bar playing at the stop, and set loop_start and loop_length to match (8 bars at most; needs the clock running).
 * loop_start (1..256) and loop_length ("Off" / "1 Bar" / "2 Bars" ... "8 Bars": the 6502 is saved at the top of bar
 * loop_start of the run and put back every loop_length bars, so that stretch repeats. Counted on the MIDI clock, 96 ticks to a bar,
 * bar 1 starting at the first tick after Start; a start bar already passed waits for the next run. Host Lock makes the repeats
 * frame-exact).
 * song_bpm reads 0 until the tempo is known (catalogued, estimated or typed); song_key reads "--" until the scan has chosen one.
 * song_bpm reads the known/estimated tempo of the tune and ignores writes; song_bpm_override (set only)
 * pins the reference tempo until the tune changes.
 * Values are strings so they map straight onto the host's key/value API.
 * set returns 0 on success, -1 for an unknown key or unparsable value.
 * get returns the string length, or -1 for an unknown key. */
int tinysid_set_param(tinysid_t *t, const char *key, const char *value);
int tinysid_get_param(const tinysid_t *t, const char *key, char *buf, int buf_len);

/* AutoTune (parameters pitch_snap: "Off" / "A440 Chromatic" / "In-Key (Scale)", and master_tune, -100..100 cents). Notes are
 * measured on the tune's own clock (PAL 985248 Hz or NTSC 1022727 Hz from the file header, whatever the sync mode), against the
 * grid A = 440 Hz + master_tune. Two things happen, per voice and per play() frame:
 *   - the tune's own tuning (a constant distance from that grid, learned from its notes) is taken off every frame, so leaps,
 *     arpeggios, glides, drum sweeps and vibrato keep exactly their shape;
 *   - a voice that holds a note (less than 40 cents a frame for 3 frames) is also drawn onto the nearest note, or the nearest
 *     note of song_key; the pitch's smoothed centre moves, so a vibrato stays a vibrato.
 * Noise voices and the pad voice are never touched. In-Key does nothing until the scan has found a key. */
enum { TINYSID_SNAP_OFF = 0, TINYSID_SNAP_CHROMATIC, TINYSID_SNAP_KEY };
/* The word of the note nearest to `word` on the grid A = 440 Hz + tune_cents (the rule the held notes are pulled by). */
uint16_t tinysid_snap_word(uint16_t word, double clock_hz, int mode, int key, int scale, double tune_cents);

/* Audio glue: stereo_spread (0..1, default 0.5: voice 1 centre, voice 2 left, voice 3 right, equal power; 0 is the old mono
 * sound), ducking_depth (0..1, default 0 = off) and ducking_release (20..300 ms, default 80): the output is dipped by
 * `depth` on every quarter note of the host's MIDI clock (counted from Start, in every sync mode) and on MIDI note 36, over
 * 2 ms, and recovers exponentially. The output always passes a 35 Hz DC-blocking high-pass, one per channel. */
typedef struct { float x1, y1, coef; } tinysid_hpf_t;
void tinysid_hpf_init(tinysid_hpf_t *f, double sample_rate, double hz);
float tinysid_hpf_run(tinysid_hpf_t *f, float x);       /* y[n] = x[n] - x[n-1] + R y[n-1] */

/* Performance: transpose (-12..12 semitones, moves every song voice on top of AutoTune), MIDI pitch bend (+-2 semitones),
 * pad_mode ("Play" / "Control": in Control the pads from note 36 up are buttons: 36 37 38 mute voice 1 2 3, 39 unmutes all,
 * 40 41 42 transpose down / up / reset; the notes the pad plays are in tune with the song as it sounds: the tune's own
 * tuning when AutoTune is off, the master-tuned grid when it is on; a pad note rings out for 300 ms at most, and recorded pitch
 * movement wider than a semitone is not carried onto it), voice_mutes (bit mask), filter_pump (0..1: a low-pass on the output closes on every beat that triggers
 * the ducker, from 20 kHz down to about 310 Hz at full depth, and opens with the ducker's release; it is the output's own filter, so
 * it works on any tune and leaves the tune's filter alone), lofi (0..1: bit depth and sample rate down together), last_note (readout: the last note-on seen)
 * and mix_feed (the voice meters: "M,<l1>,<l2>,<l3>,<mute mask>,<transpose>").
 * song_key and the snoop feed show the key as transposed. */
/* Raw MIDI bytes: note on/off, CC123, and real-time clock/start/stop/continue (transport only in Host Lock modes). */
void tinysid_midi(tinysid_t *t, const uint8_t *msg, int len);

/* Renders `n` stereo frames. Output is always finite and within [-1, 1]. */
void tinysid_render(tinysid_t *t, float *left, float *right, int n);

/* Introspection (used by tests and the UI). */
const psid_info_t *tinysid_info(const tinysid_t *t);   /* NULL before a tune is loaded */
uint8_t tinysid_sid_reg(const tinysid_t *t, int reg);  /* effective value the SID chip sees */
uint8_t tinysid_ram(const tinysid_t *t, uint16_t addr);
uint64_t tinysid_play_calls(const tinysid_t *t);
double tinysid_frame_hz(const tinysid_t *t);   /* current play() rate */
double tinysid_host_bpm(const tinysid_t *t);   /* smoothed MIDI-clock tempo, 120 until a clock arrives */
int tinysid_song_bpm(const tinysid_t *t);      /* BPM readout, 60..200 */
/* Frames per 16th-note row as a ratio (num / den); returns 0 when the row length is not known. Host Lock counts
 * frames from MIDI clock ticks with it: 6 ticks per row at 1x, so the count never drifts. */
int tinysid_song_fpr(const tinysid_t *t, int *num, int *den);
int64_t tinysid_clock_ticks(const tinysid_t *t);   /* MIDI clock ticks since the tune started; -1 before the first */
double tinysid_samples_per_frame(const tinysid_t *t);   /* sample_rate / play() rate */
int tinysid_song_key(const tinysid_t *t);      /* 0..11 = C..B, 12 = not known yet */
int tinysid_song_scale(const tinysid_t *t);    /* 0 major, 1 minor, 2 not known */
int tinysid_analysis_done(const tinysid_t *t); /* the key/scale/tempo pre-scan of the current tune has finished */
int tinysid_running(const tinysid_t *t);       /* the 6502 is being stepped */
uint16_t tinysid_cpu_pc(const tinysid_t *t);
int tinysid_phrase_armed(const tinysid_t *t);          /* the Bar Looper holds a snapshot of the 6502 */
int tinysid_song_bar(const tinysid_t *t);              /* bar of song time (1 = first), 0 before the first clock tick */
uint64_t tinysid_phrase_rewinds(const tinysid_t *t);   /* times the 6502 has been rewound since the engine was created */
int tinysid_snoop_note(const tinysid_t *t);    /* MIDI note the snooped voice sounds now, 0 = idle */

/* The key/scale/tempo/instrument scan normally advances a few frames per audio block. Hosts and tests can also run
 * it headlessly (6502 only, no audio): frames done so far, and a way to advance it. */
void tinysid_scan_advance(tinysid_t *t, int frames);
int tinysid_scan_frames(const tinysid_t *t);
int tinysid_pad_gesture_length(const tinysid_t *t);   /* frames of recorded movement in the chosen pad sound; 0 = none */

/* JSON array of the Pad Sound choices for the current pad voice: the tune's own instruments by name, or just
 * ["Lead"] while none are known. A list too long for the buffer ends after the last name that fits. Returns its length, or -1 if
 * the buffer is too small for even ["Lead"]. */
int tinysid_pad_options(const tinysid_t *t, char *buf, int buf_len);

/* Sound effects: the tune's other subtunes that sound once and stop are found by a silent scan (a few frames per audio block, like
 * the key scan; FX_SLICE in the engine) and offered as up to 8 effects, in subtune order. Parameters: fx_trigger (set only,
 * "<effect>" or "<effect>,<velocity>", effects counted from 0: plays it on a spare engine mixed over the song), fx_duck (0..1,
 * default 0.4: how far the song dips when one hits, back up with the ducking release), and the readouts fx_count (effects found so
 * far), fx_ready (1 once every subtune has been listened to) and fx_list (their subtunes, comma separated). An effect sounds with the
 * song's chip, filter, level, spread, tuning and transpose, but not its mutes or loop. At most 3 sound at once. */
void tinysid_fx_scan_advance(tinysid_t *t, int frames);   /* the effect scan, headlessly (tests and benchmarks) */
int tinysid_fx_count(const tinysid_t *t);
int tinysid_fx_ready(const tinysid_t *t);
int tinysid_fx_scan_frames(const tinysid_t *t);   /* frames the effect scan has listened to, over every subtune */

/* Notes out. Parameters midi_out_v1, midi_out_v2, midi_out_v3 ("Off" / "On", default Off: which voices are sent) and midi_out_ch1,
 * midi_out_ch2, midi_out_ch3 (1..16, default 1, 2 and 3: the channel each voice is sent on; pitch bend is per channel, so voices
 * that share one bend each other).
 * A voice's notes become MIDI events: the note is the nearest semitone to the pitch the chip is playing (after AutoTune, transpose
 * and bend), and the rest of the pitch, up to half a semitone and any vibrato or glide, is carried by pitch bend, assuming the
 * receiver's bend range is +-2 semitones, so a tune that sits 37 cents sharp is played 37 cents sharp downstream. A pitch more
 * than 2 semitones from its note, and every new gate, is a new note. Noise and the pad's voice send nothing. Muting a voice in the
 * audio does not stop its notes: the two are muted apart. Stopping, switching a voice off or changing its channel lets go of the
 * sounding note. The engine only queues the events (type 0 note off, 1 note on with "value" the velocity, 2 pitch bend with
 * "value" 0..16383, each with its channel 0..15); the host drains them and sends them. */
int tinysid_midi_out_peek(const tinysid_t *t, int *type, int *note, int *value, int *channel);   /* the oldest event, 0 if none; it stays queued */
void tinysid_midi_out_drop(tinysid_t *t);              /* the host has taken that event */
int tinysid_midi_out_listens(const tinysid_t *t, int channel);   /* notes are being sent on this channel (0..15) */
void tinysid_midi_out_release(tinysid_t *t);           /* queue a note off for every sounding note (before the instance goes away) */
int tinysid_midi_out_pending(const tinysid_t *t);       /* events waiting now */
int tinysid_midi_out_longest(const tinysid_t *t);       /* the most that have waited at once */
/* A pitch bend queued while an older one for the same channel is still waiting (with no note after it) takes that one's place: only
 * the latest bend matters, and a voice's vibrato can never build up a queue for its notes to wait behind. */

/* What Notes Out sends comes back to the Move's screens as plain note-ons, and with the channel gone: a note in the pad range (68..99)
 * is byte for byte a pad press, whichever channel it was sent on. A screen that treats pads as buttons cannot tell the two apart by
 * the bytes. get_param "mout_echo" lists the notes the host has taken in the last 0.5 s, on any channel ("72,76", or empty): the
 * screen may ignore a "press" that is one of them. Every note is also sent at velocity 101.
 * The reliable answer is the engine's: the module's screen hands each pad press in Pad Mode Control to set_param "pad_press"
 * ("<pad note 68..99>,<velocity>") and the engine carries it out (mutes, transpose, Notes Out switches, sound effects).
 * (On the device the screen's feed was found not to carry our notes back; the loop that pressed buttons was the next item.)
 * get_param "pad_ack" is
 * "<presses carried out>,<last pad>,<presses dropped as echoes>".
 * The same notes also come back into this slot as MIDI (the host routes them by channel to every slot listening there, and not always
 * on the channel they left on): tinysid_midi drops the first note-on for a note still owed back (once, within 0.3 s), and that note's note-off
 * (readout midi_echoes counts them), so they neither play the pad voice nor press a button. note_controls (0 / 1, default 1) is
 * whether Pad Mode Control turns incoming notes 36..42 into buttons; the plugin sets it to 0 when the module's own screen
 * (ui_chain.js) runs the pads, which go through pad_press instead. */

/* audio_delay (0..200 ms; 0 in the engine, 50 as the module starts, see plugin.c): while Notes Out sends at least one voice, the
 * engine's whole output waits this long. The notes reach the other tracks through the Move's MIDI input and their own instruments,
 * which takes longer than TinySID's own sound takes to come out; delaying TinySID by the difference lines them up (tuned by ear). With
 * nothing sent there is no delay. All three voices wait together (delaying only the ones sent would put the tune out of time with
 * itself), the pad voice and the sound effects too. Switching it in or out fades out, leaves a gap as long as the delay while the line
 * fills, and fades back in. */

/* Key detection from 12 pitch-class weights (Krumhansl-Schmuckler). Returns the tonic 0..11, or 12 when the
 * material is too sparse or atonal; *scale gets 0 major / 1 minor / 2 unknown. */
int tinysid_detect_key(const float pc[12], int *scale);

/* BPM from a histogram of the gaps (in frames, index = gap) between gate-ons on voices 1 and 3 over a whole
 * song: the most common gap is the row length, the shorter wins a tie. 0 when there is too little data. */
double tinysid_bpm_from_gaps(const uint16_t *hist, int n, double frame_hz);

/* Optional: the host's transport-state query (host_api_v1 get_clock_status; 1 stopped, 2 running). */
void tinysid_set_clock_status_fn(tinysid_t *t, int (*fn)(void));

#endif
