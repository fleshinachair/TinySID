/* Engine behaviour: audio sanity, voice muting, parameter clamping, register
 * intercepts, pad jamming, timing / host sync, robustness and leaks. */
#include <stdlib.h>

#include "fixtures.h"
#include "psid.h"
#include "test_util.h"
#include "tinysid.h"

#include "default_sid.h"

#define SR 44100.0

/* ---- counting allocator -------------------------------------------------- */
static long g_live, g_total;
static void *count_malloc(size_t n)
{
    void *p = malloc(n);
    if (p) { g_live++; g_total++; }
    return p;
}
static void count_free(void *p)
{
    if (p) g_live--;
    free(p);
}

/* ---- helpers --------------------------------------------------------------- */
static uint8_t fxbuf[0x1000];
static float L[1 << 16], R[1 << 16];

static tinysid_t *make(int variant, int songs, uint32_t speed, uint16_t flags)
{
    size_t len = fx_tone(fxbuf, variant, songs, speed, flags);
    tinysid_t *t = tinysid_create(SR);
    if (!t || tinysid_load(t, fxbuf, len) != PSID_OK) { printf("fixture load failed\n"); exit(2); }
    return t;
}

static void setp(tinysid_t *t, const char *k, const char *v) { tinysid_set_param(t, k, v); }

static const char *getp(tinysid_t *t, const char *k)
{
    static char out[8][64];
    static int slot;
    char *b = out[slot++ & 7];
    if (tinysid_get_param(t, k, b, 64) < 0) snprintf(b, 64, "<none>");
    return b;
}

static double rms(const float *x, int n)
{
    double s = 0;
    for (int i = 0; i < n; i++) s += (double)x[i] * x[i];
    return sqrt(s / n);
}

static int all_finite_bounded(const float *x, int n)
{
    for (int i = 0; i < n; i++)
        if (!isfinite(x[i]) || fabsf(x[i]) > 1.0f) return 0;
    return 1;
}

static void scan_to_end(tinysid_t *t)
{
    for (int blocks = 0; blocks < 3000 && !tinysid_analysis_done(t); blocks++) tinysid_render(t, L, R, 128);
}

/* ---- tests ----------------------------------------------------------------- */

static void test_audio_sanity(void)
{
    SECTION("audio generation");
    tinysid_t *t = make(FX_PLAIN, 1, 0, 0);
    tinysid_render(t, L, R, 8192);
    CHECK(all_finite_bounded(L, 8192));
    CHECK(rms(L + 1000, 4000) > 0.05);
    CHECK(memcmp(L, R, 8192 * sizeof(float)) == 0);       /* mono source, duplicated to both channels */
    tinysid_destroy(t);

    /* the embedded default tune makes sound from the first block */
    t = tinysid_create(48000.0);
    CHECK_EQ(tinysid_load(t, default_sid_data, default_sid_len), PSID_OK);
    tinysid_render(t, L, R, 48000);
    CHECK(all_finite_bounded(L, 48000));
    CHECK(rms(L, 48000) > 0.01);
    CHECK(tinysid_play_calls(t) >= 49);

    /* pulse waves are unipolar, so without the output DC blocker this tune sits well off zero */
    double mean = 0;
    for (int i = 8000; i < 48000; i++) mean += L[i];
    mean /= 40000.0;
    CHECK(fabs(mean) < 0.02);
    tinysid_destroy(t);
}

static void test_voice_muting(void)
{
    SECTION("voice muting");
    static float all[8192], v[3][8192];

    /* -6 dB trim keeps the three-voice peak below the output soft knee so the sum stays linear */
    tinysid_t *t = make(FX_PLAIN, 1, 0, 0);
    setp(t, "out_trim", "-6");
    tinysid_render(t, L, R, 8192);
    memcpy(all, L, sizeof(all));
    tinysid_destroy(t);

    for (int solo = 0; solo < 3; solo++) {
        t = make(FX_PLAIN, 1, 0, 0);
        setp(t, "out_trim", "-6");
        for (int k = 0; k < 3; k++) {
            char key[8];
            snprintf(key, sizeof(key), "v%d_vol", k + 1);
            setp(t, key, k == solo ? "1" : "0");
        }
        tinysid_render(t, L, R, 8192);
        memcpy(v[solo], L, sizeof(v[solo]));
        CHECK(rms(v[solo] + 4000, 4000) > 0.01);          /* each voice is audible on its own */
        tinysid_destroy(t);
    }

    /* the direct path is linear and oscillator phase is independent of gain, so the
     * three solo renders must add up to the full mix once the gain ramp has settled */
    double maxdiff = 0;
    for (int i = 4000; i < 8192; i++) {
        double d = fabs((double)v[0][i] + v[1][i] + v[2][i] - all[i]);
        if (d > maxdiff) maxdiff = d;
    }
    CHECK(maxdiff < 1e-4);

    /* the solo renders differ from each other (distinct pitches) */
    double d01 = 0;
    for (int i = 4000; i < 8192; i++) d01 += fabs((double)v[0][i] - v[1][i]);
    CHECK(d01 > 1.0);

    /* live mute: gate everything off mid-stream, output decays to silence without a hard step */
    t = make(FX_PLAIN, 1, 0, 0);
    tinysid_render(t, L, R, 4096);
    float before = L[4095];
    setp(t, "v1_vol", "0"); setp(t, "v2_vol", "0"); setp(t, "v3_vol", "0");
    tinysid_render(t, L, R, 4096);
    CHECK(fabsf(L[0] - before) < 0.1f);                    /* ramped, not a click */
    CHECK(rms(L + 2500, 1500) < 1e-4);                     /* gain ramp (~4 ms) has finished */
    setp(t, "v2_vol", "1");
    tinysid_render(t, L, R, 4096);
    CHECK(rms(L + 1024, 3072) > 0.01);                     /* and back */
    tinysid_destroy(t);
}

static void test_param_clamping(void)
{
    SECTION("parameter clamping");
    tinysid_t *t = make(FX_PLAIN, 3, 0, 0);

    setp(t, "v1_vol", "5");      CHECK_STR(getp(t, "v1_vol"), "1");
    setp(t, "v2_vol", "-3");     CHECK_STR(getp(t, "v2_vol"), "0");
    setp(t, "v3_vol", "0.25");   CHECK_STR(getp(t, "v3_vol"), "0.25");

    setp(t, "out_trim", "99");   CHECK_STR(getp(t, "out_trim"), "6");
    setp(t, "out_trim", "-99");  CHECK_STR(getp(t, "out_trim"), "-18");
    setp(t, "out_trim", "-4.5"); CHECK_STR(getp(t, "out_trim"), "-4.5");

    setp(t, "subtune", "99");    CHECK_STR(getp(t, "subtune"), "3");
    setp(t, "subtune", "0");     CHECK_STR(getp(t, "subtune"), "1");
    setp(t, "subtune", "-7");    CHECK_STR(getp(t, "subtune"), "1");
    setp(t, "subtune", "2");     CHECK_STR(getp(t, "subtune"), "2");
    CHECK_STR(getp(t, "subtune_count"), "3");

    setp(t, "cutoff_bias", "1000");  CHECK_STR(getp(t, "cutoff_bias"), "127");
    setp(t, "cutoff_bias", "-1000"); CHECK_STR(getp(t, "cutoff_bias"), "-128");
    setp(t, "res_bias", "99");       CHECK_STR(getp(t, "res_bias"), "15");
    setp(t, "res_bias", "-99");      CHECK_STR(getp(t, "res_bias"), "-15");

    setp(t, "chip_model", "8580");   CHECK_STR(getp(t, "chip_model"), "8580");
    setp(t, "chip_model", "0");      CHECK_STR(getp(t, "chip_model"), "6581");
    setp(t, "chip_model", "9");      CHECK_STR(getp(t, "chip_model"), "6581");   /* invalid: unchanged */
    CHECK_STR(getp(t, "sync_mode"), "Free (PAL 50Hz)");                                  /* defaults */
    CHECK_STR(getp(t, "tempo_trim"), "1");
    CHECK_STR(getp(t, "song_bpm"), "116");
    setp(t, "sync_mode", "Host Lock 2x");     CHECK_STR(getp(t, "sync_mode"), "Host Lock 2x");
    setp(t, "sync_mode", "host lock 0.5x");   CHECK_STR(getp(t, "sync_mode"), "Host Lock 0.5x");
    setp(t, "sync_mode", "1");                CHECK_STR(getp(t, "sync_mode"), "Free (NTSC 60Hz)");
    setp(t, "sync_mode", "9");                CHECK_STR(getp(t, "sync_mode"), "Free (NTSC 60Hz)");   /* invalid: unchanged */
    setp(t, "sync_mode", "0");                CHECK_STR(getp(t, "sync_mode"), "Free (PAL 50Hz)");
    setp(t, "tempo_trim", "9");      CHECK_STR(getp(t, "tempo_trim"), "1.5");
    setp(t, "tempo_trim", "0.1");    CHECK_STR(getp(t, "tempo_trim"), "0.75");
    setp(t, "tempo_trim", "-4");     CHECK_STR(getp(t, "tempo_trim"), "0.75");
    setp(t, "tempo_trim", "1.25");   CHECK_STR(getp(t, "tempo_trim"), "1.25");
    CHECK_EQ(tinysid_set_param(t, "tempo_trim", "nan"), -1);
    CHECK_STR(getp(t, "tempo_trim"), "1.25");
    setp(t, "tempo_trim", "1");
    setp(t, "song_bpm_override", "500");  CHECK_STR(getp(t, "song_bpm"), "200");
    setp(t, "song_bpm_override", "3");    CHECK_STR(getp(t, "song_bpm"), "60");
    setp(t, "song_bpm_override", "133.4"); CHECK_STR(getp(t, "song_bpm"), "133");
    setp(t, "song_bpm", "77");            CHECK_STR(getp(t, "song_bpm"), "133");   /* the readout ignores writes */
    CHECK_EQ(tinysid_set_param(t, "song_bpm", "junk"), -1);
    setp(t, "pad_target", "Voice 2"); CHECK_STR(getp(t, "pad_target"), "Voice 2");
    setp(t, "pad_target", "3");      CHECK_STR(getp(t, "pad_target"), "Voice 3");
    setp(t, "pad_target", "7");      CHECK_STR(getp(t, "pad_target"), "Voice 3");
    setp(t, "pad_target", "off");    CHECK_STR(getp(t, "pad_target"), "Off");
    CHECK_EQ(tinysid_set_param(t, "run_mode", "1"), -1);            /* the transport is not a setting any more */
    setp(t, "filter_mode", "high pass"); CHECK_STR(getp(t, "filter_mode"), "High Pass");
    setp(t, "filter_mode", "0");     CHECK_STR(getp(t, "filter_mode"), "Song");
    CHECK_STR(getp(t, "pad_sound"), "Lead");                    /* nothing known yet */
    CHECK_EQ(tinysid_set_param(t, "pad_sound", "Triangle"), -1);   /* the fixed leads are gone */
    setp(t, "pad_sound", "3");       CHECK_STR(getp(t, "pad_sound_index"), "3");     /* kept: a saved set can name an instrument before the scan */
    setp(t, "pad_sound", "99");      CHECK_STR(getp(t, "pad_sound_index"), "3");      /* past any list: unchanged */
    CHECK_EQ(tinysid_set_param(t, "pad_sound", "nonsense"), -1);
    setp(t, "pad_sound", "0");

    /* garbage never changes state and is reported */
    CHECK_EQ(tinysid_set_param(t, "v1_vol", "nan"), -1);
    CHECK_STR(getp(t, "v1_vol"), "1");
    CHECK_EQ(tinysid_set_param(t, "v1_vol", "abc"), -1);
    CHECK_EQ(tinysid_set_param(t, "v1_vol", NULL), -1);
    CHECK_EQ(tinysid_set_param(t, "no_such_key", "1"), -1);
    char scratch[8];
    CHECK_EQ(tinysid_get_param(t, "no_such_key", scratch, sizeof(scratch)), -1);
    CHECK_EQ(tinysid_set_param(t, "load_path", "/definitely/not/here.sid"), -1);
    CHECK_STR(getp(t, "subtune"), "2");                    /* failed load kept the tune */

    char tiny[4];
    setp(t, "pad_target", "Voice 2");
    CHECK_EQ(tinysid_get_param(t, "pad_target", tiny, sizeof(tiny)), 3);   /* truncated, still terminated */
    CHECK_STR(tiny, "Voi");

    /* trim is real gain: +6 dB doubles, -18 dB is ~0.126 */
    tinysid_destroy(t);
    double base, hi, lo;
    t = make(FX_PLAIN, 1, 0, 0);
    setp(t, "v2_vol", "0"); setp(t, "v3_vol", "0");
    tinysid_render(t, L, R, 8192);
    base = rms(L + 4096, 4096);
    setp(t, "out_trim", "6");
    tinysid_render(t, L, R, 8192);
    hi = rms(L + 4096, 4096);
    setp(t, "out_trim", "-18");
    tinysid_render(t, L, R, 8192);
    lo = rms(L + 4096, 4096);
    CHECK_NEAR(hi / base, 1.995, 0.03);
    CHECK_NEAR(lo / base, 0.1259, 0.005);
    tinysid_destroy(t);
}

static void test_register_overrides(void)
{
    SECTION("register intercepts");
    tinysid_t *t = make(FX_WRITER, 1, 0, 0);
    tinysid_render(t, L, R, 2048);                         /* a few play frames */
    CHECK_EQ(tinysid_sid_reg(t, 0x16), 0x40);
    CHECK_EQ(tinysid_sid_reg(t, 0x17), 0x80);

    setp(t, "cutoff_bias", "10");
    CHECK_EQ(tinysid_sid_reg(t, 0x16), 0x4A);              /* applies immediately, no new write needed */
    tinysid_render(t, L, R, 2048);
    CHECK_EQ(tinysid_sid_reg(t, 0x16), 0x4A);              /* and survives the tune rewriting $D416 */
    setp(t, "cutoff_bias", "-100");
    CHECK_EQ(tinysid_sid_reg(t, 0x16), 0x00);              /* clamped at 0 */
    setp(t, "cutoff_bias", "127");
    CHECK_EQ(tinysid_sid_reg(t, 0x16), 0xBF);
    setp(t, "cutoff_bias", "0");
    CHECK_EQ(tinysid_sid_reg(t, 0x16), 0x40);

    setp(t, "res_bias", "3");
    CHECK_EQ(tinysid_sid_reg(t, 0x17), 0xB0);              /* resonance nibble 8 -> 11, routing bits untouched */
    setp(t, "res_bias", "-15");
    CHECK_EQ(tinysid_sid_reg(t, 0x17), 0x00);
    setp(t, "res_bias", "15");
    CHECK_EQ(tinysid_sid_reg(t, 0x17), 0xF0);
    setp(t, "res_bias", "0");
    CHECK_EQ(tinysid_sid_reg(t, 0x17), 0x80);

    /* chip model change is audible: the same tune through a filter sounds different */
    tinysid_destroy(t);

    static float a[4096], b[4096];
    for (int m = 0; m < 2; m++) {
        t = tinysid_create(SR);
        tinysid_load(t, default_sid_data, default_sid_len);
        setp(t, "chip_model", m ? "8580" : "6581");
        tinysid_render(t, L, R, 4096);
        tinysid_render(t, L, R, 4096);
        memcpy(m ? b : a, L, sizeof(a));
        CHECK(all_finite_bounded(L, 4096));
        tinysid_destroy(t);
    }
    double diff = 0;
    for (int i = 0; i < 4096; i++) diff += fabs((double)a[i] - b[i]);
    CHECK(diff > 1.0);
}

/* ---- pad: a voice you play from the keyboard --------------------------------- */

static int sid_word(tinysid_t *t, int lo_reg) { return tinysid_sid_reg(t, lo_reg) | (tinysid_sid_reg(t, lo_reg + 1) << 8); }

static int middle_c_word(void) { return (int)(440.0 * pow(2.0, -9.0 / 12.0) * 16777216.0 / 985248.0 + 0.5); }

static const uint8_t NOTE_ON_60[] = { 0x90, 60, 100 }, NOTE_OFF_60[] = { 0x80, 60, 0 };

static void test_pad_takeover(void)
{
    SECTION("pad: choosing a voice mutes it and hands it to the keyboard");
    tinysid_t *t = make(FX_COUNTER, 1, 0, 0);
    setp(t, "v1_vol", "0");
    setp(t, "v3_vol", "0");                                /* listen to voice 2 alone */
    for (int i = 0; i < 2; i++) tinysid_render(t, L, R, 4410);
    CHECK(rms(L + 2000, 2000) > 0.05);                     /* the song is playing voice 2 */
    CHECK_EQ(tinysid_sid_reg(t, 8), tinysid_ram(t, 3));
    CHECK_EQ(tinysid_sid_reg(t, 11) & 1, 1);

    setp(t, "pad_target", "Voice 2");
    tinysid_render(t, L, R, 64);
    float fade_peak = 0;                                   /* the voice fades out first, it is not cut dead */
    for (int i = 0; i < 64; i++) if (fabsf(L[i]) > fade_peak) fade_peak = fabsf(L[i]);
    CHECK(fade_peak > 0.01f);                              /* still sounding for a moment ... */
    tinysid_render(t, L, R, 3000);
    CHECK_EQ(tinysid_sid_reg(t, 11) & 1, 0);               /* ... then handed over: gate dropped */
    CHECK(rms(L + 1800, 1000) < 1e-3);                     /* silent, no tail from the song's note */
    float max_step = 0;
    tinysid_render(t, L, R, 4410);
    for (int i = 1; i < 4410; i++) if (fabsf(L[i] - L[i - 1]) > max_step) max_step = fabsf(L[i] - L[i - 1]);
    CHECK(max_step < 0.01f);                               /* no click after the handover */

    tinysid_render(t, L, R, 4410);
    CHECK(tinysid_sid_reg(t, 8) != tinysid_ram(t, 3));     /* the song's writes to voice 2 are ignored ... */
    setp(t, "v1_vol", "1"); setp(t, "v3_vol", "1");
    tinysid_render(t, L, R, 4410);
    CHECK_EQ(tinysid_sid_reg(t, 1), tinysid_ram(t, 3));    /* ... while voices 1 and 3 carry on */
    CHECK_EQ(tinysid_sid_reg(t, 15), tinysid_ram(t, 3));
    CHECK_EQ(tinysid_sid_reg(t, 4), 0x21);
    CHECK_EQ(tinysid_sid_reg(t, 18), 0x21);

    /* play: the scan has not finished, so the sound is the placeholder saw lead */
    setp(t, "v1_vol", "0"); setp(t, "v3_vol", "0");
    tinysid_midi(t, NOTE_ON_60, 3);
    CHECK_EQ(sid_word(t, 7), middle_c_word());             /* $D407/$D408 */
    CHECK_EQ(tinysid_sid_reg(t, 11), 0x21);                /* sawtooth + gate on $D40B */
    CHECK_EQ(tinysid_sid_reg(t, 12), 0x00);                /* instant attack */
    CHECK_EQ(tinysid_sid_reg(t, 13), 0xF8);                /* full sustain */
    tinysid_render(t, L, R, 6000);
    CHECK_EQ(sid_word(t, 7), middle_c_word());             /* the song cannot retune it */
    CHECK(rms(L + 3000, 3000) > 0.015);                     /* and it sounds */
    CHECK(all_finite_bounded(L, 6000));

    tinysid_midi(t, NOTE_OFF_60, 3);
    CHECK_EQ(tinysid_sid_reg(t, 11) & 1, 0);               /* gate off, release plays out */
    tinysid_render(t, L, R, 44100);
    CHECK(rms(L + 30000, 4000) < 1e-4);                    /* silent again ... */
    CHECK(tinysid_sid_reg(t, 8) != tinysid_ram(t, 3));     /* ... and the song has NOT taken the voice back */
    CHECK_EQ(tinysid_sid_reg(t, 11) & 1, 0);

    /* Off returns the voice to the song */
    setp(t, "pad_target", "Off");
    CHECK_EQ(tinysid_sid_reg(t, 11), 0x21);
    tinysid_render(t, L, R, 1000);
    CHECK_EQ(tinysid_sid_reg(t, 8), tinysid_ram(t, 3));

    /* moving the pad to another voice restores the first and mutes the second */
    setp(t, "pad_target", "Voice 2");
    setp(t, "pad_target", "Voice 3");
    CHECK_EQ(tinysid_sid_reg(t, 11), 0x21);                /* voice 2 is the song's again */
    tinysid_render(t, L, R, 4410);
    CHECK_EQ(tinysid_sid_reg(t, 18) & 1, 0);               /* voice 3 is the keyboard's once its fade is done */
    CHECK_EQ(tinysid_sid_reg(t, 8), tinysid_ram(t, 3));
    CHECK(tinysid_sid_reg(t, 15) != tinysid_ram(t, 3));

    /* notes do nothing while the pad is off */
    setp(t, "pad_target", "Off");
    uint8_t before = tinysid_sid_reg(t, 4);
    tinysid_midi(t, NOTE_ON_60, 3);
    CHECK_EQ(tinysid_sid_reg(t, 4), before);
    tinysid_midi(t, NOTE_OFF_60, 3);
    tinysid_destroy(t);
}

static void test_pad_sounds(void)
{
    SECTION("pad: sounds come from the song");
    char opts[256];

    /* until the scan has finished nothing is known: a plain saw lead */
    tinysid_t *t = make(FX_PLAIN, 1, 0, 0);
    setp(t, "pad_target", "Voice 2");
    CHECK_STR(getp(t, "pad_sound"), "Lead");
    CHECK(tinysid_pad_options(t, opts, sizeof(opts)) > 0);
    CHECK_STR(opts, "[\"Lead\"]");
    tinysid_midi(t, NOTE_ON_60, 3);
    CHECK_EQ(tinysid_sid_reg(t, 11), 0x21);
    CHECK_EQ(tinysid_sid_reg(t, 12), 0x00);
    CHECK_EQ(tinysid_sid_reg(t, 13), 0xF8);
    tinysid_midi(t, NOTE_OFF_60, 3);

    /* afterwards it is the song's own instrument (the fixture plays a saw, AD $20, SR $E8, on every voice) */
    scan_to_end(t);
    CHECK(tinysid_pad_options(t, opts, sizeof(opts)) > 0);
    CHECK_STR(opts, "[\"1 Saw 20 E8\"]");
    CHECK_STR(getp(t, "pad_sound"), "1 Saw 20 E8");
    tinysid_midi(t, NOTE_ON_60, 3);
    CHECK_EQ(tinysid_sid_reg(t, 11), 0x21);
    CHECK_EQ(tinysid_sid_reg(t, 12), 0x20);
    CHECK_EQ(tinysid_sid_reg(t, 13), 0xE8);
    tinysid_destroy(t);

    /* a voice the song never sounds borrows the instruments of the song's other voices */
    t = make(FX_KILLV2, 1, 0, 0);
    scan_to_end(t);
    setp(t, "v1_vol", "0"); setp(t, "v3_vol", "0");
    setp(t, "pad_target", "Voice 2");
    CHECK(tinysid_pad_options(t, opts, sizeof(opts)) > 0);
    CHECK_STR(opts, "[\"1 Saw 20 E8\"]");
    tinysid_render(t, L, R, 4410);
    tinysid_midi(t, NOTE_ON_60, 3);
    CHECK_EQ(tinysid_sid_reg(t, 11), 0x21);
    CHECK_EQ(tinysid_sid_reg(t, 12), 0x20);
    tinysid_render(t, L, R, 6000);
    CHECK(rms(L + 3000, 3000) > 0.015);
    tinysid_destroy(t);

    /* a silent tune has no instruments at all: the placeholder lead keeps the keys working */
    fx_code_t quiet = { {0}, 0 };                          /* volume up, but no voice is ever gated */
    fx_store(&quiet, 0x0F, 0xD418);
    fx_byte(&quiet, 0x60);
    size_t len = fx_psid(fxbuf, quiet.b, quiet.n, FX_LOAD, FX_LOAD, FX_LOAD, 1, 0, 0x0004);
    t = tinysid_create(SR);
    CHECK_EQ(tinysid_load(t, fxbuf, len), PSID_OK);
    scan_to_end(t);
    setp(t, "pad_target", "Voice 1");
    CHECK(tinysid_pad_options(t, opts, sizeof(opts)) > 0);
    CHECK_STR(opts, "[\"Lead\"]");
    tinysid_render(t, L, R, 4410);
    tinysid_midi(t, NOTE_ON_60, 3);
    CHECK_EQ(tinysid_sid_reg(t, 4), 0x21);
    tinysid_render(t, L, R, 6000);
    CHECK(rms(L + 3000, 3000) > 0.015);
    tinysid_destroy(t);
}

static void test_pad_playing(void)
{
    SECTION("pad: playing behaviour");
    tinysid_t *t = make(FX_WRITER, 1, 0, 0);               /* the song rewrites voice 1's pitch every frame */
    const uint8_t on69[] = { 0x90, 69, 100 }, off69[] = { 0x80, 69, 0 };
    const uint8_t on_v0[] = { 0x90, 69, 0 };
    setp(t, "pad_target", "Voice 1");

    int a4 = (int)(440.0 * 16777216.0 / 985248.0 + 0.5);
    tinysid_midi(t, on69, 3);
    CHECK_EQ(sid_word(t, 0), a4);
    tinysid_render(t, L, R, 2048);
    CHECK_EQ(sid_word(t, 0), a4);                          /* the song's $D401 rewrites do nothing */

    /* last note wins; releasing it returns to the one still held, without a new attack */
    tinysid_midi(t, NOTE_ON_60, 3);
    CHECK_NEAR(sid_word(t, 0), (int)(261.6256 * 16777216.0 / 985248.0 + 0.5), 1);
    tinysid_midi(t, NOTE_OFF_60, 3);
    CHECK_EQ(sid_word(t, 0), a4);
    CHECK_EQ(tinysid_sid_reg(t, 4) & 1, 1);
    tinysid_midi(t, on_v0, 3);                             /* velocity 0 is a note-off */
    CHECK_EQ(tinysid_sid_reg(t, 4) & 1, 0);
    tinysid_midi(t, off69, 3);                             /* a stray note-off changes nothing */
    CHECK_EQ(tinysid_sid_reg(t, 4) & 1, 0);

    /* all notes off */
    tinysid_midi(t, on69, 3);
    CHECK_EQ(tinysid_sid_reg(t, 4) & 1, 1);
    const uint8_t panic[] = { 0xB0, 123, 0 };
    tinysid_midi(t, panic, 3);
    CHECK_EQ(tinysid_sid_reg(t, 4) & 1, 0);

    /* more simultaneous notes than slots never overflows, and the sound stays finite */
    for (int n = 0; n < 40; n++) { uint8_t m[3] = { 0x90, (uint8_t)(30 + n), 90 }; tinysid_midi(t, m, 3); }
    tinysid_render(t, L, R, 512);
    CHECK(all_finite_bounded(L, 512));
    tinysid_destroy(t);

    /* a restart (new subtune) keeps the pad voice yours */
    t = make(FX_COUNTER, 2, 0, 0);
    setp(t, "pad_target", "Voice 2");
    tinysid_render(t, L, R, 4410);
    setp(t, "subtune", "2");
    CHECK_EQ(tinysid_sid_reg(t, 11) & 1, 0);
    tinysid_render(t, L, R, 4410);
    CHECK(tinysid_sid_reg(t, 8) != tinysid_ram(t, 3));
    CHECK_EQ(tinysid_sid_reg(t, 1), tinysid_ram(t, 3));
    tinysid_destroy(t);
}

/* ---- tempo: frame rate, host lock, BPM readout ------------------------------ */

/* Sends `ticks` MIDI clocks at `bpm`, rendering the sample gap between them. Returns samples rendered. */
static long feed_clocks(tinysid_t *t, double bpm, int ticks)
{
    const uint8_t clk = 0xF8;
    double gap = SR * 60.0 / (bpm * 24.0), acc = 0.0;
    long total = 0;
    for (int i = 0; i < ticks; i++) {
        tinysid_midi(t, &clk, 1);
        acc += gap;
        int n = (int)acc;
        acc -= n;
        tinysid_render(t, L, R, n);
        total += n;
    }
    return total;
}

/* play() calls made while rendering `samples` samples in 441-sample blocks. */
static uint64_t plays_over(tinysid_t *t, int samples)
{
    uint64_t before = tinysid_play_calls(t);
    for (int done = 0; done < samples; done += 441) tinysid_render(t, L, R, 441);
    return tinysid_play_calls(t) - before;
}

static void test_free_modes(void)
{
    SECTION("tempo: free-running modes");
    tinysid_t *t = make(FX_PLAIN, 1, 0, 0x0004);
    CHECK_NEAR(tinysid_frame_hz(t), 50.0, 1e-9);
    CHECK_NEAR(tinysid_samples_per_frame(t), 882.0, 1e-6);
    uint64_t n = plays_over(t, 44100);
    CHECK(n >= 50 && n <= 51);

    setp(t, "sync_mode", "Free (NTSC 60Hz)");
    CHECK_NEAR(tinysid_frame_hz(t), 59.826, 1e-9);
    n = plays_over(t, 44100);
    CHECK(n >= 59 && n <= 61);

    setp(t, "sync_mode", "Free (PAL 50Hz)");
    setp(t, "tempo_trim", "1.5");
    CHECK_NEAR(tinysid_frame_hz(t), 75.0, 1e-9);
    n = plays_over(t, 44100 * 2);
    CHECK(n >= 149 && n <= 151);
    setp(t, "tempo_trim", "0.75");
    CHECK_NEAR(tinysid_frame_hz(t), 37.5, 1e-9);
    n = plays_over(t, 44100 * 2);
    CHECK(n >= 74 && n <= 76);
    setp(t, "sync_mode", "Free (NTSC 60Hz)");
    CHECK_NEAR(tinysid_frame_hz(t), 59.826 * 0.75, 1e-9);
    tinysid_destroy(t);

    /* a file plays at its own speed: loading it picks the Free mode from the header's video standard */
    t = make(FX_PLAIN, 2, 0, 0x0008);                      /* header says NTSC */
    CHECK_STR(getp(t, "sync_mode"), "Free (NTSC 60Hz)");
    CHECK_NEAR(tinysid_frame_hz(t), 59.826, 1e-9);
    setp(t, "sync_mode", "Free (PAL 50Hz)");               /* the user can still choose otherwise ... */
    CHECK_NEAR(tinysid_frame_hz(t), 50.0, 1e-9);
    setp(t, "subtune", "2");                               /* ... and a subtune change leaves the choice alone */
    CHECK_STR(getp(t, "sync_mode"), "Free (PAL 50Hz)");
    size_t pal_len = fx_tone(fxbuf, FX_PLAIN, 1, 0, 0x0004);
    tinysid_load(t, fxbuf, pal_len);                       /* a new PAL file takes the mode back to PAL */
    CHECK_STR(getp(t, "sync_mode"), "Free (PAL 50Hz)");
    size_t ntsc_len = fx_tone(fxbuf, FX_PLAIN, 1, 0, 0x0008);
    tinysid_load(t, fxbuf, ntsc_len);
    CHECK_STR(getp(t, "sync_mode"), "Free (NTSC 60Hz)");
    setp(t, "sync_mode", "Host Lock 1x");                  /* a Host Lock is the user's tempo choice: files do not override it */
    tinysid_load(t, fxbuf, pal_len = fx_tone(fxbuf, FX_PLAIN, 1, 0, 0x0004));
    CHECK_STR(getp(t, "sync_mode"), "Host Lock 1x");
    tinysid_destroy(t);

    /* CIA-timed tunes keep the rate their timer asks for in either free mode */
    t = make(FX_CIA, 1, 1, 0x0004);
    CHECK_NEAR(tinysid_frame_hz(t), 985248.0 / 16421.0, 0.01);
    setp(t, "sync_mode", "1");
    CHECK_NEAR(tinysid_frame_hz(t), 985248.0 / 16421.0, 0.01);
    tinysid_destroy(t);
    t = make(FX_PLAIN, 1, 1, 0x0004);                      /* speed bit set, timer never programmed */
    CHECK_NEAR(tinysid_frame_hz(t), 60.0, 1e-9);
    tinysid_destroy(t);

    /* subtune selection re-inits: the fixture stores A (= subtune-1) at $02 */
    t = make(FX_PLAIN, 2, 0, 0);
    setp(t, "subtune", "2");
    CHECK_EQ(tinysid_ram(t, 0x02), 1);
    setp(t, "subtune", "1");
    CHECK_EQ(tinysid_ram(t, 0x02), 0);
    tinysid_destroy(t);
}

static void test_host_lock(void)
{
    SECTION("tempo: host lock follows MIDI clock");
    tinysid_t *t = make(FX_PLAIN, 1, 0, 0x0004);
    setp(t, "song_bpm_override", "125");                            /* 6 frames per 16th at 50 Hz */
    setp(t, "sync_mode", "Host Lock 1x");

    /* before any clock the host tempo defaults to 120 */
    CHECK_NEAR(tinysid_host_bpm(t), 120.0, 1e-9);
    CHECK_NEAR(tinysid_frame_hz(t), 50.0 * 120.0 / 125.0, 1e-9);

    /* 100 BPM, then 140 BPM */
    feed_clocks(t, 100.0, 200);
    double bpm100 = tinysid_host_bpm(t);
    double spf100 = tinysid_samples_per_frame(t);
    CHECK_NEAR(bpm100, 100.0, 0.5);
    CHECK_NEAR(spf100, SR / (50.0 * bpm100 / 125.0), 1e-6);
    CHECK_NEAR(spf100, 1102.5, 6.0);

    uint64_t before = tinysid_play_calls(t);
    long samples = feed_clocks(t, 100.0, 96);               /* four beats */
    CHECK_NEAR((double)(tinysid_play_calls(t) - before) / (samples / SR), 40.0, 1.0);

    feed_clocks(t, 140.0, 200);
    double bpm140 = tinysid_host_bpm(t);
    double spf140 = tinysid_samples_per_frame(t);
    CHECK_NEAR(bpm140, 140.0, 0.7);
    CHECK_NEAR(spf140, 787.5, 6.0);
    CHECK(spf140 < spf100);
    CHECK_NEAR(spf100 / spf140, 1.4, 0.02);                 /* the ratio of the two tempos */

    before = tinysid_play_calls(t);
    samples = feed_clocks(t, 140.0, 96);
    CHECK_NEAR((double)(tinysid_play_calls(t) - before) / (samples / SR), 56.0, 1.5);

    /* the change is gradual, not a jump: sample the tempo while it moves 140 -> 100 */
    double prev = tinysid_host_bpm(t);
    int jumps = 0, monotone = 1;
    for (int i = 0; i < 120; i++) {
        feed_clocks(t, 100.0, 1);
        double cur = tinysid_host_bpm(t);
        if (fabs(cur - prev) > 8.0) jumps++;
        if (cur > prev + 0.5) monotone = 0;
        prev = cur;
    }
    CHECK_EQ(jumps, 0);
    CHECK(monotone);
    CHECK_NEAR(tinysid_host_bpm(t), 100.0, 0.7);

    /* half / double / trim multiply the locked rate */
    feed_clocks(t, 140.0, 200);
    setp(t, "sync_mode", "Host Lock 0.5x");
    CHECK_NEAR(tinysid_frame_hz(t), 28.0, 0.2);
    setp(t, "sync_mode", "Host Lock 2x");
    CHECK_NEAR(tinysid_frame_hz(t), 112.0, 0.8);
    setp(t, "tempo_trim", "1.2");
    CHECK_NEAR(tinysid_frame_hz(t), 112.0 * 1.2, 1.0);
    setp(t, "tempo_trim", "1");

    /* block-quantised timestamps (alternating 850/914-sample gaps, mean 882 = 125 BPM) */
    tinysid_destroy(t);
    t = make(FX_PLAIN, 1, 0, 0x0004);
    setp(t, "sync_mode", "2");
    const uint8_t clk = 0xF8;
    for (int i = 0; i < 300; i++) {
        tinysid_midi(t, &clk, 1);
        tinysid_render(t, L, R, i & 1 ? 914 : 850);
    }
    CHECK_NEAR(tinysid_host_bpm(t), 125.0, 1.0);

    /* a long gap restarts the measurement: a new tempo is picked up within a couple of beats */
    for (int i = 0; i < 3; i++) tinysid_render(t, L, R, 44100);
    feed_clocks(t, 150.0, 48);
    CHECK_NEAR(tinysid_host_bpm(t), 150.0, 3.0);
    tinysid_destroy(t);
}

/* rising zero crossings per second of a mono render */
static double crossings_per_second(const float *x, int n)
{
    int c = 0;
    for (int i = 1; i < n; i++) if (x[i - 1] < 0.0f && x[i] >= 0.0f) c++;
    return c * SR / n;
}

static void test_tempo_trim(void)
{
    SECTION("tempo trim: rate scales, pitch and audio stay intact");

    /* rate: a per-frame counter measures it directly */
    tinysid_t *t = make(FX_COUNTER, 1, 0, 0x0004);
    uint64_t base = plays_over(t, 44100 * 2);
    setp(t, "tempo_trim", "1.5");
    uint64_t fast = plays_over(t, 44100 * 2);
    setp(t, "tempo_trim", "0.75");
    uint64_t slow = plays_over(t, 44100 * 2);
    CHECK_NEAR((double)fast / (double)base, 1.5, 0.03);
    CHECK_NEAR((double)slow / (double)base, 0.75, 0.03);
    CHECK_NEAR((double)fast / (double)slow, 2.0, 0.05);
    tinysid_destroy(t);

    /* pitch: frequency registers never change, and a solo voice keeps its cycle count */
    static float buf[22050];
    double cps[3];
    const char *trims[3] = { "0.75", "1", "1.5" };
    for (int i = 0; i < 3; i++) {
        t = make(FX_PLAIN, 1, 0, 0x0004);
        setp(t, "v1_vol", "0"); setp(t, "v3_vol", "0");    /* voice 2 only: 480 Hz saw */
        setp(t, "tempo_trim", trims[i]);
        tinysid_render(t, L, R, 4410);
        tinysid_render(t, buf, R, 22050);
        cps[i] = crossings_per_second(buf, 22050);
        for (int v = 0; v < 3; v++) {
            CHECK_EQ(tinysid_sid_reg(t, v * 7), 0);
            CHECK_EQ(tinysid_sid_reg(t, v * 7 + 1), 0x10 * (v + 1));
        }
        tinysid_destroy(t);
    }
    CHECK_NEAR(cps[0], 480.0, 6.0);
    CHECK_NEAR(cps[1], 480.0, 6.0);
    CHECK_NEAR(cps[2], 480.0, 6.0);

    /* the same holds in host lock while the tempo itself is sliding */
    t = make(FX_PLAIN, 1, 0, 0x0004);
    setp(t, "sync_mode", "Host Lock 1x");
    for (int tick = 0; tick < 300; tick++) {
        const uint8_t clk = 0xF8;
        tinysid_midi(t, &clk, 1);
        tinysid_render(t, L, R, 700 + (tick * 13) % 400);
    }
    for (int v = 0; v < 3; v++) CHECK_EQ(tinysid_sid_reg(t, v * 7 + 1), 0x10 * (v + 1));
    tinysid_destroy(t);

    /* sweeping the trim (and the mode) on every block never produces NaN / Inf / out-of-range audio */
    t = tinysid_create(SR);
    tinysid_load(t, default_sid_data, default_sid_len);
    int bad = 0;
    const char *modes[] = { "0", "1", "2", "3", "4" };
    for (int i = 0; i < 400; i++) {
        char v[16];
        snprintf(v, sizeof(v), "%.3f", 0.5 + (i % 19) * 0.07);          /* includes out-of-range values */
        setp(t, "tempo_trim", v);
        if (i % 40 == 0) setp(t, "sync_mode", modes[(i / 40) % 5]);
        if (i % 7 == 0) { const uint8_t clk = 0xF8; tinysid_midi(t, &clk, 1); }
        int n = 64 + (i * 37) % 700;
        tinysid_render(t, L, R, n);
        if (!all_finite_bounded(L, n) || !all_finite_bounded(R, n)) bad++;
        double hz = tinysid_frame_hz(t);
        if (!isfinite(hz) || hz < 1.0 || hz > 1000.0) bad++;
        if (!isfinite(tinysid_samples_per_frame(t)) || !isfinite(tinysid_host_bpm(t))) bad++;
    }
    CHECK_EQ(bad, 0);
    tinysid_destroy(t);
}

/* ---- Host Lock phase lock: frames are counted from MIDI clock ticks, so nothing drifts ------------------ */

static const uint8_t MIDI_CLOCK = 0xF8, MIDI_START = 0xFA, MIDI_CONT = 0xFB, MIDI_STOP = 0xFC;

/* A Commando-titled fixture (so the table's row length applies), in Host Lock at the given mode, started with 0xFA. */
static tinysid_t *make_locked(int subtune, const char *mode)
{
    size_t len = fx_tone(fxbuf, FX_PLAIN, 2, 0, 0x0004);
    fx_set_title(fxbuf, "Commando");
    tinysid_t *t = tinysid_create(SR);
    if (!t || tinysid_load(t, fxbuf, len) != PSID_OK) { printf("fixture load failed\n"); exit(2); }
    if (subtune != 1) { char s[8]; snprintf(s, sizeof(s), "%d", subtune); setp(t, "subtune", s); }
    setp(t, "sync_mode", mode);
    tinysid_midi(t, &MIDI_START, 1);
    return t;
}

typedef struct {
    long long ticks;                 /* ticks sent */
    long long off_grid;              /* tick instants where the play count was not exactly the expected one */
    long long off_row;               /* 16th boundaries where it was not exactly rows * frames-per-row + 1 */
    long long out_of_range;          /* blocks where the count was behind this tick's or ahead of the next tick's */
    long long frames;                /* play() calls at the last tick instant (one sample after it) */
} lock_run_t;

/* Sends `ticks` clocks (the first is tick 0), each followed by 128-sample blocks, tempo bpm_a until tick `switch_at`
 * and bpm_b after. `jitter` moves each tick by up to +-40 samples (arrival is block-quantised, as on the Move).
 * The count after tick K must be exactly floor(K * en / ed) + 1 whatever the tempo or the jitter. */
static lock_run_t drive_lock(tinysid_t *t, int ticks, double bpm_a, double bpm_b, int switch_at, int jitter, long en, long ed)
{
    lock_run_t r;
    memset(&r, 0, sizeof(r));
    uint64_t base = tinysid_play_calls(t);
    double next_tick = 0.0, tick_len = SR * 60.0 / (bpm_a * 24.0), when = 0.0;
    uint32_t rng = 12345;
    long long pos = 0;
    int k = -1;
    while (k < ticks - 1 || pos < (long long)next_tick) {
        int ticked = 0;
        if (k < ticks - 1 && (double)pos >= next_tick) {
            tinysid_midi(t, &MIDI_CLOCK, 1);
            k++;
            ticked = 1;
            if (k == switch_at) tick_len = SR * 60.0 / (bpm_b * 24.0);
            when += tick_len;
            rng = rng * 1664525u + 1013904223u;
            next_tick = when + (jitter ? (double)((int)((rng >> 16) % 81) - 40) : 0.0);
            if (next_tick < (double)pos + 128.0) next_tick = (double)pos + 128.0;
        }
        int first = ticked ? 1 : 128;
        tinysid_render(t, L, R, first);
        pos += first;
        if (ticked) {
            long long want = (long long)k * en / ed + 1, got = (long long)(tinysid_play_calls(t) - base);
            if (got != want) r.off_grid++;
            r.frames = got;
            if (k % 6 == 0 && ed == 6 && got != (long long)(k / 6) * en + 1) r.off_row++;      /* at 1x a row is 6 ticks */
            tinysid_render(t, L, R, 127);
            pos += 127;
        }
        long long got = (long long)(tinysid_play_calls(t) - base);
        if (k >= 0 && (got < (long long)k * en / ed + 1 || got > (long long)(k + 1) * en / ed + 1)) r.out_of_range++;
        if (pos > 1000000000LL) break;
    }
    r.ticks = k + 1;
    return r;
}

static void test_phase_lock(void)
{
    SECTION("phase lock: frames follow MIDI clock ticks, 64 bars without drift");
    const int bars64 = 64 * 16 * 6;                        /* 6144 ticks: 64 bars of 16 rows at 6 ticks a row; tick 6144 is bar 65's downbeat */

    /* the table carries the row length: Commando subtune 1 is 125.0 BPM, exactly 6 frames a row */
    tinysid_t *t = make_locked(1, "Host Lock 1x");
    int fn = 0, fd = 0;
    CHECK(tinysid_song_fpr(t, &fn, &fd));
    CHECK_EQ(fn, 6);
    CHECK_EQ(fd, 1);
    CHECK_EQ(tinysid_song_bpm(t), 125);

    /* nothing plays between Start and the first tick: the first downbeat waits for Step 1 */
    CHECK_EQ(tinysid_play_calls(t), 0u);
    tinysid_render(t, L, R, 5000);
    CHECK_EQ(tinysid_play_calls(t), 0u);
    CHECK_EQ(tinysid_clock_ticks(t), -1);

    /* 120 BPM: 64 bars, six frames per row = one frame per tick */
    lock_run_t r = drive_lock(t, bars64 + 1, 120.0, 120.0, -1, 0, 6, 6);
    CHECK_EQ(r.ticks, bars64 + 1);
    CHECK_EQ(r.off_grid, 0);
    CHECK_EQ(r.off_row, 0);
    CHECK_EQ(r.out_of_range, 0);
    CHECK_EQ(r.frames, 64LL * 16 * 6 + 1);                /* zero drift at bar 64: the 1025th row's downbeat */
    tinysid_destroy(t);

    /* 125 BPM, the tune's own tempo, with the tick arrival jittered */
    t = make_locked(1, "Host Lock 1x");
    r = drive_lock(t, bars64 + 1, 125.0, 125.0, -1, 1, 6, 6);
    CHECK_EQ(r.off_grid, 0);
    CHECK_EQ(r.off_row, 0);
    CHECK_EQ(r.out_of_range, 0);
    CHECK_EQ(r.frames, 64LL * 16 * 6 + 1);
    tinysid_destroy(t);

    /* a tempo change half way through moves no frame: the count belongs to the ticks */
    t = make_locked(1, "Host Lock 1x");
    r = drive_lock(t, bars64 + 1, 120.0, 125.0, bars64 / 2, 1, 6, 6);
    CHECK_EQ(r.off_grid, 0);
    CHECK_EQ(r.out_of_range, 0);
    CHECK_EQ(r.frames, 64LL * 16 * 6 + 1);
    tinysid_destroy(t);

    /* a row that is not a whole number of frames per tick: subtune 2 is 8 frames a row (4/3 of a frame a tick) */
    t = make_locked(2, "Host Lock 1x");
    CHECK(tinysid_song_fpr(t, &fn, &fd) && fn == 8 && fd == 1);
    r = drive_lock(t, bars64 + 1, 120.0, 120.0, -1, 1, 8, 6);
    CHECK_EQ(r.off_grid, 0);
    CHECK_EQ(r.off_row, 0);
    CHECK_EQ(r.out_of_range, 0);
    CHECK_EQ(r.frames, 64LL * 16 * 8 + 1);
    tinysid_destroy(t);

    /* Half and 2x change the ticks per row (12 and 3) and nothing else */
    t = make_locked(1, "Host Lock 0.5x");
    r = drive_lock(t, bars64 + 1, 125.0, 125.0, -1, 1, 6, 12);
    CHECK_EQ(r.off_grid, 0);
    CHECK_EQ(r.out_of_range, 0);
    CHECK_EQ(r.frames, (long long)bars64 * 6 / 12 + 1);
    tinysid_destroy(t);
    t = make_locked(1, "Host Lock 2x");
    r = drive_lock(t, bars64 + 1, 125.0, 125.0, -1, 1, 6, 3);
    CHECK_EQ(r.off_grid, 0);
    CHECK_EQ(r.out_of_range, 0);
    CHECK_EQ(r.frames, (long long)bars64 * 6 / 3 + 1);
    tinysid_destroy(t);

    /* Start again part way through: the phase resets, the first tick afterwards is Step 1 once more */
    t = make_locked(1, "Host Lock 1x");
    drive_lock(t, 1000, 120.0, 120.0, -1, 0, 6, 6);
    tinysid_midi(t, &MIDI_START, 1);
    uint64_t base = tinysid_play_calls(t);
    tinysid_render(t, L, R, 3000);
    CHECK_EQ(tinysid_play_calls(t), base);
    tinysid_midi(t, &MIDI_CLOCK, 1);
    tinysid_render(t, L, R, 1);
    CHECK_EQ(tinysid_play_calls(t), base + 1);             /* frame 1 on tick 0 */
    CHECK_EQ(tinysid_clock_ticks(t), 0);
    feed_clocks(t, 120.0, 600);
    CHECK_EQ(tinysid_play_calls(t), base + 601);           /* ticks 1..600 on top of tick 0's frame */
    tinysid_destroy(t);

    /* Stop holds the tune, clock or not; Continue picks up on the next tick where it left off */
    t = make_locked(1, "Host Lock 1x");
    base = tinysid_play_calls(t);
    for (int i = 0; i < 100; i++) { tinysid_midi(t, &MIDI_CLOCK, 1); tinysid_render(t, L, R, 882); }
    CHECK_EQ(tinysid_play_calls(t) - base, 100u);
    tinysid_midi(t, &MIDI_STOP, 1);
    for (int i = 0; i < 50; i++) { tinysid_midi(t, &MIDI_CLOCK, 1); tinysid_render(t, L, R, 882); }
    CHECK_EQ(tinysid_play_calls(t) - base, 100u);
    CHECK_EQ(tinysid_running(t), 0);
    tinysid_midi(t, &MIDI_CONT, 1);
    tinysid_midi(t, &MIDI_CLOCK, 1);
    tinysid_render(t, L, R, 1);
    CHECK_EQ(tinysid_play_calls(t) - base, 101u);          /* tick 100's frame, the one after the 100 played */
    tinysid_destroy(t);

    /* the clock vanishes without a Stop: the tune pauses, and resumes in place when the ticks return */
    t = make_locked(1, "Host Lock 1x");
    base = tinysid_play_calls(t);
    for (int i = 0; i < 60; i++) { tinysid_midi(t, &MIDI_CLOCK, 1); tinysid_render(t, L, R, 882); }
    for (int i = 0; i < 4; i++) tinysid_render(t, L, R, 44100);
    CHECK_EQ(tinysid_running(t), 0);
    CHECK_EQ(tinysid_play_calls(t) - base, 60u);
    tinysid_midi(t, &MIDI_CLOCK, 1);
    tinysid_render(t, L, R, 1);
    CHECK_EQ(tinysid_running(t), 1);
    CHECK_EQ(tinysid_play_calls(t) - base, 61u);
    tinysid_destroy(t);

    /* no clock at all: after a second the tune free-runs at the legacy tempo ratio rather than staying silent */
    t = make_locked(1, "Host Lock 1x");
    base = tinysid_play_calls(t);
    for (int i = 0; i < 4; i++) tinysid_render(t, L, R, 44100);
    printf("  no clock: %llu frames in 3 s\n", (unsigned long long)(tinysid_play_calls(t) - base));
    CHECK(tinysid_play_calls(t) - base >= 140 && tinysid_play_calls(t) - base <= 150);   /* 3 s at 50 * 120 / 125 */
    tinysid_destroy(t);

    /* a tempo trim leaves the grid on purpose: the lock stands aside and the rate scales as before */
    t = make_locked(1, "Host Lock 1x");
    setp(t, "tempo_trim", "1.2");
    drive_lock(t, 48, 125.0, 125.0, -1, 0, 6, 6);
    base = tinysid_play_calls(t);
    long samples = feed_clocks(t, 125.0, 240);
    CHECK_NEAR((double)(tinysid_play_calls(t) - base) / (samples / SR), 50.0 * 1.2, 1.5);
    tinysid_destroy(t);

    /* a row length measured from the scan is a ratio too, and the BPM readout is a whole number */
    size_t len = fx_tone(fxbuf, FX_GATER + 8, 1, 0, 0x0004);
    t = tinysid_create(SR);
    tinysid_load(t, fxbuf, len);
    CHECK(!tinysid_song_fpr(t, &fn, &fd));
    scan_to_end(t);
    CHECK(tinysid_song_fpr(t, &fn, &fd));
    CHECK_EQ(fn, 8);
    CHECK_EQ(fd, 1);
    CHECK_EQ(tinysid_song_bpm(t), 94);                     /* 93.75 snapped */
    tinysid_destroy(t);
    len = fx_tone(fxbuf, FX_GATER + 24, 1, 0, 0x0004);     /* 31.25 BPM is below range: rows of 12 frames, i.e. 24 / 2 */
    t = tinysid_create(SR);
    tinysid_load(t, fxbuf, len);
    scan_to_end(t);
    CHECK(tinysid_song_fpr(t, &fn, &fd));
    CHECK_EQ(fn, 12 * fd);
    tinysid_destroy(t);
}

static void test_song_bpm(void)
{
    SECTION("song BPM: known-tune table");
    /* a tune titled "Commando" reports 125 immediately on load. Its gates imply 107 BPM (7-frame rows),
     * so the known value is distinguishable from both the estimator and the 116 default. */
    size_t len = fx_tone(fxbuf, FX_GATER + 7, 3, 0, 0x0004);
    fx_set_title(fxbuf, "Commando");
    tinysid_t *t = tinysid_create(SR);
    CHECK_EQ(tinysid_song_bpm(t), 116);                    /* nothing loaded: the default */
    CHECK_EQ(tinysid_load(t, fxbuf, len), PSID_OK);
    CHECK_EQ(tinysid_song_bpm(t), 125);
    scan_to_end(t);
    CHECK_EQ(tinysid_song_bpm(t), 125);                    /* the table wins over what the gates say */

    setp(t, "subtune", "2");
    CHECK_EQ(tinysid_song_bpm(t), 94);                     /* catalogued per subtune, no measuring needed */
    scan_to_end(t);
    CHECK_EQ(tinysid_song_bpm(t), 94);

    setp(t, "subtune", "3");                               /* not catalogued: default, then the estimator */
    CHECK_EQ(tinysid_song_bpm(t), 116);
    scan_to_end(t);
    CHECK_EQ(tinysid_song_bpm(t), 107);
    setp(t, "subtune", "1");
    CHECK_EQ(tinysid_song_bpm(t), 125);                    /* and back */
    tinysid_destroy(t);

    len = fx_tone(fxbuf, FX_GATER + 8, 2, 0, 0x0004);
    fx_set_title(fxbuf, "  armalyte ");                    /* case and padding do not matter, any subtune */
    t = tinysid_create(SR);
    CHECK_EQ(tinysid_load(t, fxbuf, len), PSID_OK);
    CHECK_EQ(tinysid_song_bpm(t), 128);
    setp(t, "subtune", "2");
    CHECK_EQ(tinysid_song_bpm(t), 128);
    tinysid_destroy(t);

    fx_set_title(fxbuf, "Commando Remix");                 /* only exact titles match */
    t = tinysid_create(SR);
    tinysid_load(t, fxbuf, len);
    scan_to_end(t);
    CHECK_EQ(tinysid_song_bpm(t), 94);
    tinysid_destroy(t);

    /* the bundled demo is catalogued per subtune */
    t = tinysid_create(SR);
    tinysid_load(t, default_sid_data, default_sid_len);
    CHECK_EQ(tinysid_song_bpm(t), 125);
    setp(t, "subtune", "2");
    CHECK_EQ(tinysid_song_bpm(t), 188);                    /* 187.5 */
    tinysid_destroy(t);

#ifdef TINYSID_ASSET_DIR
    {
        /* the real file, when it has been dropped into assets/ (git-ignored, never committed) */
        static uint8_t file[0x10400];
        FILE *fp = fopen(TINYSID_ASSET_DIR "/Commando.sid", "rb");
        size_t flen = fp ? fread(file, 1, sizeof(file), fp) : 0;
        if (fp) fclose(fp);
        if (flen > 0x7C) {
            tinysid_t *c = tinysid_create(SR);
            CHECK_EQ(tinysid_load(c, file, flen), PSID_OK);
            printf("  assets/Commando.sid found: \"%s\"\n", tinysid_info(c)->name);
            CHECK_EQ(tinysid_song_bpm(c), 125);                   /* table, subtune 1 */
            for (int i = 0; i < 3; i++) tinysid_render(c, L, R, 44100);
            CHECK_EQ(tinysid_song_bpm(c), 125);
            setp(c, "subtune", "2");
            CHECK_EQ(tinysid_song_bpm(c), 94);                    /* table, subtune 2 */
            tinysid_destroy(c);

            /* with the title changed so the table cannot match, the estimator must reach the same numbers */
            fx_set_title(file, "Estimator Only");
            static const int want[2] = { 125, 94 };
            for (int sub = 1; sub <= 2; sub++) {
                c = tinysid_create(SR);
                tinysid_load(c, file, flen);
                char v[4];
                snprintf(v, sizeof(v), "%d", sub);
                setp(c, "subtune", v);
                for (int i = 0; i < 10; i++) tinysid_render(c, L, R, 44100);
                printf("  estimator on real file, subtune %d: %d BPM\n", sub, tinysid_song_bpm(c));
                CHECK_EQ(tinysid_song_bpm(c), want[sub - 1]);
                tinysid_destroy(c);
            }
        } else {
            printf("  assets/Commando.sid not present: real-file checks skipped\n");
        }
    }
#endif
}

static void test_bpm_estimator(void)
{
    SECTION("song BPM: gate-interval estimator");
    struct { int frames; int bpm; } cases[] = { { 6, 125 }, { 7, 107 }, { 8, 94 }, { 5, 150 }, { 10, 75 } };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t len = fx_tone(fxbuf, FX_GATER + cases[i].frames, 1, 0, 0x0004);
        tinysid_t *t = tinysid_create(SR);
        tinysid_load(t, fxbuf, len);
        CHECK_EQ(tinysid_song_bpm(t), 116);                /* default until something is measured */
        scan_to_end(t);
        printf("  %d frames/row -> %d BPM\n", cases[i].frames, tinysid_song_bpm(t));
        CHECK_EQ(tinysid_song_bpm(t), cases[i].bpm);

        /* the estimate counts tune frames, so playing faster or slower does not move it */
        setp(t, "tempo_trim", "1.5");
        for (int s = 0; s < 4; s++) tinysid_render(t, L, R, 44100);
        CHECK_EQ(tinysid_song_bpm(t), cases[i].bpm);
        tinysid_destroy(t);
    }

    /* fast gating (arpeggio-style retriggers closer than 3 frames) is not mistaken for the groove */
    size_t len = fx_tone(fxbuf, FX_GATER + 2, 1, 0, 0x0004);
    tinysid_t *t = tinysid_create(SR);
    tinysid_load(t, fxbuf, len);
    scan_to_end(t);
    CHECK_EQ(tinysid_song_bpm(t), 116);
    tinysid_destroy(t);

    /* slow rows fold up into range: 24 frames would be 31 BPM, shown as 63 */
    len = fx_tone(fxbuf, FX_GATER + 24, 1, 0, 0x0004);
    t = tinysid_create(SR);
    tinysid_load(t, fxbuf, len);
    scan_to_end(t);
    CHECK_EQ(tinysid_song_bpm(t), 63);                     /* 31.25 doubled -> 62.5, rounded to nearest */
    tinysid_destroy(t);

    /* NTSC-rate (60 Hz) tune: the same 6-frame rows are 150 BPM */
    len = fx_tone(fxbuf, FX_GATER + 6, 1, 0, 0x0008);
    t = tinysid_create(SR);
    tinysid_load(t, fxbuf, len);
    scan_to_end(t);
    CHECK_EQ(tinysid_song_bpm(t), 150);                    /* 59.826 * 15 / 6 = 149.6 */
    tinysid_destroy(t);

    /* a manual value sticks until the tune changes, and the estimator stays out of the way */
    len = fx_tone(fxbuf, FX_GATER + 6, 2, 0, 0x0004);
    t = tinysid_create(SR);
    tinysid_load(t, fxbuf, len);
    setp(t, "song_bpm_override", "140");
    scan_to_end(t);
    CHECK_EQ(tinysid_song_bpm(t), 140);
    setp(t, "subtune", "2");
    scan_to_end(t);
    CHECK_EQ(tinysid_song_bpm(t), 125);
    tinysid_destroy(t);

    /* a tune that never gates voices 1 or 3 keeps the default */
    len = fx_tone(fxbuf, FX_PLAIN, 1, 0, 0x0004);
    t = tinysid_create(SR);
    tinysid_load(t, fxbuf, len);
    scan_to_end(t);
    CHECK_EQ(tinysid_song_bpm(t), 116);
    tinysid_destroy(t);
}

/* ---- song BPM is stable: set once, from the whole song ------------------------------ */

static void test_bpm_stability(void)
{
    SECTION("song BPM: one value from the whole song, never wandering");
    uint16_t h[97];

    /* gap histograms measured from real tunes (frames between gate-ons -> count) */
    memset(h, 0, sizeof(h)); h[6] = 215; h[24] = 60; h[12] = 22;                       /* Cybernoid II, subtune 1 */
    CHECK_NEAR(tinysid_bpm_from_gaps(h, 97, 50.0), 125.0, 1e-9);
    memset(h, 0, sizeof(h)); h[6] = 187; h[12] = 108; h[24] = 18; h[18] = 8;           /* Commando, subtune 1 */
    CHECK_NEAR(tinysid_bpm_from_gaps(h, 97, 50.0), 125.0, 1e-9);
    memset(h, 0, sizeof(h)); h[12] = 12; h[6] = 4; h[24] = 1;                          /* Cybernoid II, subtune 2 */
    CHECK_NEAR(tinysid_bpm_from_gaps(h, 97, 50.0), 62.5, 1e-9);

    memset(h, 0, sizeof(h)); h[6] = 10; h[12] = 10;                                    /* a tie goes to the shorter gap */
    CHECK_NEAR(tinysid_bpm_from_gaps(h, 97, 50.0), 125.0, 1e-9);
    memset(h, 0, sizeof(h)); h[24] = 20;                                               /* folded into range */
    CHECK_NEAR(tinysid_bpm_from_gaps(h, 97, 50.0), 62.5, 1e-9);
    memset(h, 0, sizeof(h)); h[8] = 30;
    CHECK_NEAR(tinysid_bpm_from_gaps(h, 97, 59.826), 59.826 * 15.0 / 8.0, 1e-9);     /* NTSC */
    memset(h, 0, sizeof(h)); h[1] = 500; h[2] = 500; h[6] = 9;                         /* retriggers under 3 frames are not rhythm */
    CHECK_NEAR(tinysid_bpm_from_gaps(h, 97, 50.0), 125.0, 1e-9);
    memset(h, 0, sizeof(h)); h[6] = 3;                                                 /* too little to go on */
    CHECK_EQ(tinysid_bpm_from_gaps(h, 97, 50.0) == 0.0, 1);
    memset(h, 0, sizeof(h));
    CHECK_EQ(tinysid_bpm_from_gaps(h, 97, 50.0) == 0.0, 1);

    /* the readout holds the default while the scan runs, changes once when it finishes, and then never moves */
    size_t len = fx_tone(fxbuf, FX_GATER + 6, 1, 0, 0x0004);
    tinysid_t *t = tinysid_create(SR);
    tinysid_load(t, fxbuf, len);
    int changes = 0, last = tinysid_song_bpm(t);
    for (int b = 0; b < 4000; b++) {
        tinysid_render(t, L, R, 128);
        if (!tinysid_analysis_done(t)) CHECK_EQ(tinysid_song_bpm(t), 116);
        if (tinysid_song_bpm(t) != last) { changes++; last = tinysid_song_bpm(t); }
    }
    CHECK_EQ(changes, 1);
    CHECK_EQ(last, 125);
    tinysid_destroy(t);

#ifdef TINYSID_ASSET_DIR
    {
        static uint8_t file[0x10400];
        const struct { const char *name; int subtune, bpm; } real[] = {
            { "Cybernoid_II.sid", 1, 125 }, { "Cybernoid_II.sid", 2, 63 }, { "Boulder_Dash.sid", 1, 112 }
        };
        for (size_t i = 0; i < sizeof(real) / sizeof(real[0]); i++) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", TINYSID_ASSET_DIR, real[i].name);
            FILE *fp = fopen(path, "rb");
            size_t flen = fp ? fread(file, 1, sizeof(file), fp) : 0;
            if (fp) fclose(fp);
            if (flen <= 0x7C) continue;
            t = tinysid_create(SR);
            tinysid_load(t, file, flen);
            char sub[8];
            snprintf(sub, sizeof(sub), "%d", real[i].subtune);
            setp(t, "subtune", sub);
            scan_to_end(t);
            int settled = tinysid_song_bpm(t), moved = 0;
            for (int s = 0; s < 120; s++) {                /* 60 s of playing: the readout does not budge */
                tinysid_render(t, L, R, 22050);
                if (tinysid_song_bpm(t) != settled) moved++;
            }
            printf("  %s subtune %d: %d BPM, moved %d times while playing\n", real[i].name, real[i].subtune, settled, moved);
            CHECK_EQ(settled, real[i].bpm);
            CHECK_EQ(moved, 0);
            tinysid_destroy(t);
        }
    }
#endif
}

/* ---- transport ------------------------------------------------------------------ */

static int g_clock_status = 1;
static int fake_clock_status(void) { return g_clock_status; }

static void test_transport(void)
{
    SECTION("transport: the tune follows the host");
    const uint8_t start = 0xFA, cont = 0xFB, stop = 0xFC;

    /* a fresh tune plays; Stop halts it; Start goes back to the top; Continue resumes where it was */
    tinysid_t *t = make(FX_COUNTER, 1, 0, 0x0004);
    CHECK(plays_over(t, 22050) >= 24);
    uint8_t reached = tinysid_ram(t, 3);
    tinysid_midi(t, &stop, 1);
    CHECK_EQ(tinysid_running(t), 0);
    CHECK_EQ(plays_over(t, 22050), 0);
    tinysid_midi(t, &cont, 1);
    CHECK(plays_over(t, 22050) >= 24);
    CHECK(tinysid_ram(t, 3) > reached);                    /* continued, not restarted */
    tinysid_midi(t, &start, 1);
    CHECK_EQ(tinysid_ram(t, 3), 0);                        /* Start always goes back to beat 1 */
    CHECK_EQ(tinysid_sid_reg(t, 4), 0x21);
    tinysid_midi(t, &stop, 1);
    tinysid_midi(t, &start, 1);                            /* and works again after a Stop */
    CHECK(plays_over(t, 22050) >= 24);
    tinysid_destroy(t);

    /* the worst case: a note being held with a 24 s release must go quiet, and stay dead after Continue */
    t = make(FX_LONGREL, 1, 0, 0x0004);
    setp(t, "v1_vol", "0"); setp(t, "v3_vol", "0");
    for (int i = 0; i < 2; i++) tinysid_render(t, L, R, 4410);
    CHECK(rms(L + 2000, 2000) > 0.05);
    tinysid_midi(t, &stop, 1);
    tinysid_render(t, L, R, 3000);                         /* 68 ms */
    CHECK(rms(L + 2200, 800) < 1e-4);
    CHECK(fabsf(L[0]) < 0.5f);                             /* faded, not a hard cut */
    tinysid_render(t, L, R, 44100);
    CHECK(rms(L + 40000, 4000) < 1e-4);
    tinysid_midi(t, &cont, 1);
    tinysid_render(t, L, R, 4410);
    tinysid_render(t, L, R, 4410);
    CHECK(rms(L + 2000, 2000) < 1e-4);                     /* the old note does not come back from the dead */
    tinysid_midi(t, &start, 1);
    tinysid_render(t, L, R, 4410);
    tinysid_render(t, L, R, 4410);
    CHECK(rms(L + 3000, 3000) > 0.015);                     /* a Start brings the song back */
    tinysid_destroy(t);

    /* hosts that report transport state but send no Start/Stop bytes */
    t = make(FX_COUNTER, 1, 0, 0x0004);
    tinysid_set_clock_status_fn(t, fake_clock_status);
    g_clock_status = 1;
    CHECK_EQ(plays_over(t, 11025), 0);                     /* host stopped when the module loaded: stays silent */
    g_clock_status = 2;
    tinysid_render(t, L, R, 128);
    CHECK(tinysid_ram(t, 3) <= 2);                         /* running: started from the top */
    CHECK(plays_over(t, 22050) >= 24);
    g_clock_status = 0;                                    /* "unavailable" says nothing: keep going */
    CHECK(plays_over(t, 22050) >= 24);
    g_clock_status = 1;
    tinysid_render(t, L, R, 128);
    CHECK_EQ(plays_over(t, 22050), 0);
    tinysid_destroy(t);

    /* a stopped tune stays stopped across a new file or subtune */
    t = make(FX_PLAIN, 2, 0, 0x0004);
    tinysid_midi(t, &stop, 1);
    size_t len = fx_tone(fxbuf, FX_PLAIN, 2, 0, 0x0004);
    CHECK_EQ(tinysid_load(t, fxbuf, len), PSID_OK);
    CHECK_EQ(tinysid_running(t), 0);
    setp(t, "subtune", "2");
    CHECK_EQ(tinysid_running(t), 0);
    tinysid_render(t, L, R, 6000);
    CHECK(rms(L + 3000, 3000) < 1e-4);
    tinysid_midi(t, &start, 1);
    tinysid_render(t, L, R, 6000);
    CHECK(rms(L + 3000, 3000) > 0.015);
    tinysid_destroy(t);

    /* the pad voice is yours: it keeps working while the song is stopped */
    t = make(FX_COUNTER, 1, 0, 0x0004);
    setp(t, "pad_target", "Voice 2");
    tinysid_render(t, L, R, 2000);
    tinysid_midi(t, &stop, 1);
    tinysid_midi(t, NOTE_ON_60, 3);
    tinysid_render(t, L, R, 6000);
    CHECK(rms(L + 3000, 3000) > 0.015);
    CHECK_EQ(sid_word(t, 7), middle_c_word());
    tinysid_destroy(t);
}

/* ---- filter override ------------------------------------------------------------ */

static void test_filter_override(void)
{
    SECTION("filter: cutoff and resonance work on any tune");
    static float a[8192], b[8192];

    /* Song mode only offsets the tune's own filter: a tune that never routes a voice hears nothing */
    tinysid_t *t = make(FX_PLAIN, 1, 0, 0);
    tinysid_render(t, L, R, 8192);
    memcpy(a, L, sizeof(a));
    tinysid_destroy(t);
    t = make(FX_PLAIN, 1, 0, 0);
    setp(t, "cutoff_bias", "100");
    setp(t, "res_bias", "15");
    tinysid_render(t, L, R, 8192);
    memcpy(b, L, sizeof(b));
    double same = 0;
    for (int i = 0; i < 8192; i++) same += fabs((double)a[i] - b[i]);
    CHECK(same < 1e-6);
    tinysid_destroy(t);

    /* Low Pass forces all three voices through the filter, with the knobs as the absolute cutoff / resonance */
    double rms_dark = 0, rms_bright = 0;
    for (int pass = 0; pass < 2; pass++) {
        t = make(FX_PLAIN, 1, 0, 0);
        setp(t, "filter_mode", "Low Pass");
        CHECK_STR(getp(t, "filter_mode"), "Low Pass");
        CHECK_EQ(tinysid_sid_reg(t, 0x17) & 7, 7);
        CHECK_EQ(tinysid_sid_reg(t, 0x18) & 0x70, 0x10);
        CHECK_EQ(tinysid_sid_reg(t, 0x16), 128);
        setp(t, "cutoff_bias", pass ? "120" : "-120");
        CHECK_EQ(tinysid_sid_reg(t, 0x16), pass ? 248 : 8);
        tinysid_render(t, L, R, 8192);
        CHECK(all_finite_bounded(L, 8192));
        (pass ? &rms_bright : &rms_dark)[0] = rms(L + 4096, 4096);
        tinysid_destroy(t);
    }
    CHECK(rms_dark < 0.6 * rms_bright);                    /* closing the filter removes the saws' harmonics */

    /* High Pass does the opposite */
    double hp_low = 0, hp_high = 0;
    for (int pass = 0; pass < 2; pass++) {
        t = make(FX_PLAIN, 1, 0, 0);
        setp(t, "filter_mode", "High Pass");
        CHECK_EQ(tinysid_sid_reg(t, 0x18) & 0x70, 0x40);
        setp(t, "cutoff_bias", pass ? "120" : "-120");
        tinysid_render(t, L, R, 8192);
        (pass ? &hp_high : &hp_low)[0] = rms(L + 4096, 4096);
        tinysid_destroy(t);
    }
    CHECK(hp_high < 0.6 * hp_low);

    /* resonance changes the sound, and so does the chip model */
    for (int pass = 0; pass < 2; pass++) {
        t = make(FX_PLAIN, 1, 0, 0);
        setp(t, "filter_mode", "Band Pass");
        setp(t, "cutoff_bias", "-20");
        setp(t, "res_bias", pass ? "11" : "-4");
        CHECK_EQ(tinysid_sid_reg(t, 0x17) >> 4, pass ? 15 : 0);
        tinysid_render(t, L, R, 8192);
        memcpy(pass ? b : a, L, sizeof(a));
        CHECK(all_finite_bounded(L, 8192));
        tinysid_destroy(t);
    }
    double dres = 0;
    for (int i = 4096; i < 8192; i++) dres += fabs((double)a[i] - b[i]);
    CHECK(dres > 1.0);

    /* back to Song restores the tune's own registers */
    t = make(FX_PLAIN, 1, 0, 0);
    setp(t, "filter_mode", "High Pass");
    setp(t, "filter_mode", "Song");
    CHECK_EQ(tinysid_sid_reg(t, 0x16), 0);
    CHECK_EQ(tinysid_sid_reg(t, 0x17), 0x80);
    CHECK_EQ(tinysid_sid_reg(t, 0x18), 0x0F);
    tinysid_destroy(t);

    /* the real tune never uses its filter, so the override is what makes the knobs audible */
#ifdef TINYSID_ASSET_DIR
    {
        static uint8_t file[0x10400];
        FILE *fp = fopen(TINYSID_ASSET_DIR "/Commando.sid", "rb");
        size_t flen = fp ? fread(file, 1, sizeof(file), fp) : 0;
        if (fp) fclose(fp);
        if (flen > 0x7C) {
            static float x[44100], y[44100];
            for (int pass = 0; pass < 2; pass++) {
                t = tinysid_create(SR);
                tinysid_load(t, file, flen);
                if (pass) setp(t, "filter_mode", "Low Pass");
                if (pass) setp(t, "cutoff_bias", "-60");
                tinysid_render(t, L, R, 44100);
                tinysid_render(t, L, R, 44100);
                memcpy(pass ? y : x, L, sizeof(x));
                tinysid_destroy(t);
            }
            double d = 0, e = 0;
            for (int i = 0; i < 44100; i++) { d += fabs((double)x[i] - y[i]); e += fabs(x[i]); }
            printf("  Commando with the low-pass override: audio changes by %.0f%%\n", 100 * d / e);
            CHECK(d / e > 0.2);
        }
    }
#endif
}

/* ---- key / scale / tempo pre-scan --------------------------------------------- */

static void test_key_detection(void)
{
    SECTION("key and scale detection");

    /* the detector must recover every key from the profile it is built on, major and minor */
    static const float major[12] = { 6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f };
    static const float minor[12] = { 6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f };
    int wrong = 0;
    for (int tonic = 0; tonic < 12; tonic++)
        for (int mode = 0; mode < 2; mode++) {
            float pc[12];
            for (int i = 0; i < 12; i++) pc[(tonic + i) % 12] = (mode ? minor : major)[i] * 3.0f;
            int scale;
            int key = tinysid_detect_key(pc, &scale);
            if (key != tonic || scale != mode) wrong++;
        }
    CHECK_EQ(wrong, 0);

    float flat[12], sparse[12] = { 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0 };
    for (int i = 0; i < 12; i++) flat[i] = 5.0f;
    int scale;
    CHECK_EQ(tinysid_detect_key(flat, &scale), 12);        /* no tonal centre */
    CHECK_EQ(scale, 2);
    CHECK_EQ(tinysid_detect_key(sparse, &scale), 12);      /* too few notes to say */

    /* end to end on small tunes with known notes */
    const int c_major[] = { 60, 64, 67, 72, 62, 64, 67, 72, 65, 69, 72, 67, 71, 74, 67, 72 };
    const int a_minor[] = { 57, 60, 64, 69, 59, 60, 64, 69, 62, 65, 69, 64, 68, 71, 64, 69 };
    const struct { const int *notes; const char *key, *scale; } cases[] = {
        { c_major, "C Maj", "Major" }, { a_minor, "A Min", "Minor" }
    };
    for (int i = 0; i < 2; i++) {
        size_t len = fx_melody(fxbuf, cases[i].notes, 16);
        tinysid_t *t = tinysid_create(SR);
        CHECK_EQ(tinysid_load(t, fxbuf, len), PSID_OK);
        CHECK_STR(getp(t, "song_key"), "--");              /* nothing until the scan has finished */
        CHECK_STR(getp(t, "song_scale"), "--");
        CHECK_EQ(tinysid_analysis_done(t), 0);
        scan_to_end(t);
        CHECK_EQ(tinysid_analysis_done(t), 1);
        printf("  melody %d: %s %s\n", i + 1, getp(t, "song_key"), getp(t, "song_scale"));
        CHECK_STR(getp(t, "song_key"), cases[i].key);
        CHECK_STR(getp(t, "song_scale"), cases[i].scale);
        tinysid_destroy(t);
    }

    /* readouts ignore writes */
    tinysid_t *t = tinysid_create(SR);
    size_t len = fx_melody(fxbuf, c_major, 16);
    tinysid_load(t, fxbuf, len);
    scan_to_end(t);
    CHECK_EQ(tinysid_set_param(t, "song_key", "F#"), 0);
    CHECK_STR(getp(t, "song_key"), "C Maj");

    /* the scan runs in slices: a single audio block does a handful of frames, never the whole tune */
    tinysid_load(t, fxbuf, len);
    CHECK_EQ(tinysid_analysis_done(t), 0);
    tinysid_render(t, L, R, 128);
    CHECK_EQ(tinysid_analysis_done(t), 0);
    CHECK_STR(getp(t, "song_key"), "--");

    /* choosing another subtune starts a fresh scan; destroying mid-scan leaks nothing (checked by the allocator test) */
    setp(t, "subtune", "1");
    CHECK_EQ(tinysid_analysis_done(t), 0);
    tinysid_destroy(t);

    /* a tune with no pitched notes yields no key */
    t = make(FX_HANG, 1, 0, 0x0004);
    scan_to_end(t);
    CHECK_STR(getp(t, "song_key"), "--");
    tinysid_destroy(t);

    /* the tempo found by the scan is available without waiting for the live estimate */
    size_t glen = fx_tone(fxbuf, FX_GATER + 7, 1, 0, 0x0004);
    t = tinysid_create(SR);
    tinysid_load(t, fxbuf, glen);
    scan_to_end(t);
    CHECK_EQ(tinysid_song_bpm(t), 107);
    tinysid_destroy(t);

    /* the bundled demo (Am F C G) is A minor or its relative, C major */
    t = tinysid_create(SR);
    tinysid_load(t, default_sid_data, default_sid_len);
    scan_to_end(t);
    printf("  demo tune: %s %s\n", getp(t, "song_key"), getp(t, "song_scale"));
    CHECK((tinysid_song_key(t) == 9 && tinysid_song_scale(t) == 1) || (tinysid_song_key(t) == 0 && tinysid_song_scale(t) == 0));
    CHECK_EQ(tinysid_song_bpm(t), 125);                    /* catalogued tempo is never overwritten by the scan */
    tinysid_destroy(t);

#ifdef TINYSID_ASSET_DIR
    {
        static uint8_t file[0x10400];
        FILE *fp = fopen(TINYSID_ASSET_DIR "/Commando.sid", "rb");
        size_t flen = fp ? fread(file, 1, sizeof(file), fp) : 0;
        if (fp) fclose(fp);
        if (flen > 0x7C) {
            t = tinysid_create(SR);
            tinysid_load(t, file, flen);
            scan_to_end(t);
            printf("  Commando subtune 1: key %s %s, %d BPM\n", getp(t, "song_key"), getp(t, "song_scale"), tinysid_song_bpm(t));
            CHECK(tinysid_song_key(t) != 12);
            CHECK_STR(getp(t, "song_key"), "A Min");        /* every method tried on this file says A minor, not D minor */
            CHECK_EQ(tinysid_song_bpm(t), 125);
            tinysid_destroy(t);
        }

        /* a tune that stays in one key reads that key in every window (local file, skipped when absent) */
        fp = fopen(TINYSID_ASSET_DIR "/Boulder_Dash.sid", "rb");
        flen = fp ? fread(file, 1, sizeof(file), fp) : 0;
        if (fp) fclose(fp);
        if (flen > 0x7C) {
            t = tinysid_create(SR);
            tinysid_load(t, file, flen);
            scan_to_end(t);
            printf("  Boulder Dash: key %s %s, %d BPM\n", getp(t, "song_key"), getp(t, "song_scale"), tinysid_song_bpm(t));
            CHECK_STR(getp(t, "song_key"), "D# Maj");
            CHECK_STR(getp(t, "song_scale"), "Major");
            tinysid_destroy(t);
        }
    }
#endif
}

/* ---- the tune's own instruments in the Pad Sound list ---------------------------- */

static void test_instruments(void)
{
    SECTION("pad sound: the tune's instruments, listed per voice");
    /* voice 1 plays pulse x3, saw x2, noise x1 per cycle: the list must rank them 1..3 by use */
    const int pattern[] = { 0, 0, 0, 1, 1, 2 };
    const uint8_t wave[] = { 0x41, 0x21, 0x81 }, ad[] = { 0x06, 0x00, 0x04 }, sr[] = { 0x98, 0xF8, 0x00 };
    size_t len = fx_instruments(fxbuf, pattern, 6, wave, ad, sr, 3, 1);

    tinysid_t *t = tinysid_create(SR);
    CHECK_EQ(tinysid_load(t, fxbuf, len), PSID_OK);
    CHECK_STR(getp(t, "is_loading"), "1");                 /* the scan is running: the host will re-read the list when it ends */
    char opts[512];
    CHECK(tinysid_pad_options(t, opts, sizeof(opts)) > 0);
    CHECK_STR(opts, "[\"Lead\"]");                          /* nothing known yet */
    scan_to_end(t);
    CHECK_STR(getp(t, "is_loading"), "0");
    CHECK(tinysid_pad_options(t, opts, sizeof(opts)) > 0);
    printf("  pad sounds: %s\n", opts);
    CHECK_STR(opts, "[\"1 Pul32 06 98\",\"2 Saw 00 F8\",\"3 Noi 04 00\"]");

    /* choose by name or by position */
    setp(t, "pad_sound", "2 Saw 00 f8");
    CHECK_STR(getp(t, "pad_sound"), "2 Saw 00 F8");
    CHECK_STR(getp(t, "pad_sound_index"), "1");
    setp(t, "pad_sound", "2");
    CHECK_STR(getp(t, "pad_sound"), "3 Noi 04 00");
    CHECK_EQ(tinysid_set_param(t, "pad_sound", "9 Tri FF FF"), -1);
    CHECK_STR(getp(t, "pad_sound"), "3 Noi 04 00");

    /* the pad plays the chosen instrument exactly, whatever the song itself is playing at that moment */
    setp(t, "pad_sound", "1 Pul32 06 98");
    setp(t, "pad_target", "Voice 1");
    int distinct = 0;
    uint8_t first = 0;
    for (int press = 0; press < 12; press++) {
        tinysid_render(t, L, R, 1300 + press * 211);       /* the song moves on between presses */
        tinysid_midi(t, NOTE_ON_60, 3);
        CHECK_EQ(tinysid_sid_reg(t, 4), 0x41);
        CHECK_EQ(tinysid_sid_reg(t, 5), 0x06);
        CHECK_EQ(tinysid_sid_reg(t, 6), 0x98);
        CHECK_EQ(tinysid_sid_reg(t, 2) | (tinysid_sid_reg(t, 3) << 8), 0x0523);
        if (press == 0) first = tinysid_sid_reg(t, 6);
        if (tinysid_sid_reg(t, 6) != first) distinct++;
        tinysid_midi(t, NOTE_OFF_60, 3);
    }
    CHECK_EQ(distinct, 0);

    setp(t, "pad_sound", "3 Noi 04 00");                   /* drums are on the list too: faithful, if not melodic */
    tinysid_midi(t, NOTE_ON_60, 3);
    CHECK_EQ(tinysid_sid_reg(t, 4), 0x81);
    CHECK_EQ(tinysid_sid_reg(t, 5), 0x04);
    CHECK_EQ(tinysid_sid_reg(t, 6), 0x00);
    tinysid_midi(t, NOTE_OFF_60, 3);

    setp(t, "pad_sound", "2 Saw 00 F8");
    tinysid_midi(t, NOTE_ON_60, 3);
    CHECK_EQ(tinysid_sid_reg(t, 4), 0x21);
    CHECK_EQ(tinysid_sid_reg(t, 5), 0x00);
    CHECK_EQ(tinysid_sid_reg(t, 6), 0xF8);
    tinysid_midi(t, NOTE_OFF_60, 3);

    /* the list belongs to the voice being played; voice 2 has no instruments in this tune */
    setp(t, "pad_target", "Voice 2");
    CHECK_STR(getp(t, "is_loading"), "1");                 /* asks the host to re-read the list ... */
    CHECK(tinysid_pad_options(t, opts, sizeof(opts)) > 0);
    CHECK_STR(opts, "[\"1 Pul32 06 98\",\"2 Saw 00 F8\",\"3 Noi 04 00\"]");   /* voice 2 is never gated: it borrows voice 1's */
    tinysid_render(t, L, R, 8000);
    CHECK_STR(getp(t, "is_loading"), "0");                 /* ... and only for a moment */
    setp(t, "pad_target", "Off");                          /* Off shows voice 1's list */
    CHECK(tinysid_pad_options(t, opts, sizeof(opts)) > 0);
    CHECK(strstr(opts, "1 Pul32 06 98") != NULL);

    /* a new file or subtune forgets the old list and goes back to the default sound */
    setp(t, "pad_sound", "2 Saw 00 F8");
    CHECK_STR(getp(t, "pad_sound_index"), "1");
    tinysid_load(t, fxbuf, len);
    CHECK_STR(getp(t, "pad_sound_index"), "0");
    CHECK(tinysid_pad_options(t, opts, sizeof(opts)) > 0);
    CHECK_STR(opts, "[\"Lead\"]");
    tinysid_destroy(t);

    /* a saved set can name an instrument before the scan that finds it has finished */
    t = tinysid_create(SR);
    tinysid_load(t, fxbuf, len);
    setp(t, "pad_sound", "2");
    CHECK_STR(getp(t, "pad_sound_index"), "2");
    CHECK_STR(getp(t, "pad_sound"), "Lead");               /* not known yet: plays the placeholder meanwhile */
    scan_to_end(t);
    CHECK_STR(getp(t, "pad_sound"), "3 Noi 04 00");        /* and picks it up when the list arrives */
    setp(t, "pad_target", "Voice 1");
    tinysid_render(t, L, R, 2000);
    tinysid_midi(t, NOTE_ON_60, 3);
    CHECK_EQ(tinysid_sid_reg(t, 4), 0x81);
    tinysid_destroy(t);

    /* an instrument that first appears late in the song (here 50 s in) is still found */
    {
        int late[251];
        for (int i = 0; i < 250; i++) late[i] = 0;
        late[250] = 1;
        const uint8_t lw[] = { 0x21, 0x41 }, lad[] = { 0x00, 0x06 }, lsr[] = { 0xF8, 0x98 };
        size_t llen = fx_instruments(fxbuf, late, 251, lw, lad, lsr, 2, 10);   /* each step lasts 10 frames */
        tinysid_t *u = tinysid_create(SR);
        tinysid_load(u, fxbuf, llen);
        scan_to_end(u);
        CHECK(tinysid_pad_options(u, opts, sizeof(opts)) > 0);
        printf("  late instrument: %s\n", opts);
        CHECK_STR(opts, "[\"1 Saw 00 F8\",\"2 Pul32 06 98\"]");
        tinysid_destroy(u);

        /* sounds that differ only in sustain level are one instrument: a driver's volume fade must not fill the list */
    {
        int fade[9] = { 0, 1, 2, 3, 4, 0, 1, 5, 6 };
        const uint8_t fw[] = { 0x11, 0x11, 0x11, 0x11, 0x11, 0x81, 0x41 };
        const uint8_t fad[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00 };
        const uint8_t fsr[] = { 0xB0, 0xA0, 0x80, 0x70, 0x50, 0x00, 0xB0 };   /* triangle at five volumes, a noise, a pulse */
        size_t flen = fx_instruments(fxbuf, fade, 9, fw, fad, fsr, 7, 20);
        tinysid_t *u = tinysid_create(SR);
        tinysid_load(u, fxbuf, flen);
        scan_to_end(u);
        CHECK(tinysid_pad_options(u, opts, sizeof(opts)) > 0);
        printf("  volume steps collapsed: %s\n", opts);
        CHECK_STR(opts, "[\"1 Tri 00 B0\",\"2 Noi 04 00\",\"3 Pul32 00 B0\"]");   /* the triangle, at its loudest, heard longest */
        tinysid_destroy(u);

        /* different release or attack is a different instrument */
        int two[2] = { 0, 1 };
        const uint8_t dw[] = { 0x21, 0x21 }, dad[] = { 0x00, 0x00 }, dsr[] = { 0xF8, 0xF2 };
        flen = fx_instruments(fxbuf, two, 2, dw, dad, dsr, 2, 20);
        u = tinysid_create(SR);
        tinysid_load(u, fxbuf, flen);
        scan_to_end(u);
        CHECK(tinysid_pad_options(u, opts, sizeof(opts)) > 0);
        CHECK(strstr(opts, "\"1 Saw 00 F") != NULL && strstr(opts, "\"2 Saw 00 F") != NULL);
        tinysid_destroy(u);
    }

    /* a sound that is held longer ranks higher: nine steps of A, one of B */
        int mix[10] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
        const uint8_t mw[] = { 0x21, 0x41 }, mad[] = { 0x00, 0x06 }, msr[] = { 0xF8, 0x98 };
        size_t mlen = fx_instruments(fxbuf, mix, 10, mw, mad, msr, 2, 30);
        u = tinysid_create(SR);
        tinysid_load(u, fxbuf, mlen);
        scan_to_end(u);
        CHECK(tinysid_pad_options(u, opts, sizeof(opts)) > 0);
        CHECK(strstr(opts, "\"1 Saw 00 F8\",\"2 Pul32 06 98\"") != NULL);
        tinysid_destroy(u);
    }

    /* at most INST_MAX entries, and the demo tune produces a sensible list */
    t = tinysid_create(SR);
    tinysid_load(t, default_sid_data, default_sid_len);
    scan_to_end(t);
    CHECK(tinysid_pad_options(t, opts, sizeof(opts)) > 0);
    printf("  demo tune, voice 1: %s\n", opts);
    CHECK(strstr(opts, "\"1 ") != NULL);
    tinysid_destroy(t);

#ifdef TINYSID_ASSET_DIR
    {
        static uint8_t file[0x10400];
        FILE *bp = fopen(TINYSID_ASSET_DIR "/Boulder_Dash.sid", "rb");
        size_t blen = bp ? fread(file, 1, sizeof(file), bp) : 0;
        if (bp) fclose(bp);
        if (blen > 0x7C) {                                 /* seven triangle volumes used to be seven entries */
            t = tinysid_create(SR);
            tinysid_load(t, file, blen);
            scan_to_end(t);
            CHECK(tinysid_pad_options(t, opts, sizeof(opts)) > 0);
            printf("  Boulder Dash voice 1: %s\n", opts);
            CHECK(strstr(opts, " 00 A0\"") == NULL && strstr(opts, " 00 90\"") == NULL);
            tinysid_destroy(t);
        }

        FILE *fp = fopen(TINYSID_ASSET_DIR "/Commando.sid", "rb");
        size_t flen = fp ? fread(file, 1, sizeof(file), fp) : 0;
        if (fp) fclose(fp);
        if (flen > 0x7C) {
            t = tinysid_create(SR);
            tinysid_load(t, file, flen);
            scan_to_end(t);
            for (int v = 1; v <= 3; v++) {
                char vs[4];
                snprintf(vs, sizeof(vs), "%d", v);
                setp(t, "pad_target", vs);
                CHECK(tinysid_pad_options(t, opts, sizeof(opts)) > 0);
                printf("  Commando voice %d: %s\n", v, opts);
                CHECK(strstr(opts, "\"1 ") != NULL);
                if (v == 1) {
                    const char *long_pulse = strstr(opts, " 38 7A\"");       /* the long sustained sound that first plays ~70 s in */
                    CHECK(long_pulse != NULL);
                    CHECK(long_pulse && long_pulse - opts < 120);           /* and it ranks among the first entries */
                }
            }
            tinysid_destroy(t);
        }
    }
#endif
}

/* ---- an instrument keeps the movement the song gives it ----------------------------- */

static void test_instrument_movement(void)
{
    SECTION("pad: the instrument's movement (pulse sweep, vibrato) plays on every key");
    /* the song's lead: pulse width sweeps through 16 values and the pitch trills +2% every other frame */
    uint16_t pw[16], freq[16];
    for (int i = 0; i < 16; i++) {
        pw[i] = (uint16_t)(0x300 + (i < 8 ? i : 15 - i) * 0x100);
        freq[i] = (uint16_t)(20000 + (i & 1) * 400);
    }
    size_t len = fx_movement(fxbuf, 16, pw, freq);
    tinysid_t *t = tinysid_create(SR);
    tinysid_load(t, fxbuf, len);
    scan_to_end(t);
    char opts[256];
    CHECK(tinysid_pad_options(t, opts, sizeof(opts)) > 0);
    printf("  moving lead: %s\n", opts);
    CHECK(strstr(opts, " 38 7A\"") != NULL);

    setp(t, "pad_target", "Voice 1");
    tinysid_render(t, L, R, 4410);
    int mc = middle_c_word();
    tinysid_midi(t, NOTE_ON_60, 3);
    CHECK_EQ(tinysid_pad_gesture_length(t), 64);           /* the whole recorded note */
    CHECK_EQ(sid_word(t, 0), mc);                          /* frame 0 is the key you pressed ... */
    CHECK_EQ(tinysid_sid_reg(t, 2) | (tinysid_sid_reg(t, 3) << 8), 0x300);   /* ... with the song's first pulse width */

    int seen_pw[16] = {0}, distinct_pw = 0, high = 0, low = 0;
    int first_pw[8], first_freq[8];
    for (int frame = 1; frame <= 150; frame++) {           /* well past the 64 recorded frames: it keeps going */
        tinysid_render(t, L, R, 882);                      /* exactly one song frame */
        int w = tinysid_sid_reg(t, 2) | (tinysid_sid_reg(t, 3) << 8), fr = sid_word(t, 0);
        int k = (w - 0x300) / 0x100;
        if (k >= 0 && k < 16 && !seen_pw[k]) { seen_pw[k] = 1; distinct_pw++; }
        if (frame <= 8) { first_pw[frame - 1] = w; first_freq[frame - 1] = fr; }
        if (fr == mc) low++;
        else if (fabs(fr / (double)mc - 1.02) < 0.002) high++;
        else CHECK(0);                                      /* the pitch only ever takes the two trill values */
    }
    CHECK(distinct_pw >= 6);                               /* the sweep is audible */
    CHECK(low > 50 && high > 50);                          /* and so is the trill */
    CHECK_EQ(first_pw[0], 0x400);                          /* frame 1 follows frame 0 as the song played it */
    CHECK_NEAR(first_freq[0] / (double)mc, 1.02, 0.002);
    CHECK_EQ(first_freq[1], mc);

    /* the same key plays the same movement every time ... */
    tinysid_midi(t, NOTE_OFF_60, 3);
    tinysid_midi(t, NOTE_ON_60, 3);
    CHECK_EQ(sid_word(t, 0), mc);
    CHECK_EQ(tinysid_sid_reg(t, 2) | (tinysid_sid_reg(t, 3) << 8), 0x300);
    for (int frame = 1; frame <= 8; frame++) {
        tinysid_render(t, L, R, 882);
        CHECK_EQ(tinysid_sid_reg(t, 2) | (tinysid_sid_reg(t, 3) << 8), first_pw[frame - 1]);
        CHECK_EQ(sid_word(t, 0), first_freq[frame - 1]);
    }

    /* ... relative to the key: an octave up, the trill is an octave up */
    const uint8_t on72[] = { 0x90, 72, 100 }, off72[] = { 0x80, 72, 0 };
    tinysid_midi(t, off72, 3);
    tinysid_midi(t, NOTE_OFF_60, 3);
    tinysid_midi(t, on72, 3);
    int c5 = sid_word(t, 0);
    CHECK_NEAR(c5 / (double)mc, 2.0, 0.001);
    tinysid_render(t, L, R, 882);
    CHECK_NEAR(sid_word(t, 0) / (double)c5, 1.02, 0.002);

    /* releasing the key freezes the movement while the release plays out */
    tinysid_midi(t, off72, 3);
    int frozen_pw = tinysid_sid_reg(t, 2) | (tinysid_sid_reg(t, 3) << 8), frozen_f = sid_word(t, 0);
    tinysid_render(t, L, R, 882 * 6);
    CHECK_EQ(tinysid_sid_reg(t, 2) | (tinysid_sid_reg(t, 3) << 8), frozen_pw);
    CHECK_EQ(sid_word(t, 0), frozen_f);

    /* it sounds, and nothing goes wrong in the output */
    tinysid_midi(t, NOTE_ON_60, 3);
    tinysid_render(t, L, R, 882 * 20);
    CHECK(all_finite_bounded(L, 882 * 20));
    CHECK(rms(L + 4000, 8000) > 0.015);
    tinysid_destroy(t);

    /* an instrument with no movement, or no scan yet, stays put */
    t = make(FX_PLAIN, 1, 0, 0x0004);
    setp(t, "pad_target", "Voice 2");
    tinysid_render(t, L, R, 4410);
    tinysid_midi(t, NOTE_ON_60, 3);
    CHECK_EQ(tinysid_pad_gesture_length(t), 0);            /* the placeholder lead has none */
    tinysid_render(t, L, R, 882 * 20);
    CHECK_EQ(sid_word(t, 7), middle_c_word());
    tinysid_destroy(t);

    /* the movement keeps its pace when the song's tempo is scaled */
    t = tinysid_create(SR);
    tinysid_load(t, fxbuf, len);
    len = fx_movement(fxbuf, 16, pw, freq);
    tinysid_load(t, fxbuf, len);
    scan_to_end(t);
    setp(t, "pad_target", "Voice 1");
    setp(t, "tempo_trim", "1.5");
    tinysid_render(t, L, R, 4410);
    tinysid_midi(t, NOTE_ON_60, 3);
    tinysid_render(t, L, R, 588);                          /* one frame at 75 Hz */
    CHECK_EQ(tinysid_sid_reg(t, 2) | (tinysid_sid_reg(t, 3) << 8), 0x400);
    tinysid_destroy(t);

#ifdef TINYSID_ASSET_DIR
    {
        static uint8_t file[0x10400];
        FILE *fp = fopen(TINYSID_ASSET_DIR "/Commando.sid", "rb");
        size_t flen = fp ? fread(file, 1, sizeof(file), fp) : 0;
        if (fp) fclose(fp);
        if (flen > 0x7C) {
            t = tinysid_create(SR);
            tinysid_load(t, file, flen);
            scan_to_end(t);
            CHECK(tinysid_pad_options(t, opts, sizeof(opts)) > 0);
            char label[40] = "";
            const char *at = strstr(opts, " 38 7A\"");     /* the long, sax-like lead on voice 1 */
            CHECK(at != NULL);
            if (at) {
                const char *start = at;
                while (start > opts && *(start - 1) != '"') start--;
                snprintf(label, sizeof(label), "%.*s", (int)(at - start) + 6, start);
            }
            setp(t, "pad_sound", label);
            setp(t, "pad_target", "Voice 1");
            tinysid_render(t, L, R, 4410);
            tinysid_midi(t, NOTE_ON_60, 3);
            int glen = tinysid_pad_gesture_length(t);
            int pws[64], npw = 0, distinct = 0, fr0 = sid_word(t, 0), fmin = fr0, fmax = fr0;
            for (int frame = 0; frame < 60; frame++) {
                tinysid_render(t, L, R, 882);
                int w = tinysid_sid_reg(t, 2) | (tinysid_sid_reg(t, 3) << 8), f = sid_word(t, 0), k;
                for (k = 0; k < npw; k++) if (pws[k] == w) break;
                if (k == npw && npw < 64) { pws[npw++] = w; distinct++; }
                if (f < fmin) fmin = f;
                if (f > fmax) fmax = f;
            }
            printf("  Commando \"%s\": %d recorded frames, %d different pulse widths and a pitch range of %.1f%% over 1.2 s\n",
                   label, glen, distinct, 100.0 * (fmax - fmin) / fr0);
            CHECK(glen >= 40);                              /* a real note's worth of movement was captured */
            CHECK(distinct >= 8);                           /* the pulse width wobbles, not a fixed 50% pulse */
            CHECK(fmax != fmin);                            /* and the pitch moves */
            tinysid_render(t, L, R, 22050);
            CHECK(all_finite_bounded(L, 22050));
            tinysid_destroy(t);
        }
    }
#endif
}

/* ---- voice snooper ----------------------------------------------------------------- */

static void test_snoop(void)
{
    SECTION("voice snooper: the note a voice is sounding");
    /* voice 1: A2 (110 Hz = $0751, gate on)   voice 2: A4 (440 Hz = $1D45, gate on)   voice 3: $1168, gate off */
    const int words[3] = { 0x0751, 0x1D45, 0x1168 };
    const int ctrls[3] = { 0x21, 0x21, 0x20 };
    size_t len = fx_snoop(fxbuf, words, ctrls, 0x0004);
    tinysid_t *t = tinysid_create(SR);
    CHECK_EQ(tinysid_load(t, fxbuf, len), PSID_OK);
    CHECK_STR(getp(t, "snoop_voice"), "Voice 2 (Lead)");   /* the default */
    CHECK_STR(getp(t, "snoop_note"), "69");
    tinysid_render(t, L, R, 128);
    CHECK_EQ(tinysid_snoop_note(t), 69);                   /* $1D45 on a PAL machine is A4 */

    setp(t, "snoop_voice", "Voice 1 (Bass)");
    CHECK_EQ(tinysid_snoop_note(t), 45);                   /* A2 */
    setp(t, "snoop_voice", "2");
    CHECK_EQ(tinysid_snoop_note(t), 0);                    /* gate low: idle */
    setp(t, "snoop_voice", "all voices");
    tinysid_render(t, L, R, 128);
    CHECK_EQ(tinysid_snoop_note(t), 69);                   /* the voice gated on most recently */
    CHECK_STR(getp(t, "snoop_voice"), "All Voices");
    CHECK_EQ(tinysid_set_param(t, "snoop_voice", "Voice 9"), -1);
    CHECK_STR(getp(t, "snoop_voice"), "All Voices");
    CHECK_EQ(tinysid_set_param(t, "snoop_note", "12"), 0); /* a readout: writes are dropped */
    CHECK_EQ(tinysid_snoop_note(t), 69);
    tinysid_destroy(t);

    /* $1168 is C4 (261.6 Hz), not A4: the brief's example word, taken at face value */
    const int words2[3] = { 0x0751, 0x1D45, 0x1168 };
    const int ctrls2[3] = { 0x20, 0x20, 0x21 };
    len = fx_snoop(fxbuf, words2, ctrls2, 0x0004);
    t = tinysid_create(SR);
    tinysid_load(t, fxbuf, len);
    setp(t, "snoop_voice", "Voice 3 (Arp/Drm)");
    tinysid_render(t, L, R, 128);
    CHECK_EQ(tinysid_snoop_note(t), 60);
    tinysid_destroy(t);

    /* noise has no pitch: idle; a waveform-less voice is idle too */
    const int ctrls3[3] = { 0x81, 0x01, 0x21 };
    len = fx_snoop(fxbuf, words, ctrls3, 0x0004);
    t = tinysid_create(SR);
    tinysid_load(t, fxbuf, len);
    setp(t, "snoop_voice", "0");
    tinysid_render(t, L, R, 128);
    CHECK_EQ(tinysid_snoop_note(t), 0);
    setp(t, "snoop_voice", "1");
    tinysid_render(t, L, R, 128);
    CHECK_EQ(tinysid_snoop_note(t), 0);
    setp(t, "snoop_voice", "All Voices");
    tinysid_render(t, L, R, 128);
    CHECK_EQ(tinysid_snoop_note(t), 60);                   /* only voice 3 is a pitched, gated voice: $1168 = C4 */
    tinysid_destroy(t);

    /* the file's own clock is used: the same word is a quarter-tone-ish higher on NTSC */
    len = fx_snoop(fxbuf, words, ctrls, 0x0008);
    t = tinysid_create(SR);
    tinysid_load(t, fxbuf, len);
    tinysid_render(t, L, R, 128);
    CHECK_EQ(tinysid_snoop_note(t), 70);                   /* 456.7 Hz */
    tinysid_destroy(t);

    /* a driver that gates a voice on and off shows notes and idle in turn */
    len = fx_tone(fxbuf, FX_GATER + 4, 1, 0, 0x0004);      /* voice 1, word $1000 (240.5 Hz = note 59), gated every 4th frame */
    t = tinysid_create(SR);
    tinysid_load(t, fxbuf, len);
    setp(t, "snoop_voice", "0");
    int saw_note = 0, saw_idle = 0;
    for (int frame = 0; frame < 24; frame++) {
        tinysid_render(t, L, R, 882);
        int n = tinysid_snoop_note(t);
        if (n == 59) saw_note++;
        else if (n == 0) saw_idle++;
        else CHECK(0);
    }
    CHECK(saw_note >= 4 && saw_idle >= 12);
    tinysid_destroy(t);

    /* pitch detail and the packed feed the piano roll reads: a voice sharp of A4 by 26 steps (6 cents), full envelope */
    const int sharp[3] = { 0x0751, 0x1D45 + 26, 0x1168 }, gates[3] = { 0x20, 0x21, 0x20 };
    len = fx_snoop(fxbuf, sharp, gates, 0x0004);
    t = tinysid_create(SR);
    tinysid_load(t, fxbuf, len);
    tinysid_render(t, L, R, 128);
    CHECK_STR(getp(t, "snoop_note"), "69");
    CHECK_STR(getp(t, "snoop_hz"), "441.6");
    CHECK_STR(getp(t, "snoop_cents"), "6");
    CHECK_STR(getp(t, "snoop_level"), "100");
    CHECK_STR(getp(t, "snoop_feed"), "K,69,1,24,6,100");     /* key 24 = not scanned yet */
    CHECK_EQ(tinysid_set_param(t, "snoop_feed", "K,1,1,1,1,1"), 0);        /* readouts ignore writes */
    CHECK_STR(getp(t, "snoop_feed"), "K,69,1,24,6,100");
    setp(t, "snoop_voice", "Voice 3 (Arp/Drm)");            /* gate low: idle, and the A4 that just sounded is still in the trail */
    CHECK_STR(getp(t, "snoop_feed"), "K,0,2,24,0,0,69");
    CHECK_STR(getp(t, "snoop_hz"), "0.0");
    tinysid_destroy(t);

    /* the trail: notes that sounded in the last 600 ms travel with the feed, newest first, then expire */
    t = make(FX_PLAIN, 1, 0, 0x0004);
    setp(t, "snoop_voice", "Voice 3 (Arp/Drm)");
    setp(t, "pad_target", "Voice 3");
    tinysid_render(t, L, R, 30000);                          /* let what the song played before the takeover age out of the trail */
    const uint8_t on64[] = { 0x90, 64, 100 }, off64[] = { 0x80, 64, 0 }, on67[] = { 0x90, 67, 100 }, off67[] = { 0x80, 67, 0 };
    tinysid_midi(t, NOTE_ON_60, 3); tinysid_render(t, L, R, 256); tinysid_midi(t, NOTE_OFF_60, 3);
    tinysid_midi(t, on64, 3);       tinysid_render(t, L, R, 256); tinysid_midi(t, off64, 3);
    tinysid_midi(t, on67, 3);       tinysid_render(t, L, R, 256);
    {
        const char *feed = getp(t, "snoop_feed");
        CHECK(strncmp(feed, "K,67,2,24,", 10) == 0);
        const char *tail = strrchr(feed, ',');
        CHECK(strstr(feed, ",64,60") != NULL && tail && !strcmp(tail, ",60"));   /* newest first: 64, then 60 */
        CHECK(strstr(feed, ",67,67") == NULL);                                      /* the sounding note is not repeated in its own trail */
    }
    tinysid_midi(t, off67, 3);
    tinysid_render(t, L, R, 128);
    CHECK_STR(getp(t, "snoop_note"), "0");
    tinysid_render(t, L, R, 44100 / 2);                      /* half a second more: 600 ms after the first note, the trail is gone */
    tinysid_render(t, L, R, 4410);
    CHECK_STR(getp(t, "snoop_feed"), "K,0,2,24,0,0");
    tinysid_destroy(t);

    /* once the scan has finished the feed carries the key (D# major = 6 * 2) */
    t = tinysid_create(SR);
    {
        FILE *fp = NULL;
        (void)fp;
        const int notes[16] = { 63, 67, 70, 75, 65, 67, 70, 75, 68, 72, 75, 70, 74, 77, 70, 75 };   /* E-flat major material */
        len = fx_melody(fxbuf, notes, 16);
        tinysid_load(t, fxbuf, len);
        scan_to_end(t);
        CHECK_STR(getp(t, "song_key"), "D# Maj");
        const char *feed = getp(t, "snoop_feed");
        char want[16];
        snprintf(want, sizeof(want), ",%d,", 3 * 2);               /* tonic D# = 3, major: key code 6 */
        CHECK(strstr(feed, want) != NULL);
    }
    tinysid_destroy(t);

    /* it follows what the chip is really playing, which includes the pad */
    t = make(FX_PLAIN, 1, 0, 0x0004);
    setp(t, "snoop_voice", "Voice 3 (Arp/Drm)");
    setp(t, "pad_target", "Voice 3");
    tinysid_render(t, L, R, 4410);
    tinysid_midi(t, NOTE_ON_60, 3);
    tinysid_render(t, L, R, 128);
    CHECK_EQ(tinysid_snoop_note(t), 60);
    tinysid_midi(t, NOTE_OFF_60, 3);
    tinysid_render(t, L, R, 128);
    CHECK_EQ(tinysid_snoop_note(t), 0);
    tinysid_destroy(t);
}

/* ---- the scan is headless and leaves the playing engine alone ----------------------- */

static void test_scan_headless(void)
{
    SECTION("pre-scan: headless, and the playing engine is not disturbed");
    static float a[128 * 40], b[128 * 40];

    tinysid_t *x = tinysid_create(SR), *y = tinysid_create(SR);
    tinysid_load(x, default_sid_data, default_sid_len);
    tinysid_load(y, default_sid_data, default_sid_len);
    CHECK_EQ(tinysid_scan_frames(y), 0);
    tinysid_scan_advance(y, 1500);                         /* the brief's 30 s, with no audio synthesis at all */
    CHECK_EQ(tinysid_scan_frames(y), 1500);
    CHECK_EQ(tinysid_analysis_done(y), 0);
    tinysid_scan_advance(y, 100000);                       /* and the rest */
    CHECK_EQ(tinysid_analysis_done(y), 1);
    CHECK_EQ(tinysid_scan_frames(y), 15000);
    tinysid_scan_advance(y, 10);                           /* harmless once finished */

    for (int i = 0; i < 40; i++) {
        tinysid_render(x, a + i * 128, R, 128);            /* x: the scan runs alongside the audio */
        tinysid_render(y, b + i * 128, R, 128);            /* y: the scan finished before the first sample */
    }
    CHECK(memcmp(a, b, sizeof(a)) == 0);                   /* identical audio */
    CHECK_EQ(tinysid_play_calls(x), tinysid_play_calls(y));
    int same_regs = 1;
    for (int r = 0; r < 25; r++) if (tinysid_sid_reg(x, r) != tinysid_sid_reg(y, r)) same_regs = 0;
    CHECK(same_regs);
    CHECK_EQ(tinysid_ram(x, 0x00FB), tinysid_ram(y, 0x00FB));      /* the demo's frame counter */
    tinysid_destroy(x);
    tinysid_destroy(y);

    /* the same results whether the scan is fed by audio blocks or run headlessly */
    x = tinysid_create(SR);
    y = tinysid_create(SR);
    tinysid_load(x, default_sid_data, default_sid_len);
    tinysid_load(y, default_sid_data, default_sid_len);
    scan_to_end(x);
    tinysid_scan_advance(y, 20000);
    CHECK_EQ(tinysid_song_key(x), tinysid_song_key(y));
    CHECK_EQ(tinysid_song_scale(x), tinysid_song_scale(y));
    CHECK_EQ(tinysid_song_bpm(x), tinysid_song_bpm(y));
    char ox[256], oy[256];
    tinysid_pad_options(x, ox, sizeof(ox));
    tinysid_pad_options(y, oy, sizeof(oy));
    CHECK_STR(ox, oy);
    tinysid_destroy(x);
    tinysid_destroy(y);
}

static uint32_t rng_state = 12345;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}

static void test_robustness(void)
{
    SECTION("robustness (NaN / Inf / hangs / garbage)");

    /* a play routine that never returns must not stall the audio thread */
    tinysid_t *t = make(FX_HANG, 1, 0, 0);
    tinysid_render(t, L, R, 4410);
    CHECK(all_finite_bounded(L, 4410));
    tinysid_destroy(t);

    /* sweep every extreme parameter combination over the default tune */
    t = tinysid_create(44100.0);
    tinysid_load(t, default_sid_data, default_sid_len);
    const char *models[] = { "6581", "8580" };
    const char *cut[] = { "-128", "0", "127" };
    const char *res[] = { "-15", "0", "15" };
    const char *trims[] = { "-18", "6" };
    int blocks = 0;
    for (int m = 0; m < 2; m++)
        for (int c = 0; c < 3; c++)
            for (int r = 0; r < 3; r++)
                for (int g = 0; g < 2; g++) {
                    setp(t, "chip_model", models[m]);
                    setp(t, "cutoff_bias", cut[c]);
                    setp(t, "res_bias", res[r]);
                    setp(t, "out_trim", trims[g]);
                    tinysid_render(t, L, R, 3000);
                    blocks++;
                    if (!all_finite_bounded(L, 3000) || !all_finite_bounded(R, 3000)) {
                        CHECK(0);
                        goto swept;
                    }
                }
    CHECK_EQ(blocks, 36);
swept:
    /* pad notes across the whole MIDI range, every target */
    for (int target = 1; target <= 3; target++) {
        char tv[4];
        snprintf(tv, sizeof(tv), "%d", target);
        setp(t, "pad_target", tv);
        for (int note = 0; note < 128; note += 7) {
            uint8_t on[3] = { 0x90, (uint8_t)note, 127 };
            tinysid_midi(t, on, 3);
            tinysid_render(t, L, R, 700);
            CHECK(all_finite_bounded(L, 700));
        }
    }
    tinysid_destroy(t);

    /* random code in a valid wrapper never crashes or emits non-finite audio */
    int bad = 0;
    for (int iter = 0; iter < 20; iter++) {
        uint8_t code[1500];
        for (size_t i = 0; i < sizeof(code); i++) code[i] = (uint8_t)rnd();
        static uint8_t big[0x7C + sizeof(code)];
        size_t len = fx_psid(big, code, sizeof(code), 0x1000, 0x1000, (iter & 1) ? 0x1100 : 0, 1, 0, 0);
        t = tinysid_create(44100.0);
        if (tinysid_load(t, big, len) == PSID_OK) {
            tinysid_render(t, L, R, 4000);
            if (!all_finite_bounded(L, 4000)) bad++;
        }
        tinysid_destroy(t);
    }
    CHECK_EQ(bad, 0);

    /* odd block sizes and a tiny / huge render */
    t = tinysid_create(48000.0);
    tinysid_load(t, default_sid_data, default_sid_len);
    int sizes[] = { 1, 2, 3, 7, 64, 127, 128, 129, 1000, 20000 };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        tinysid_render(t, L, R, sizes[i]);
        CHECK(all_finite_bounded(L, sizes[i]));
    }
    tinysid_render(t, L, R, 0);
    tinysid_render(t, L, R, -5);
    tinysid_destroy(t);
}

static void test_leaks(void)
{
    SECTION("allocation balance");
    void *(*old_m)(size_t) = tinysid_malloc;
    void (*old_f)(void *) = tinysid_free;
    tinysid_malloc = count_malloc;
    tinysid_free = count_free;
    g_live = g_total = 0;

    for (int i = 0; i < 50; i++) {
        tinysid_t *t = tinysid_create(44100.0);
        tinysid_load(t, default_sid_data, default_sid_len);
        tinysid_load(t, default_sid_data, default_sid_len);        /* reload frees the previous image */
        setp(t, "subtune", "2");
        setp(t, "pad_target", "Voice 1");
        tinysid_render(t, L, R, 512);
        tinysid_load(t, (const uint8_t *)"junk", 4);                /* failed load must not leak */
        tinysid_destroy(t);
    }
    CHECK_EQ(g_live, 0);
    CHECK(g_total > 0);
    printf("  %ld allocations, %ld live after teardown\n", g_total, g_live);

    tinysid_destroy(NULL);                                           /* tolerated */
    CHECK_EQ(g_live, 0);

    tinysid_malloc = old_m;
    tinysid_free = old_f;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);                      /* keep output if a test crashes */
    test_audio_sanity();
    test_voice_muting();
    test_param_clamping();
    test_register_overrides();
    test_pad_takeover();
    test_pad_sounds();
    test_pad_playing();
    test_free_modes();
    test_host_lock();
    test_phase_lock();
    test_tempo_trim();
    test_song_bpm();
    test_bpm_estimator();
    test_bpm_stability();
    test_transport();
    test_filter_override();
    test_key_detection();
    test_instruments();
    test_instrument_movement();
    test_snoop();
    test_scan_headless();
    test_robustness();
    test_leaks();
    return test_finish("test_engine");
}
