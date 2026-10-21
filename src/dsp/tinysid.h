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
 * load_path (set only), host_bpm (get only).
 * song_bpm reads the known/estimated tempo of the tune and ignores writes; song_bpm_override (set only)
 * pins the reference tempo until the tune changes.
 * Values are strings so they map straight onto the host's key/value API.
 * set returns 0 on success, -1 for an unknown key or unparsable value.
 * get returns the string length, or -1 for an unknown key. */
int tinysid_set_param(tinysid_t *t, const char *key, const char *value);
int tinysid_get_param(const tinysid_t *t, const char *key, char *buf, int buf_len);

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
int tinysid_snoop_note(const tinysid_t *t);    /* MIDI note the snooped voice sounds now, 0 = idle */

/* The key/scale/tempo/instrument scan normally advances a few frames per audio block. Hosts and tests can also run
 * it headlessly (6502 only, no audio): frames done so far, and a way to advance it. */
void tinysid_scan_advance(tinysid_t *t, int frames);
int tinysid_scan_frames(const tinysid_t *t);
int tinysid_pad_gesture_length(const tinysid_t *t);   /* frames of recorded movement in the chosen pad sound; 0 = none */

/* JSON array of the Pad Sound choices for the current pad voice: the tune's own instruments by name, or just
 * ["Lead"] while none are known. Returns its length, or -1 if it does not fit. */
int tinysid_pad_options(const tinysid_t *t, char *buf, int buf_len);

/* Key detection from 12 pitch-class weights (Krumhansl-Schmuckler). Returns the tonic 0..11, or 12 when the
 * material is too sparse or atonal; *scale gets 0 major / 1 minor / 2 unknown. */
int tinysid_detect_key(const float pc[12], int *scale);

/* BPM from a histogram of the gaps (in frames, index = gap) between gate-ons on voices 1 and 3 over a whole
 * song: the most common gap is the row length, the shorter wins a tie. 0 when there is too little data. */
double tinysid_bpm_from_gaps(const uint16_t *hist, int n, double frame_hz);

/* Optional: the host's transport-state query (host_api_v1 get_clock_status; 1 stopped, 2 running). */
void tinysid_set_clock_status_fn(tinysid_t *t, int (*fn)(void));

#endif
