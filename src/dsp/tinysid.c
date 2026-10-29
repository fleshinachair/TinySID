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
#define SNAP_STEADY_CENTS 40.0       /* a voice that moves less than this between frames is holding a note (vibrato included) */
#define SNAP_STEADY_FRAMES 3         /* ... for this many frames before it is pulled onto a note */
#define SNAP_HYSTERESIS 65.0         /* a held note stays on its note until it is this far from it */
#define SNAP_TUNING_FRAMES 24        /* the tune's own tuning is trusted after this many tonal frames ... */
#define SNAP_TUNING_NOTES 5          /* ... and they span at least this many different notes: one repeated pitch (a sound effect) says nothing about a tuning */
#define SNAP_TUNING_CONC 0.5         /* ... if their errors from the grid agree this well (1 = all the same) */
#define PAD_FADE_SEC 0.020           /* a chosen pad voice ramps out this long before the keyboard takes it */
#define STOP_FADE_SEC 0.012          /* voices ramp down this long before their envelopes are cut */
#define ANA_FRAMES 15000             /* the pre-scan listens to this many frames (5 min at 50 Hz): instruments can first appear late ... */
#define ANA_SLICE 8                  /* ... a few per audio block, so it never stalls the audio thread */
#define FX_MAX 8                     /* sound effects offered as pads: the tune's other subtunes that sound once and stop */
#define FX_SLOTS 3                   /* effects that can sound at once, each on a spare engine of its own */
#define FX_PROBE_SUBTUNES 64         /* subtunes probed at most */
#define FX_PROBE_FRAMES 600          /* a subtune is listened to for 12 s; one still sounding near the end is music, not an effect */
#define FX_QUIET_FRAMES 150          /* ... an effect has been silent for this long when the listening ends */
#define FX_MAX_LAST_FRAME 300        /* ... and its last sound is within 6 s: a subtune that runs longer is a second tune, not an effect */
#define FX_MIN_FRAMES 3              /* ... and it sounded for at least this many frames (not a glitch) */
#define FX_TAIL_FRAMES 50            /* an effect plays on for 1 s after its last gate for the envelope's release */
#define FX_SLICE 8                   /* frames of that listening per audio block, next to the key scan's own */
#define FX_FADE_SEC 0.010            /* an effect is faded out over this long when its time is up */
#define FX_DUCK_DEFAULT 0.4f         /* how far the song dips when an effect hits */
#define AUDIO_DELAY_MAX_MS 200        /* audio_delay: TinySID's own sound can wait this long for tracks fed by Notes Out */
#define AUDIO_DELAY_FRAMES 9600       /* ... room for 200 ms at up to 48 kHz */
#define AUDIO_DELAY_FADE_SEC 0.005    /* switching the delay in or out fades the output out and back in over this long */
#define MOUT_Q 64                    /* notes out: events waiting for the host to take them */
#define MOUT_BEND_RANGE 200.0        /* ... a pitch bend of +-2 semitones is assumed at the receiving end: the bend carries the cents */
#define MOUT_BEND_STEP 3.0           /* ... and is resent when the pitch has moved this many cents */
#define MOUT_VELOCITY 101            /* ... an odd velocity on purpose: it marks our notes when they come back to a screen as pad presses */
#define MOUT_ECHO_SEC 0.5            /* ... notes handed to the host are listed for this long (see mout_echo) */
#define ECHO_EXPIRE_SEC 0.3          /* a note we handed to the host comes back into this slot at most once, within this long */
#define SNOOP_RECENT 6               /* notes remembered for the piano roll's trail ... */
#define SNOOP_TRAIL_MS 600           /* ... for this long */
#define LOCK_MAX_PLAYS 16            /* play() calls a Host Lock catch-up may make per audio block */
#define LOCK_TOLERANCE 64            /* frames the lock will wait out or catch up; further off, it adopts the tune's phase */
#define LOCK_CLOCK_LOST_SEC 2.0      /* no MIDI clock for this long: the tune pauses until it returns */
#define LOCK_WAIT_SEC 1.0            /* after a start, wait this long for the first clock tick before free-running */
#define OUT_BASE_GAIN 0.56f          /* -5 dB: the raw SID mix peaks near full scale; -9 dB (to sit level with Schwung's other generators)
                                        was found too quiet on a Move, and the soft knee above 0.8 takes the loudest tunes */
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
#define DUCK_TICKS 24                /* MIDI clock ticks per quarter note */
#define BEAT_CLOCK_LIVE_SEC 0.25     /* a host clock tick this recent means the host keeps the beat; otherwise the song's own tempo does */
#define PAD_MAX_RELEASE 8            /* a pad note rings out for 300 ms at most (release 8): the song's own releases run to 24 s, which its driver */
                                     /* cuts short with a hard restart, and a pad has no driver to do that */
#define PAD_MAX_BEND_CENTS 100.0     /* recorded pitch movement wider than a semitone (an arpeggio, a swoop) is not carried onto the pad notes */
#define PUMP_OPEN_HZ 20000.0f        /* the filter pump's low-pass at rest ... */
#define PUMP_OCTAVES 6.0f            /* ... and how far a full-depth beat closes it: 6 octaves, down to about 310 Hz */
#define PUMP_Q 1.2f
#define CONTROL_BASE_NOTE 36         /* in Pad Mode Control the pads from this note up are controls, not notes */
enum { PADMODE_PLAY = 0, PADMODE_CONTROL };
#define LOOP_BAR_TICKS 96            /* one 4/4 bar of MIDI clock: 4 beats x 24 ticks */

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
    tinysid_hpf_t hpf[2];            /* output DC blocker per channel (the SID's unipolar waves carry real DC), 35 Hz */
    float stereo_spread;             /* 0 = mono, 1 = voice 2 left and voice 3 right */
    float duck_depth, duck_release_ms;
    float duck_env, duck_goal, duck_alpha;   /* sidechain ducking: the gain now, what it is dropping to, the release step */
    int duck_atk_left;               /* samples of the 2 ms attack still to go */
    int64_t duck_ticks;              /* the host's own MIDI clock ticks since Start (-1 before the first): a kick every 24 */

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
    double beat_left;                /* play() frames to the song's own next quarter note (the ducker's beat when no host clock is running) */

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

    /* Sound effects: a silent scan finds the subtunes that sound once and stop; a hit plays one on a spare engine, mixed over the song */
    int is_fx;                       /* this engine plays an effect for another: no scan of its own */
    struct tinysid *fxs;             /* the scan's engine, on the subtune being listened to */
    int fxs_sub, fxs_frame, fxs_first, fxs_last, fxs_sounding, fxs_done;
    int fxs_total;                   /* frames listened to so far, over every subtune */
    int fx_n;                        /* effects found */
    uint8_t fx_sub[FX_MAX];          /* ... their subtunes */
    uint16_t fx_frames[FX_MAX];      /* ... and how long each plays, in frames */
    struct tinysid *fx_eng[FX_SLOTS];
    float fx_gain[FX_SLOTS];
    int fx_left[FX_SLOTS];           /* samples left; 0 = the slot is free */
    float fx_duck;                   /* how far the song dips on a hit, 0..1 */

    /* Notes out: one voice's notes as MIDI, with the pitch bend that carries the exact pitch the chip is playing */
    int audio_delay_ms;              /* the whole output waits this long, so tracks played by Notes Out line up with it */
    int dly_want;                    /* ... in samples; it is in force only while Notes Out sends a voice */
    int dly_n, dly_pos;              /* the delay in force now, in samples, and where the delay line is */
    int dly_fade_out, dly_wait, dly_fade_in;   /* a switch in progress: fading out, the new line filling, fading in */
    float *dly;                      /* the delay line, two channels of AUDIO_DELAY_FRAMES (allocated the first time a delay is set) */
    int mout_mask;                   /* bit v: voice v is sent */
    int mout_ch[3];                  /* each voice's MIDI channel, 1..16 */
    struct { uint8_t type, note, channel; uint16_t value; } mout_q[MOUT_Q];   /* type 0 note off, 1 note on (value = velocity), 2 pitch bend (value 0..16383) */
    int mout_head, mout_n;
    int mout_max;                    /* the longest the queue has been (a readout, for the host's pace) */
    int mout_note[3];                /* the note each voice is sounding, -1 = none */
    double mout_bend_cents[3];       /* the bend last sent for it, in cents from that note */
    uint8_t mout_rise[3];            /* a voice was gated on during this frame */
    uint64_t mout_echo_at[128];      /* per note: sample_clock + 1 when the host last took it, on whatever channel; 0 = never */
    uint32_t pad_seq;                /* pad presses carried out (pad_press), and the last one: pad_ack */
    int pad_last;
    uint32_t pad_echoes;             /* pad presses dropped as our own notes coming back */
    uint8_t echo_held[128];          /* a note-on was dropped as our own note coming back: its note-off goes the same way */
    uint8_t echo_midi[128];          /* per note: echoes still to come back as MIDI into this slot (tinysid_midi) ... */
    uint64_t echo_until[128];        /* ... until this sample_clock; after it they are not coming */
    uint32_t midi_echoes;            /* incoming notes dropped as our own coming back */
    int note_controls;               /* Pad Mode Control turns incoming notes 36..42 into buttons (the stock editor's way; 0 when the
                                        module's own screen does the pads through pad_press) */

    /* Timbre Lock: the voices' pulse width / waveform / envelope and the filter stay as they were when it was switched on */
    int timbre_lock;
    uint8_t timbre_snap[25];         /* the tune's registers at that moment; only the timbre registers are used */
    uint8_t timbre_voice[3];         /* the voice had an instrument then: a voice that was silent is not locked */
    uint8_t timbre_hr[3];            /* the tune last wrote this voice's AD and SR as zeros (a hard restart) */
    struct { uint8_t ctrl, ad, sr; uint16_t pw; uint8_t valid; } tl_inst[3];   /* each voice's last real instrument, kept up to date while the tune plays */

    /* AutoTune: the voices' pitch registers are moved to the nearest note before the chip sees them */
    int transpose;                   /* semitones, applied to every song voice after AutoTune */
    double bend_cents;               /* MIDI pitch bend, +-200 cents */
    int mute_mask;                   /* bit v: song voice v is muted from the pads */
    int pad_mode;                    /* PADMODE_* */
    int last_note;                   /* the last MIDI note-on seen (a readout, so the pad layout can be read off the device) */
    float pump_amt, pump_env;        /* filter pump: how far its low-pass closes on a beat, and where it is now (1 = fully closed) */
    float pump_s1[2], pump_s2[2];    /* the pump's low-pass (a state-variable filter) per channel */
    int pump_on;                     /* it is running, so its state is current */
    float lofi;                      /* 0 = off: bit depth and sample rate come down together */
    float crush_hold[2];
    int crush_n[2];
    int pitch_snap;                  /* TINYSID_SNAP_* */
    int master_tune;                 /* cents: the A = 440 Hz grid the notes are moved onto is shifted by this */
    double tune_cos, tune_sin;       /* where the tune's notes sit against that grid, as a circular mean (a unit vector per frame) */
    int tune_n;
    uint64_t tune_notes[2];          /* which MIDI notes the learning has heard (bit n of word n / 64) */
    struct {
        int has_last, steady;
        double last_m;               /* the voice's pitch last frame, in cents (MIDI note x 100) */
        double center;               /* its pitch as the tune writes it, smoothed over a few frames */
        double last_shift;           /* the global shift of the last frame, so a change of it does not step the pull */
        double target;               /* the note the held pitch is pulled to (kept until it is clearly wrong) */
        int has_target;
        double fine;                 /* the pull onto that note, in cents */
        double snap_corr;            /* AutoTune's part of the correction: the tune's offset plus the pull */
        double corr;                 /* total correction applied to the voice this frame, in cents (AutoTune plus transpose and bend) */
    } at[3];

    /* Bar Looper: at the top of bar `loop_start_bar` the whole 6502 is saved; `loop_bars` bars later it is put back */
    int loop_start_bar;              /* 1..32: the bar whose downbeat is the loop's start */
    int loop_bars;                   /* 0 = off, else 1, 2, 4 or 8 */
    int loop_valid;                  /* a snapshot is held (loop_ready) */
    int loop_nudge;                  /* 0..15 rows: the tune's bar lines are this many rows after the first tick (an intro pickup) */
    int loop_len_ticks;              /* the bar length the snapshot was taken on (it changes with the sync mode) */
    uint8_t seam_hold[3];            /* a note was still sounding at the seam: its control and release stay as they were until it dies */
    int64_t song_pos;                /* clock ticks of song time since the run began (-1 before the first); a rewind sets it back, clk_ticks never goes back */
    uint64_t loop_rewinds;
    uint8_t loop_ram[65536];
    uint8_t loop_raw[32];
    uint8_t loop_raster;
    uint16_t loop_pc;
    uint8_t loop_a, loop_x, loop_y, loop_sp, loop_p;
    /* Loop capture: the 6502 as it was at the top of the bar now playing, so a loop can start on it after the downbeat has passed */
    uint8_t bar_ram[65536];
    uint8_t bar_raw[32];
    uint8_t bar_raster;
    uint16_t bar_pc;
    uint8_t bar_a, bar_x, bar_y, bar_sp, bar_p;
    int64_t bar_snap_bar;            /* the bar that copy is of, 0 = none */
    int cap_recording;               /* a capture is running (started, not yet stopped) ... */
    int cap_start;                   /* ... from this bar */

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
static const char *const k_mout_names[2] = { "Off", "On" };
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
static const char *const k_padmode_names[2] = { "Play", "Control" };
static const char *const k_snap_names[3] = { "Off", "A440 Chromatic", "In-Key (Scale)" };
static const char *const k_lock_names[2] = { "Unlocked", "Lock Timbre" };
#define LOOP_NUDGE_MAX 15
#define LOOP_LEN_COUNT 9
static const char *const k_looplen_names[LOOP_LEN_COUNT] = { "Off", "1 Bar", "2 Bars", "3 Bars", "4 Bars", "5 Bars", "6 Bars", "7 Bars", "8 Bars" };
static const int k_loop_bars[LOOP_LEN_COUNT] = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };
#define LOOP_START_MAX 256           /* a captured loop can start anywhere in a long tune */

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static double tuning_offset(const tinysid_t *t);
static void pad_retune(tinysid_t *t);

/* A song voice's level: its knob, unless it has been muted from the pads. */
static float voice_vol(const tinysid_t *t, int v) { return (t->mute_mask >> v) & 1 ? 0.0f : t->vol[v]; }

/* The notes the pad plays are in tune with the song as it sounds: the tune's own offset from A = 440 Hz when AutoTune leaves
 * it alone (a tune 37 cents sharp is a pad 37 cents sharp), or the grid's master tune when AutoTune has brought the song onto it. */
static double pad_tuning_cents(const tinysid_t *t)
{
    return t->pitch_snap ? (double)t->master_tune : tuning_offset(t);
}

static uint16_t pad_note_word(const tinysid_t *t, int note)
{
    double w = 440.0 * pow(2.0, ((note - 69) * 100.0 + pad_tuning_cents(t)) / 1200.0) * 16777216.0 / t->clock_hz + 0.5;
    return (uint16_t)(w < 0.0 ? 0.0 : (w > 65535.0 ? 65535.0 : w));
}

/* The song's key as the song now plays it (the tune's key moved by the transpose), or -1 while it is not known. */
static int shifted_key(const tinysid_t *t)
{
    return t->song_key == KEY_NONE ? -1 : (((t->song_key + t->transpose) % 12) + 12) % 12;
}

/* Transpose and pitch bend, in cents, added to every song voice's pitch. */
static double pitch_offset(const tinysid_t *t) { return t->transpose * 100.0 + t->bend_cents; }
static int pitch_active(const tinysid_t *t) { return t->pitch_snap || t->transpose || t->bend_cents != 0.0; }

/* ---- AutoTune --------------------------------------------------------------------- */

static const uint8_t k_scale_steps[2][7] = { { 0, 2, 4, 5, 7, 9, 11 }, { 0, 2, 3, 5, 7, 8, 10 } };

static double midi_cents(double word, double clock_hz)
{
    return 6900.0 + 1200.0 * log2(word * clock_hz / 16777216.0 / 440.0);
}

/* The pitch (cents, MIDI note x 100) of the note nearest to `c`: any semitone of the grid A = 440 Hz + `tune` cents, or
 * the nearest note of the key on that grid. A key that is not known gives -1e9. */
static double nearest_note(double c, int mode, int key, int scale, double tune)
{
    double u = (c - tune) / 100.0;
    if (mode == TINYSID_SNAP_CHROMATIC) return floor(u + 0.5) * 100.0 + tune;
    if (key < 0 || key > 11 || (scale != SCALE_MAJOR && scale != SCALE_MINOR)) return -1e9;
    double best = 1e9, target = floor(u);
    for (int k = (int)floor(u) - 2; k <= (int)floor(u) + 2; k++) {
        int degree = (((k - key) % 12) + 12) % 12;
        int in_scale = 0;
        for (int i = 0; i < 7; i++) if (k_scale_steps[scale][i] == degree) in_scale = 1;
        if (in_scale && fabs(u - k) < best - 1e-9) { best = fabs(u - k); target = k; }
    }
    return target * 100.0 + tune;
}

static uint16_t cents_to_word(double c, double clock_hz)
{
    double w = 440.0 * pow(2.0, (c - 6900.0) / 1200.0) * 16777216.0 / clock_hz + 0.5;
    return (uint16_t)(w < 1.0 ? 1.0 : (w > 65535.0 ? 65535.0 : w));
}

/* The SID frequency word of the note nearest to `word` on the grid A = 440 Hz + `tune_cents`: any semitone (chromatic) or
 * the nearest note of the key (a tie goes down). A key that is not known gives the word back unchanged. */
uint16_t tinysid_snap_word(uint16_t word, double clock_hz, int mode, int key, int scale, double tune_cents)
{
    if (!word || mode == TINYSID_SNAP_OFF) return word;
    double t = nearest_note(midi_cents(word, clock_hz), mode, key, scale, tune_cents);
    return t < -1e8 ? word : cents_to_word(t, clock_hz);
}

/* ---- SID register intercepts ------------------------------------------- */

static int pad_voice(const tinysid_t *t) { return t->pad_target - 1; }   /* -1 when off */

/* Pulse width, waveform + gate and envelope of each voice, and the filter registers. Frequency is not timbre. */
static int is_timbre_reg(int reg)
{
    return (reg < 21 && reg % 7 >= 2) || (reg >= 0x15 && reg <= 0x18);
}

/* A voice is let through untouched while the tune has it on a pure noise waveform (a drum hit): the lock holds the tone of
 * the notes, not the noise of the drums that share their voice. */
static int timbre_passes(const tinysid_t *t, int v)
{
    return !t->timbre_voice[v] || (t->raw[v * 7 + 4] & 0xF0) == 0x80;
}

/* The tune's "hard restart": it writes AD and SR as 0 before a note so the envelope's rate counter is small when the gate
 * comes (the SID's ADSR delay bug otherwise stalls an attack for up to 30 ms, which swallows short notes). Those writes are
 * not part of an instrument and the lock lets them through. */
static int timbre_hard_restart(const tinysid_t *t, int v)
{
    return t->timbre_hr[v];
}

/* While Timbre Lock holds, the tune still sets the pitch, the gate bits and the master volume; the rest is the snapshot. */
static int locked_value(const tinysid_t *t, int reg)
{
    if (reg < 21 && timbre_passes(t, reg / 7)) return t->raw[reg];
    if (reg < 21 && (reg % 7 == 5 || reg % 7 == 6) && timbre_hard_restart(t, reg / 7)) return t->raw[reg];
    if (reg < 21 && reg % 7 == 4) return (t->timbre_snap[reg] & 0xFE) | (t->raw[reg] & 0x01);
    if (reg == 0x18) return (t->timbre_snap[reg] & 0xF0) | (t->raw[reg] & 0x0F);
    return t->timbre_snap[reg];
}

static void apply_reg(tinysid_t *t, int reg);

/* One byte of voice `v`'s frequency word after AutoTune: the word moved by this frame's correction, as a ratio, so the
 * movement of the pitch (vibrato, glides, arpeggios, drum sweeps) is the tune's own. Noise has no pitch. */
static int snapped_reg(const tinysid_t *t, int v, int r)
{
    const uint8_t *g = &t->raw[v * 7];
    double corr = t->at[v].corr;
    if (corr == 0.0 || (g[4] & 0xF0) == 0x80) return g[r];
    unsigned word = (unsigned)(g[0] | (g[1] << 8));
    if (!word) return g[r];
    double w = word * pow(2.0, corr / 1200.0) + 0.5;
    unsigned q = (unsigned)(w < 1.0 ? 1.0 : (w > 65535.0 ? 65535.0 : w));
    return r ? (int)(q >> 8) : (int)(q & 0xFF);
}

static void snap_reapply(tinysid_t *t)
{
    for (int v = 0; v < 3; v++) { apply_reg(t, v * 7); apply_reg(t, v * 7 + 1); }
}

/* Transpose or bend moved: every voice's total correction follows at once. */
static void pitch_offset_changed(tinysid_t *t)
{
    for (int v = 0; v < 3; v++) t->at[v].corr = t->at[v].snap_corr + pitch_offset(t);
    snap_reapply(t);
}

/* End of a play() frame: remembers the instrument each voice is sounding, for Timbre Lock. A real instrument has a gate, a
 * tonal waveform and an envelope: the frames where a driver hard-restarts a voice (AD/SR written as 0) and the noise of a
 * drum hit are not what a lock should hold. */
static void timbre_track(tinysid_t *t)
{
    if (t->is_analysis) return;
    for (int v = 0; v < 3; v++) {
        const uint8_t *g = &t->raw[v * 7];
        if (!(g[4] & 1) || !(g[4] & 0x70) || !(g[5] | g[6])) continue;
        t->tl_inst[v].ctrl = g[4] & 0xF6;                /* waveform, ring and sync: not the gate, not the test bit */
        t->tl_inst[v].ad = g[5];
        t->tl_inst[v].sr = g[6];
        t->tl_inst[v].pw = (uint16_t)(g[2] | ((g[3] & 0x0F) << 8));
        t->tl_inst[v].valid = 1;
    }
}

/* The tune's own tuning against the A = 440 Hz grid, in cents to take off (never more than half a semitone): the circular
 * mean of every tonal note's error from its nearest semitone, so an error of +40 and one of -60 are the same 40 cents sharp.
 * A tune tuned for the other video standard's clock, or by ear, sits a constant distance from the grid; taking that distance
 * off every note moves nothing relative to the tune itself, and never moves a note to the semitone beside it. Notes
 * measured on the file's own clock, whatever the sync mode. Notes that do not agree (no steady tuning) give 0. */
static double tuning_offset(const tinysid_t *t)
{
    if (t->tune_n < SNAP_TUNING_FRAMES) return 0.0;
    int spread = 0;
    for (int i = 0; i < 2; i++) for (uint64_t b = t->tune_notes[i]; b; b &= b - 1) spread++;
    if (spread < SNAP_TUNING_NOTES) return 0.0;
    double conc = sqrt(t->tune_cos * t->tune_cos + t->tune_sin * t->tune_sin) / t->tune_n;
    if (conc < SNAP_TUNING_CONC) return 0.0;
    return atan2(t->tune_sin, t->tune_cos) * 100.0 / 6.283185307179586;
}

/* End of a play() frame: learns the tune's tuning, then works out each voice's correction in cents.
 *   - every voice gets the tune's own offset taken off (and the master tune put on), whatever it is doing, so leaps,
 *     arpeggios, glides, sweeps and vibrato keep exactly their shape;
 *   - a voice that has held a note (moved less than 40 cents a frame for 3 frames) is also drawn onto the nearest note of the
 *     grid (or the key): the smoothed centre of the pitch is moved, not the pitch, so a vibrato stays a vibrato;
 *   - a note stays on its note until it is clearly (65 cents) away from it, so a pitch near the middle between two notes
 *     does not flip between them. */
static void snap_frame(tinysid_t *t)
{
    if (t->is_analysis) return;
    for (int v = 0; v < 3; v++) {                        /* learn the tuning from the tonal notes that are sounding */
        const uint8_t *g = &t->raw[v * 7];
        unsigned word = (unsigned)(g[0] | (g[1] << 8));
        if (!word || !(g[4] & 1) || !(g[4] & 0x70)) continue;
        double hz = word * t->clock_hz / 16777216.0, m = midi_cents(word, t->clock_hz);
        if (hz < 30.0 || hz > 4200.0) continue;
        double err = (m - floor(m / 100.0 + 0.5) * 100.0) * 6.283185307179586 / 100.0;
        t->tune_cos += cos(err);
        t->tune_sin += sin(err);
        int note = (int)floor(m / 100.0 + 0.5);
        if (note >= 0 && note < 128) t->tune_notes[note >> 6] |= (uint64_t)1 << (note & 63);
        if (++t->tune_n > 4000) { t->tune_cos *= 0.5; t->tune_sin *= 0.5; t->tune_n /= 2; }
    }
    double shift = t->master_tune - tuning_offset(t);
    for (int v = 0; v < 3; v++) {
        const uint8_t *g = &t->raw[v * 7];
        unsigned word = (unsigned)(g[0] | (g[1] << 8));
        __typeof__(t->at[0]) *a = &t->at[v];
        if (!word || (g[4] & 0xF0) == 0x80) {            /* no pitch: nothing to hold on to */
            a->has_last = a->steady = a->has_target = 0;
            a->fine = a->snap_corr = 0.0;
            a->corr = pitch_offset(t);
            continue;
        }
        double m = midi_cents(word, t->clock_hz);
        if (a->has_last) a->fine -= shift - a->last_shift;   /* the total correction stays continuous when the tuning estimate moves */
        a->last_shift = shift;
        if (!a->has_last || fabs(m - a->last_m) > SNAP_STEADY_CENTS || fabs(m - a->center) > SNAP_STEADY_CENTS) {   /* a new note, a leap, a sweep or a slide */
            a->steady = 0;
            a->fine = 0.0;
            a->has_target = 0;
            a->center = m;
        } else {
            a->steady++;
            double d = m - a->center;
            a->center = fabs(d) < 0.5 ? m : a->center + d * 0.12;         /* slow: it averages a vibrato over a few cycles */
        }
        a->has_last = 1;
        a->last_m = m;
        if (t->pitch_snap && a->steady >= SNAP_STEADY_FRAMES) {
            double cb = a->center + shift;                                /* where the held pitch sits once the tune's offset is off */
            if (!a->has_target || fabs(cb - a->target) > SNAP_HYSTERESIS) {
                double n = nearest_note(cb, t->pitch_snap, t->song_key, t->song_scale, t->master_tune);
                a->has_target = n > -1e8;
                a->target = n;
            }
            double want = a->has_target ? a->target - cb : 0.0;
            double d = want - a->fine;
            a->fine = fabs(d) < 0.5 ? want : a->fine + d * 0.6;           /* within half a cent it is exact */
        }
        a->snap_corr = t->pitch_snap ? shift + a->fine : 0.0;
        a->corr = a->snap_corr + pitch_offset(t);
    }
    if (pitch_active(t)) snap_reapply(t);
    pad_retune(t);                                       /* a held pad note follows the tuning as it is learned */
}

/* A tune that does not use the filter (Song mode, the tune's $D418 mode bits all clear) leaves it free for the pad: the pad voice is
 * routed through a low-pass held fully open, so it sounds as it did, Cutoff darkens it and Reso adds a peak. (A voice sent through
 * the filter with no mode selected is silent on a real SID, and a tune that ignores the filter leaves the cutoff at 0, hence the open
 * low-pass.) A tune that does use the filter keeps it: its cutoff sweeps are made for its own voices, and routing the pad into them
 * would shut the pad every time the song closes the filter. To put the knobs in charge of the pad there, pick a Filter mode (Low /
 * Band / High Pass), which owns the whole filter.
 * Returns the voices (a bit mask) that go through the open filter, 0 if it is not open. */
static int filter_open_mask(const tinysid_t *t)
{
    if (t->filter_mode != FILT_SONG) return 0;
    int mode = t->timbre_lock ? locked_value(t, 0x18) : t->raw[0x18];
    if ((mode & 0x70) != 0) return 0;
    return t->pad_latched && pad_voice(t) >= 0 ? 1 << pad_voice(t) : 0;
}

static void apply_filter_regs(tinysid_t *t)
{
    for (int r = 0x15; r <= 0x18; r++) apply_reg(t, r);
}

static void apply_reg(tinysid_t *t, int reg)
{
    int val = t->raw[reg];

    if (t->timbre_lock && is_timbre_reg(reg)) val = locked_value(t, reg);
    if (reg < 21) {
        int v = reg / 7, r = reg % 7;
        if (v == pad_voice(t) && t->pad_latched) {        /* the tune's writes to this voice are suppressed */
            switch (r) {
            case 0: val = t->pad_cur_freq & 0xFF; break;
            case 1: val = t->pad_cur_freq >> 8; break;
            case 2: val = t->pad_cur_pw & 0xFF; break;
            case 3: val = (t->pad_cur_pw >> 8) & 0x0F; break;
            case 4: val = (t->pad_cur_wave & 0xF0) | (t->pad_gate ? 1 : 0); break;   /* never sync, ring or test: each ties the oscillator to a neighbouring voice */
            case 5: val = t->lat_ad; break;
            case 6: val = t->lat_sr; break;
            }
        } else if (r < 2 && pitch_active(t)) {
            val = snapped_reg(t, v, r);
        }
    } else if (reg == 0x15) {
        if (t->filter_mode) val = 0;
    } else if (reg == 0x16) {
        /* Song: the tune's cutoff plus the knob. Override: the knob is the cutoff, centred at 128. */
        if (filter_open_mask(t)) val = 255;
        val = t->filter_mode ? clampi(128 + t->cutoff_bias, 0, 255) : clampi(val + t->cutoff_bias, 0, 255);
    } else if (reg == 0x17) {
        if (t->filter_mode) {
            val = (clampi(4 + t->res_bias, 0, 15) << 4) | 0x07;     /* resonance around 4, all three voices routed */
        } else {
            int res = clampi((val >> 4) + t->res_bias, 0, 15);
            val = (res << 4) | (val & 0x0F);
        }
        val |= filter_open_mask(t);
    } else if (reg == 0x18) {
        if (t->filter_mode) val = (val & 0x0F) | (0x10 << (t->filter_mode - 1));   /* LP / BP / HP, and voice 3 is never cut */
        else if (filter_open_mask(t)) val |= 0x10;
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
static void duck_trigger(tinysid_t *t);
static void pad_retune(tinysid_t *t);
static void pad_takeover(tinysid_t *t);
static void ana_start(tinysid_t *t);
static void update_snoop(tinysid_t *t);
static int snoop_feed(const tinysid_t *t, char *buf, int buf_len);
static int mix_feed(const tinysid_t *t, char *buf, int buf_len);
static void ana_abort(tinysid_t *t);
static void restart_tune(tinysid_t *t);
static void engine_resume(tinysid_t *t);
static void fx_reset(tinysid_t *t);
static void fx_scan_start(tinysid_t *t);

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
/* ---- Phrase Loop ------------------------------------------------------------------------------------------
 *
 * The 6502 only ever runs inside a play() call, so at a clock tick its whole state is RAM, five registers and the
 * shadow copy of the SID registers. Saving those on a bar line and putting them back N bars later makes the tune
 * play the same phrase again, note for note. The frame counter, the cycle count and the SID chip itself are not
 * touched: timing keeps running, and envelopes ring on through the join. */

static void phrase_capture(tinysid_t *t)
{
    memcpy(t->loop_ram, t->ram, sizeof(t->ram));
    memcpy(t->loop_raw, t->raw, sizeof(t->raw));
    t->loop_raster = t->raster;
    t->loop_pc = t->cpu.pc;
    t->loop_a = t->cpu.a; t->loop_x = t->cpu.x; t->loop_y = t->cpu.y; t->loop_sp = t->cpu.sp; t->loop_p = t->cpu.p;
    t->loop_valid = 1;
}

/* The top of every bar is kept, so a capture can start on the bar that is playing. */
static void bar_snap_take(tinysid_t *t, int64_t bar)
{
    memcpy(t->bar_ram, t->ram, sizeof(t->ram));
    memcpy(t->bar_raw, t->raw, sizeof(t->raw));
    t->bar_raster = t->raster;
    t->bar_pc = t->cpu.pc;
    t->bar_a = t->cpu.a; t->bar_x = t->cpu.x; t->bar_y = t->cpu.y; t->bar_sp = t->cpu.sp; t->bar_p = t->cpu.p;
    t->bar_snap_bar = bar;
}

static void phrase_restore(tinysid_t *t)
{
    /* A voice still sounding at the seam, which the loop's first bar has not gated on, must not be cut: putting back the
     * snapshot's control byte (often "no waveform") would silence it at once, and its release rate with it. It gets its
     * gate dropped, so it dies by its own release, and its control byte and sustain / release wait until it has. */
    uint8_t hold[3];
    for (int v = 0; v < 3; v++)
        hold[v] = !(v == pad_voice(t) && t->pad_latched) && t->sid.v[v].env > 0 && !(t->loop_raw[v * 7 + 4] & 1);

    memcpy(t->ram, t->loop_ram, sizeof(t->ram));
    memcpy(t->raw, t->loop_raw, sizeof(t->raw));
    t->raster = t->loop_raster;
    t->cpu.pc = t->loop_pc;
    t->cpu.a = t->loop_a; t->cpu.x = t->loop_x; t->cpu.y = t->loop_y; t->cpu.sp = t->loop_sp; t->cpu.p = t->loop_p;
    t->cpu.jammed = 0;
    for (int r = 0; r < 25; r++) {                       /* the chip agrees with the tune's view of it again */
        int v = r < 21 ? r / 7 : -1;
        if (v >= 0 && hold[v] && (r % 7 == 4 || r % 7 == 6)) continue;
        apply_reg(t, r);
    }
    for (int v = 0; v < 3; v++) {
        t->seam_hold[v] = hold[v];
        if (hold[v]) sid_write(&t->sid, (uint8_t)(v * 7 + 4), (uint8_t)(t->sid.reg[v * 7 + 4] & 0xFE));
    }
    t->loop_rewinds++;
}

/* A held-over voice has finished its release: it takes the tune's control byte and sustain / release back. */
static void seam_release(tinysid_t *t)
{
    for (int v = 0; v < 3; v++)
        if (t->seam_hold[v] && t->sid.v[v].env == 0) {
            t->seam_hold[v] = 0;
            apply_reg(t, v * 7 + 4);
            apply_reg(t, v * 7 + 6);
        }
}

static int lock_engaged(const tinysid_t *t);

/* Ticks in one bar of the TUNE: 16 rows. A row is 6 host ticks at Host Lock 1x, 12 at 0.5x, 3 at 2x, so the bar the loop
 * measures follows (96, 192, 48). Without a lock (free modes, a row length not known, Speed off 1.0) the tune's rows are
 * not tied to the clock and the bar is the host's. */
static int loop_bar_ticks(const tinysid_t *t)
{
    if (!lock_engaged(t)) return LOOP_BAR_TICKS;
    return t->sync_mode == SYNC_HOST_HALF ? 2 * LOOP_BAR_TICKS : (t->sync_mode == SYNC_HOST_2X ? LOOP_BAR_TICKS / 2 : LOOP_BAR_TICKS);
}

/* Runs on every counted tick. Song time counts ticks, a bar of the tune's 16 rows (96 at Host Lock 1x), and bar 1 starts
 * `loop_nudge` rows after the first tick after Start (an intro pickup shifts the tune's own bar lines). The
 * top of bar `loop_start_bar` catches the phrase (whether or not the loop is on yet, so it can be switched on later and
 * jump back); with the loop on, the top of bar `loop_start_bar + loop_bars` puts it back and song time with it. A start
 * bar the song has already passed is never caught: it waits for the next run. */
static void bar_loop_tick(tinysid_t *t)
{
    t->song_pos++;
    if (!t->loaded || t->is_analysis) return;
    int len = loop_bar_ticks(t);
    if (len != t->loop_len_ticks) {                      /* the sync mode changed: a snapshot belongs to the grid it was taken on */
        t->loop_len_ticks = len;
        t->loop_valid = 0;
    }
    int64_t off = (int64_t)t->loop_nudge * (len / 16);   /* the nudge is in rows: a sixteenth of a bar */
    int64_t pos = t->song_pos - off;
    if (pos < 0 || pos % len != 0) return;
    int64_t bar = pos / len + 1;
    if (!t->loop_valid) {
        if (bar == t->loop_start_bar) phrase_capture(t);
    } else if (t->loop_bars && bar >= t->loop_start_bar + t->loop_bars) {
        phrase_restore(t);
        t->song_pos = (int64_t)(t->loop_start_bar - 1) * len + off;
        bar = t->loop_start_bar;
    }
    bar_snap_take(t, bar);                               /* the bar that is beginning now, as it begins */
}

/* Loop capture (a pad of the module's screen in Control mode, or loop_capture): the first press starts a loop on the bar that is
 * playing (from the copy of its top), the second ends it on the bar playing then. Loop Start and Loop Len take the result; a
 * capture longer than 8 bars keeps its first 8. It needs the host's clock running, as the looper does: bars are counted from Start. */
static void loop_capture_start(tinysid_t *t)
{
    int bar = tinysid_song_bar(t);
    if (bar < 1 || t->bar_snap_bar != bar) return;       /* no clock, or the bar's top was not seen (a moment after Start) */
    memcpy(t->loop_ram, t->bar_ram, sizeof(t->ram));
    memcpy(t->loop_raw, t->bar_raw, sizeof(t->raw));
    t->loop_raster = t->bar_raster;
    t->loop_pc = t->bar_pc;
    t->loop_a = t->bar_a; t->loop_x = t->bar_x; t->loop_y = t->bar_y; t->loop_sp = t->bar_sp; t->loop_p = t->bar_p;
    t->loop_valid = 1;
    t->loop_len_ticks = loop_bar_ticks(t);
    t->loop_start_bar = clampi(bar, 1, LOOP_START_MAX);
    t->loop_bars = 0;                                    /* the song plays on while it is captured (an old loop stops) */
    t->cap_start = t->loop_start_bar;
    t->cap_recording = 1;
}

static void loop_capture_stop(tinysid_t *t)
{
    if (!t->cap_recording) return;
    t->cap_recording = 0;
    int bar = tinysid_song_bar(t);
    if (!t->loop_valid || bar < t->cap_start) return;    /* the snapshot went (a new grid), or the song went back */
    t->loop_bars = clampi(bar - t->cap_start + 1, 1, 8);
}

static void host_clock_tick(tinysid_t *t)
{
    uint64_t now = t->sample_clock;
    if (++t->duck_ticks % DUCK_TICKS == 0) duck_trigger(t);          /* a quarter note: the kick of four on the floor */
    t->last_tick_sample = now + 1;
    if (t->lock_lost) {                                  /* the clock is back: carry on from where the tune paused */
        t->lock_lost = 0;
        if (t->transport_running) engine_resume(t);
    }
    if (t->playing) {                                    /* ticks are counted from the start; a paused tune keeps its place */
        t->clk_ticks++;
        t->since_tick = 0.0;
        bar_loop_tick(t);
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
            if (reg < 21 && reg % 7 == 4 && (v & 1) && !(t->raw[reg] & 1)) t->mout_rise[reg / 7] = 1;
            int mode_was_open = reg == 0x18 && filter_open_mask(t) != 0;
            if (t->is_analysis && reg < 21 && reg % 7 == 4 && (v & 1) && !(t->raw[reg] & 1)) t->sig_rise[reg / 7] = 1;
            int was_passing = reg < 21 && reg % 7 == 4 && t->timbre_lock && timbre_passes(t, reg / 7);
            int was_restart = reg < 21 && (reg % 7 == 5 || reg % 7 == 6) && t->timbre_lock && timbre_hard_restart(t, reg / 7);
            t->raw[reg] = v;
            if (reg < 21 && (reg % 7 == 5 || reg % 7 == 6)) {
                int base = reg - reg % 7;                /* written zeros: not the all-zero registers a tune starts with */
                t->timbre_hr[reg / 7] = t->raw[base + 5] == 0 && t->raw[base + 6] == 0;
            }
            if (reg < 21 && (reg % 7 == 4 || reg % 7 == 6)) t->seam_hold[reg / 7] = 0;    /* the song is driving the voice again */
            apply_reg(t, reg);
            if (reg < 21 && (reg % 7 == 5 || reg % 7 == 6) && t->timbre_lock && was_restart != timbre_hard_restart(t, reg / 7)) {
                int base = reg - reg % 7;                /* the voice entered or left a hard restart: AD and SR move together */
                apply_reg(t, base + 5); apply_reg(t, base + 6);
            }
            if (reg < 21 && reg % 7 == 4 && t->timbre_lock && was_passing != timbre_passes(t, reg / 7)) {
                int base = reg - 4;                      /* a drum hit began or ended: the voice's envelope and width follow */
                apply_reg(t, base + 2); apply_reg(t, base + 3); apply_reg(t, base + 5); apply_reg(t, base + 6);
            }
            if (reg < 21 && reg % 7 < 2 && pitch_active(t)) apply_reg(t, reg - reg % 7 + 1 - reg % 7);   /* a snapped word spans both bytes */
            if (reg == 0x18 && mode_was_open != (filter_open_mask(t) != 0)) apply_filter_regs(t);   /* the tune started or stopped using the filter */
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

/* ---- notes out ------------------------------------------------------------------------------------------
 *
 * The voices you choose as MIDI, so other instruments can play what the SID plays, out of tune in exactly the same way. The note is
 * the nearest semitone to the pitch the chip is playing (after AutoTune, transpose and bend, so it is what is heard), and the
 * rest, up to half a semitone and any vibrato or glide, rides on pitch bend, assuming the receiver's bend range is +-2 semitones.
 * A pitch that moves further than that from its note is a new note. Noise and the pad's voice send nothing. The voices are sent
 * whether or not they are muted in the audio, so the two can be muted apart. Each voice has a channel of its own (1, 2 and 3 to
 * start with): pitch bend is per channel, so voices sharing one would bend each other. */

static int mout_channel(const tinysid_t *t, int v) { return t->mout_ch[v] - 1; }

static void mout_push(tinysid_t *t, int v, int type, int note, int value)
{
    if (type == 2) {                                      /* a bend still waiting on this channel is out of date: take its place */
        int ch = mout_channel(t, v);
        for (int k = t->mout_n - 1; k >= 0; k--) {
            int j = (t->mout_head + k) % MOUT_Q;
            if (t->mout_q[j].channel != ch) continue;
            if (t->mout_q[j].type != 2) break;            /* a note after it: the bend must stay in order with it */
            t->mout_q[j].value = (uint16_t)value;
            return;
        }
    }
    if (t->mout_n > t->mout_max) t->mout_max = t->mout_n;
    if (t->mout_n >= MOUT_Q) {
        if (type == 2) return;                                    /* a bend can be dropped; a note cannot */
        t->mout_head = (t->mout_head + 1) % MOUT_Q;               /* drop the oldest instead */
        t->mout_n--;
    }
    int i = (t->mout_head + t->mout_n++) % MOUT_Q;
    t->mout_q[i].type = (uint8_t)type;
    t->mout_q[i].note = (uint8_t)note;
    t->mout_q[i].channel = (uint8_t)mout_channel(t, v);          /* the channel it is sent on is the one in force now */
    t->mout_q[i].value = (uint16_t)value;
}

static int mout_bend_value(double cents)
{
    int v = 8192 + (int)lround(cents / MOUT_BEND_RANGE * 8192.0);
    return v < 0 ? 0 : v > 16383 ? 16383 : v;
}

/* Lets go of a voice's sounding note, and puts its bend back in the middle. */
static void mout_release_voice(tinysid_t *t, int v)
{
    if (t->mout_note[v] < 0) return;
    mout_push(t, v, 0, t->mout_note[v], 0);
    if (t->mout_bend_cents[v] != 0.0) mout_push(t, v, 2, 0, 8192);
    t->mout_note[v] = -1;
    t->mout_bend_cents[v] = 0.0;
}

static void mout_release(tinysid_t *t)
{
    for (int v = 0; v < 3; v++) mout_release_voice(t, v);
}

static void mout_start(tinysid_t *t, int v, int note, double total)
{
    t->mout_bend_cents[v] = total - note * 100.0;
    mout_push(t, v, 2, 0, mout_bend_value(t->mout_bend_cents[v]));     /* the bend first, so the note starts at the right pitch */
    mout_push(t, v, 1, note, MOUT_VELOCITY);
    t->mout_note[v] = note;
}

static void mout_frame(tinysid_t *t)
{
    if (t->is_analysis || t->is_fx) return;
    for (int v = 0; v < 3; v++) {
        if (!((t->mout_mask >> v) & 1)) continue;
        uint8_t ctrl = t->raw[v * 7 + 4];
        unsigned word = (unsigned)(t->sid.reg[v * 7] | (t->sid.reg[v * 7 + 1] << 8));    /* what the chip is playing */
        double hz = word * t->clock_hz / 16777216.0;
        if (!(ctrl & 1) || !(ctrl & 0x70) || hz < 30.0 || hz > 4200.0 || v == pad_voice(t)) {
            mout_release_voice(t, v);
            continue;
        }
        double total = midi_cents(word, t->clock_hz);
        int note = clampi((int)lround(total / 100.0), 0, 127);
        if (t->mout_note[v] < 0) { mout_start(t, v, note, total); continue; }
        double rel = total - t->mout_note[v] * 100.0;
        if (t->mout_rise[v] || fabs(rel) > MOUT_BEND_RANGE) {      /* a new gate, or too far to bend: a new note */
            mout_release_voice(t, v);
            mout_start(t, v, note, total);
        } else if (fabs(rel - t->mout_bend_cents[v]) >= MOUT_BEND_STEP) {
            t->mout_bend_cents[v] = rel;
            mout_push(t, v, 2, 0, mout_bend_value(rel));
        }
    }
    memset(t->mout_rise, 0, sizeof(t->mout_rise));
}

/* The ducker and the filter pump beat on the host's MIDI clock, which only runs while the host is playing. With no clock arriving
 * they keep the song's own beat instead (a quarter note of its tempo, from the top of the tune), so they work when the SID is
 * simply being played. */
static void song_beat(tinysid_t *t)
{
    if (t->is_analysis || t->is_fx) return;
    if (t->last_tick_sample && t->sample_clock + 1 - t->last_tick_sample < (uint64_t)(BEAT_CLOCK_LIVE_SEC * t->sample_rate)) {
        t->beat_left = 0.0;
        return;
    }
    if (t->beat_left <= 0.0) {
        duck_trigger(t);
        double quarter = t->song_fpr_num > 0 && t->song_fpr_den > 0 ? 4.0 * t->song_fpr_num / t->song_fpr_den
                                                                      : target_hz(t) * 60.0 / (t->song_bpm > 0 ? t->song_bpm : BPM_DEFAULT);
        t->beat_left += quarter < 4.0 ? 4.0 : quarter;
    }
    t->beat_left -= 1.0;
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
    song_beat(t);
    run_routine(t, addr, irq, PLAY_MAX_CYCLES);
    snap_frame(t);
    timbre_track(t);
    t->play_calls++;
    pad_frame_intercept(t);
    mout_frame(t);
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
    t->beat_left = 0.0;
    t->frame_no = 0;
    t->last_gate_frame = 0;
    t->clk_ticks = -1;                                   /* the first clock tick after a start is tick 0: step 1 */
    t->since_tick = 0.0;
    t->lock_off = 0;
    t->lock_aligned = t->lock_lost = 0;
    t->lock_wait = 0.0;

    t->loop_valid = 0;                                   /* a new run has its own bar 1: the phrase is caught again at its start bar */
    t->song_pos = -1;
    memset(t->seam_hold, 0, sizeof(t->seam_hold));

    t->pad_count = 0;
    t->pad_latched = t->pad_gate = 0;
    t->pad_pending = 0;
    for (int v = 0; v < 3; v++) sid_set_voice_gain(&t->sid, v, voice_vol(t, v));
    sid_reset(&t->sid);
    memset(t->raw, 0, sizeof(t->raw));
    memset(t->tl_inst, 0, sizeof(t->tl_inst));
    memset(t->timbre_hr, 0, sizeof(t->timbre_hr));
    memset(t->at, 0, sizeof(t->at));
    for (int v = 0; v < 3; v++) t->at[v].corr = pitch_offset(t);
    t->tune_cos = t->tune_sin = 0.0;
    t->tune_n = 0;
    t->tune_notes[0] = t->tune_notes[1] = 0;
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
    mout_release(t);
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
        sid_set_voice_gain(&t->sid, v, voice_vol(t, v)); /* the next start ramps back up from silence */
    }
}

/* Continues from where the tune stopped. */
static void engine_resume(tinysid_t *t)
{
    if (!t->loaded || t->playing) return;
    t->stop_fade = 0;
    t->playing = 1;
    for (int v = 0; v < 3; v++)
        if (v != pad_voice(t)) sid_set_voice_gain(&t->sid, v, voice_vol(t, v));
}

/* Starts the tune from beat 1. */
static void engine_start(tinysid_t *t)
{
    t->duck_ticks = -1;                                  /* the first clock tick after Start is beat 1 of the host, loaded tune or not */
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

/* ---- audio glue: stereo spread, DC-blocking high-pass, sidechain ducking ------------------------------- */

#define HPF_HZ 35.0                  /* below this the SID has nothing but DC and rumble, which Move's kicks and subs need the room for */
#define DUCK_ATTACK_SEC 0.002        /* the gain drops over this long, so a kick never clicks the output */
#define SPREAD_MAX_PAN 0.7f          /* at full spread voices 2 and 3 sit at +-0.7 of the way to a side */

void tinysid_hpf_init(tinysid_hpf_t *f, double sample_rate, double hz)
{
    f->x1 = f->y1 = 0.0f;
    f->coef = (float)exp(-6.283185307179586 * hz / sample_rate);     /* about 0.995 at 35 Hz and 44.1 kHz */
}

/* y[n] = x[n] - x[n-1] + R y[n-1] */
float tinysid_hpf_run(tinysid_hpf_t *f, float x)
{
    float y = x - f->x1 + f->coef * f->y1;
    f->x1 = x;
    f->y1 = fabsf(y) < 1e-20f ? 0.0f : y;
    return y;
}

/* Voice 1 stays in the middle, voice 2 moves left and voice 3 right as the spread opens. Equal power: the squares of the two
 * gains always add to 2, so a voice does not get louder as it moves, and at spread 0 every voice has gain 1 on both sides
 * (the old mono sound, exactly). */
static void apply_spread(tinysid_t *t)
{
    const float pan[3] = { 0.0f, -SPREAD_MAX_PAN * t->stereo_spread, SPREAD_MAX_PAN * t->stereo_spread };
    for (int v = 0; v < 3; v++) {
        float theta = 0.78539816f * (1.0f + pan[v]);     /* pi/4 is the middle, 0 hard left, pi/2 hard right */
        sid_set_voice_pan(&t->sid, v, 1.41421356f * cosf(theta), 1.41421356f * sinf(theta));
    }
}

static void duck_coef(tinysid_t *t)
{
    double samples = (double)t->duck_release_ms * 0.001 * t->sample_rate;
    t->duck_alpha = (float)exp(-1.0 / (samples < 1.0 ? 1.0 : samples));
}

static void fx_trigger(tinysid_t *t, int idx, int vel);

static void duck_trigger(tinysid_t *t)
{
    if (t->duck_depth > 0.0f) {
        t->duck_goal = 1.0f - t->duck_depth;
        t->duck_atk_left = (int)(DUCK_ATTACK_SEC * t->sample_rate) + 1;
    }
    if (t->pump_amt > 0.0f) t->pump_env = t->pump_amt;   /* the low-pass closes on the same beat, and opens again with the same release */
}

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
    for (int c = 0; c < 2; c++) tinysid_hpf_init(&t->hpf[c], t->sample_rate, HPF_HZ);
    t->stereo_spread = 0.5f;
    t->fx_duck = FX_DUCK_DEFAULT;
    for (int v = 0; v < 3; v++) t->mout_ch[v] = v + 1;   /* voices 1, 2 and 3 to channels 1, 2 and 3: the Move's first three tracks */
    t->mout_note[0] = t->mout_note[1] = t->mout_note[2] = -1;
    t->note_controls = 1;
    t->duck_release_ms = 80.0f;
    t->duck_env = t->duck_goal = 1.0f;
    t->duck_ticks = -1;
    duck_coef(t);
    t->clock_hz = PAL_CLOCK;
    t->tempo_trim = 1.0f;
    t->song_key = KEY_NONE;
    t->song_scale = SCALE_NONE;
    t->snoop_voice = 1;                                  /* Voice 2 (Lead) */
    t->transport_running = 1;
    t->loop_start_bar = 1;
    t->song_pos = -1;
    t->last_clock_status = -1;
    t->host_bpm = HOST_BPM_DEFAULT;
    set_song_bpm(t, BPM_DEFAULT, BPM_SRC_DEFAULT);

    sid_init(&t->sid, PAL_CLOCK, t->sample_rate, SID_MODEL_6581);
    apply_spread(t);
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
    fx_reset(t);
    if (t->dly) tinysid_free(t->dly);
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
    fx_scan_start(t);
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

/* Switching the lock on snapshots what the tune last wrote (not what the chip sounds, so the cutoff / resonance knobs
 * are not baked in). Switching it off hands the registers back to the tune's own values. */
static void set_timbre_lock(tinysid_t *t, int on)
{
    if (on == t->timbre_lock) return;
    if (on) {
        memcpy(t->timbre_snap, t->raw, sizeof(t->timbre_snap));
        for (int v = 0; v < 3; v++) {
            uint8_t *g = &t->timbre_snap[v * 7];
            if (t->tl_inst[v].valid) {                   /* the voice's real instrument, whatever the registers say this instant */
                g[2] = (uint8_t)(t->tl_inst[v].pw & 0xFF);
                g[3] = (uint8_t)(t->tl_inst[v].pw >> 8);
                g[4] = t->tl_inst[v].ctrl;
                g[5] = t->tl_inst[v].ad;
                g[6] = t->tl_inst[v].sr;
                t->timbre_voice[v] = 1;
            } else {                                     /* not heard yet: what is there, if it is a tone with an envelope */
                g[4] &= 0xF6;
                t->timbre_voice[v] = (g[4] & 0x70) && (g[5] | g[6]);
            }
        }
    }
    t->timbre_lock = on;
    for (int r = 0; r < 25; r++)
        if (is_timbre_reg(r)) apply_reg(t, r);
}

static void set_loop_nudge(tinysid_t *t, int rows)
{
    if (rows == t->loop_nudge) return;
    t->loop_nudge = rows;
    t->loop_valid = 0;                                   /* a different grid: the old snapshot is of another bar line */
}

/* The length can change freely and Off keeps the snapshot, so a loop can be switched off and later back on. A new start
 * bar is a different phrase: the old snapshot goes. */
static void set_loop_start(tinysid_t *t, int bar)
{
    if (bar == t->loop_start_bar) return;
    t->loop_start_bar = bar;
    t->loop_valid = 0;
}

/* Is this note-on one of ours coming back into the slot? It is, if an echo of it is still owed: each one is consumed. */
static int echo_take(tinysid_t *t, int note)
{
    if (!t->echo_midi[note]) return 0;
    if (t->sample_clock >= t->echo_until[note]) {        /* the rest are not coming */
        t->echo_midi[note] = 0;
        return 0;
    }
    t->echo_midi[note]--;
    return 1;
}

/* A pad of the module's own screen in Pad Mode Control (hardware pad notes 68..99, bottom-left to top-right). The screen hands every
 * press here, so that the engine, which owns the state, is the one place a press is carried out (the screen shows the result at once
 * and checks it against the engine's state). The layout:
 *   bottom row  68 69 70 mute voice 1 2 3, 71 all on, 72 73 74 transpose down / up / 0, 75 loop capture start / stop
 *   second row  76 77 78 Notes Out for voice 1 2 3
 *   top row     92..99 the sound effects, at the press's velocity */
static void pad_press(tinysid_t *t, int note, int vel)
{
    if (note < 68 || note > 99) return;
    int i = note - 68;
    char b[16];
    if (i < 3) {
        snprintf(b, sizeof(b), "%d", t->mute_mask ^ (1 << i));
        tinysid_set_param(t, "voice_mutes", b);
    } else if (i == 3) {
        tinysid_set_param(t, "voice_mutes", "0");
    } else if (i <= 6) {
        snprintf(b, sizeof(b), "%d", i == 6 ? 0 : clampi(t->transpose + (i == 5 ? 1 : -1), -12, 12));
        tinysid_set_param(t, "transpose", b);
    } else if (i == 7) {
        if (t->cap_recording) loop_capture_stop(t);
        else loop_capture_start(t);
    } else if (i >= 8 && i <= 10) {
        char k[] = "midi_out_v1";
        k[10] = (char)('1' + (i - 8));
        tinysid_set_param(t, k, (t->mout_mask >> (i - 8)) & 1 ? "Off" : "On");
    } else if (i >= 24 && i - 24 < t->fx_n) {
        fx_trigger(t, i - 24, vel);
    } else {
        return;
    }
    t->pad_seq++;
    t->pad_last = note;
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
        sid_set_voice_gain(&t->sid, v, voice_vol(t, v));
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
    } else if (!strcmp(key, "pitch_snap")) {
        if ((e = parse_enum(value, k_snap_names, 3)) < 0) return -1;
        t->pitch_snap = e;
        for (int v = 0; v < 3; v++) { t->at[v].snap_corr = t->at[v].fine = 0.0; t->at[v].corr = pitch_offset(t); }   /* worked out afresh at the next frame */
        snap_reapply(t);                                 /* off puts the tune's own pitch back */
    } else if (!strcmp(key, "transpose")) {
        if (parse_double(value, &d)) return -1;
        t->transpose = clampi((int)lround(clampf((float)d, -1e6f, 1e6f)), -12, 12);
        pitch_offset_changed(t);
    } else if (!strcmp(key, "pad_mode")) {
        if ((e = parse_enum(value, k_padmode_names, 2)) < 0) return -1;
        if (e != t->pad_mode) {                          /* notes held across the switch would never see their note-off */
            t->pad_count = 0;
            if (t->pad_latched) { t->pad_gate = 0; apply_voice(t, pad_voice(t)); }
        }
        t->pad_mode = e;
    } else if (!strcmp(key, "voice_mutes")) {
        if (parse_double(value, &d)) return -1;
        t->mute_mask = clampi((int)lround(clampf((float)d, -1e6f, 1e6f)), 0, 7);
        for (int v = 0; v < 3; v++) if (v != pad_voice(t) || !t->pad_latched) sid_set_voice_gain(&t->sid, v, voice_vol(t, v));
    } else if (!strcmp(key, "filter_pump")) {
        if (parse_double(value, &d)) return -1;
        t->pump_amt = clampf((float)d, 0.0f, 1.0f);
    } else if (!strcmp(key, "lofi")) {
        if (parse_double(value, &d)) return -1;
        t->lofi = clampf((float)d, 0.0f, 1.0f);
    } else if (!strcmp(key, "stereo_spread")) {
        if (parse_double(value, &d)) return -1;
        t->stereo_spread = clampf((float)d, 0.0f, 1.0f);
        apply_spread(t);
    } else if (!strcmp(key, "ducking_depth")) {
        if (parse_double(value, &d)) return -1;
        t->duck_depth = clampf((float)d, 0.0f, 1.0f);
    } else if (!strcmp(key, "ducking_release")) {
        if (parse_double(value, &d)) return -1;
        t->duck_release_ms = clampf((float)d, 20.0f, 300.0f);
        duck_coef(t);
    } else if (!strncmp(key, "midi_out_v", 10) && key[10] >= '1' && key[10] <= '3' && !key[11]) {
        int v = key[10] - '1';
        if ((e = parse_enum(value, k_mout_names, 2)) < 0) return -1;
        if (!e) mout_release_voice(t, v);
        t->mout_mask = e ? t->mout_mask | (1 << v) : t->mout_mask & ~(1 << v);
        t->mout_rise[v] = 0;
    } else if (!strncmp(key, "midi_out_ch", 11) && key[11] >= '1' && key[11] <= '3' && !key[12]) {
        int v = key[11] - '1';
        if (parse_double(value, &d)) return -1;
        int ch = clampi((int)lround(clampf((float)d, -1e6f, 1e6f)), 1, 16);
        if (ch != t->mout_ch[v]) mout_release_voice(t, v);        /* the note off is queued with the channel the note on went to */
        t->mout_ch[v] = ch;
    } else if (!strcmp(key, "fx_duck")) {
        if (parse_double(value, &d)) return -1;
        t->fx_duck = clampf((float)d, 0.0f, 1.0f);
    } else if (!strcmp(key, "audio_delay")) {           /* ms */
        if (parse_double(value, &d)) return -1;
        int ms = clampi((int)lround(clampf((float)d, -1e6f, 1e6f)), 0, AUDIO_DELAY_MAX_MS);
        if (ms > 0 && !t->dly) {
            t->dly = tinysid_malloc(sizeof(float) * 2 * AUDIO_DELAY_FRAMES);
            if (!t->dly) return -1;
            memset(t->dly, 0, sizeof(float) * 2 * AUDIO_DELAY_FRAMES);
        }
        int len = (int)lround(ms * 0.001 * t->sample_rate);
        t->dly_want = len > AUDIO_DELAY_FRAMES ? AUDIO_DELAY_FRAMES : len;
        t->audio_delay_ms = ms;
    } else if (!strcmp(key, "loop_capture")) {          /* "start", "stop", "toggle" or "cancel" */
        if (!strcmp(value, "start")) loop_capture_start(t);
        else if (!strcmp(value, "stop")) loop_capture_stop(t);
        else if (!strcmp(value, "toggle")) { if (t->cap_recording) loop_capture_stop(t); else loop_capture_start(t); }
        else if (!strcmp(value, "cancel")) t->cap_recording = 0;
        else return -1;
    } else if (!strcmp(key, "note_controls")) {          /* 0 / 1: see the field */
        if (parse_double(value, &d)) return -1;
        t->note_controls = d != 0.0;
    } else if (!strcmp(key, "pad_press")) {              /* "<pad note>,<velocity>": see pad_press */
        char *end;
        long note = strtol(value, &end, 10);
        long vel = 100;
        if (end == value) return -1;
        if (*end == ',') vel = strtol(end + 1, NULL, 10);
        pad_press(t, (int)note, (int)(vel < 1 ? 1 : vel > 127 ? 127 : vel));
    } else if (!strcmp(key, "fx_trigger")) {             /* "<effect>" or "<effect>,<velocity>", effects counted from 0 */
        char *end;
        long idx = strtol(value, &end, 10);
        long vel = 127;
        if (end == value) return -1;
        if (*end == ',') vel = strtol(end + 1, NULL, 10);
        fx_trigger(t, (int)idx, (int)vel);
    } else if (!strcmp(key, "master_tune")) {
        if (parse_double(value, &d)) return -1;
        t->master_tune = clampi((int)lround(clampf((float)d, -1e6f, 1e6f)), -100, 100);
        if (t->pitch_snap) {                             /* a new grid: start the voices' pull again from the tune's own pitch */
            for (int v = 0; v < 3; v++) { t->at[v].has_target = 0; t->at[v].fine = 0.0; }
        }
    } else if (!strcmp(key, "timbre_lock")) {
        if ((e = parse_enum(value, k_lock_names, 2)) < 0) return -1;
        set_timbre_lock(t, e);
    } else if (!strcmp(key, "loop_start")) {
        if (parse_double(value, &d)) return -1;
        set_loop_start(t, clampi((int)lround(clampf((float)d, -1e6f, 1e6f)), 1, LOOP_START_MAX));
    } else if (!strcmp(key, "loop_nudge")) {
        if (parse_double(value, &d)) return -1;
        set_loop_nudge(t, clampi((int)lround(clampf((float)d, -1e6f, 1e6f)), 0, LOOP_NUDGE_MAX));
    } else if (!strcmp(key, "loop_length")) {
        if ((e = parse_enum(value, k_looplen_names, LOOP_LEN_COUNT)) < 0) return -1;
        t->loop_bars = k_loop_bars[e];
    } else if (!strcmp(key, "snoop_voice")) {
        if ((e = parse_enum(value, k_snoop_names, 4)) < 0) return -1;
        t->snoop_voice = e;
        update_snoop(t);
    } else if (!strcmp(key, "snoop_note") || !strcmp(key, "snoop_hz") || !strcmp(key, "snoop_cents") || !strcmp(key, "snoop_level") ||
               !strcmp(key, "snoop_feed") || !strcmp(key, "mix_feed") || !strcmp(key, "last_note") || !strcmp(key, "song_key") || !strcmp(key, "song_scale")) {
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
    else if (!strcmp(key, "song_bpm")) n = snprintf(buf, (size_t)buf_len, "%d", t->song_bpm_src == BPM_SRC_DEFAULT ? 0 : t->song_bpm);   /* 0 = not known (yet) */
    else if (!strcmp(key, "host_bpm")) n = snprintf(buf, (size_t)buf_len, "%.1f", t->host_bpm);
    else if (!strcmp(key, "out_trim")) n = snprintf(buf, (size_t)buf_len, "%g", (double)t->trim_db);
    else if (!strcmp(key, "chip_model")) n = snprintf(buf, (size_t)buf_len, "%s", k_model_names[t->model]);
    else if (!strcmp(key, "cutoff_bias")) n = snprintf(buf, (size_t)buf_len, "%d", t->cutoff_bias);
    else if (!strcmp(key, "res_bias")) n = snprintf(buf, (size_t)buf_len, "%d", t->res_bias);
    else if (!strcmp(key, "pad_target")) n = snprintf(buf, (size_t)buf_len, "%s", k_pad_names[t->pad_target]);
    else if (!strcmp(key, "filter_mode")) n = snprintf(buf, (size_t)buf_len, "%s", k_filter_names[t->filter_mode]);
    else if (!strcmp(key, "song_key")) n = snprintf(buf, (size_t)buf_len, "%s", t->song_key == KEY_NONE ? k_keyscale_names[24] : k_keyscale_names[shifted_key(t) * 2 + (t->song_scale == SCALE_MINOR ? 1 : 0)]);
    else if (!strcmp(key, "pitch_snap")) n = snprintf(buf, (size_t)buf_len, "%s", k_snap_names[t->pitch_snap]);
    else if (!strcmp(key, "master_tune")) n = snprintf(buf, (size_t)buf_len, "%d", t->master_tune);
    else if (!strcmp(key, "transpose")) n = snprintf(buf, (size_t)buf_len, "%d", t->transpose);
    else if (!strcmp(key, "pad_mode")) n = snprintf(buf, (size_t)buf_len, "%s", k_padmode_names[t->pad_mode]);
    else if (!strcmp(key, "voice_mutes")) n = snprintf(buf, (size_t)buf_len, "%d", t->mute_mask);
    else if (!strcmp(key, "last_note")) n = snprintf(buf, (size_t)buf_len, "%d", t->last_note);
    else if (!strcmp(key, "filter_pump")) n = snprintf(buf, (size_t)buf_len, "%g", (double)t->pump_amt);
    else if (!strcmp(key, "lofi")) n = snprintf(buf, (size_t)buf_len, "%g", (double)t->lofi);
    else if (!strcmp(key, "mix_feed")) n = mix_feed(t, buf, buf_len);
    else if (!strcmp(key, "stereo_spread")) n = snprintf(buf, (size_t)buf_len, "%g", (double)t->stereo_spread);
    else if (!strcmp(key, "ducking_depth")) n = snprintf(buf, (size_t)buf_len, "%g", (double)t->duck_depth);
    else if (!strcmp(key, "ducking_release")) n = snprintf(buf, (size_t)buf_len, "%g", (double)t->duck_release_ms);
    else if (!strncmp(key, "midi_out_v", 10) && key[10] >= '1' && key[10] <= '3' && !key[11])
        n = snprintf(buf, (size_t)buf_len, "%s", k_mout_names[(t->mout_mask >> (key[10] - '1')) & 1]);
    else if (!strncmp(key, "midi_out_ch", 11) && key[11] >= '1' && key[11] <= '3' && !key[12])
        n = snprintf(buf, (size_t)buf_len, "%d", t->mout_ch[key[11] - '1']);
    else if (!strcmp(key, "pad_ack")) n = snprintf(buf, (size_t)buf_len, "%u,%d,%u", (unsigned)t->pad_seq, t->pad_last, (unsigned)t->pad_echoes);
    else if (!strcmp(key, "audio_delay")) n = snprintf(buf, (size_t)buf_len, "%d", t->audio_delay_ms);
    else if (!strcmp(key, "loop_capture"))
        n = snprintf(buf, (size_t)buf_len, "%s", t->cap_recording ? "recording" : (t->loop_valid && t->loop_bars ? "looping" : "idle"));
    else if (!strcmp(key, "note_controls")) n = snprintf(buf, (size_t)buf_len, "%d", t->note_controls);
    else if (!strcmp(key, "midi_echoes")) n = snprintf(buf, (size_t)buf_len, "%u", (unsigned)t->midi_echoes);
    else if (!strcmp(key, "mout_echo")) {                 /* the notes sent lately, on any channel, comma separated (see tinysid.h) */
        n = 0;
        buf[0] = '\0';
        for (int note = 0; note < 128 && n < buf_len - 5; note++) {
            uint64_t at = t->mout_echo_at[note];
            if (at && t->sample_clock + 1 - at < (uint64_t)(MOUT_ECHO_SEC * t->sample_rate))
                n += snprintf(buf + n, (size_t)(buf_len - n), "%s%d", n ? "," : "", note);
        }
    }
    else if (!strcmp(key, "fx_duck")) n = snprintf(buf, (size_t)buf_len, "%g", (double)t->fx_duck);
    else if (!strcmp(key, "fx_count")) n = snprintf(buf, (size_t)buf_len, "%d", t->fx_n);
    else if (!strcmp(key, "fx_ready")) n = snprintf(buf, (size_t)buf_len, "%d", t->fxs_done ? 1 : 0);
    else if (!strcmp(key, "fx_list")) {                  /* the subtune of each effect pad, comma separated */
        n = 0;
        buf[0] = '\0';
        for (int i = 0; i < t->fx_n && n < buf_len; i++) n += snprintf(buf + n, (size_t)(buf_len - n), "%s%d", i ? "," : "", t->fx_sub[i]);
    }
    else if (!strcmp(key, "timbre_lock")) n = snprintf(buf, (size_t)buf_len, "%s", k_lock_names[t->timbre_lock]);
    else if (!strcmp(key, "loop_start")) n = snprintf(buf, (size_t)buf_len, "%d", t->loop_start_bar);
    else if (!strcmp(key, "loop_nudge")) n = snprintf(buf, (size_t)buf_len, "%d", t->loop_nudge);
    else if (!strcmp(key, "loop_length")) {
        int i = 0;
        while (k_loop_bars[i] != t->loop_bars) i++;
        n = snprintf(buf, (size_t)buf_len, "%s", k_looplen_names[i]);
    }
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
        t->lat_sr = (uint8_t)((in->sr & 0xF0) | ((in->sr & 0x0F) > PAD_MAX_RELEASE ? PAD_MAX_RELEASE : (in->sr & 0x0F)));
        t->lat_pw = in->pw;
        t->pad_glen = in->glen;
        memcpy(t->pad_gpw, in->gpw, sizeof(t->pad_gpw));
        memcpy(t->pad_gratio, in->gratio, sizeof(t->pad_gratio));
        memcpy(t->pad_gw, in->gwave, sizeof(t->pad_gw));
        int lo = 0, hi = 0;                              /* vibrato and trills stay; an arpeggio or a swoop of more than a semitone does not */
        for (int k = 0; k < in->glen; k++) {
            if (in->gratio[k] < lo) lo = in->gratio[k];
            if (in->gratio[k] > hi) hi = in->gratio[k];
        }
        double range = 1200.0 * (log2(1.0 + hi / 16384.0) - log2(1.0 + (lo < -16000 ? -16000 : lo) / 16384.0));
        if (range > PAD_MAX_BEND_CENTS) memset(t->pad_gratio, 0, sizeof(t->pad_gratio));
    }
    pad_mod_apply(t);
}

int tinysid_pad_options(const tinysid_t *t, char *buf, int buf_len)
{
    if (!buf || buf_len < 10) return -1;
    int n = 0;
    buf[n++] = '[';
    for (int i = 0; i < pad_sound_count(t); i++) {
        char name[40], item[64];
        pad_sound_name(t, i, name, sizeof(name));
        int len = snprintf(item, sizeof(item), "%s\"%s\"", i ? "," : "", name);
        if (len < 0 || len >= (int)sizeof(item) || n + len + 2 > buf_len) break;      /* whole names only: the list ends where the room does */
        memcpy(buf + n, item, (size_t)len);
        n += len;
    }
    if (n == 1) n += snprintf(buf + n, (size_t)(buf_len - n), "\"Lead\"");
    buf[n++] = ']';
    buf[n] = '\0';
    return n;
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
    apply_filter_regs(t);                                /* route the voice through the filter */
}

/* Gives the voice back to the 6502 by replaying its shadow registers onto the chip. */
static void pad_unlatch(tinysid_t *t, int voice)
{
    t->pad_latched = t->pad_gate = 0;
    t->pad_pending = 0;
    for (int v = 0; v < 3; v++) sid_set_voice_gain(&t->sid, v, voice_vol(t, v));   /* drop pad velocity scaling */
    apply_voice(t, voice);
    apply_filter_regs(t);                                /* back to the tune's own filter */
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
    t->pad_freq = pad_note_word(t, note);
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
        t->pad_freq = pad_note_word(t, t->active_note);
        pad_mod_apply(t);
    } else {
        t->pad_gate = 0;                                 /* the release plays out; the voice stays the keyboard's */
    }
    apply_voice(t, pad_voice(t));
}

static void pad_retune(tinysid_t *t)
{
    if (!t->pad_latched || t->pad_count == 0) return;
    uint16_t w = pad_note_word(t, t->active_note);
    if (w == t->pad_freq) return;
    t->pad_freq = w;
    pad_mod_apply(t);
    apply_voice(t, pad_voice(t));
}

/* Pad Mode Control: the pads from CONTROL_BASE_NOTE up are buttons.
 *   0 1 2   mute / unmute voice 1, 2, 3        3  unmute all
 *   4 5 6   transpose down / up / back to 0 */
static void control_action(tinysid_t *t, int n)
{
    switch (n) {
    case 0: case 1: case 2:
        t->mute_mask ^= 1 << n;
        if (n != pad_voice(t) || !t->pad_latched) sid_set_voice_gain(&t->sid, n, voice_vol(t, n));
        break;
    case 3:
        t->mute_mask = 0;
        for (int v = 0; v < 3; v++) if (v != pad_voice(t) || !t->pad_latched) sid_set_voice_gain(&t->sid, v, voice_vol(t, v));
        break;
    case 4: case 5: case 6:
        t->transpose = n == 6 ? 0 : clampi(t->transpose + (n == 5 ? 1 : -1), -12, 12);
        pitch_offset_changed(t);
        break;
    default: break;
    }
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
    if (kind == 0xE0) {                                  /* pitch bend: +-2 semitones across the whole song */
        double bend = (((msg[2] & 0x7F) << 7 | (msg[1] & 0x7F)) - 8192) * (200.0 / 8192.0);
        if (bend != t->bend_cents) { t->bend_cents = bend; pitch_offset_changed(t); }
        return;
    }
    int on = kind == 0x90 && (msg[2] & 0x7F);
    /* Notes Out's own notes come back: the host routes them by channel to any slot listening there, this one included, and not
     * always on the channel they left on. Each note we handed to the host is owed back once (within 0.3 s): the first note-on for it
     * while it is owed is that note, and so is its note-off; neither is played, nor taken for a button. */
    if (on) {
        if (echo_take(t, note)) {
            t->echo_held[note] = 1;
            t->midi_echoes++;
            return;
        }
    } else if ((kind == 0x80 || kind == 0x90) && t->echo_held[note]) {
        t->echo_held[note] = 0;
        return;
    }
    if (on) t->last_note = note;
    if (t->pad_mode == PADMODE_CONTROL && (kind == 0x90 || kind == 0x80)) {      /* the pads are buttons: no notes, and no kick */
        if (on && t->note_controls) control_action(t, note - CONTROL_BASE_NOTE);
        return;
    }
    if (on && note == 36) duck_trigger(t);               /* a kick */
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
    int key = t->song_key == KEY_NONE ? 24 : shifted_key(t) * 2 + (t->song_scale == SCALE_MINOR ? 1 : 0);
    int n = snprintf(buf, (size_t)buf_len, "K,%d,%d,%d,%d,%d", t->snoop_note, t->snoop_voice, key, t->snoop_cents, t->snoop_level);
    uint64_t window = (uint64_t)(SNOOP_TRAIL_MS * 0.001 * t->sample_rate);
    for (int i = 0; i < t->snoop_recent_n && n < buf_len; i++) {
        if (t->sample_clock - t->snoop_recent[i].at > window) break;
        if (t->snoop_recent[i].note == t->snoop_note) continue;
        n += snprintf(buf + n, (size_t)(buf_len - n), ",%d", t->snoop_recent[i].note);
    }
    return n >= buf_len ? buf_len - 1 : n;
}

/* The three voices' levels (0..100, 0 while muted) and which are muted, for the meters on the piano roll page:
 *   M,<level 1>,<level 2>,<level 3>,<mute mask>,<transpose> */
static int mix_feed(const tinysid_t *t, char *buf, int buf_len)
{
    int lv[3];
    for (int v = 0; v < 3; v++) lv[v] = (t->mute_mask >> v) & 1 ? 0 : (int)lround(t->sid.v[v].env * 100.0 / 255.0);
    int n = snprintf(buf, (size_t)buf_len, "M,%d,%d,%d,%d,%d", lv[0], lv[1], lv[2], t->mute_mask, t->transpose);
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
    if (t->is_analysis || t->is_fx) return;
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
    for (int v = 0; v < 3; v++) t->at[v].has_target = 0;            /* In-Key was waiting for this: pick the notes again */
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

/* ---- sound effects ------------------------------------------------------------------------------------
 *
 * Many tunes carry more than the song: the other subtunes are jingles and effects (a shot, a pickup, a death sting). The player
 * has one 6502 and one chip, and starting another subtune restarts it, so an effect cannot share the song's engine. A silent
 * scan listens to every subtune on a second engine (like the key scan, a few frames per audio block) and keeps the ones that
 * sound and then stop; a hit plays one on a spare engine of its own that is mixed over the song, and the song ducks for it. */

/* ---- the output delay ------------------------------------------------------------------------------------
 *
 * The tracks Notes Out plays sound some way after TinySID's own sound (the notes go through the Move's MIDI input and the tracks' own
 * instruments). While a voice is being sent, the whole output waits audio_delay ms so they line up; with nothing sent there is no
 * delay at all. All three voices wait together: delaying only the voices being sent would put the tune out of time with itself.
 * Switching it in or out fades the output out, lets the new delay line fill (a gap as long as the delay), and fades back in. */
static void audio_delay_run(tinysid_t *t, float *left, float *right, int n)
{
    int want = (t->mout_mask && t->dly) ? t->dly_want : 0;
    if (!t->dly_n && !want && !t->dly_fade_out && !t->dly_wait && !t->dly_fade_in) return;     /* the common case: nothing to do */
    const int fade = (int)(AUDIO_DELAY_FADE_SEC * t->sample_rate) + 1;
    for (int i = 0; i < n; i++) {
        if (want != t->dly_n && !t->dly_fade_out && !t->dly_wait && !t->dly_fade_in) t->dly_fade_out = fade;
        float l = left[i], r = right[i];
        if (t->dly_n > 0) {
            float *dl = t->dly, *dr = t->dly + AUDIO_DELAY_FRAMES;
            float ol = dl[t->dly_pos], orr = dr[t->dly_pos];
            dl[t->dly_pos] = l;
            dr[t->dly_pos] = r;
            l = ol;
            r = orr;
            if (++t->dly_pos >= t->dly_n) t->dly_pos = 0;
        }
        float g = 1.0f;
        if (t->dly_fade_out > 0) {
            g = (float)t->dly_fade_out / (float)fade;
            if (--t->dly_fade_out == 0) {                /* faded out: the new length, from an empty line */
                t->dly_n = want;
                t->dly_pos = 0;
                memset(t->dly, 0, sizeof(float) * 2 * AUDIO_DELAY_FRAMES);
                t->dly_wait = want;
                t->dly_fade_in = fade;
            }
        } else if (t->dly_wait > 0) {
            g = 0.0f;
            t->dly_wait--;
        } else if (t->dly_fade_in > 0) {
            g = 1.0f - (float)t->dly_fade_in / (float)fade;
            t->dly_fade_in--;
        }
        left[i] = l * g;
        right[i] = r * g;
    }
}

static void fx_reset(tinysid_t *t)
{
    if (t->fxs) { tinysid_destroy(t->fxs); t->fxs = NULL; }
    for (int s = 0; s < FX_SLOTS; s++) {
        if (t->fx_eng[s]) { tinysid_destroy(t->fx_eng[s]); t->fx_eng[s] = NULL; }
        t->fx_left[s] = 0;
    }
    t->fx_n = 0;
    t->fxs_sub = 1;
    t->fxs_frame = 0;
    t->fxs_total = 0;
    t->fxs_done = 0;
}

static void fx_scan_start(tinysid_t *t)
{
    if (t->is_analysis || t->is_fx) return;
    fx_reset(t);
    if (!t->loaded) t->fxs_done = 1;
}

/* One play() frame of the subtune being listened to: when voices sounded, as gate and a waveform on any of them. */
static void fxs_frame_step(tinysid_t *t)
{
    tinysid_t *a = t->fxs;
    do_play(a);
    int sounding = 0;
    for (int v = 0; v < 3; v++) {
        uint8_t ctrl = a->raw[v * 7 + 4];
        if ((ctrl & 1) && (ctrl & 0xF0)) sounding = 1;
    }
    if (sounding) {
        if (t->fxs_first < 0) t->fxs_first = t->fxs_frame;
        t->fxs_last = t->fxs_frame;
        t->fxs_sounding++;
    }
    t->fxs_frame++;
    t->fxs_total++;
}

/* The listening to one subtune is over: it is an effect if it sounded, and was quiet again well before the end. */
static void fxs_subtune_done(tinysid_t *t)
{
    if (t->fxs_sounding >= FX_MIN_FRAMES && t->fxs_last <= FX_MAX_LAST_FRAME && t->fxs_last < FX_PROBE_FRAMES - FX_QUIET_FRAMES && t->fx_n < FX_MAX) {
        int frames = t->fxs_last + FX_TAIL_FRAMES;
        t->fx_sub[t->fx_n] = (uint8_t)t->fxs_sub;
        t->fx_frames[t->fx_n] = (uint16_t)(frames > FX_PROBE_FRAMES ? FX_PROBE_FRAMES : frames);
        t->fx_n++;
    }
    tinysid_destroy(t->fxs);
    t->fxs = NULL;
    t->fxs_sub++;
    if (t->fxs_sub > t->info.songs || t->fxs_sub > FX_PROBE_SUBTUNES || t->fx_n >= FX_MAX) t->fxs_done = 1;
}

static void fxs_run(tinysid_t *t, int budget)
{
    if (t->is_analysis || t->is_fx || t->fxs_done || !t->loaded) return;
    for (int i = 0; i < budget && !t->fxs_done; i++) {
        if (!t->fxs) {
            tinysid_t *a = tinysid_create(t->sample_rate);
            if (!a) { t->fxs_done = 1; return; }
            a->is_analysis = 1;
            if (tinysid_load(a, t->file, t->file_len) != PSID_OK) { tinysid_destroy(a); t->fxs_done = 1; return; }
            a->subtune = t->fxs_sub;
            init_tune(a);
            t->fxs = a;
            t->fxs_frame = 0;
            t->fxs_first = t->fxs_last = -1;
            t->fxs_sounding = 0;
        }
        fxs_frame_step(t);
        if (t->fxs_frame >= FX_PROBE_FRAMES) fxs_subtune_done(t);
    }
}

void tinysid_fx_scan_advance(tinysid_t *t, int frames) { fxs_run(t, frames); }
int tinysid_fx_count(const tinysid_t *t) { return t->fx_n; }
int tinysid_fx_ready(const tinysid_t *t) { return t->fxs_done; }
int tinysid_fx_scan_frames(const tinysid_t *t) { return t->fxs_total; }

/* ---- notes out: the queue the host drains ------------------------------------------------------------ */

int tinysid_midi_out_peek(const tinysid_t *t, int *type, int *note, int *value, int *channel)
{
    if (t->mout_n <= 0) return 0;
    *type = t->mout_q[t->mout_head].type;
    *note = t->mout_q[t->mout_head].note;
    *value = t->mout_q[t->mout_head].value;
    *channel = t->mout_q[t->mout_head].channel;
    return 1;
}

void tinysid_midi_out_drop(tinysid_t *t)
{
    if (t->mout_n <= 0) return;
    if (t->mout_q[t->mout_head].type == 1) {             /* the host has a note-on now: it may come back, once on each path */
        int note = t->mout_q[t->mout_head].note & 127;
        t->mout_echo_at[note] = t->sample_clock + 1;
        if (t->echo_midi[note] < 255) t->echo_midi[note]++;
        t->echo_until[note] = t->sample_clock + (uint64_t)(ECHO_EXPIRE_SEC * t->sample_rate);
    }
    t->mout_head = (t->mout_head + 1) % MOUT_Q;
    t->mout_n--;
}

/* Is `channel` (0..15) one that notes are being sent on? */
int tinysid_midi_out_listens(const tinysid_t *t, int channel)
{
    for (int v = 0; v < 3; v++)
        if (((t->mout_mask >> v) & 1) && mout_channel(t, v) == channel) return 1;
    return 0;
}

void tinysid_midi_out_release(tinysid_t *t) { mout_release(t); }
int tinysid_midi_out_pending(const tinysid_t *t) { return t->mout_n; }
int tinysid_midi_out_longest(const tinysid_t *t) { return t->mout_max; }

/* Plays effect `idx` (0-based, in subtune order) on a spare engine, velocity 1..127 setting its level. A new hit takes a free
 * slot, or the one nearest to finishing. */
static void fx_trigger(tinysid_t *t, int idx, int vel)
{
    if (t->is_analysis || t->is_fx || !t->loaded || idx < 0 || idx >= t->fx_n) return;
    vel = vel < 1 ? 1 : vel > 127 ? 127 : vel;

    int s = 0;
    for (int i = 1; i < FX_SLOTS; i++)
        if (t->fx_left[i] < t->fx_left[s]) s = i;
    tinysid_t *e = t->fx_eng[s];
    if (!e) {
        e = tinysid_create(t->sample_rate);
        if (!e) return;
        e->is_fx = 1;
        if (tinysid_load(e, t->file, t->file_len) != PSID_OK) { tinysid_destroy(e); return; }
        t->fx_eng[s] = e;
    }

    /* The effect sounds the way the song does: same chip, filter, level, spread, tuning and transpose. */
    e->subtune = t->fx_sub[idx];
    e->model = t->model;
    sid_set_model(&e->sid, (sid_model_t)t->model);
    e->filter_mode = t->filter_mode;
    e->cutoff_bias = t->cutoff_bias;
    e->res_bias = t->res_bias;
    e->trim_gain = e->trim_cur = t->trim_gain;
    e->stereo_spread = t->stereo_spread;
    apply_spread(e);
    e->pitch_snap = t->pitch_snap;
    e->master_tune = t->master_tune;
    e->transpose = t->transpose;
    e->transport_running = 1;
    restart_tune(e);
    e->tune_cos = t->tune_cos;                           /* an effect is too short to learn the tuning, so it borrows the song's */
    e->tune_sin = t->tune_sin;
    e->tune_n = t->tune_n;
    e->tune_notes[0] = t->tune_notes[0];
    e->tune_notes[1] = t->tune_notes[1];

    t->fx_gain[s] = 0.25f + 0.75f * (float)vel / 127.0f;
    t->fx_left[s] = (int)(t->fx_frames[idx] * t->sample_rate / target_hz(e)) + 1;

    float goal = 1.0f - t->fx_duck;                      /* the song dips so the effect cuts through, and comes back with the ducker's release */
    if (t->fx_duck > 0.0f && t->duck_env > goal) {
        t->duck_goal = goal;
        t->duck_atk_left = (int)(DUCK_ATTACK_SEC * t->sample_rate) + 1;
    }
}

/* Adds the effects that are sounding to the finished song, with a short fade on the last few milliseconds. */
static void fx_mix(tinysid_t *t, float *left, float *right, int n)
{
    if (t->is_analysis || t->is_fx) return;
    const float fade = (float)(FX_FADE_SEC * t->sample_rate);
    for (int s = 0; s < FX_SLOTS; s++) {
        tinysid_t *e = t->fx_eng[s];
        if (!e || t->fx_left[s] <= 0) continue;
        for (int off = 0; off < n; off += 128) {
            float fl[128], fr[128];
            int c = n - off < 128 ? n - off : 128;
            tinysid_render(e, fl, fr, c);
            for (int i = 0; i < c; i++) {
                int remain = t->fx_left[s] - off - i;
                if (remain <= 0) break;
                float g = t->fx_gain[s] * (remain < fade ? (float)remain / fade : 1.0f);
                left[off + i] = clampf(left[off + i] + fl[i] * g, -1.0f, 1.0f);
                right[off + i] = clampf(right[off + i] + fr[i] * g, -1.0f, 1.0f);
            }
        }
        t->fx_left[s] -= n;
        if (t->fx_left[s] < 0) t->fx_left[s] = 0;
    }
}

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
    fxs_run(t, FX_SLICE * ((n + 127) / 128));
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

        sid_render(&t->sid, left + done, right + done, chunk);
        if (t->seam_hold[0] | t->seam_hold[1] | t->seam_hold[2]) seam_release(t);

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

    const int crush_hold = 1 + (int)(t->lofi * 7.0f + 0.5f);                 /* sample-and-hold: down to a fifth of the sample rate */
    const float crush_levels = t->lofi > 0.0f ? powf(2.0f, 15.0f - 12.0f * t->lofi) : 0.0f;   /* 16 bits down to 4 */
    for (int i = 0; i < n; i++) {
        t->trim_cur += (t->trim_gain - t->trim_cur) * t->trim_coef;
        if (t->duck_atk_left > 0) {                      /* a kick: down to the ducked level over a couple of milliseconds ... */
            t->duck_env += (t->duck_goal - t->duck_env) / (float)t->duck_atk_left;
            t->duck_atk_left--;
        } else {                                         /* ... then back up: E = E a + (1 - a) */
            t->duck_env = t->duck_env * t->duck_alpha + (1.0f - t->duck_alpha);
            if (t->duck_env > 0.99999f) t->duck_env = 1.0f;
        }
        /* Filter Pump: a low-pass on the output that closes on a beat (from 20 kHz down to about 310 Hz at full depth) and opens as
         * the ducker's release runs out. It is the output's own filter, not the SID's, so it works on any tune, costs nothing at
         * rest, and leaves the tune's filter registers alone. */
        float pg = 0.0f, pk = 0.0f, pa = 0.0f;
        int pump_run = t->pump_amt > 0.0f || t->pump_env > 0.0f;
        if (pump_run) {
            float fc = PUMP_OPEN_HZ * exp2f(-t->pump_env * PUMP_OCTAVES);
            if (fc > 0.45f * (float)t->sample_rate) fc = 0.45f * (float)t->sample_rate;
            pg = tanf(3.14159265f * fc / (float)t->sample_rate);
            pk = 1.0f / PUMP_Q;
            pa = 1.0f / (1.0f + pg * (pg + pk));
            t->pump_env *= t->duck_alpha;
            if (t->pump_env < 0.001f) t->pump_env = 0.0f;
        }
        float *ch[2] = { &left[i], &right[i] };
        for (int c = 0; c < 2; c++) {
            float x = *ch[c];
            if (!(x == x)) x = 0.0f;                     /* NaN guard, before it can poison the filter state */
            x = tinysid_hpf_run(&t->hpf[c], x);
            if (pump_run) {
                if (!t->pump_on) { t->pump_s1[c] = 0.0f; t->pump_s2[c] = x; }      /* start from the signal: no click */
                float hp = (x - (pk + pg) * t->pump_s1[c] - t->pump_s2[c]) * pa;
                float bp = pg * hp + t->pump_s1[c];
                t->pump_s1[c] = pg * hp + bp;
                float lp = pg * bp + t->pump_s2[c];
                t->pump_s2[c] = pg * bp + lp;
                if (fabsf(t->pump_s1[c]) < 1e-20f) t->pump_s1[c] = 0.0f;
                if (fabsf(t->pump_s2[c]) < 1e-20f) t->pump_s2[c] = 0.0f;
                x = lp;
            }
            float s = x * t->duck_env * t->trim_cur;
            if (crush_levels > 0.0f) {                   /* Lo-Fi */
                if (--t->crush_n[c] <= 0) { t->crush_hold[c] = floorf(s * crush_levels + 0.5f) / crush_levels; t->crush_n[c] = crush_hold; }
                s = t->crush_hold[c];
            }
            float a = fabsf(s);
            if (a > 0.8f) s = copysignf(0.8f + 0.2f * tanhf((a - 0.8f) * 5.0f), s);   /* soft knee above 0.8 */
            *ch[c] = clampf(s, -1.0f, 1.0f);
        }
        t->pump_on = pump_run;
    }
    fx_mix(t, left, right, n);
    audio_delay_run(t, left, right, n);
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
uint16_t tinysid_cpu_pc(const tinysid_t *t) { return t->cpu.pc; }
int tinysid_phrase_armed(const tinysid_t *t) { return t->loop_valid; }
int tinysid_song_bar(const tinysid_t *t)
{
    int len = loop_bar_ticks(t);
    int64_t pos = t->song_pos - (int64_t)t->loop_nudge * (len / 16);
    return pos < 0 ? 0 : (int)(pos / len) + 1;
}
uint64_t tinysid_phrase_rewinds(const tinysid_t *t) { return t->loop_rewinds; }
void tinysid_set_clock_status_fn(tinysid_t *t, int (*fn)(void)) { t->clock_status_fn = fn; }
double tinysid_samples_per_frame(const tinysid_t *t) { return t->sample_rate / target_hz(t); }
