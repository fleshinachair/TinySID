#include "tinysid.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu6502.h"
#include "sid.h"

void *(*tinysid_malloc)(size_t) = malloc;
void (*tinysid_free)(void *) = free;

#define PAL_CLOCK   985248.0
#define NTSC_CLOCK  1022727.0
#define PAL_FRAME_CYCLES   19656.0   /* 312 lines x 63 cycles */
#define NTSC_FRAME_CYCLES  17095.0   /* 263 lines x 65 cycles */

#define TRAP_ADDR        0xFFF8u     /* where init/play return to; never fetched */
#define INIT_MAX_CYCLES  3000000
#define PLAY_MAX_CYCLES  300000
#define MAX_PAD_NOTES    16

enum { PAD_OFF = 0, PAD_V1, PAD_V2, PAD_V3 };
enum { FILT_SONG = 0, FILT_LP, FILT_BP, FILT_HP, FILT_COUNT };
enum { KEY_NONE = 12, SCALE_MAJOR = 0, SCALE_MINOR = 1, SCALE_NONE = 2 };
#define PAD_FADE_SEC 0.020           /* a chosen pad voice ramps out this long before the keyboard takes it */
#define STOP_FADE_SEC 0.012          /* voices ramp down this long before their envelopes are cut */
#define ANA_FRAMES 15000             /* the pre-scan listens to this many frames (5 min at 50 Hz): instruments can first appear late ... */
#define ANA_SLICE 8                  /* ... a few per audio block, so it never stalls the audio thread */
#define SNOOP_RECENT 6               /* notes remembered for the piano roll's trail ... */
#define SNOOP_TRAIL_MS 600           /* ... for this long */
#define LOCK_MAX_PLAYS 16            /* play() calls a Host Lock catch-up may make per audio block */
#define LOCK_TOLERANCE 64            /* frames the lock will wait out or catch up; further off, it adopts the tune's phase */
#define LOCK_CLOCK_LOST_SEC 2.0      /* no MIDI clock for this long: the tune pauses until it returns */
#define LOCK_WAIT_SEC 1.0            /* after a start, wait this long for the first clock tick before free-running */
#define OUT_BASE_GAIN 0.355f         /* -9 dB: the raw SID mix peaks near full scale, ~10 dB hotter than Schwung's other generators */
#define KEY_WINDOW 1500             /* the key is judged per 30 s window and the windows vote: tunes modulate */
#define INST_MIN_FRAMES 4            /* a sound heard for less than this is a transition between two settings, not an instrument */
#define CLOCK_RUNNING 2              /* host_api_v1 get_clock_status() */
#define CLOCK_STOPPED 1
enum { SYNC_FREE_PAL = 0, SYNC_FREE_NTSC, SYNC_HOST_1X, SYNC_HOST_HALF, SYNC_HOST_2X, SYNC_COUNT };
enum { BPM_SRC_DEFAULT = 0, BPM_SRC_KNOWN, BPM_SRC_ESTIMATE, BPM_SRC_MANUAL };

#define BPM_MIN 60
#define BPM_MAX 200
#define BPM_DEFAULT 116
#define HOST_BPM_DEFAULT 120.0
#define FREE_PAL_HZ  50.0
#define FREE_NTSC_HZ 59.826
#define CLOCK_RING 49                /* 48 intervals = two beats of MIDI clock */
#define GATE_GAP_MIN 3               /* closer gate-ons are arpeggio / drum retriggers, not the groove */
#define GATE_GAP_MAX 96

/* One instrument a tune uses on a voice: what it writes before gating the voice on. */
#define GESTURE_FRAMES 64            /* how much of one real note's movement an instrument remembers (1.3 s at 50 Hz) */
typedef struct {
    uint8_t wave, ad, sr;
    uint16_t pw, count;
    /* The movement of the longest note heard with this instrument, one entry per frame: what the driver does to
     * the voice while a key is held (pulse-width sweeps, vibrato, trills, arpeggios, waveform changes). */
    uint8_t glen;
    uint16_t gpw[GESTURE_FRAMES];
    int16_t gratio[GESTURE_FRAMES];              /* pitch relative to the note's first frame, in 1/16384 */
    uint8_t gwave[GESTURE_FRAMES];
} inst_t;
#define INST_SIGS 32                 /* distinct instruments tracked per voice while scanning */
#define INST_MAX 12                  /* instruments offered per voice, most used first */

struct tinysid {
    cpu6502_t cpu;
    sid_t sid;
    uint8_t ram[65536];
    uint8_t raw[32];                 /* what the tune last wrote to each SID register */

    uint8_t *file;                   /* private copy of the loaded PSID */
    size_t file_len;
    psid_info_t info;
    int loaded;

    double sample_rate;
    /* parameters */
    int subtune;
    float vol[3];
    int sync_mode;                   /* SYNC_* */
    float tempo_trim;
    float trim_db;
    int model;
    int cutoff_bias, res_bias;
    int pad_target;
    float trim_gain, trim_cur, trim_coef;
    float dc_x1, dc_y1, dc_coef;     /* output DC blocker (the SID's unipolar waves carry real DC) */

    /* playback */
    double clock_hz, tick_acc;       /* tick_acc: samples until the next play() call */
    int uses_cia;
    int playing;
    uint64_t play_calls;
    uint8_t raster;
    /* host MIDI clock tracking */
    uint64_t sample_clock;           /* samples rendered so far; timestamps the clock ticks */
    uint64_t clock_ts[CLOCK_RING];
    int clock_n, clock_head;
    double host_bpm;
    int host_bpm_valid;

    /* song BPM: known-tune table, else estimated from gate cadence, else manual */
    float song_bpm_f;
    int song_bpm, song_bpm_src;
    int song_fpr_num, song_fpr_den;  /* frames per 16th-note row as a ratio; 0 = not known. Host Lock is built on it. */
    /* Host Lock: the tune's frames are a function of the MIDI clock ticks since it started, not of elapsed time */
    int64_t clk_ticks;               /* ticks since the tune was (re)started, counted from 0; -1 = none yet */
    double since_tick;               /* samples since the last tick */
    int64_t lock_off;                /* frames the lock adopted when it joined a tune already running */
    int lock_aligned, lock_lost;
    double lock_wait;                /* samples spent waiting for the first tick after a start */
    uint32_t frame_no;               /* play() calls since init */
    uint32_t last_gate_frame;
    uint16_t gap_hist[GATE_GAP_MAX + 1];   /* scanning engine only: how often voices 1 and 3 gated on this many frames apart */

    /* pad jamming: held notes, plus the timbre latched from the tune at note-on */
    uint8_t pad_notes[MAX_PAD_NOTES];
    int pad_count;
    int pad_gate;
    uint16_t pad_freq;
    float pad_vel;
    int filter_mode;                 /* FILT_*: Song keeps the tune's own filter, the rest force all voices through one */
    int transport_running;           /* what the host transport last said; the tune only runs while it is true */
    int stop_fade;                   /* samples until the stopped voices are cut; 0 = not stopping */
    int (*clock_status_fn)(void);    /* host_api_v1 get_clock_status, if the host provides it */
    int last_clock_status;
    uint64_t last_tick_sample;       /* sample_clock + 1 at the last MIDI clock tick; 0 = none yet */

    /* key / scale / tempo pre-scan: a second engine plays the tune silently, a few frames per block */
    int is_analysis;
    struct tinysid *ana;
    int ana_frames;
    float ana_pc[12];                /* pitch-class weights from the notes of the current window */
    int key_votes[12][2];            /* windows that picked each key / mode ... */
    double key_strength[12][2];      /* ... and the summed correlation of those windows */
    int ana_note[3];
    uint8_t ana_gate[3];
    inst_t sig[3][INST_SIGS];        /* scanning engine only: instruments seen so far */
    int sig_n[3];
    uint8_t sig_rise[3];             /* scanning engine only: voice gated on during this frame */
    int nt_active[3], nt_len[3];     /* scanning engine only: the note being recorded on each voice */
    uint8_t nt_wave[3], nt_ad[3], nt_sr[3];
    uint16_t nt_base[3];
    uint16_t nt_pw[3][GESTURE_FRAMES];
    int16_t nt_ratio[3][GESTURE_FRAMES];
    uint8_t nt_gw[3][GESTURE_FRAMES];
    inst_t inst[3][INST_MAX];        /* result of the scan: this tune's instruments per voice, most used first */
    int inst_n[3];
    inst_t merged[INST_MAX];         /* the song's instruments across all voices, for a voice that has none of its own */
    int merged_n;
    int contract_pulse;              /* samples left in which is_loading reads 1, to make the host re-read its option lists */
    int song_key, song_scale;        /* KEY_NONE / SCALE_NONE until a scan finishes */
    int snoop_voice;                 /* which voice snoop_note follows: 0..2, or 3 = all voices */
    int snoop_note;                  /* MIDI note that voice is sounding right now, 0 when idle */
    uint8_t snoop_gate[3];
    uint32_t snoop_seq, snoop_rise[3];   /* order in which the voices were last gated on, for "All Voices" */
    double snoop_hz;                 /* the snooped voice's actual pitch, vibrato and all */
    int snoop_cents;                 /* how far that is from the nearest note, -50..50 */
    int snoop_level;                 /* its envelope, 0..100 */
    int snoop_prev;
    struct { uint8_t note; uint64_t at; } snoop_recent[SNOOP_RECENT];   /* newest first: notes that sounded lately */
    int snoop_recent_n;
    int ana_done;

    int pad_latched;                 /* the pad owns the target voice's registers (true whenever a target is set) */
    int pad_sound;                   /* position in the Pad Sound list (resolved lazily: the scan that builds the list may still be running) */
    int pad_pending;                 /* samples of fade-out left before the pad voice is taken over; 0 = none */
    int active_note;
    uint8_t lat_wave, lat_ad, lat_sr;
    uint16_t lat_pw;
    int pad_glen, pad_gframe;        /* the chosen instrument's movement, and how many frames into it the held note is */
    double pad_gacc;                 /* samples since the last movement step */
    uint16_t pad_gpw[GESTURE_FRAMES];
    int16_t pad_gratio[GESTURE_FRAMES];
    uint8_t pad_gw[GESTURE_FRAMES];
    uint16_t pad_cur_freq, pad_cur_pw;   /* what the pad voice is playing right now */
    uint8_t pad_cur_wave;
};

static const char *const k_pad_names[] = { "Off", "Voice 1", "Voice 2", "Voice 3" };
static const char *const k_filter_names[FILT_COUNT] = { "Song", "Low Pass", "Band Pass", "High Pass" };
static const char *const k_scale_names[3] = { "Major", "Minor", "--" };
/* key and scale as one readout: index = tonic * 2 + (minor ? 1 : 0), 24 = unknown */
static const char *const k_keyscale_names[25] = {
    "C Maj", "C Min", "C# Maj", "C# Min", "D Maj", "D Min", "D# Maj", "D# Min", "E Maj", "E Min", "F Maj", "F Min",
    "F# Maj", "F# Min", "G Maj", "G Min", "G# Maj", "G# Min", "A Maj", "A Min", "A# Maj", "A# Min", "B Maj", "B Min", "--"
};
static const char *const k_snoop_names[4] = { "Voice 1 (Bass)", "Voice 2 (Lead)", "Voice 3 (Arp/Drm)", "All Voices" };
static const char *const k_sync_names[SYNC_COUNT] = {
    "Free (PAL 50Hz)", "Free (NTSC 60Hz)", "Host Lock 1x", "Host Lock 0.5x", "Host Lock 2x"
};

/* Tempos for tunes whose rows are known. Matched on the PSID title (case-insensitive); subtune 0 = any.
 * Commando: Hubbard's PAL driver steps a 16th-note row every 6 frames (50 / 6 * 15 = 125 BPM); subtune 2 runs
 * at the 94 BPM cadence the gate estimator measures. Armalyte's 128 was supplied by the project owner, unmeasured. */
static const struct { const char *title; int subtune; float bpm; int fpr_num, fpr_den; } k_known_tunes[] = {
    { "Commando", 1, 125.0f, 6, 1 },             /* 6 frames per row: 50 / 6 * 15 = 125.0 exactly */
    { "Commando", 2, 94.0f, 8, 1 },              /* 8 frames per row: 93.75, shown as 94 */
    { "Armalyte", 0, 128.0f, 0, 0 },             /* row length unknown: the legacy tempo ratio applies */
    { "TinySID Demo", 1, 125.0f, 6, 1 },
    { "TinySID Demo", 2, 187.5f, 4, 1 },
};
static const char *const k_model_names[] = { "6581", "8580" };

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ---- SID register intercepts ------------------------------------------- */

static int pad_voice(const tinysid_t *t) { return t->pad_target - 1; }   /* -1 when off */

static void apply_reg(tinysid_t *t, int reg)
{
    int val = t->raw[reg];

    if (reg < 21) {
        int v = reg / 7, r = reg % 7;
        if (v == pad_voice(t) && t->pad_latched) {        /* the tune's writes to this voice are suppressed */
            switch (r) {
            case 0: val = t->pad_cur_freq & 0xFF; break;
            case 1: val = t->pad_cur_freq >> 8; break;
            case 2: val = t->pad_cur_pw & 0xFF; break;
            case 3: val = (t->pad_cur_pw >> 8) & 0x0F; break;
            case 4: val = t->pad_cur_wave | (t->pad_gate ? 1 : 0); break;
            case 5: val = t->lat_ad; break;
            case 6: val = t->lat_sr; break;
            }
        }
    } else if (reg == 0x15) {
        if (t->filter_mode) val = 0;
    } else if (reg == 0x16) {
        /* Song: the tune's cutoff plus the knob. Override: the knob is the cutoff, centred at 128. */
        val = t->filter_mode ? clampi(128 + t->cutoff_bias, 0, 255) : clampi(val + t->cutoff_bias, 0, 255);
    } else if (reg == 0x17) {
        if (t->filter_mode) {
            val = (clampi(4 + t->res_bias, 0, 15) << 4) | 0x07;     /* resonance around 4, all three voices routed */
        } else {
            int res = clampi((val >> 4) + t->res_bias, 0, 15);
            val = (res << 4) | (val & 0x0F);
        }
    } else if (reg == 0x18 && t->filter_mode) {
        val = (val & 0x0F) | (0x10 << (t->filter_mode - 1));        /* LP / BP / HP, and voice 3 is never cut */
    }
    sid_write(&t->sid, (uint8_t)reg, (uint8_t)val);
}

static void apply_all(tinysid_t *t)
{
    for (int r = 0; r < 25; r++) apply_reg(t, r);
}

static void apply_voice(tinysid_t *t, int v)
{
    if (v < 0 || v > 2) return;
    for (int r = 0; r < 7; r++) apply_reg(t, v * 7 + r);
}

static void pad_frame_intercept(tinysid_t *t);
static void pad_takeover(tinysid_t *t);
static void ana_start(tinysid_t *t);
static void update_snoop(tinysid_t *t);
static int snoop_feed(const tinysid_t *t, char *buf, int buf_len);
static void ana_abort(tinysid_t *t);
static void restart_tune(tinysid_t *t);
static void engine_resume(tinysid_t *t);

/* ---- C64 memory map ----------------------------------------------------- */

/* ---- tempo: frame rate, host clock, song BPM -------------------------------- */

/* CIA-timed tunes set their own interrupt rate; 60 Hz if they never programmed the timer. */
static double cia_hz(const tinysid_t *t)
{
    uint16_t latch = (uint16_t)(t->ram[0xDC04] | (t->ram[0xDC05] << 8));
    return latch >= 100 ? t->clock_hz / (double)latch : 60.0;
}

/* The tune's own frame rate: what the file was written for. */
static double native_hz(const tinysid_t *t)
{
    if (t->uses_cia) return cia_hz(t);
    return t->info.is_ntsc ? FREE_NTSC_HZ : FREE_PAL_HZ;
}

/* Rate at which play() is called. Only the call rate changes; SID registers are never rescaled, so pitch holds. */
static double target_hz(const tinysid_t *t)
{
    double hz;
    switch (t->sync_mode) {
    case SYNC_FREE_PAL:  hz = t->uses_cia ? cia_hz(t) : FREE_PAL_HZ; break;
    case SYNC_FREE_NTSC: hz = t->uses_cia ? cia_hz(t) : FREE_NTSC_HZ; break;
    default: {
        /* One song beat per host beat: scale the native frame rate by host tempo over song tempo. */
        double mult = t->sync_mode == SYNC_HOST_HALF ? 0.5 : (t->sync_mode == SYNC_HOST_2X ? 2.0 : 1.0);
        hz = native_hz(t) * (t->host_bpm / (double)t->song_bpm_f) * mult;
        break;
    }
    }
    hz *= t->tempo_trim;
    return hz < 1.0 ? 1.0 : (hz > 1000.0 ? 1000.0 : hz);
}

/* 24 MIDI clocks per quarter note. Timestamps are block-quantised, so the tempo is measured across a
 * two-beat window (the quantisation error divides by 48 intervals) and then smoothed. */
static void host_clock_tick(tinysid_t *t)
{
    uint64_t now = t->sample_clock;
    t->last_tick_sample = now + 1;
    if (t->lock_lost) {                                  /* the clock is back: carry on from where the tune paused */
        t->lock_lost = 0;
        if (t->transport_running) engine_resume(t);
    }
    if (t->playing) {                                    /* ticks are counted from the start; a paused tune keeps its place */
        t->clk_ticks++;
        t->since_tick = 0.0;
    }
    if (t->clock_n > 0) {
        uint64_t last = t->clock_ts[(t->clock_head + CLOCK_RING - 1) % CLOCK_RING];
        if (now - last > (uint64_t)(2.0 * t->sample_rate)) t->clock_n = 0;     /* clock stopped: start over */
    }
    t->clock_ts[t->clock_head] = now;
    t->clock_head = (t->clock_head + 1) % CLOCK_RING;
    if (t->clock_n < CLOCK_RING) t->clock_n++;

    if (t->clock_n >= 8) {
        uint64_t oldest = t->clock_ts[(t->clock_head + CLOCK_RING - t->clock_n) % CLOCK_RING];
        uint64_t span = now - oldest;
        if (span > 0) {
            double bpm = 60.0 * t->sample_rate * (t->clock_n - 1) / (24.0 * (double)span);
            if (bpm >= 20.0 && bpm <= 300.0) {
                t->host_bpm = t->host_bpm_valid ? t->host_bpm + (bpm - t->host_bpm) * 0.25 : bpm;
                t->host_bpm_valid = 1;
            }
        }
    }
}

static void set_song_bpm(tinysid_t *t, double bpm, int src)
{
    if (bpm < BPM_MIN) bpm = BPM_MIN;
    if (bpm > BPM_MAX) bpm = BPM_MAX;
    t->song_bpm_f = (float)bpm;
    t->song_bpm = (int)lround(bpm);
    t->song_bpm_src = src;
}

static int title_matches(const char *a, const char *b)
{
    while (*a == ' ') a++;
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
    while (*a == ' ') a++;
    return !*a && !*b;
}

/* Resets the BPM readout for a newly loaded tune / subtune and applies the known-tune table. */
static void song_bpm_reset(tinysid_t *t)
{
    set_song_bpm(t, BPM_DEFAULT, BPM_SRC_DEFAULT);
    t->song_fpr_num = t->song_fpr_den = 0;
    if (!t->loaded) return;
    for (size_t i = 0; i < sizeof(k_known_tunes) / sizeof(k_known_tunes[0]); i++) {
        if (title_matches(t->info.name, k_known_tunes[i].title) &&
            (k_known_tunes[i].subtune == 0 || k_known_tunes[i].subtune == t->subtune)) {
            set_song_bpm(t, k_known_tunes[i].bpm, BPM_SRC_KNOWN);
            t->song_fpr_num = k_known_tunes[i].fpr_num;
            t->song_fpr_den = k_known_tunes[i].fpr_num ? k_known_tunes[i].fpr_den : 0;
            return;
        }
    }
}

/* The row length as a ratio of frames, and the tempo it implies (not yet snapped to a whole BPM). Folding a
 * tempo into range changes the row length by powers of two: a tempo that was too fast means rows twice as long. */
static double gaps_to_fpr(const uint16_t *hist, int n, double frame_hz, int *num, int *den)
{
    long total = 0;
    int best = 0;
    for (int g = GATE_GAP_MIN; g < n; g++) {
        total += hist[g];
        if (hist[g] > hist[best]) best = g;
    }
    if (total < 8 || hist[best] < 4) return 0.0;
    double bpm = frame_hz * 15.0 / best;
    int nu = best, de = 1;
    while (bpm > BPM_MAX) { bpm *= 0.5; nu *= 2; }
    while (bpm < BPM_MIN) { bpm *= 2.0; de *= 2; }
    if (num) *num = nu;
    if (den) *den = de;
    return bpm;
}

/* The row length is the most common gap between gate-ons over the whole scan; on a tie the shorter gap wins.
 * One gap = one 16th-note row: bpm = frame rate * 15 / frames, folded into 60..200. A tune's rhythm shifts
 * between sections, which is why a running median wanders; the histogram of the whole song does not.
 * Returns 0 when there is too little to go on. */
double tinysid_bpm_from_gaps(const uint16_t *hist, int n, double frame_hz)
{
    return gaps_to_fpr(hist, n, frame_hz, NULL, NULL);
}

/* The tune gated voice 1 or 3 on. Only the silent scan measures: the readout never moves while you play. */
static void gate_event(tinysid_t *t)
{
    if (!t->is_analysis || t->frame_no == 0) return;              /* init-time writes carry no timing */
    if (t->last_gate_frame) {
        uint32_t gap = t->frame_no - t->last_gate_frame;
        if (gap >= GATE_GAP_MIN && gap <= GATE_GAP_MAX) t->gap_hist[gap]++;
    }
    t->last_gate_frame = t->frame_no;
}

/* Called once per frame for a voice that is gated on, so an instrument's count is how many frames it was heard
 * (a long held note outranks a flurry of short ones). The registers are read at the END of the frame: drivers
 * that "hard restart" write AD/SR = 0 before the gate and the real envelope after it. */
static void inst_record(tinysid_t *t, int voice)
{
    const uint8_t *r = &t->raw[voice * 7];
    uint8_t wave = r[4] & 0xF0;
    if (!wave) return;
    uint16_t pw = (uint16_t)(r[2] | ((r[3] & 0x0F) << 8));
    for (int i = 0; i < t->sig_n[voice]; i++) {
        inst_t *x = &t->sig[voice][i];
        if (x->wave == wave && x->ad == r[5] && (x->sr & 0x0F) == (r[6] & 0x0F)) {
            /* One instrument, however loud it is or whatever its pulse width is doing: drivers fake a volume
             * with the sustain level, and a PWM sweep must not fill the list either. Keep the loudest sustain
             * and the width nearest 50%, which are the fullest sound to play. */
            if ((r[6] >> 4) > (x->sr >> 4)) x->sr = r[6];
            int cur = x->pw > 0x800 ? x->pw - 0x800 : 0x800 - x->pw, now = pw > 0x800 ? pw - 0x800 : 0x800 - pw;
            if (now < cur) x->pw = pw;
            if (x->count < 65535) x->count++;
            return;
        }
    }
    inst_t *x;
    if (t->sig_n[voice] < INST_SIGS) {
        x = &t->sig[voice][t->sig_n[voice]++];
    } else {                                             /* full: drop the weakest, which is junk from transitions */
        x = &t->sig[voice][0];
        for (int i = 1; i < INST_SIGS; i++)
            if (t->sig[voice][i].count < x->count) x = &t->sig[voice][i];
    }
    memset(x, 0, sizeof(*x));
    x->wave = wave; x->ad = r[5]; x->sr = r[6]; x->pw = pw; x->count = 1;
}

/* A note ends (gate off, or the next one begins): its movement goes to its instrument if it is the longest so far. */
static void note_finish(tinysid_t *t, int v)
{
    if (!t->nt_active[v]) return;
    t->nt_active[v] = 0;
    int n = t->nt_len[v];
    if (n < 4) return;
    for (int i = 0; i < t->sig_n[v]; i++) {
        inst_t *x = &t->sig[v][i];
        if (x->wave == t->nt_wave[v] && x->ad == t->nt_ad[v] && (x->sr & 0x0F) == (t->nt_sr[v] & 0x0F)) {
            if (n > x->glen) {
                x->glen = (uint8_t)n;
                memcpy(x->gpw, t->nt_pw[v], sizeof(uint16_t) * (size_t)n);
                memcpy(x->gratio, t->nt_ratio[v], sizeof(int16_t) * (size_t)n);
                memcpy(x->gwave, t->nt_gw[v], (size_t)n);
            }
            return;
        }
    }
}

static void note_start(tinysid_t *t, int v)
{
    const uint8_t *r = &t->raw[v * 7];
    t->nt_active[v] = 1;
    t->nt_len[v] = 0;
    t->nt_wave[v] = r[4] & 0xF0;
    t->nt_ad[v] = r[5];
    t->nt_sr[v] = r[6];
    t->nt_base[v] = (uint16_t)(r[0] | (r[1] << 8));       /* pitch at the first frame is the note itself */
}

/* One frame of the note: pulse width, pitch relative to the note, waveform. */
static void note_sample(tinysid_t *t, int v)
{
    if (!t->nt_active[v]) return;
    const uint8_t *r = &t->raw[v * 7];
    int n = t->nt_len[v];
    double freq = r[0] | (r[1] << 8);
    double ratio = t->nt_base[v] ? (freq / t->nt_base[v] - 1.0) * 16384.0 : 0.0;
    t->nt_pw[v][n] = (uint16_t)(r[2] | ((r[3] & 0x0F) << 8));
    t->nt_ratio[v][n] = (int16_t)(ratio > 32767.0 ? 32767 : (ratio < -32767.0 ? -32767 : lround(ratio)));
    t->nt_gw[v][n] = r[4] & 0xF0;
    if (++t->nt_len[v] == GESTURE_FRAMES) note_finish(t, v);           /* long enough: stop recording this note */
}

static uint8_t bus_read(void *ctx, uint16_t a)
{
    tinysid_t *t = ctx;
    if (a >= 0xD400 && a < 0xD800) return sid_read(&t->sid, (uint8_t)(a & 0x1F));
    if (a == 0xD012) return ++t->raster;                    /* free-running so raster waits terminate */
    if (a == 0xD011) return (uint8_t)(t->ram[0xD011] ^ ((t->raster & 0x80) ? 0x80 : 0));
    return t->ram[a];
}

static void bus_write(void *ctx, uint16_t a, uint8_t v)
{
    tinysid_t *t = ctx;
    if (a >= 0xD400 && a < 0xD800) {
        int reg = a & 0x1F;
        if (reg < 25) {
            if ((reg == 4 || reg == 18) && (v & 1) && !(t->raw[reg] & 1)) gate_event(t);   /* voices 1 and 3 */
            if (t->is_analysis && reg < 21 && reg % 7 == 4 && (v & 1) && !(t->raw[reg] & 1)) t->sig_rise[reg / 7] = 1;
            t->raw[reg] = v;
            apply_reg(t, reg);
        }
        return;
    }
    t->ram[a] = v;
}

/* ---- running the tune --------------------------------------------------- */

static int is_return_addr(uint16_t pc)
{
    /* KERNAL IRQ exits that an IRQ-driven tune may jump to instead of RTI */
    return pc == TRAP_ADDR || pc == 0xEA31 || pc == 0xEA7E || pc == 0xEA81 || pc == 0xFEBC;
}

static void run_routine(tinysid_t *t, uint16_t addr, int irq_style, int max_cycles)
{
    cpu6502_t *c = &t->cpu;
    uint8_t saved_sp = c->sp;
    uint64_t start = c->cycles;

    if (irq_style) {
        cpu_push(c, TRAP_ADDR >> 8);
        cpu_push(c, TRAP_ADDR & 0xFF);
        cpu_push(c, CPU_U);
        c->p |= CPU_I;
    } else {
        cpu_push(c, (uint8_t)((TRAP_ADDR - 1) >> 8));
        cpu_push(c, (uint8_t)((TRAP_ADDR - 1) & 0xFF));
    }
    c->pc = addr;
    c->jammed = 0;
    while (!is_return_addr(c->pc) && !c->jammed && c->cycles - start < (uint64_t)max_cycles)
        cpu_step(c);
    c->sp = saved_sp;
}

static uint16_t play_vector(const tinysid_t *t)
{
    const uint8_t *r = t->ram;
    int bank = r[1] & 7;
    return bank == 5 ? (uint16_t)(r[0xFFFE] | (r[0xFFFF] << 8))
                     : (uint16_t)(r[0x0314] | (r[0x0315] << 8));
}

static void do_play(tinysid_t *t)
{
    uint16_t addr = t->info.play_addr;
    int irq = 0;
    if (!addr) {
        addr = play_vector(t);
        irq = 1;
        if (addr == 0xEA31 || addr == 0) return;
    }
    t->frame_no++;
    run_routine(t, addr, irq, PLAY_MAX_CYCLES);
    t->play_calls++;
    pad_frame_intercept(t);
}

static void init_tune(tinysid_t *t)
{
    if (!t->loaded) return;

    memset(t->ram, 0, sizeof(t->ram));
    memcpy(t->ram + t->info.load_addr, t->info.data, t->info.data_len);
    t->ram[0] = 0x2F;
    t->ram[1] = 0x37;
    t->ram[0x0314] = 0x31;                  /* default KERNAL IRQ vector */
    t->ram[0x0315] = 0xEA;
    t->raster = 0;

    t->clock_hz = t->info.is_ntsc ? NTSC_CLOCK : PAL_CLOCK;
    t->sid.clock_hz = t->clock_hz;
    t->uses_cia = psid_subtune_uses_cia(&t->info, t->subtune);
    t->tick_acc = 0.0;
    t->frame_no = 0;
    t->last_gate_frame = 0;
    t->clk_ticks = -1;                                   /* the first clock tick after a start is tick 0: step 1 */
    t->since_tick = 0.0;
    t->lock_off = 0;
    t->lock_aligned = t->lock_lost = 0;
    t->lock_wait = 0.0;

    t->pad_count = 0;
    t->pad_latched = t->pad_gate = 0;
    t->pad_pending = 0;
    for (int v = 0; v < 3; v++) sid_set_voice_gain(&t->sid, v, t->vol[v]);
    sid_reset(&t->sid);
    memset(t->raw, 0, sizeof(t->raw));
    t->raw[0x18] = 0;
    if (t->pad_target != PAD_OFF) pad_takeover(t);       /* a new tune or a restart keeps the pad voice yours */
    apply_all(t);

    cpu_reset(&t->cpu);
    t->cpu.sp = 0xFF;
    t->cpu.a = (uint8_t)(t->subtune - 1);
    t->cpu.x = t->cpu.y = 0;
    run_routine(t, t->info.init_addr, 0, INIT_MAX_CYCLES);
    t->playing = 1;
    update_snoop(t);                                     /* the readouts are right from the moment the tune is loaded */
}

/* ---- run control: stop with a fast fade, restart from the top ------------- */

/* Halts the 6502 and ramps the song's voices out over ~12 ms, then cuts their envelopes so nothing keeps
 * ringing (a held note with a 24 s release would otherwise sound on for seconds). The pad voice is yours
 * and is left alone. */
static void engine_stop(tinysid_t *t)
{
    if (!t->playing) return;
    t->playing = 0;
    t->stop_fade = (int)(STOP_FADE_SEC * t->sample_rate) + 1;
    for (int v = 0; v < 3; v++)
        if (v != pad_voice(t)) sid_set_voice_gain(&t->sid, v, 0.0f);
}

static void stop_cut(tinysid_t *t)
{
    t->stop_fade = 0;
    for (int v = 0; v < 3; v++) {
        if (v == pad_voice(t)) continue;
        sid_write(&t->sid, (uint8_t)(v * 7 + 4), 0);
        sid_kill_voice(&t->sid, v);
        sid_set_voice_gain(&t->sid, v, t->vol[v]);       /* the next start ramps back up from silence */
    }
}

/* Continues from where the tune stopped. */
static void engine_resume(tinysid_t *t)
{
    if (!t->loaded || t->playing) return;
    t->stop_fade = 0;
    t->playing = 1;
    for (int v = 0; v < 3; v++)
        if (v != pad_voice(t)) sid_set_voice_gain(&t->sid, v, t->vol[v]);
}

/* Starts the tune from beat 1. */
static void engine_start(tinysid_t *t)
{
    if (!t->loaded) return;
    t->stop_fade = 0;
    init_tune(t);
}

/* (Re)initialises after a load or subtune change, staying silent if the run mode says the tune is not running. */
static void restart_tune(tinysid_t *t)
{
    t->stop_fade = 0;
    init_tune(t);
    if (!t->transport_running) {
        engine_stop(t);
        stop_cut(t);
    }
}

/* ---- construction / loading --------------------------------------------- */

tinysid_t *tinysid_create(double sample_rate)
{
    tinysid_t *t = tinysid_malloc(sizeof(*t));
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));

    t->sample_rate = sample_rate > 8000.0 ? sample_rate : 44100.0;
    t->subtune = 1;
    t->vol[0] = t->vol[1] = t->vol[2] = 1.0f;
    t->model = SID_MODEL_6581;
    t->trim_db = 0.0f;
    t->trim_gain = t->trim_cur = OUT_BASE_GAIN;
    t->trim_coef = 1.0f - expf(-1.0f / (0.005f * (float)t->sample_rate));
    t->dc_coef = expf(-6.2831853f * 8.0f / (float)t->sample_rate);       /* ~8 Hz high-pass */
    t->clock_hz = PAL_CLOCK;
    t->tempo_trim = 1.0f;
    t->song_key = KEY_NONE;
    t->song_scale = SCALE_NONE;
    t->snoop_voice = 1;                                  /* Voice 2 (Lead) */
    t->transport_running = 1;
    t->last_clock_status = -1;
    t->host_bpm = HOST_BPM_DEFAULT;
    set_song_bpm(t, BPM_DEFAULT, BPM_SRC_DEFAULT);

    sid_init(&t->sid, PAL_CLOCK, t->sample_rate, SID_MODEL_6581);
    t->cpu.read = bus_read;
    t->cpu.write = bus_write;
    t->cpu.ctx = t;
    cpu_reset(&t->cpu);
    return t;
}

void tinysid_destroy(tinysid_t *t)
{
    if (!t) return;
    ana_abort(t);
    if (t->file) tinysid_free(t->file);
    tinysid_free(t);
}

int tinysid_load(tinysid_t *t, const uint8_t *data, size_t len)
{
    psid_info_t probe;
    int err = psid_parse(data, len, &probe);
    if (err != PSID_OK) return err;

    uint8_t *copy = tinysid_malloc(len);
    if (!copy) return PSID_ERR_TOO_BIG;
    memcpy(copy, data, len);

    /* re-parse against our private copy so info.data points at it */
    psid_parse(copy, len, &t->info);
    if (t->file) tinysid_free(t->file);
    t->file = copy;
    t->file_len = len;
    t->loaded = 1;
    t->subtune = clampi(t->info.start_song, 1, t->info.songs);
    if (t->sync_mode == SYNC_FREE_PAL || t->sync_mode == SYNC_FREE_NTSC)       /* follow the file's video standard; a Host Lock stays */
        t->sync_mode = t->info.is_ntsc ? SYNC_FREE_NTSC : SYNC_FREE_PAL;
    song_bpm_reset(t);
    restart_tune(t);
    ana_start(t);
    return PSID_OK;
}

int tinysid_load_file(tinysid_t *t, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return PSID_ERR_TOO_SHORT;
    uint8_t *buf = tinysid_malloc(0x10200);
    if (!buf) { fclose(f); return PSID_ERR_TOO_BIG; }
    size_t n = fread(buf, 1, 0x10200, f);
    fclose(f);
    int err = tinysid_load(t, buf, n);
    tinysid_free(buf);
    return err;
}

/* ---- parameters ---------------------------------------------------------- */

static int parse_double(const char *s, double *out)
{
    char *end;
    if (!s) return -1;
    double v = strtod(s, &end);
    if (end == s || isnan(v)) return -1;
    while (*end == ' ') end++;
    if (*end) return -1;                                 /* trailing text: not a number */
    *out = v;
    return 0;
}

static int parse_enum(const char *s, const char *const *names, int n)
{
    if (!s) return -1;
    for (int i = 0; i < n; i++) {
        const char *a = s, *b = names[i];
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
        if (!*a && !*b) return i;
    }
    double v;
    if (parse_double(s, &v) == 0 && v >= 0 && v < n) return (int)v;
    return -1;
}

static void pad_unlatch(tinysid_t *t, int voice);
static void pad_takeover(tinysid_t *t);
static void pad_choose_patch(tinysid_t *t);
static void pad_sound_name(const tinysid_t *t, int index, char *buf, size_t n);
static int pad_sound_parse(const tinysid_t *t, const char *value);

static void set_pad_target(tinysid_t *t, int target)
{
    if (target == t->pad_target) return;
    int old = pad_voice(t);
    t->pad_count = 0;                                    /* never leave a note stuck on the old target */
    t->pad_pending = 0;
    if (old >= 0) pad_unlatch(t, old);
    t->pad_target = target;
    t->contract_pulse = (int)(0.15 * t->sample_rate);
    if (target != PAD_OFF) {
        /* ramp the song's voice out first: cutting it dead would click and ring through the output's DC blocker */
        sid_set_voice_gain(&t->sid, pad_voice(t), 0.0f);
        t->pad_pending = (int)(PAD_FADE_SEC * t->sample_rate) + 1;
    }
}

int tinysid_set_param(tinysid_t *t, const char *key, const char *value)
{
    double d;
    int e;

    if (!strcmp(key, "subtune")) {
        if (parse_double(value, &d)) return -1;
        t->subtune = clampi((int)lround(clampf((float)d, -1e6f, 1e6f)), 1, t->loaded ? t->info.songs : 1);
        song_bpm_reset(t);
        restart_tune(t);
        ana_start(t);
    } else if (key[0] == 'v' && key[1] >= '1' && key[1] <= '3' && !strcmp(key + 2, "_vol")) {
        if (parse_double(value, &d)) return -1;
        int v = key[1] - '1';
        t->vol[v] = clampf((float)d, 0.0f, 1.0f);
        sid_set_voice_gain(&t->sid, v, t->vol[v]);
    } else if (!strcmp(key, "sync_mode")) {
        if ((e = parse_enum(value, k_sync_names, SYNC_COUNT)) < 0) return -1;
        t->sync_mode = e;
    } else if (!strcmp(key, "tempo_trim")) {
        if (parse_double(value, &d)) return -1;
        t->tempo_trim = clampf((float)d, 0.75f, 1.5f);
    } else if (!strcmp(key, "song_bpm")) {                    /* a readout: hosts write every param's default at load */
        if (parse_double(value, &d)) return -1;               /* validated, then deliberately ignored */
    } else if (!strcmp(key, "song_bpm_override")) {           /* pins the reference tempo until the tune changes */
        if (parse_double(value, &d)) return -1;
        set_song_bpm(t, clampf((float)d, BPM_MIN, BPM_MAX), BPM_SRC_MANUAL);
        t->song_fpr_num = t->song_fpr_den = 0;                /* a typed tempo has no known row length: the tempo ratio rules */
    } else if (!strcmp(key, "out_trim")) {
        if (parse_double(value, &d)) return -1;
        t->trim_db = clampf((float)d, -18.0f, 6.0f);
        t->trim_gain = OUT_BASE_GAIN * powf(10.0f, t->trim_db / 20.0f);
    } else if (!strcmp(key, "chip_model")) {
        if ((e = parse_enum(value, k_model_names, 2)) < 0) return -1;
        t->model = e;
        sid_set_model(&t->sid, (sid_model_t)e);
    } else if (!strcmp(key, "cutoff_bias")) {
        if (parse_double(value, &d)) return -1;
        t->cutoff_bias = clampi((int)lround(clampf((float)d, -255.0f, 255.0f)), -128, 127);
        apply_reg(t, 0x16);
    } else if (!strcmp(key, "res_bias")) {
        if (parse_double(value, &d)) return -1;
        t->res_bias = clampi((int)lround(clampf((float)d, -255.0f, 255.0f)), -15, 15);
        apply_reg(t, 0x17);
    } else if (!strcmp(key, "pad_target")) {
        if ((e = parse_enum(value, k_pad_names, 4)) < 0) return -1;
        set_pad_target(t, e);
    } else if (!strcmp(key, "filter_mode")) {
        if ((e = parse_enum(value, k_filter_names, FILT_COUNT)) < 0) return -1;
        t->filter_mode = e;
        for (int r = 0x15; r <= 0x18; r++) apply_reg(t, r);
    } else if (!strcmp(key, "snoop_voice")) {
        if ((e = parse_enum(value, k_snoop_names, 4)) < 0) return -1;
        t->snoop_voice = e;
        update_snoop(t);
    } else if (!strcmp(key, "snoop_note") || !strcmp(key, "snoop_hz") || !strcmp(key, "snoop_cents") || !strcmp(key, "snoop_level") ||
               !strcmp(key, "snoop_feed") || !strcmp(key, "song_key") || !strcmp(key, "song_scale")) {
        /* readouts: hosts write every param's default at load, so the write is accepted and dropped */
    } else if (!strcmp(key, "pad_sound")) {
        if ((e = pad_sound_parse(t, value)) < 0) return -1;
        t->pad_sound = e;
        if (t->pad_latched && t->pad_count == 0) pad_choose_patch(t);   /* held notes keep their sound */
    } else if (!strcmp(key, "load_path")) {
        return tinysid_load_file(t, value) == PSID_OK ? 0 : -1;
    } else {
        return -1;
    }
    return 0;
}

int tinysid_get_param(const tinysid_t *t, const char *key, char *buf, int buf_len)
{
    if (!buf || buf_len <= 0) return -1;
    int n;

    if (!strcmp(key, "subtune")) n = snprintf(buf, (size_t)buf_len, "%d", t->subtune);
    else if (key[0] == 'v' && key[1] >= '1' && key[1] <= '3' && !strcmp(key + 2, "_vol"))
        n = snprintf(buf, (size_t)buf_len, "%g", (double)t->vol[key[1] - '1']);
    else if (!strcmp(key, "sync_mode")) n = snprintf(buf, (size_t)buf_len, "%s", k_sync_names[t->sync_mode]);
    else if (!strcmp(key, "tempo_trim")) n = snprintf(buf, (size_t)buf_len, "%g", (double)t->tempo_trim);
    else if (!strcmp(key, "song_bpm")) n = snprintf(buf, (size_t)buf_len, "%d", t->song_bpm);
    else if (!strcmp(key, "host_bpm")) n = snprintf(buf, (size_t)buf_len, "%.1f", t->host_bpm);
    else if (!strcmp(key, "out_trim")) n = snprintf(buf, (size_t)buf_len, "%g", (double)t->trim_db);
    else if (!strcmp(key, "chip_model")) n = snprintf(buf, (size_t)buf_len, "%s", k_model_names[t->model]);
    else if (!strcmp(key, "cutoff_bias")) n = snprintf(buf, (size_t)buf_len, "%d", t->cutoff_bias);
    else if (!strcmp(key, "res_bias")) n = snprintf(buf, (size_t)buf_len, "%d", t->res_bias);
    else if (!strcmp(key, "pad_target")) n = snprintf(buf, (size_t)buf_len, "%s", k_pad_names[t->pad_target]);
    else if (!strcmp(key, "filter_mode")) n = snprintf(buf, (size_t)buf_len, "%s", k_filter_names[t->filter_mode]);
    else if (!strcmp(key, "song_key")) n = snprintf(buf, (size_t)buf_len, "%s", t->song_key == KEY_NONE ? k_keyscale_names[24] : k_keyscale_names[t->song_key * 2 + (t->song_scale == SCALE_MINOR ? 1 : 0)]);
    else if (!strcmp(key, "snoop_voice")) n = snprintf(buf, (size_t)buf_len, "%s", k_snoop_names[t->snoop_voice]);
    else if (!strcmp(key, "snoop_note")) n = snprintf(buf, (size_t)buf_len, "%d", t->snoop_note);
    else if (!strcmp(key, "snoop_hz")) n = snprintf(buf, (size_t)buf_len, "%.1f", t->snoop_hz);
    else if (!strcmp(key, "snoop_cents")) n = snprintf(buf, (size_t)buf_len, "%d", t->snoop_cents);
    else if (!strcmp(key, "snoop_level")) n = snprintf(buf, (size_t)buf_len, "%d", t->snoop_level);
    else if (!strcmp(key, "snoop_feed")) n = snoop_feed(t, buf, buf_len);
    else if (!strcmp(key, "song_scale")) n = snprintf(buf, (size_t)buf_len, "%s", k_scale_names[t->song_scale]);
    else if (!strcmp(key, "pad_sound")) pad_sound_name(t, t->pad_sound, buf, (size_t)buf_len), n = (int)strlen(buf);
    else if (!strcmp(key, "pad_sound_index")) n = snprintf(buf, (size_t)buf_len, "%d", t->pad_sound);
    else if (!strcmp(key, "is_loading")) n = snprintf(buf, (size_t)buf_len, "%d", (t->ana || t->contract_pulse > 0) ? 1 : 0);
    else if (!strcmp(key, "subtune_count")) n = snprintf(buf, (size_t)buf_len, "%d", t->loaded ? t->info.songs : 0);
    else if (!strcmp(key, "title")) n = snprintf(buf, (size_t)buf_len, "%s", t->loaded ? t->info.name : "");
    else if (!strcmp(key, "author")) n = snprintf(buf, (size_t)buf_len, "%s", t->loaded ? t->info.author : "");
    else return -1;

    return n >= buf_len ? buf_len - 1 : n;
}

/* ---- MIDI: pad jamming and host clock ------------------------------------ */

static uint16_t note_to_sid_freq(const tinysid_t *t, int note)
{
    double hz = 440.0 * pow(2.0, (note - 69) / 12.0);
    double word = hz * 16777216.0 / t->clock_hz;
    return (uint16_t)(word < 0.0 ? 0.0 : (word > 65535.0 ? 65535.0 : word + 0.5));
}

/* Voice whose instrument list the Pad Sound knob shows: the pad voice, or voice 1 while the pad is off. */
static int inst_voice(const tinysid_t *t) { return pad_voice(t) >= 0 ? pad_voice(t) : 0; }

/* "1 Pul32 06 98": rank, waveforms (pulse with its width in %), then AD and SR in hex as a C64 programmer writes them. */
static void inst_label(const inst_t *in, int rank, char *buf, size_t n)
{
    char w[32] = "";
    static const struct { uint8_t bit; const char *name; } k_waves[] = { { 0x10, "Tri" }, { 0x20, "Saw" }, { 0x40, "Pul" }, { 0x80, "Noi" } };
    for (int i = 0; i < 4; i++)
        if (in->wave & k_waves[i].bit) {
            if (w[0]) strcat(w, "+");
            strcat(w, k_waves[i].name);
            if (k_waves[i].bit == 0x40) sprintf(w + strlen(w), "%d", (int)in->pw * 100 / 4096);   /* pulse width, % */
        }
    snprintf(buf, n, "%d %s %02X %02X", rank, w, in->ad, in->sr);
}

/* The Pad Sound choices for the voice being played: that voice's instruments from the scan, longest heard first,
 * or, for a voice the song never sounds, the song's other instruments. Returns the count, with `*list` pointing at
 * them; 0 means nothing is known (the scan is still running, or the tune is silent). */
static int pad_list(const tinysid_t *t, const inst_t **list)
{
    int v = inst_voice(t);
    if (t->inst_n[v] > 0) { *list = t->inst[v]; return t->inst_n[v]; }
    *list = t->merged;
    return t->merged_n;
}

static void pad_sound_name(const tinysid_t *t, int index, char *buf, size_t n)
{
    const inst_t *list;
    int cnt = pad_list(t, &list);
    if (cnt == 0) {
        snprintf(buf, n, "Lead");                        /* nothing known: a plain saw lead, so there is always a sound */
        return;
    }
    if (index < 0 || index >= cnt) index = 0;            /* a stored position past the list plays the first entry */
    inst_label(&list[index], index + 1, buf, n);
}

static int pad_sound_count(const tinysid_t *t)
{
    const inst_t *list;
    int cnt = pad_list(t, &list);
    return cnt ? cnt : 1;
}

static int ci_equal(const char *a, const char *b)
{
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
    return !*a && !*b;
}

/* A name from the current list, or a number. Numbers past the list are kept: a saved set can name an
 * instrument before the scan that finds it has finished. */
static int pad_sound_parse(const tinysid_t *t, const char *value)
{
    char name[40];
    if (!value) return -1;
    for (int i = 0; i < pad_sound_count(t); i++) {
        pad_sound_name(t, i, name, sizeof(name));
        if (ci_equal(value, name)) return i;
    }
    double d;
    if (parse_double(value, &d) == 0 && d >= 0 && d < INST_MAX) return (int)d;
    return -1;
}

/* Works out what the pad voice is playing this frame: the held note's pitch with the instrument's recorded movement
 * applied (pitch as a ratio, so it follows the key you pressed), or the plain patch when the instrument has none. */
static void pad_mod_apply(tinysid_t *t)
{
    t->pad_cur_wave = t->lat_wave;
    t->pad_cur_pw = t->lat_pw;
    t->pad_cur_freq = t->pad_freq;
    int n = t->pad_glen;
    if (n <= 0) return;
    int i = t->pad_gframe;
    if (i >= n) {                                        /* past the recording: keep looping its second half (vibrato) */
        int half = n / 2, span = n - half;
        i = half + (i - n) % span;
    }
    if (t->pad_gw[i]) t->pad_cur_wave = t->pad_gw[i];
    t->pad_cur_pw = t->pad_gpw[i];
    double f = t->pad_freq * (1.0 + t->pad_gratio[i] / 16384.0);
    t->pad_cur_freq = (uint16_t)(f < 0.0 ? 0.0 : (f > 65535.0 ? 65535.0 : f + 0.5));
}

/* Sets up what the pad voice plays: the chosen instrument exactly as the song sets it up (envelope, waveform, pulse
 * width and the movement of its longest note), or, when nothing is known yet, a plain saw lead (instant attack,
 * full sustain, 300 ms release). */
static void pad_choose_patch(tinysid_t *t)
{
    const inst_t *list;
    int cnt = pad_list(t, &list);
    t->lat_pw = 0x0800;
    t->lat_ad = 0x00;
    t->lat_sr = 0xF8;
    t->lat_wave = 0x20;
    t->pad_glen = 0;
    if (cnt > 0) {
        const inst_t *in = &list[t->pad_sound < cnt ? t->pad_sound : 0];
        t->lat_wave = in->wave;
        t->lat_ad = in->ad;
        t->lat_sr = in->sr;
        t->lat_pw = in->pw;
        t->pad_glen = in->glen;
        memcpy(t->pad_gpw, in->gpw, sizeof(t->pad_gpw));
        memcpy(t->pad_gratio, in->gratio, sizeof(t->pad_gratio));
        memcpy(t->pad_gw, in->gwave, sizeof(t->pad_gw));
    }
    pad_mod_apply(t);
}

int tinysid_pad_options(const tinysid_t *t, char *buf, int buf_len)
{
    int n = snprintf(buf, (size_t)buf_len, "[");
    for (int i = 0; i < pad_sound_count(t) && n < buf_len; i++) {
        char name[40];
        pad_sound_name(t, i, name, sizeof(name));
        n += snprintf(buf + n, (size_t)(buf_len - n), "%s\"%s\"", i ? "," : "", name);
    }
    if (n < buf_len) n += snprintf(buf + n, (size_t)(buf_len - n), "]");
    return n >= buf_len ? -1 : n;
}

/* Takes the target voice away from the song: silent at once, then the keyboard owns it. */
static void pad_takeover(tinysid_t *t)
{
    int v = pad_voice(t);
    if (v < 0) return;
    t->pad_latched = 1;
    t->pad_gate = 0;
    t->pad_pending = 0;
    t->pad_gframe = 0;
    t->pad_gacc = 0.0;
    sid_set_voice_gain(&t->sid, v, t->vol[v]);
    pad_choose_patch(t);
    sid_write(&t->sid, (uint8_t)(v * 7 + 4), 0);         /* no waveform, no gate ... */
    sid_kill_voice(&t->sid, v);                          /* ... and no envelope tail from the song's last note */
    apply_voice(t, v);
}

/* Gives the voice back to the 6502 by replaying its shadow registers onto the chip. */
static void pad_unlatch(tinysid_t *t, int voice)
{
    t->pad_latched = t->pad_gate = 0;
    t->pad_pending = 0;
    for (int v = 0; v < 3; v++) sid_set_voice_gain(&t->sid, v, t->vol[v]);   /* drop pad velocity scaling */
    apply_voice(t, voice);
}

/* Runs right after the 6502 frame routine: whatever it wrote, the pad voice is put back. */
static void pad_frame_intercept(tinysid_t *t)
{
    if (t->pad_latched) apply_voice(t, pad_voice(t));
}

static int pad_remove(tinysid_t *t, int note)
{
    for (int i = 0; i < t->pad_count; i++) {
        if (t->pad_notes[i] == note) {
            memmove(&t->pad_notes[i], &t->pad_notes[i + 1], (size_t)(t->pad_count - i - 1));
            t->pad_count--;
            return 1;
        }
    }
    return 0;
}

static void pad_note_on(tinysid_t *t, int note, int vel)
{
    int v = pad_voice(t);
    int fresh = t->pad_count == 0;

    if (t->pad_pending) pad_takeover(t);                 /* a key pressed mid-fade does not wait for it */
    pad_remove(t, note);
    if (t->pad_count == MAX_PAD_NOTES) {
        memmove(&t->pad_notes[0], &t->pad_notes[1], MAX_PAD_NOTES - 1);
        t->pad_count--;
    }
    t->pad_notes[t->pad_count++] = (uint8_t)note;
    t->pad_vel = (float)vel / 127.0f;
    t->active_note = note;
    t->pad_freq = note_to_sid_freq(t, note);
    t->pad_gframe = 0;                                   /* the instrument's movement starts over with each key */
    t->pad_gacc = 0.0;

    pad_choose_patch(t);
    t->pad_gate = 1;
    sid_set_voice_gain(&t->sid, v, t->vol[v] * (0.4f + 0.6f * t->pad_vel));
    if (fresh) sid_write(&t->sid, (uint8_t)(v * 7 + 4), t->lat_wave);   /* gate low first so the attack restarts */
    apply_voice(t, v);
}

static void pad_note_off(tinysid_t *t, int note)
{
    if (!pad_remove(t, note) || !t->pad_latched) return;
    if (t->pad_count > 0) {                              /* other notes still held: fall back to the newest */
        t->active_note = t->pad_notes[t->pad_count - 1];
        t->pad_freq = note_to_sid_freq(t, t->active_note);
        pad_mod_apply(t);
    } else {
        t->pad_gate = 0;                                 /* the release plays out; the voice stays the keyboard's */
    }
    apply_voice(t, pad_voice(t));
}

void tinysid_midi(tinysid_t *t, const uint8_t *msg, int len)
{
    if (!msg || len < 1) return;
    uint8_t st = msg[0];

    if (st >= 0xF8) {
        if (st == 0xF8) {                                /* clock: tracked so the tempo is ready for Host Lock */
            host_clock_tick(t);
        } else if (st == 0xFA) {                         /* Start: back to beat 1 */
            t->transport_running = 1;
            engine_start(t);
        } else if (st == 0xFB) {                         /* Continue */
            t->transport_running = 1;
            engine_resume(t);
        } else if (st == 0xFC) {                         /* Stop */
            t->transport_running = 0;
            engine_stop(t);
        }
        return;
    }

    if (len < 3) return;
    uint8_t kind = st & 0xF0;
    int note = msg[1] & 0x7F;
    if (kind == 0x90 || kind == 0x80) {
        if (pad_voice(t) < 0) return;
        if (kind == 0x90 && (msg[2] & 0x7F)) pad_note_on(t, note, msg[2] & 0x7F);
        else pad_note_off(t, note);
    } else if (kind == 0xB0 && msg[1] == 123) {          /* all notes off */
        t->pad_count = 0;
        if (t->pad_latched) {
            t->pad_gate = 0;
            apply_voice(t, pad_voice(t));
        }
    }
}

/* ---- voice snooper ----------------------------------------------------------------- */

/* The MIDI note a voice is sounding, from the registers the chip actually has (so the tune, the pad and the pad's
 * movement all show up). 0 when the gate is low, the voice has no waveform, or it is noise only (no pitch). */
static int voice_note(const tinysid_t *t, int v)
{
    const uint8_t *r = &t->sid.reg[v * 7];
    uint8_t ctrl = r[4], wave = ctrl & 0xF0;
    int word = r[0] | (r[1] << 8);
    if (!(ctrl & 1) || !wave || wave == 0x80 || !word) return 0;
    double hz = word * t->clock_hz / 16777216.0;
    long n = lround(69.0 + 12.0 * log2(hz / 440.0));
    return n < 0 ? 0 : (n > 127 ? 127 : (int)n);
}

/* The voice snoop_note follows right now, or -1 when it is idle. */
static int snoop_pick(const tinysid_t *t)
{
    if (t->snoop_voice < 3) return voice_note(t, t->snoop_voice) > 0 ? t->snoop_voice : -1;
    int best = -1;                                       /* all voices: the one that was gated on most recently */
    for (int v = 0; v < 3; v++)
        if (voice_note(t, v) > 0 && (best < 0 || t->snoop_rise[v] > t->snoop_rise[best])) best = v;
    return best;
}

static void update_snoop(tinysid_t *t)
{
    for (int v = 0; v < 3; v++) {
        uint8_t gate = t->sid.reg[v * 7 + 4] & 1;
        if (gate && !t->snoop_gate[v]) t->snoop_rise[v] = ++t->snoop_seq;
        t->snoop_gate[v] = gate;
    }

    int v = snoop_pick(t);
    if (v < 0) {
        t->snoop_note = 0;
        t->snoop_hz = 0.0;
        t->snoop_cents = 0;
        t->snoop_level = 0;
        t->snoop_prev = 0;
        return;
    }
    const uint8_t *r = &t->sid.reg[v * 7];
    int note = voice_note(t, v);
    double hz = (r[0] | (r[1] << 8)) * t->clock_hz / 16777216.0;
    double ideal = 440.0 * pow(2.0, (note - 69) / 12.0);
    long cents = lround(1200.0 * log2(hz / ideal));
    t->snoop_note = note;
    t->snoop_hz = hz;
    t->snoop_cents = (int)(cents < -50 ? -50 : (cents > 50 ? 50 : cents));
    t->snoop_level = (int)lround(t->sid.v[v].env * 100.0 / 255.0);

    if (note != t->snoop_prev) {                         /* a new note: remember it for the trail */
        memmove(&t->snoop_recent[1], &t->snoop_recent[0], sizeof(t->snoop_recent[0]) * (SNOOP_RECENT - 1));
        t->snoop_recent[0].note = (uint8_t)note;
        t->snoop_recent[0].at = t->sample_clock;
        if (t->snoop_recent_n < SNOOP_RECENT) t->snoop_recent_n++;
        t->snoop_prev = note;
    }
}

/* One string with everything the piano roll draws, so a single read per refresh is enough (the host polls a widget's
 * extra keys a few times a second, which is why the trail of recent notes travels with it):
 *   K,<note>,<voice 0..3>,<key 0..24>,<cents>,<level %>,<recent notes, newest first, lit within SNOOP_TRAIL_MS>
 * key = tonic * 2 + (minor ? 1 : 0), 24 when unknown. */
static int snoop_feed(const tinysid_t *t, char *buf, int buf_len)
{
    int key = t->song_key == KEY_NONE ? 24 : t->song_key * 2 + (t->song_scale == SCALE_MINOR ? 1 : 0);
    int n = snprintf(buf, (size_t)buf_len, "K,%d,%d,%d,%d,%d", t->snoop_note, t->snoop_voice, key, t->snoop_cents, t->snoop_level);
    uint64_t window = (uint64_t)(SNOOP_TRAIL_MS * 0.001 * t->sample_rate);
    for (int i = 0; i < t->snoop_recent_n && n < buf_len; i++) {
        if (t->sample_clock - t->snoop_recent[i].at > window) break;
        if (t->snoop_recent[i].note == t->snoop_note) continue;
        n += snprintf(buf + n, (size_t)(buf_len - n), ",%d", t->snoop_recent[i].note);
    }
    return n >= buf_len ? buf_len - 1 : n;
}

/* ---- key / scale / tempo pre-scan ----------------------------------------- */

/* Krumhansl-Schmuckler key profiles. */
static const float k_profile_major[12] = { 6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f };
static const float k_profile_minor[12] = { 6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f };

static double pearson12(const float *x, const float *profile, int shift)
{
    double mx = 0, my = 0;
    for (int i = 0; i < 12; i++) { mx += x[(i + shift) % 12]; my += profile[i]; }
    mx /= 12.0; my /= 12.0;
    double sxy = 0, sxx = 0, syy = 0;
    for (int i = 0; i < 12; i++) {
        double dx = x[(i + shift) % 12] - mx, dy = profile[i] - my;
        sxy += dx * dy; sxx += dx * dx; syy += dy * dy;
    }
    return sxx > 0 && syy > 0 ? sxy / sqrt(sxx * syy) : 0.0;
}

static int detect_key_r(const float pc[12], int *scale, double *strength)
{
    float total = 0;
    for (int i = 0; i < 12; i++) total += pc[i];
    *scale = SCALE_NONE;
    if (total < 6.0f) return KEY_NONE;                   /* too few notes to say anything */

    double best = -2.0;
    int best_key = KEY_NONE, best_scale = SCALE_NONE;
    for (int tonic = 0; tonic < 12; tonic++) {
        double rmaj = pearson12(pc, k_profile_major, tonic);
        double rmin = pearson12(pc, k_profile_minor, tonic);
        if (rmaj > best) { best = rmaj; best_key = tonic; best_scale = SCALE_MAJOR; }
        if (rmin > best) { best = rmin; best_key = tonic; best_scale = SCALE_MINOR; }
    }
    if (best < 0.25) return KEY_NONE;                    /* no tonal centre (drums, atonal effects) */
    *scale = best_scale;
    *strength = best;
    return best_key;
}

int tinysid_detect_key(const float pc[12], int *scale)
{
    double strength;
    return detect_key_r(pc, scale, &strength);
}

/* Closes a window: its key gets a vote. */
static void ana_close_window(tinysid_t *t)
{
    int scale;
    double strength = 0;
    int key = detect_key_r(t->ana_pc, &scale, &strength);
    if (key != KEY_NONE) {
        t->key_votes[key][scale]++;
        t->key_strength[key][scale] += strength;
    }
    memset(t->ana_pc, 0, sizeof(t->ana_pc));
}

/* Most votes wins; a tie goes to the key with the clearer tonal centre, then to the one found first. */
static void ana_pick_key(tinysid_t *t)
{
    int best_key = KEY_NONE, best_scale = SCALE_NONE, best_votes = 0;
    double best_strength = 0;
    for (int k = 0; k < 12; k++)
        for (int sc = 0; sc < 2; sc++) {
            int v = t->key_votes[k][sc];
            if (v > best_votes || (v == best_votes && v > 0 && t->key_strength[k][sc] > best_strength + 1e-9)) {
                best_votes = v;
                best_strength = t->key_strength[k][sc];
                best_key = k;
                best_scale = sc;
            }
        }
    t->song_key = best_key;
    t->song_scale = best_scale;
}

static void ana_abort(tinysid_t *t)
{
    if (t->ana) {
        tinysid_destroy(t->ana);
        t->ana = NULL;
    }
}

/* Starts a silent scan of the current tune and subtune on a second engine. */
static void ana_start(tinysid_t *t)
{
    if (t->is_analysis) return;
    ana_abort(t);
    t->song_key = KEY_NONE;
    t->song_scale = SCALE_NONE;
    t->ana_done = 0;
    t->ana_frames = 0;
    memset(t->ana_pc, 0, sizeof(t->ana_pc));
    memset(t->key_votes, 0, sizeof(t->key_votes));
    memset(t->key_strength, 0, sizeof(t->key_strength));
    memset(t->inst_n, 0, sizeof(t->inst_n));             /* the new tune's instruments are not known yet */
    t->merged_n = 0;
    t->pad_sound = 0;
    for (int v = 0; v < 3; v++) { t->ana_note[v] = -1; t->ana_gate[v] = 0; }
    if (!t->loaded) return;

    tinysid_t *a = tinysid_create(t->sample_rate);
    if (!a) return;
    a->is_analysis = 1;
    if (tinysid_load(a, t->file, t->file_len) != PSID_OK) {
        tinysid_destroy(a);
        return;
    }
    a->subtune = t->subtune;
    init_tune(a);
    t->ana = a;
}

/* Ranks the scan's instruments per voice by how long the tune plays them. */
static void collect_instruments(tinysid_t *t)
{
    for (int v = 0; v < 3; v++) {
        uint8_t idx[INST_SIGS];
        int n = 0;
        for (int i = 0; i < t->ana->sig_n[v]; i++)
            if (t->ana->sig[v][i].count >= INST_MIN_FRAMES) idx[n++] = (uint8_t)i;
        for (int i = 1; i < n; i++) {                    /* stable insertion sort of the indexes, longest heard first */
            uint8_t key = idx[i];
            int j = i - 1;
            while (j >= 0 && t->ana->sig[v][idx[j]].count < t->ana->sig[v][key].count) { idx[j + 1] = idx[j]; j--; }
            idx[j + 1] = key;
        }
        t->inst_n[v] = n < INST_MAX ? n : INST_MAX;
        for (int i = 0; i < t->inst_n[v]; i++) t->inst[v][i] = t->ana->sig[v][idx[i]];
    }

    /* A voice the song never sounds borrows the instruments of the other voices: pooled, longest heard first. */
    t->merged_n = 0;
    for (int o = 0; o < 3; o++)
        for (int i = 0; i < t->inst_n[o]; i++) {
            const inst_t *x = &t->inst[o][i];
            int k;
            for (k = 0; k < t->merged_n; k++)
                if (t->merged[k].wave == x->wave && t->merged[k].ad == x->ad && (t->merged[k].sr & 0x0F) == (x->sr & 0x0F)) break;
            if (k < t->merged_n) {
                inst_t *m = &t->merged[k];
                unsigned sum = (unsigned)m->count + x->count;
                if (x->glen > m->glen) {                 /* keep the longest recorded movement */
                    unsigned total = sum;
                    *m = *x;
                    sum = total;
                } else if ((x->sr >> 4) > (m->sr >> 4)) {
                    m->sr = x->sr;
                }
                m->count = (uint16_t)(sum > 65535u ? 65535u : sum);
            } else if (t->merged_n < INST_MAX) {
                t->merged[t->merged_n++] = *x;
            }
        }
    for (int i = 1; i < t->merged_n; i++) {
        inst_t key = t->merged[i];
        int j = i - 1;
        while (j >= 0 && t->merged[j].count < key.count) { t->merged[j + 1] = t->merged[j]; j--; }
        t->merged[j + 1] = key;
    }
}

static void ana_finish(tinysid_t *t)
{
    collect_instruments(t);
    ana_pick_key(t);
    /* The tempo is set once, from the whole scan. A catalogued or typed tempo is left alone. */
    int fn = 0, fd = 1;
    double bpm = gaps_to_fpr(t->ana->gap_hist, GATE_GAP_MAX + 1, native_hz(t), &fn, &fd);
    if (bpm > 0.0 && t->song_bpm_src == BPM_SRC_DEFAULT) {
        set_song_bpm(t, floor(bpm + 0.5), BPM_SRC_ESTIMATE);           /* a whole BPM: the readout and the reference agree */
        t->song_fpr_num = fn;
        t->song_fpr_den = fd;
    }
    t->ana_done = 1;
    ana_abort(t);
}

static void ana_frame(tinysid_t *t)
{
    tinysid_t *a = t->ana;
    do_play(a);
    for (int v = 0; v < 3; v++) {
        uint8_t ctrl = a->raw[v * 7 + 4];
        if ((ctrl & 1) && (ctrl & 0xF0)) {
            if (a->sig_rise[v]) { note_finish(a, v); note_start(a, v); }   /* a new note began in this frame */
            inst_record(a, v);                           /* one sample per frame the voice is sounding */
            note_sample(a, v);
        } else {
            note_finish(a, v);
        }
        a->sig_rise[v] = 0;
    }
    for (int v = 0; v < 3; v++) {
        uint8_t ctrl = a->raw[v * 7 + 4];
        uint8_t wave = ctrl & 0xF0;
        int gate = ctrl & 1;
        double freq = a->raw[v * 7] | (a->raw[v * 7 + 1] << 8);
        double hz = freq * a->clock_hz / 16777216.0;
        int note = -1;
        if (gate && wave && !(wave & 0x80) && hz >= 30.0 && hz <= 4200.0)
            note = (int)lround(69.0 + 12.0 * log2(hz / 440.0));
        if (note >= 0 && (!t->ana_gate[v] || note != t->ana_note[v]))
            t->ana_pc[((note % 12) + 12) % 12] += t->ana_gate[v] ? 0.3f : 1.0f;      /* a new gate counts more than a slide */
        t->ana_note[v] = note;
        t->ana_gate[v] = note >= 0;
    }
    t->ana_frames++;
    if (t->ana_frames % KEY_WINDOW == 0) ana_close_window(t);
}

/* Runs up to `budget` frames of the scan: 6502 only, no audio. */
static void ana_run(tinysid_t *t, int budget)
{
    if (!t->ana) return;
    for (int i = 0; i < budget && t->ana_frames < ANA_FRAMES; i++) ana_frame(t);
    if (t->ana_frames >= ANA_FRAMES) ana_finish(t);
}

/* The scan advances ANA_SLICE frames per 128 samples rendered, so its pace is the same whatever block size the host uses. */
static void ana_step(tinysid_t *t, int samples)
{
    ana_run(t, ANA_SLICE * ((samples + 127) / 128));
}

void tinysid_scan_advance(tinysid_t *t, int frames) { ana_run(t, frames); }
int tinysid_scan_frames(const tinysid_t *t) { return t->ana_done ? ANA_FRAMES : t->ana_frames; }

/* ---- Host Lock: the tune's frames follow the MIDI clock ----------------------------------------------
 *
 * The lock works on ticks, not on time. A 16th-note row is 6 ticks at 24 PPQN (12 at Half, 3 at 2x) and the tune
 * spends fpr = num / den frames on a row, so after K ticks exactly floor(K * num / (den * 6)) + 1 frames are due:
 * integers throughout, so no rounding error can build up however long the clip runs, and the host's tempo never
 * enters the count (it only places the frames between ticks). Frame 1 is played on tick 0, which is the first
 * tick after Start, so the tune's first downbeat falls on Step 1 of the clip. */

static int lock_engaged(const tinysid_t *t)
{
    return t->sync_mode >= SYNC_HOST_1X && t->song_fpr_num > 0 && t->song_fpr_den > 0 && fabsf(t->tempo_trim - 1.0f) < 1e-6f;
}

/* Frames due now (before any adopted offset). *next is the number of samples until the next frame falls inside this
 * tick, or 0 when it belongs to a later tick. Between ticks the position is interpolated with the measured tempo and
 * held just short of the next tick, so a frame never runs ahead of the count that tick will demand. */
static int64_t lock_due(const tinysid_t *t, double *next)
{
    int64_t mnum = t->sync_mode == SYNC_HOST_2X ? 2 : 1, mden = t->sync_mode == SYNC_HOST_HALF ? 2 : 1;
    int64_t a = (int64_t)t->song_fpr_num * mnum, b = (int64_t)t->song_fpr_den * 6 * mden;
    int64_t q = t->clk_ticks * a / b, rem = t->clk_ticks * a % b;
    double tick_len = t->sample_rate * 60.0 / ((t->host_bpm_valid ? t->host_bpm : HOST_BPM_DEFAULT) * 24.0);
    double frac = t->since_tick / tick_len;
    if (frac > 0.999999) frac = 0.999999;
    int64_t whole = (int64_t)floor(((double)rem + frac * (double)a) / (double)b);
    double need = (double)((whole + 1) * b - rem) / (double)a;       /* the tick fraction at which the next frame falls */
    *next = 0.0;
    if (need < 0.999999) {
        double s = ceil(need * tick_len - t->since_tick);
        *next = s < 1.0 ? 1.0 : s;
    }
    return q + whole + 1;
}

/* ---- rendering ----------------------------------------------------------- */

void tinysid_render(tinysid_t *t, float *left, float *right, int n)
{
    if (n <= 0) return;

    ana_step(t, n);
    if (t->clock_status_fn) {                                      /* hosts that report transport state but send no 0xFA/0xFC */
        int st = t->clock_status_fn();
        if (st != t->last_clock_status) {
            t->last_clock_status = st;
            if (st == CLOCK_RUNNING) { t->transport_running = 1; engine_start(t); }
            else if (st == CLOCK_STOPPED) { t->transport_running = 0; engine_stop(t); }
        }
    }

    int done = 0;
    while (done < n) {
        int chunk = n - done;

        int lock = 0;                                    /* 1 = frames follow the clock, 2 = waiting for the first tick */
        if (t->playing && t->loaded) {
            if (lock_engaged(t)) {
                if (t->clk_ticks < 0) {
                    if (t->lock_wait < LOCK_WAIT_SEC * t->sample_rate) lock = 2;
                } else if (t->since_tick > LOCK_CLOCK_LOST_SEC * t->sample_rate) {
                    engine_stop(t);                      /* the clock has gone: pause, and carry on when it returns */
                    t->lock_lost = 1;
                } else {
                    lock = 1;
                }
            } else {
                t->lock_aligned = 0;
            }
        }
        if (lock == 1) {
            double next;
            int64_t raw = lock_due(t, &next);
            if (!t->lock_aligned) {                      /* joining a tune that is already running: keep its place */
                t->lock_off = t->frame_no ? (int64_t)t->frame_no - raw : 0;
                t->lock_aligned = 1;
            }
            int64_t d = raw + t->lock_off - (int64_t)t->frame_no;
            if (d > LOCK_TOLERANCE || d < -LOCK_TOLERANCE) t->lock_off = (int64_t)t->frame_no - raw;
            int plays = 0;
            while ((int64_t)t->frame_no < raw + t->lock_off && plays++ < LOCK_MAX_PLAYS) do_play(t);
            if (next > 0.0 && next < chunk) chunk = (int)next;
            t->tick_acc = 0.0;
        } else if (lock == 2) {
            t->tick_acc = 0.0;
        } else if (t->playing && t->loaded) {
            /* Sample accumulator: tick_acc counts samples to the next play(); the rate is re-read every
             * pass so tempo changes apply immediately, and plays land on their exact sample. */
            double spt = t->sample_rate / target_hz(t);
            int guard = 0;
            while (t->tick_acc <= 0.0 && guard++ < 64) {
                do_play(t);
                t->tick_acc += spt;
            }
            int c = (int)ceil(t->tick_acc);
            if (c < 1) c = 1;
            if (c < chunk) chunk = c;
        }

        int moving = t->pad_latched && t->pad_gate && t->pad_glen > 1;   /* a held pad note with recorded movement */
        double spf = t->sample_rate / target_hz(t);
        if (moving) {
            int c = (int)ceil(spf - t->pad_gacc);        /* land the next movement step on its exact sample */
            if (c < 1) c = 1;
            if (c < chunk) chunk = c;
        }

        sid_render(&t->sid, left + done, chunk);

        if (moving) {
            t->pad_gacc += chunk;
            int stepped = 0;
            while (t->pad_gacc >= spf - 1e-9) { t->pad_gacc -= spf; t->pad_gframe++; stepped = 1; }
            if (stepped) {
                pad_mod_apply(t);
                apply_voice(t, pad_voice(t));
            }
        }
        if (t->pad_pending > 0) {
            t->pad_pending -= chunk;
            if (t->pad_pending <= 0) pad_takeover(t);
        }
        if (t->stop_fade > 0) {
            t->stop_fade -= chunk;
            if (t->stop_fade <= 0) stop_cut(t);
        }
        if (t->playing && !lock) t->tick_acc -= chunk;
        t->since_tick += chunk;
        if (lock == 2) t->lock_wait += chunk;
        done += chunk;
    }
    t->sample_clock += (uint64_t)n;
    update_snoop(t);
    if (t->contract_pulse > 0) t->contract_pulse -= n;

    for (int i = 0; i < n; i++) {
        t->trim_cur += (t->trim_gain - t->trim_cur) * t->trim_coef;
        float x = left[i];
        if (!(x == x)) x = 0.0f;                         /* NaN guard, before it can poison the filter state */
        float y = x - t->dc_x1 + t->dc_coef * t->dc_y1;
        t->dc_x1 = x;
        t->dc_y1 = fabsf(y) < 1e-20f ? 0.0f : y;
        float s = y * t->trim_cur;
        float a = fabsf(s);
        if (a > 0.8f) s = copysignf(0.8f + 0.2f * tanhf((a - 0.8f) * 5.0f), s);   /* soft knee above 0.8 */
        s = clampf(s, -1.0f, 1.0f);
        left[i] = right[i] = s;
    }
}

/* ---- introspection --------------------------------------------------------- */

const psid_info_t *tinysid_info(const tinysid_t *t) { return t->loaded ? &t->info : NULL; }
uint8_t tinysid_sid_reg(const tinysid_t *t, int reg) { return t->sid.reg[reg & 0x1F]; }
uint8_t tinysid_ram(const tinysid_t *t, uint16_t addr) { return t->ram[addr]; }
uint64_t tinysid_play_calls(const tinysid_t *t) { return t->play_calls; }
double tinysid_frame_hz(const tinysid_t *t) { return target_hz(t); }
double tinysid_host_bpm(const tinysid_t *t) { return t->host_bpm; }
int tinysid_song_bpm(const tinysid_t *t) { return t->song_bpm; }
int tinysid_song_fpr(const tinysid_t *t, int *num, int *den)
{
    if (num) *num = t->song_fpr_num;
    if (den) *den = t->song_fpr_den;
    return t->song_fpr_num > 0 && t->song_fpr_den > 0;
}
int64_t tinysid_clock_ticks(const tinysid_t *t) { return t->clk_ticks; }
int tinysid_pad_gesture_length(const tinysid_t *t) { return t->pad_glen; }
int tinysid_snoop_note(const tinysid_t *t) { return t->snoop_note; }
int tinysid_song_key(const tinysid_t *t) { return t->song_key; }
int tinysid_song_scale(const tinysid_t *t) { return t->song_scale; }
int tinysid_analysis_done(const tinysid_t *t) { return t->ana_done; }
int tinysid_running(const tinysid_t *t) { return t->playing; }
void tinysid_set_clock_status_fn(tinysid_t *t, int (*fn)(void)) { t->clock_status_fn = fn; }
double tinysid_samples_per_frame(const tinysid_t *t) { return t->sample_rate / target_hz(t); }
