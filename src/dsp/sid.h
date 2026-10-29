/* Compact MOS 6581/8580 SID model.
 *
 * Oscillators, envelopes (with the exponential decay curve and 15-bit rate
 * counter), hard sync, ring mod and a state-variable filter are modelled.
 * The output is a float stereo mix: each voice has a left and a right gain (1 / 1 = centre). Per-voice gains are applied to each voice
 * before it is summed or routed into the filter, so a gain of 0 removes the
 * voice completely.
 *
 * Simplifications: combined waveforms are the AND of their parts (no measured
 * chip tables), noise does not lock up, and the filter is a TPT state-variable
 * filter with a model-specific cutoff curve (6581 adds soft saturation). */
#ifndef SID_H
#define SID_H

#include <stdint.h>

typedef enum { SID_MODEL_6581 = 0, SID_MODEL_8580 = 1 } sid_model_t;

typedef struct {
    uint32_t acc;
    uint32_t freq;
    uint32_t noise;
    uint16_t pw;
    uint8_t  ctrl, ad, sr;
    /* envelope */
    uint8_t  env, state, exp_counter, exp_period, hold_zero;
    uint16_t rate_counter, rate_period;
    uint16_t wave;           /* last 12-bit waveform output */
    float    gain, gain_target;
    float    pan_l, pan_r;   /* the voice's gain into each channel */
} sid_voice_t;

typedef struct {
    sid_voice_t v[3];
    uint8_t reg[32];
    sid_model_t model;
    double clock_hz, sample_rate;
    double cycle_acc;
    float gain_coef;
    /* filter */
    int filter_dirty;
    float g, k, a1, s1, s2;
} sid_t;

void sid_init(sid_t *s, double clock_hz, double sample_rate, sid_model_t model);
void sid_reset(sid_t *s);
void sid_set_model(sid_t *s, sid_model_t model);
void sid_set_voice_gain(sid_t *s, int voice, float gain);  /* smoothed toward target */
void sid_set_voice_pan(sid_t *s, int voice, float left, float right);   /* gains into each output channel */
void sid_snap_voice_gains(sid_t *s);                       /* jump to targets (init / tests) */
void sid_kill_voice(sid_t *s, int voice);                  /* envelope to zero now, no release tail */
void sid_write(sid_t *s, uint8_t reg, uint8_t value);
uint8_t sid_read(const sid_t *s, uint8_t reg);
/* Renders n frames into `left` and `right`. A voice routed through the filter is panned with the average pan of the voices that
 * are routed, since the filter is one mono filter: the filtered sum cannot be split voice by voice. */
void sid_render(sid_t *s, float *left, float *right, int n);

#endif
