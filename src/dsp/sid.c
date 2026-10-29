#include "sid.h"

#include <math.h>
#include <string.h>

#define SUBSTEPS 4            /* oscillators are sampled this many times per output sample */
#define MIX_SCALE 0.30f       /* headroom for three full-scale voices */

enum { ST_ATTACK, ST_DECAY, ST_RELEASE };

static const uint16_t rate_periods[16] = {
    9, 32, 63, 95, 149, 220, 267, 313, 392, 977, 1954, 3126, 3907, 11720, 19532, 31251
};

static uint8_t exp_period_for(uint8_t env)
{
    if (env >= 0x5D) return 1;
    if (env >= 0x36) return 2;
    if (env >= 0x1A) return 4;
    if (env >= 0x0E) return 8;
    if (env >= 0x06) return 16;
    if (env >= 0x01) return 30;
    return 1;
}

static void env_update_rate(sid_voice_t *v)
{
    switch (v->state) {
    case ST_ATTACK:  v->rate_period = rate_periods[v->ad >> 4]; break;
    case ST_DECAY:   v->rate_period = rate_periods[v->ad & 15]; break;
    default:         v->rate_period = rate_periods[v->sr & 15]; break;
    }
}

static void voice_set_gate(sid_voice_t *v, int gate)
{
    if (gate) {
        v->state = ST_ATTACK;
        v->hold_zero = 0;
    } else {
        v->state = ST_RELEASE;
        v->exp_period = exp_period_for(v->env);
        v->hold_zero = v->env == 0;
    }
    env_update_rate(v);
}

static inline void env_clock(sid_voice_t *v)
{
    v->rate_counter = (uint16_t)((v->rate_counter + 1) & 0x7FFF);
    if (v->rate_counter != v->rate_period) return;
    v->rate_counter = 0;

    if (v->state != ST_ATTACK && ++v->exp_counter != v->exp_period) return;
    v->exp_counter = 0;
    if (v->hold_zero) return;

    switch (v->state) {
    case ST_ATTACK:
        if (++v->env == 0xFF) { v->state = ST_DECAY; env_update_rate(v); }
        return;
    case ST_DECAY:
        if (v->env == (uint8_t)((v->sr >> 4) * 17)) return;
        --v->env;
        break;
    default:
        --v->env;
        break;
    }
    v->exp_period = exp_period_for(v->env);
    if (v->env == 0) v->hold_zero = 1;
}

void sid_reset(sid_t *s)
{
    memset(s->reg, 0, sizeof(s->reg));
    for (int i = 0; i < 3; i++) {
        sid_voice_t *v = &s->v[i];
        float gain = v->gain, target = v->gain_target, pl = v->pan_l, pr = v->pan_r;
        memset(v, 0, sizeof(*v));
        v->gain = gain;
        v->gain_target = target;
        v->pan_l = pl;
        v->pan_r = pr;
        v->noise = 0x7FFFF8;
        v->state = ST_RELEASE;
        v->hold_zero = 1;
        v->exp_period = 1;
        env_update_rate(v);
    }
    s->s1 = s->s2 = 0.0f;
    s->cycle_acc = 0.0;
    s->filter_dirty = 1;
}

void sid_init(sid_t *s, double clock_hz, double sample_rate, sid_model_t model)
{
    memset(s, 0, sizeof(*s));
    s->clock_hz = clock_hz;
    s->sample_rate = sample_rate;
    s->model = model;
    s->gain_coef = 1.0f - expf(-1.0f / (0.004f * (float)sample_rate));
    for (int i = 0; i < 3; i++) { s->v[i].gain = s->v[i].gain_target = 1.0f; s->v[i].pan_l = s->v[i].pan_r = 1.0f; }
    sid_reset(s);
}

void sid_set_model(sid_t *s, sid_model_t model)
{
    if (s->model == model) return;
    s->model = model;
    s->filter_dirty = 1;
}

void sid_set_voice_gain(sid_t *s, int voice, float gain)
{
    if (voice < 0 || voice > 2) return;
    s->v[voice].gain_target = gain;
}

void sid_set_voice_pan(sid_t *s, int voice, float left, float right)
{
    if (voice < 0 || voice > 2) return;
    s->v[voice].pan_l = left;
    s->v[voice].pan_r = right;
}

void sid_snap_voice_gains(sid_t *s)
{
    for (int i = 0; i < 3; i++) s->v[i].gain = s->v[i].gain_target;
}

void sid_kill_voice(sid_t *s, int voice)
{
    if (voice < 0 || voice > 2) return;
    sid_voice_t *v = &s->v[voice];
    v->env = 0;
    v->state = ST_RELEASE;
    v->hold_zero = 1;
    v->exp_period = 1;
    env_update_rate(v);
}

void sid_write(sid_t *s, uint8_t reg, uint8_t val)
{
    reg &= 0x1F;
    s->reg[reg] = val;
    if (reg < 21) {
        sid_voice_t *v = &s->v[reg / 7];
        switch (reg % 7) {
        case 0: v->freq = (v->freq & 0xFF00u) | val; break;
        case 1: v->freq = (v->freq & 0x00FFu) | ((uint32_t)val << 8); break;
        case 2: v->pw = (uint16_t)((v->pw & 0x0F00) | val); break;
        case 3: v->pw = (uint16_t)((v->pw & 0x00FF) | ((val & 0x0F) << 8)); break;
        case 4: {
            int gate_old = v->ctrl & 1, gate_new = val & 1;
            v->ctrl = val;
            if (gate_old != gate_new) voice_set_gate(v, gate_new);
            break;
        }
        case 5: v->ad = val; env_update_rate(v); break;
        case 6: v->sr = val; env_update_rate(v); break;
        }
    } else if (reg == 0x15 || reg == 0x16 || reg == 0x17) {
        s->filter_dirty = 1;
    }
}

uint8_t sid_read(const sid_t *s, uint8_t reg)
{
    reg &= 0x1F;
    if (reg == 0x1B) return (uint8_t)(s->v[2].wave >> 4);
    if (reg == 0x1C) return s->v[2].env;
    return 0;
}

static uint16_t voice_wave(const sid_voice_t *v, const sid_voice_t *src)
{
    uint8_t sel = v->ctrl >> 4;
    if (!sel) return 0;

    uint32_t acc = v->acc;
    uint16_t out = 0xFFF;
    if (sel & 1) {
        uint32_t msb = acc;
        if ((v->ctrl & 4) && src) msb ^= src->acc;       /* ring mod flips the triangle fold */
        uint16_t tri = (uint16_t)((acc >> 11) & 0xFFF);
        if (msb & 0x800000) tri ^= 0xFFF;
        out &= tri;
    }
    if (sel & 2) out &= (uint16_t)(acc >> 12);
    if (sel & 4) out &= ((v->ctrl & 8) || (uint16_t)(acc >> 12) >= v->pw) ? 0xFFF : 0;
    if (sel & 8) {
        uint32_t n = v->noise;
        out &= (uint16_t)(((n >> 11) & 0x800) | ((n >> 10) & 0x400) | ((n >> 7) & 0x200) |
                          ((n >> 5) & 0x100) | ((n >> 4) & 0x080) | ((n >> 1) & 0x040) |
                          ((n << 1) & 0x020) | ((n << 3) & 0x010));
    }
    return out;
}

/* Advances all three voices by `cyc` SID clocks and stores their outputs in out[]. */
static void clock_voices(sid_t *s, int cyc, float out[3])
{
    int msb_rise[3];

    for (int i = 0; i < 3; i++) {
        sid_voice_t *v = &s->v[i];
        if (v->ctrl & 8) {                       /* test bit holds the oscillator at zero */
            v->acc = 0;
            msb_rise[i] = 0;
            continue;
        }
        uint64_t old = v->acc;
        uint64_t total = old + (uint64_t)v->freq * (uint64_t)cyc;
        v->acc = (uint32_t)(total & 0xFFFFFF);
        msb_rise[i] = (total >> 23) > (old >> 23);

        uint64_t edges = (total >> 19) - (old >> 19);
        if (edges > 8) edges = 8;
        while (edges--) {
            uint32_t bit = ((v->noise >> 22) ^ (v->noise >> 17)) & 1;
            v->noise = ((v->noise << 1) | bit) & 0x7FFFFF;
        }
    }
    for (int i = 0; i < 3; i++) {
        int src = (i + 2) % 3;
        if ((s->v[i].ctrl & 2) && msb_rise[src]) s->v[i].acc = 0;
    }
    for (int i = 0; i < 3; i++) {
        sid_voice_t *v = &s->v[i];
        for (int c = 0; c < cyc; c++) env_clock(v);
        v->wave = voice_wave(v, &s->v[(i + 2) % 3]);
        out[i] = v->ctrl >> 4
                     ? ((float)v->wave - 2048.0f) * (1.0f / 2048.0f) * ((float)v->env * (1.0f / 255.0f))
                     : 0.0f;
    }
}

static inline float sat(float x)
{
    if (x > 3.0f) x = 3.0f;
    if (x < -3.0f) x = -3.0f;
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}

static void update_filter(sid_t *s)
{
    int cutoff = (s->reg[0x15] & 7) | (s->reg[0x16] << 3);        /* 11 bits */
    int res = s->reg[0x17] >> 4;
    float x = (float)cutoff * (1.0f / 2047.0f);
    float fc, q;

    if (s->model == SID_MODEL_6581) {
        fc = 120.0f + 8000.0f * x * x;       /* sluggish low end, steep top */
        q = 0.7f + 0.25f * (float)res;
    } else {
        fc = 30.0f + 12000.0f * x;           /* linear */
        q = 0.7f + 0.17f * (float)res;
    }
    float max_fc = 0.45f * (float)s->sample_rate;
    if (fc > max_fc) fc = max_fc;

    s->g = tanf(3.14159265f * fc / (float)s->sample_rate);
    s->k = 1.0f / q;
    s->a1 = 1.0f / (1.0f + s->g * (s->g + s->k));
    s->filter_dirty = 0;
}

void sid_render(sid_t *s, float *left, float *right, int n)
{
    const double cps = s->clock_hz / s->sample_rate;
    const int is6581 = s->model == SID_MODEL_6581;

    for (int i = 0; i < n; i++) {
        s->cycle_acc += cps;
        int cycles = (int)s->cycle_acc;
        s->cycle_acc -= cycles;

        float vo[3] = { 0, 0, 0 };
        int steps = 0;
        float acc[3] = { 0, 0, 0 };
        for (int k = 0; k < SUBSTEPS; k++) {
            int c = cycles / SUBSTEPS + (k < cycles % SUBSTEPS ? 1 : 0);
            if (!c) continue;
            clock_voices(s, c, vo);
            for (int j = 0; j < 3; j++) acc[j] += vo[j];
            steps++;
        }
        if (steps) for (int j = 0; j < 3; j++) acc[j] /= (float)steps;

        float direct_l = 0.0f, direct_r = 0.0f, filt_in = 0.0f, fpan_l = 0.0f, fpan_r = 0.0f;
        int routed = 0;
        const uint8_t routing = s->reg[0x17];
        for (int j = 0; j < 3; j++) {
            sid_voice_t *v = &s->v[j];
            float d = v->gain_target - v->gain;
            v->gain = fabsf(d) < 1e-5f ? v->gain_target : v->gain + d * s->gain_coef;
            float x = acc[j] * v->gain;
            if (routing & (1u << j)) {
                filt_in += x;
                fpan_l += v->pan_l;
                fpan_r += v->pan_r;
                routed++;
            } else if (!(j == 2 && (s->reg[0x18] & 0x80))) {                 /* voice 3 off */
                direct_l += x * v->pan_l;
                direct_r += x * v->pan_r;
            }
        }
        if (routed) { fpan_l /= (float)routed; fpan_r /= (float)routed; }

        if (s->filter_dirty) update_filter(s);
        float x = is6581 ? sat(filt_in * 1.5f) * (1.0f / 1.5f) : filt_in;
        float hp = (x - (s->k + s->g) * s->s1 - s->s2) * s->a1;
        float bp = s->g * hp + s->s1;
        s->s1 = s->g * hp + bp;
        float lp = s->g * bp + s->s2;
        s->s2 = s->g * bp + lp;
        if (is6581) s->s1 = sat(s->s1);
        if (fabsf(s->s1) < 1e-20f) s->s1 = 0.0f;                         /* flush denormals */
        if (fabsf(s->s2) < 1e-20f) s->s2 = 0.0f;

        uint8_t mode = s->reg[0x18];
        float fout = 0.0f;
        if (mode & 0x10) fout += lp;
        if (mode & 0x20) fout += bp;
        if (mode & 0x40) fout += hp;

        float vol = ((float)(mode & 0x0F) * (1.0f / 15.0f)) * MIX_SCALE;
        left[i] = (direct_l + fout * fpan_l) * vol;
        right[i] = (direct_r + fout * fpan_r) * vol;
    }
}
