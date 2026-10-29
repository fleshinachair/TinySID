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
    CHECK(all_finite_bounded(R, 8192));
    CHECK(memcmp(L, R, 8192 * sizeof(float)) != 0);       /* the default spread (0.5) is stereo: voices 2 and 3 sit apart */
    tinysid_destroy(t);
    t = make(FX_PLAIN, 1, 0, 0);
    setp(t, "stereo_spread", "0");
    tinysid_render(t, L, R, 8192);
    CHECK(memcmp(L, R, 8192 * sizeof(float)) == 0);       /* spread 0: the mono sound, duplicated to both channels */
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
    CHECK_STR(getp(t, "song_bpm"), "0");                   /* not known yet: no fallback shown */
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

    /* a list too long for its room ends after the last whole name that fits, and is still a list */
    char tight[40];
    int tn = tinysid_pad_options(t, tight, 40);
    CHECK_EQ(tn, (int)strlen(tight));
    CHECK_STR(tight, "[\"1 Pul32 06 98\",\"2 Saw 00 F8\"]");     /* two names (34 bytes) fit in 40 with the brackets, three do not */
    tn = tinysid_pad_options(t, tight, 22);
    CHECK_STR(tight, "[\"1 Pul32 06 98\"]");
    tn = tinysid_pad_options(t, tight, 12);
    CHECK_STR(tight, "[\"Lead\"]");                           /* not even one name: the default, so the knob still has a choice */
    CHECK_EQ(tinysid_pad_options(t, tight, 8), -1);

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
    {
        /* three held notes are too few different pitches to say what the tuning is (it takes five), so the pad plays at A440 and the
         * song cannot move it from there */
        CHECK(abs(sid_word(t, 7) - middle_c_word()) <= 1);
        int before = sid_word(t, 7);
        tinysid_render(t, L, R, 882 * 20);
        CHECK_EQ(sid_word(t, 7), before);
    }
    setp(t, "pitch_snap", "A440 Chromatic");               /* with AutoTune the song is brought onto A440 and the pad is exact */
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
            CHECK(fmax - fmin <= 2);                        /* but its +224 cent swoop is not carried onto the notes you play (a word or two is the tuning settling) */
            tinysid_render(t, L, R, 22050);
            CHECK(all_finite_bounded(L, 22050));
            tinysid_destroy(t);
        }
    }
#endif
}

/* ---- pad notes: they stop, and they do not swoop --------------------------------------- */

static void note_on(tinysid_t *t, int note);
static void note_off(tinysid_t *t, int note);

static void test_pad_release_and_bend(void)
{
    SECTION("pad notes: a capped release, and no wide pitch movement");
    /* a lead whose pitch swings 8 semitones every other frame, and whose release is 10 (1.5 s): both are the song's own, and neither
     * belongs on a pad note */
    uint16_t pw[16], wide[16], vibrato[16];
    for (int i = 0; i < 16; i++) {
        pw[i] = (uint16_t)(0x300 + (i < 8 ? i : 15 - i) * 0x100);
        wide[i] = (uint16_t)(20000 + (i & 1) * 10000);        /* about 8 semitones */
        vibrato[i] = (uint16_t)(20000 + (i & 1) * 400);       /* about 34 cents */
    }
    for (int kind = 0; kind < 2; kind++) {
        size_t len = fx_movement(fxbuf, 16, pw, kind ? vibrato : wide);
        tinysid_t *t = tinysid_create(SR);
        CHECK_EQ(tinysid_load(t, fxbuf, len), PSID_OK);
        scan_to_end(t);
        setp(t, "pad_target", "Voice 1");
        tinysid_render(t, L, R, 4410);
        note_on(t, 60);
        CHECK_EQ(tinysid_sid_reg(t, 6), 0x78);              /* the song's SR $7A: sustain 7 kept, release 10 capped to 8 */
        int base = sid_word(t, 0), moved = 0, widest = 0;
        int pw_first = tinysid_sid_reg(t, 2) | (tinysid_sid_reg(t, 3) << 8), pw_changed = 0;
        for (int f = 0; f < 40; f++) {
            tinysid_render(t, L, R, 882);
            int w = sid_word(t, 0);
            if (w != base) moved++;
            if (abs(w - base) > widest) widest = abs(w - base);
            if ((tinysid_sid_reg(t, 2) | (tinysid_sid_reg(t, 3) << 8)) != pw_first) pw_changed = 1;
        }
        if (kind) {
            CHECK(moved > 10);                              /* a 34 cent trill stays: vibrato is part of the sound */
            CHECK(widest < base / 20);
        } else {
            CHECK_EQ(moved, 0);                             /* an 8 semitone swing is not carried onto every note you play */
        }
        CHECK(pw_changed);                                  /* the pulse width sweep is, either way */
        note_off(t, 60);
        CHECK_EQ(tinysid_sid_reg(t, 4) & 1, 0);
        tinysid_render(t, L, R, 44100 / 2);                 /* half a second after the key goes up: silent */
        tinysid_render(t, L, R, 4410);
        CHECK(rms(L, 4410) < 0.005);
        tinysid_destroy(t);
    }

    /* a note held across a switch of Pad Mode is let go, not left ringing for ever */
    tinysid_t *t = make(FX_PLAIN, 1, 0, 0);
    setp(t, "pad_target", "Voice 2");
    tinysid_render(t, L, R, 4410);
    note_on(t, 60);
    CHECK_EQ(tinysid_sid_reg(t, 0x0B) & 1, 1);
    setp(t, "pad_mode", "Control");
    CHECK_EQ(tinysid_sid_reg(t, 0x0B) & 1, 0);              /* its note-off will never arrive: it was let go here */
    note_off(t, 60);                                        /* (ignored in Control mode) */
    setp(t, "pad_mode", "Play");
    CHECK_EQ(tinysid_sid_reg(t, 0x0B) & 1, 0);
    note_on(t, 62);
    CHECK_EQ(tinysid_sid_reg(t, 0x0B) & 1, 1);
    note_off(t, 62);
    CHECK_EQ(tinysid_sid_reg(t, 0x0B) & 1, 0);
    tinysid_destroy(t);

    /* random note-on / note-off order never leaves a gate high once every key is up */
    t = make(FX_PLAIN, 1, 0, 0);
    setp(t, "pad_target", "Voice 3");
    tinysid_render(t, L, R, 4410);
    unsigned seed = 12345;
    int stuck = 0;
    for (int round = 0; round < 200; round++) {
        int held[8], n = 0;
        for (int i = 0; i < 6; i++) {
            seed = seed * 1103515245u + 12345u;
            int note = 48 + (int)((seed >> 16) % 12);
            note_on(t, note);
            held[n++] = note;
            tinysid_render(t, L, R, 100);
        }
        for (int i = n - 1; i >= 0; i--) {                  /* released in any order, some twice */
            seed = seed * 1103515245u + 12345u;
            int k = (int)((seed >> 16) % (unsigned)n);
            note_off(t, held[k]);
            note_off(t, held[i]);
        }
        for (int i = 0; i < n; i++) note_off(t, held[i]);
        if (tinysid_sid_reg(t, 0x12) & 1) stuck++;
    }
    CHECK_EQ(stuck, 0);
    tinysid_destroy(t);
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

/* ---- pad voice filter routing --------------------------------------------------- */

static void next_frame(tinysid_t *t);

/* A tune that sets $D417 (routing and resonance), $D418 (volume 15 plus `mode` bits) and a cutoff of $40, and never touches them again. */
static tinysid_t *make_routed(int d417, int mode)
{
    fx_code_t c = { {0}, 0 };
    fx_store(&c, 0x0F | mode, 0xD418);
    fx_store(&c, d417, 0xD417);
    fx_store(&c, 0x40, 0xD416);
    fx_byte(&c, 0x60);
    uint16_t play = (uint16_t)(FX_LOAD + c.n);
    fx_byte(&c, 0x60);
    size_t len = fx_psid(fxbuf, c.b, c.n, FX_LOAD, FX_LOAD, play, 1, 0, 0x0004);
    tinysid_t *t = tinysid_create(SR);
    if (!t || tinysid_load(t, fxbuf, len) != PSID_OK) { printf("fixture load failed\n"); exit(2); }
    return t;
}

static void test_pad_filter_route(void)
{
    SECTION("pad voice: Cutoff and Reso reach it when the song leaves the filter free");
    /* a tune that filters voices 1 and 3 ($55: resonance 5, routing %101) with a low-pass at cutoff $40 */
    tinysid_t *t = make_routed(0x55, 0x10);
    tinysid_render(t, L, R, 2048);
    CHECK_EQ(tinysid_sid_reg(t, 0x17), 0x55);

    setp(t, "pad_target", "2");
    tinysid_render(t, L, R, 2048);                         /* past the fade-out: the pad owns voice 2 */
    CHECK_EQ(tinysid_sid_reg(t, 0x17), 0x55);              /* the song uses its filter: the pad is not forced into it, and nothing else moves */
    CHECK_EQ(tinysid_sid_reg(t, 0x16), 0x40);              /* the tune's own filter is left alone */
    CHECK_EQ(tinysid_sid_reg(t, 0x18), 0x1F);
    setp(t, "res_bias", "3");
    CHECK_EQ(tinysid_sid_reg(t, 0x17), 0x85);              /* the knobs still work on the song's filter */
    setp(t, "cutoff_bias", "10");
    CHECK_EQ(tinysid_sid_reg(t, 0x16), 0x4A);
    setp(t, "res_bias", "0");
    setp(t, "cutoff_bias", "0");

    setp(t, "filter_mode", "Low Pass");                    /* an override routes everything anyway */
    CHECK_EQ(tinysid_sid_reg(t, 0x17) & 7, 7);
    setp(t, "filter_mode", "Song");
    CHECK_EQ(tinysid_sid_reg(t, 0x17), 0x55);

    setp(t, "pad_target", "Voice 3");
    tinysid_render(t, L, R, 2048);
    CHECK_EQ(tinysid_sid_reg(t, 0x17), 0x55);
    setp(t, "pad_target", "Voice 1");
    tinysid_render(t, L, R, 2048);
    CHECK_EQ(tinysid_sid_reg(t, 0x17), 0x55);
    setp(t, "pad_target", "Off");
    CHECK_EQ(tinysid_sid_reg(t, 0x17), 0x55);
    tinysid_destroy(t);

    /* a tune that does not use the filter (no mode): routing alone would silence the pad, so it gets an open low-pass */
    t = make_routed(0x80, 0x00);
    tinysid_render(t, L, R, 2048);
    CHECK_EQ(tinysid_sid_reg(t, 0x18), 0x0F);
    setp(t, "pad_target", "Voice 3");
    tinysid_render(t, L, R, 2048);
    CHECK_EQ(tinysid_sid_reg(t, 0x17), 0x84);
    CHECK_EQ(tinysid_sid_reg(t, 0x18), 0x1F);
    CHECK_EQ(tinysid_sid_reg(t, 0x16), 0xFF);
    setp(t, "cutoff_bias", "-100");                        /* Cutoff darkens the pad */
    CHECK_EQ(tinysid_sid_reg(t, 0x16), 0x9B);
    setp(t, "res_bias", "6");
    CHECK_EQ(tinysid_sid_reg(t, 0x17), 0xE4);
    setp(t, "cutoff_bias", "0");
    setp(t, "res_bias", "0");
    tinysid_midi(t, NOTE_ON_60, 3);
    tinysid_render(t, L, R, 6000);
    CHECK(rms(L + 3000, 3000) > 0.015);                    /* and the pad still sounds: routed, but through an open filter */
    tinysid_midi(t, NOTE_OFF_60, 3);
    setp(t, "pad_target", "Off");
    CHECK_EQ(tinysid_sid_reg(t, 0x17), 0x80);
    CHECK_EQ(tinysid_sid_reg(t, 0x18), 0x0F);              /* the tune's own filter state comes back */
    CHECK_EQ(tinysid_sid_reg(t, 0x16), 0x40);
    tinysid_destroy(t);

    /* the tune starts using the filter part-way through: the pad is handed back to the song's routing, and routed again when it stops */
    {
        fx_code_t c = { {0}, 0 };
        fx_store(&c, 0x0F, 0xD418);
        fx_store(&c, 0x80, 0xD417);
        fx_store(&c, 0x40, 0xD416);
        fx_byte(&c, 0x60);
        uint16_t play = (uint16_t)(FX_LOAD + c.n);
        static const uint8_t body[] = { 0xE6, 0x03, 0xA5, 0x03, 0x29, 0x10, 0x09, 0x0F, 0x8D, 0x18, 0xD4, 0x60 };   /* low-pass on for 16 frames in 32 */
        for (size_t i = 0; i < sizeof(body); i++) fx_byte(&c, body[i]);
        size_t len = fx_psid(fxbuf, c.b, c.n, FX_LOAD, FX_LOAD, play, 1, 0, 0x0004);
        t = tinysid_create(SR);
        CHECK_EQ(tinysid_load(t, fxbuf, len), PSID_OK);
        setp(t, "pad_target", "Voice 2");
        tinysid_render(t, L, R, 2048);
        int wrong_route = 0, seen_on = 0, seen_off = 0;
        for (int f = 0; f < 80; f++) {
            next_frame(t);
            int tune_filters = (tinysid_ram(t, 0x03) & 0x10) != 0;
            if (tinysid_sid_reg(t, 0x17) != (tune_filters ? 0x80 : 0x82)) wrong_route++;
            if (tune_filters) seen_on++; else seen_off++;
        }
        CHECK_EQ(wrong_route, 0);
        CHECK(seen_on > 10 && seen_off > 10);
        tinysid_destroy(t);
    }

    /* with Timbre Lock holding a filter the tune uses, the pad is not forced into it either */
    t = make_routed(0x55, 0x10);
    tinysid_render(t, L, R, 2048);
    setp(t, "timbre_lock", "Lock Timbre");
    setp(t, "pad_target", "2");
    tinysid_render(t, L, R, 2048);
    CHECK_EQ(tinysid_sid_reg(t, 0x17), 0x55);
    tinysid_destroy(t);
}

/* ---- pad voice isolation ---------------------------------------------------------- */

/* A tune that treats voice 1 and 2 roughly. Every frame the play routine writes voice 1: sync + ring + pulse + gate,
 * a counting frequency, then gate off with frequency 0; and voice 2, the pad's: control $47 (pulse, sync, ring, gate),
 * a counting frequency, AD/SR 0. With `filter` set it also runs a low-pass at cutoff 0 routed to voice 1 only, the way a
 * song's filter sweep closes on a drum hit. */
static tinysid_t *make_rough(int filter)
{
    fx_code_t c = { {0}, 0 };
    fx_store(&c, filter ? 0x1F : 0x0F, 0xD418);
    fx_store(&c, filter ? 0xF1 : 0x00, 0xD417);
    fx_byte(&c, 0x60);
    uint16_t play = (uint16_t)(FX_LOAD + c.n);
    fx_byte(&c, 0xE6); fx_byte(&c, 0x03);                         /* INC $03 */
    fx_byte(&c, 0xA5); fx_byte(&c, 0x03);                         /* LDA $03 */
    fx_byte(&c, 0x8D); fx_byte(&c, 0x00); fx_byte(&c, 0xD4);      /* STA $D400 */
    fx_byte(&c, 0x8D); fx_byte(&c, 0x07); fx_byte(&c, 0xD4);      /* STA $D407: the pad voice's own frequency */
    fx_byte(&c, 0x8D); fx_byte(&c, 0x08); fx_byte(&c, 0xD4);      /* STA $D408 */
    fx_store(&c, 0x47, 0xD404);                                   /* voice 1: pulse + ring + sync + gate */
    fx_store(&c, 0x47, 0xD40B);                                   /* voice 2 (the pad's): the same, trying to take it over */
    fx_store(&c, 0x00, 0xD40C);                                   /* ... and zero its envelope */
    fx_store(&c, 0x00, 0xD40D);
    fx_store(&c, 0x00, 0xD401);                                   /* voice 1: frequency zero, gate off */
    fx_store(&c, 0x00, 0xD400);
    fx_store(&c, 0x00, 0xD404);
    if (filter) fx_store(&c, 0x00, 0xD416);                       /* cutoff closed */
    fx_byte(&c, 0x60);
    size_t len = fx_psid(fxbuf, c.b, c.n, FX_LOAD, FX_LOAD, play, 1, 0, 0x0004);
    tinysid_t *t = tinysid_create(SR);
    if (!t || tinysid_load(t, fxbuf, len) != PSID_OK) { printf("fixture load failed\n"); exit(2); }
    return t;
}

static void test_pad_isolation(void)
{
    SECTION("pad voice: the song cannot touch it, whatever the other voices do");
    tinysid_t *t = make_rough(0);
    setp(t, "pad_target", "Voice 2");
    tinysid_render(t, L, R, 2048);
    tinysid_midi(t, NOTE_ON_60, 3);

    int bad_freq = 0, bad_ctrl = 0, bad_env = 0, silent = 0;
    for (int f = 0; f < 30; f++) {                         /* 30 frames of the tune doing its worst */
        tinysid_render(t, L, R, 882);
        if (sid_word(t, 7) != middle_c_word()) bad_freq++;
        int ctrl = tinysid_sid_reg(t, 0x0B);
        if ((ctrl & 0x0E) != 0 || (ctrl & 0x01) != 1 || (ctrl & 0xF0) == 0) bad_ctrl++;   /* no sync, ring or test; gate and a waveform */
        if (tinysid_sid_reg(t, 0x0C) != 0x00 || tinysid_sid_reg(t, 0x0D) != 0xF8) bad_env++;   /* the pad's own envelope */
        if (f > 0 && rms(L + 100, 700) < 0.015) silent++;      /* (the first block holds the note's own attack) */
    }
    CHECK_EQ(bad_freq, 0);                                 /* voice 1's frequency and gate (and the tune's own writes to voice 2) change nothing */
    CHECK_EQ(bad_ctrl, 0);
    CHECK_EQ(bad_env, 0);
    CHECK_EQ(silent, 0);

    /* the song's own voice 1 is still the song's: it carries on being written */
    CHECK_EQ(tinysid_sid_reg(t, 0x04), 0x00);
    CHECK_EQ(sid_word(t, 0), 0);
    tinysid_midi(t, NOTE_OFF_60, 3);
    tinysid_render(t, L, R, 882);
    CHECK_EQ(tinysid_sid_reg(t, 0x0B) & 1, 0);             /* releasing is the pad's call, not the song's */
    tinysid_destroy(t);

    /* the same on voice 3, and the pad target moved: voice 2 goes back to the song, which now drives it */
    t = make_rough(0);
    setp(t, "pad_target", "Voice 2");
    tinysid_render(t, L, R, 2048);
    setp(t, "pad_target", "Off");
    tinysid_render(t, L, R, 882);
    CHECK_EQ(tinysid_sid_reg(t, 0x0B), 0x47);              /* given back, with the tune's own control byte */
    tinysid_destroy(t);

    /* a song whose filter closes on every hit: the filter runs for the song's voices, and the pad is not shut in it */
    t = make_rough(1);
    setp(t, "pad_target", "Voice 2");
    tinysid_render(t, L, R, 2048);
    tinysid_midi(t, NOTE_ON_60, 3);
    silent = 0;
    for (int f = 0; f < 30; f++) {
        tinysid_render(t, L, R, 882);
        if (f > 0 && rms(L + 100, 700) < 0.015) silent++;
    }
    CHECK_EQ(tinysid_sid_reg(t, 0x16), 0x00);              /* the song's filter really is closed ... */
    CHECK_EQ(tinysid_sid_reg(t, 0x17) & 0xF1, 0xF1);       /* ... and still routes the song's voice 1 */
    CHECK_EQ(silent, 0);                                   /* ... but the pad note is heard */
    tinysid_destroy(t);
}

/* ---- timbre lock ----------------------------------------------------------------- */

/* Pulse width, waveform / gate and envelope of each voice, then cutoff lo / hi, resonance + routing, mode + volume. */
static const int k_timbre_regs[] = { 2, 3, 4, 5, 6, 9, 10, 11, 12, 13, 16, 17, 18, 19, 20, 0x15, 0x16, 0x17, 0x18 };
#define TIMBRE_N ((int)(sizeof(k_timbre_regs) / sizeof(k_timbre_regs[0])))

static void timbre_read(tinysid_t *t, int out[TIMBRE_N])
{
    for (int i = 0; i < TIMBRE_N; i++) out[i] = tinysid_sid_reg(t, k_timbre_regs[i]);
}

/* How many timbre registers differ from `want`; the gate bit of a control register and the volume nibble belong to the tune. */
static int timbre_diffs(tinysid_t *t, const int want[TIMBRE_N])
{
    int n = 0;
    for (int i = 0; i < TIMBRE_N; i++) {
        int reg = k_timbre_regs[i], mask = 0xFF;
        if (reg < 21 && reg % 7 == 4) mask = 0xFE;
        if (reg == 0x18) mask = 0xF0;
        if ((tinysid_sid_reg(t, reg) & mask) != (want[i] & mask)) n++;
    }
    return n;
}

static void load_into(tinysid_t *t, size_t len)
{
    if (tinysid_load(t, fxbuf, len) != PSID_OK) { printf("fixture load failed\n"); exit(2); }
}

static void test_timbre_lock(void)
{
    SECTION("timbre lock: the sound outlives the tune");
    tinysid_t *t = make(FX_WRITER, 2, 0, 0x0004);
    CHECK_STR(getp(t, "timbre_lock"), "Unlocked");
    CHECK_EQ(tinysid_set_param(t, "timbre_lock", "maybe"), -1);
    CHECK_EQ(tinysid_set_param(t, "timbre_lock", "Lock Timbre"), 0);
    CHECK_STR(getp(t, "timbre_lock"), "Lock Timbre");
    setp(t, "timbre_lock", "0");
    CHECK_STR(getp(t, "timbre_lock"), "Unlocked");

    /* tune A: saw voices (AD $20, SR $E8), filter routing $80, cutoff $40 */
    tinysid_render(t, L, R, 4096);
    int a[TIMBRE_N];
    timbre_read(t, a);
    CHECK_EQ(a[2], 0x21);
    CHECK_EQ(a[3], 0x20);
    CHECK_EQ(a[4], 0xE8);
    CHECK_EQ(tinysid_sid_reg(t, 0x16), 0x40);
    CHECK_EQ(tinysid_sid_reg(t, 0x17), 0x80);
    setp(t, "timbre_lock", "Lock Timbre");
    CHECK_EQ(timbre_diffs(t, a), 0);                       /* locking changes nothing by itself */

    /* a subtune reset resets the chip and the 6502, and the sound is still there */
    setp(t, "subtune", "2");
    CHECK_EQ(timbre_diffs(t, a), 0);
    tinysid_render(t, L, R, 4096);
    CHECK_EQ(timbre_diffs(t, a), 0);
    CHECK_EQ(tinysid_sid_reg(t, 0x01), 0x99);              /* the tune's own pitch writes still land */

    /* the same reset unlocked: the tune's cutoff write is back to its init state (the chip was cleared, $D416 never written) */
    tinysid_t *u = make(FX_WRITER, 2, 0, 0x0004);
    tinysid_render(u, L, R, 4096);
    setp(u, "subtune", "2");
    CHECK_EQ(tinysid_sid_reg(u, 0x16), 0x00);
    tinysid_destroy(u);

    /* tune B: another SID with a pulse voice (PW $0523, AD $09, SR $A5, then a noise voice), loaded over the top */
    static uint8_t fx2[0x1000];
    const int pattern[] = { 0, 1 };
    const uint8_t wave[] = { 0x41, 0x81 }, ad[] = { 0x09, 0x5A }, sr[] = { 0xA5, 0x37 };
    size_t len = fx_instruments(fx2, pattern, 2, wave, ad, sr, 2, 2);
    memcpy(fxbuf, fx2, len);
    load_into(t, len);
    CHECK_EQ(timbre_diffs(t, a), 0);                       /* straight after the load, before B has played a frame */
    tinysid_render(t, L, R, 8192);
    CHECK_EQ(timbre_diffs(t, a), 0);                       /* and while B plays, whatever it writes to the timbre registers */
    CHECK_EQ(tinysid_sid_reg(t, 0x01), 0x10);              /* B's pitch, not A's */
    CHECK_EQ(tinysid_sid_reg(t, 0x04) & 1, 1);             /* B gated voice 1 on */
    CHECK_EQ(tinysid_ram(t, 0x06) < 2, 1);                 /* ... and B is the tune running */

    /* control: unlocked, B sounds like B */
    setp(t, "timbre_lock", "Unlocked");
    CHECK_EQ(tinysid_sid_reg(t, 0x02), 0x23);
    CHECK_EQ(tinysid_sid_reg(t, 0x03), 0x05);
    CHECK(tinysid_sid_reg(t, 0x04) == 0x41 || tinysid_sid_reg(t, 0x04) == 0x81);
    CHECK(timbre_diffs(t, a) > 0);

    /* locking again takes a new snapshot: B's sound is now what is held */
    int b[TIMBRE_N];
    timbre_read(t, b);
    setp(t, "timbre_lock", "Lock Timbre");
    CHECK_EQ(timbre_diffs(t, b), 0);

    /* tune C kills voice 2 (control, AD, SR = 0): the gate follows the tune, the held waveform stays, the zeros are the tune's */
    len = fx_tone(fxbuf, FX_KILLV2, 1, 0, 0x0004);
    load_into(t, len);
    tinysid_render(t, L, R, 4096);
    tinysid_destroy(t);

    t = make(FX_WRITER, 1, 0, 0x0004);
    tinysid_render(t, L, R, 4096);
    timbre_read(t, a);
    setp(t, "timbre_lock", "Lock Timbre");
    len = fx_tone(fxbuf, FX_KILLV2, 1, 0, 0x0004);
    load_into(t, len);
    tinysid_render(t, L, R, 4096);
    CHECK_EQ(tinysid_sid_reg(t, 0x0B), 0x20);              /* gate off (the tune's), saw kept (the lock's) */
    CHECK_EQ(tinysid_sid_reg(t, 0x0C), 0x00);              /* the tune wrote AD and SR as zeros (a hard restart): that gets through */
    CHECK_EQ(tinysid_sid_reg(t, 0x0D), 0x00);
    CHECK_EQ(tinysid_sid_reg(t, 0x04), 0x21);              /* voices 1 and 3 still gate on */
    setp(t, "timbre_lock", "Unlocked");
    CHECK_EQ(tinysid_sid_reg(t, 0x0B), 0x00);              /* unlocking gives the tune its own registers back */
    CHECK_EQ(tinysid_sid_reg(t, 0x0C), 0x00);
    CHECK_EQ(tinysid_sid_reg(t, 0x0D), 0x00);
    tinysid_destroy(t);

    /* a voice with no waveform when the lock was taken is not held: it passes the next tune through */
    t = make(FX_KILLV2, 1, 0, 0x0004);
    tinysid_render(t, L, R, 4096);
    setp(t, "timbre_lock", "Lock Timbre");
    len = fx_tone(fxbuf, FX_PLAIN, 1, 0, 0x0004);
    load_into(t, len);
    tinysid_render(t, L, R, 4096);
    CHECK_EQ(tinysid_sid_reg(t, 0x0B), 0x21);
    CHECK_EQ(tinysid_sid_reg(t, 0x0C), 0x20);
    tinysid_destroy(t);

    /* the filter knobs still work on top of a locked filter, and master volume is the tune's */
    t = make(FX_WRITER, 1, 0, 0x0004);
    tinysid_render(t, L, R, 4096);
    setp(t, "timbre_lock", "Lock Timbre");
    setp(t, "cutoff_bias", "10");
    CHECK_EQ(tinysid_sid_reg(t, 0x16), 0x4A);
    tinysid_render(t, L, R, 4096);
    CHECK_EQ(tinysid_sid_reg(t, 0x16), 0x4A);
    setp(t, "cutoff_bias", "0");
    setp(t, "timbre_lock", "Unlocked");
    tinysid_destroy(t);
}

/* ---- timbre lock and the tune's hard restart ------------------------------------------ */

/* A voice-1 driver in two-frame notes. Odd frames: the instrument (`wave`, AD, SR) and the gate on. Even frames: a hard
 * restart: AD and SR written as 0 with the gate off, so the envelope rate counter is small when the next gate comes. */
static size_t fx_hardrestart(uint8_t *out, int wave, int ad, int sr)
{
    fx_code_t c = { {0}, 0 };
    fx_store(&c, 0x0F, 0xD418);
    fx_store(&c, 0x10, 0xD401);
    fx_store(&c, 0x08, 0xD403);                            /* pulse width 50% */
    fx_byte(&c, 0x60);
    uint16_t play = (uint16_t)(FX_LOAD + c.n);
    const uint8_t body[] = {
        0xE6, 0x03, 0xA5, 0x03, 0x29, 0x01, 0xD0, 0x0E,                   /* INC $03 ; LDA $03 ; AND #1 ; BNE odd */
        0xA9, 0x00, 0x8D, 0x05, 0xD4, 0x8D, 0x06, 0xD4,                   /* LDA #0 ; STA $D405 ; STA $D406 */
        0xA9, (uint8_t)(wave & 0xFE), 0x8D, 0x04, 0xD4, 0x60,             /* LDA #wave ; STA $D404 (gate off) ; RTS */
        0xA9, (uint8_t)ad, 0x8D, 0x05, 0xD4,                              /* odd: LDA #ad ; STA $D405 */
        0xA9, (uint8_t)sr, 0x8D, 0x06, 0xD4,                              /* LDA #sr ; STA $D406 */
        0xA9, (uint8_t)(wave | 1), 0x8D, 0x04, 0xD4, 0x60                 /* LDA #wave|gate ; STA $D404 ; RTS */
    };
    for (size_t i = 0; i < sizeof(body); i++) fx_byte(&c, body[i]);
    return fx_psid(out, c.b, c.n, FX_LOAD, FX_LOAD, play, 1, 0, 0x0004);
}

static void test_timbre_hard_restart(void)
{
    SECTION("timbre lock: the tune's hard restart gets through, so short notes still start");
    tinysid_t *t = tinysid_create(SR);
    size_t len = fx_hardrestart(fxbuf, 0x40, 0x09, 0xA5);
    CHECK_EQ(tinysid_load(t, fxbuf, len), PSID_OK);
    for (int i = 0; i < 4; i++) next_frame(t);
    setp(t, "timbre_lock", "Lock Timbre");                 /* locks pulse, AD $09, SR $A5, however the registers look this instant */

    len = fx_hardrestart(fxbuf, 0x20, 0x05, 0x5A);         /* another tune: a saw with its own envelope */
    CHECK_EQ(tinysid_load(t, fxbuf, len), PSID_OK);
    int restart_ok = 0, restart_bad = 0, note_ok = 0, note_bad = 0;
    for (int f = 0; f < 12; f++) {
        next_frame(t);
        int ad = tinysid_sid_reg(t, 5), sr = tinysid_sid_reg(t, 6), ctrl = tinysid_sid_reg(t, 4);
        if (ctrl & 1) {                                    /* a note frame: the held instrument, the tune's gate */
            if (ad == 0x09 && sr == 0xA5 && (ctrl & 0xF0) == 0x40) note_ok++; else note_bad++;
        } else {                                           /* a hard-restart frame: the tune's zeros, not the held envelope */
            if (ad == 0x00 && sr == 0x00) restart_ok++; else restart_bad++;
        }
    }
    CHECK(restart_ok >= 5 && note_ok >= 5);
    CHECK_EQ(restart_bad, 0);
    CHECK_EQ(note_bad, 0);

    /* and the notes really sound (an envelope left at the end of a long release would swallow them) */
    setp(t, "snoop_voice", "Voice 1 (Bass)");
    int lively = 0;
    for (int f = 0; f < 20; f++) {
        next_frame(t);
        tinysid_render(t, L, R, 441);
        if (rms(L, 441) > 0.01) lively++;
    }
    CHECK(lively >= 15);                                   /* every window sounds; an attack stalled by the delay bug would leave gaps */
    tinysid_destroy(t);

    /* the real thing, when it is here: locking Commando at any moment must leave it about as loud as it was */
    FILE *f = fopen(TINYSID_ASSET_DIR "/Commando.sid", "rb");
    if (f) {
        fclose(f);
        tinysid_t *a = tinysid_create(SR), *b = tinysid_create(SR);
        CHECK_EQ(tinysid_load_file(a, TINYSID_ASSET_DIR "/Commando.sid"), PSID_OK);
        CHECK_EQ(tinysid_load_file(b, TINYSID_ASSET_DIR "/Commando.sid"), PSID_OK);
        static float La[2048], Ra[2048], Lb[2048], Rb[2048];
        double sum = 0, worst = 1e9;
        int n = 0;
        for (int step = 0; step < 14; step++) {
            for (int i = 0; i < 40; i++) { tinysid_render(a, La, Ra, 1024); tinysid_render(b, Lb, Rb, 1024); }
            tinysid_set_param(b, "timbre_lock", "Lock Timbre");
            double ra = 0, rb = 0;
            for (int i = 0; i < 20; i++) { tinysid_render(a, La, Ra, 2048); tinysid_render(b, Lb, Rb, 2048); ra += rms(La, 2048); rb += rms(Lb, 2048); }
            tinysid_set_param(b, "timbre_lock", "Unlocked");
            for (int i = 0; i < 20; i++) { tinysid_render(a, La, Ra, 2048); tinysid_render(b, Lb, Rb, 2048); }
            double r = rb / (ra > 1e-9 ? ra : 1e-9);
            sum += r; n++;
            if (r < worst) worst = r;
        }
        printf("  Commando, locked at %d moments: loudness vs unlocked avg %.2f, worst %.2f\n", n, sum / n, worst);
        CHECK(sum / n > 0.85);
        CHECK(worst > 0.3);                                /* it was 0.00: voices held at AD/SR 0, or the attack swallowed */
        tinysid_destroy(a);
        tinysid_destroy(b);
    }
}

/* ---- phrase loop ---------------------------------------------------------------------- */

/* FX_COUNTER plays a different value every frame (zero-page $03 counts up and is mirrored into the voices' frequency
 * high bytes), titled so the Host Lock row length is known: 125 BPM, 6 frames a row, one frame a tick, 96 frames a bar. */
static tinysid_t *make_counter(void)
{
    size_t len = fx_tone(fxbuf, FX_COUNTER, 1, 0, 0x0004);
    fx_set_title(fxbuf, "Commando");
    tinysid_t *t = tinysid_create(SR);
    if (!t || tinysid_load(t, fxbuf, len) != PSID_OK) { printf("fixture load failed\n"); exit(2); }
    setp(t, "sync_mode", "Host Lock 1x");
    tinysid_midi(t, &MIDI_START, 1);
    return t;
}

/* one clock tick at 125 BPM: the tick, then the 882 samples up to the next */
static void tick_send(tinysid_t *t) { tinysid_midi(t, &MIDI_CLOCK, 1); }
static void tick_run(tinysid_t *t) { tinysid_render(t, L, R, 882); }

static int zero_page_equals(tinysid_t *t, const uint8_t ref[256])
{
    for (int i = 0; i < 256; i++)
        if (tinysid_ram(t, (uint16_t)i) != ref[i]) return 0;
    return 1;
}

static void test_bar_loop(void)
{
    SECTION("bar looper: a bounded region of the song repeats");
    enum { BAR = 96 };
    tinysid_t *t = make_counter();

    /* parameters */
    CHECK_STR(getp(t, "loop_start"), "1");
    CHECK_STR(getp(t, "loop_length"), "Off");
    CHECK_EQ(tinysid_set_param(t, "loop_length", "16 Bars"), -1);
    CHECK_EQ(tinysid_set_param(t, "loop_start", "junk"), -1);
    setp(t, "loop_start", "0");   CHECK_STR(getp(t, "loop_start"), "1");        /* clamped to 1..256 */
    setp(t, "loop_start", "999"); CHECK_STR(getp(t, "loop_start"), "256");
    setp(t, "loop_start", "3.4"); CHECK_STR(getp(t, "loop_start"), "3");
    const char *lens[] = { "Off", "1 Bar", "2 Bars", "3 Bars", "4 Bars", "5 Bars", "6 Bars", "7 Bars", "8 Bars" };
    for (int i = 0; i < 9; i++) { setp(t, "loop_length", lens[i]); CHECK_STR(getp(t, "loop_length"), lens[i]); }
    setp(t, "loop_length", "4");  CHECK_STR(getp(t, "loop_length"), "4 Bars");    /* by position too */
    CHECK_EQ(tinysid_song_bar(t), 0);                      /* no tick yet */
    tinysid_destroy(t);

    /* loop_start 3, 2 Bars: bars 1 and 2 play through, the snapshot is taken at bar 3, and bar 5 rewinds to bar 3 */
    t = make_counter();
    setp(t, "loop_start", "3");
    setp(t, "loop_length", "2 Bars");
    uint8_t zp0[256];
    for (int i = 0; i < 256; i++) zp0[i] = tinysid_ram(t, (uint16_t)i);
    uint16_t pc0 = tinysid_cpu_pc(t);
    int trace[4][2 * BAR];                                 /* the register trace of bars 1-2, then three passes of bars 3-4 */
    uint8_t zp_at_start[256];
    uint16_t pc_at_start = 0;

    for (int k = 0; k < 2 * BAR; k++) {                    /* bars 1 and 2 */
        tick_send(t);
        CHECK(!tinysid_phrase_armed(t));
        CHECK_EQ(tinysid_song_bar(t), k / BAR + 1);
        tick_run(t);
        trace[0][k] = tinysid_sid_reg(t, 0x01);
    }
    CHECK_EQ(tinysid_phrase_rewinds(t), 0);
    CHECK_EQ(tinysid_ram(t, 0x03), 2 * BAR);               /* the song played straight through: 192 frames */
    CHECK(zero_page_equals(t, zp0) == 0);

    for (int pass = 1; pass <= 3; pass++) {
        for (int k = 0; k < 2 * BAR; k++) {
            tick_send(t);
            if (k == 0) {                                  /* the downbeat of bar 3, first by the song and then by the loop */
                CHECK(tinysid_phrase_armed(t));
                CHECK_EQ(tinysid_song_bar(t), 3);
                CHECK_EQ(tinysid_phrase_rewinds(t), pass - 1);
                if (pass == 1) {
                    for (int i = 0; i < 256; i++) zp_at_start[i] = tinysid_ram(t, (uint16_t)i);
                    pc_at_start = tinysid_cpu_pc(t);
                    CHECK_EQ(zp_at_start[0x03], 2 * BAR);  /* caught at the top of bar 3, 192 frames in */
                } else {
                    CHECK(zero_page_equals(t, zp_at_start));   /* rewound from bar 5: zero page and PC are bar 3's again */
                    CHECK_EQ(tinysid_cpu_pc(t), pc_at_start);
                }
            }
            if (k == BAR) CHECK_EQ(tinysid_song_bar(t), 4);
            tick_run(t);
            trace[pass][k] = tinysid_sid_reg(t, 0x01);
        }
        CHECK_EQ(tinysid_ram(t, 0x03) & 0xFF, (4 * BAR) & 0xFF);   /* 192 + 192 frames, counter wrapped, before the rewind */
    }
    CHECK_EQ(pc_at_start, pc0);
    int same = 0, linear = 1;
    for (int k = 0; k < 2 * BAR; k++) {
        if (trace[0][k] != ((k + 1) & 0xFF)) linear = 0;
        if (trace[1][k] == trace[2][k] && trace[2][k] == trace[3][k]) same++;
        if (trace[1][k] != ((2 * BAR + k + 1) & 0xFF)) linear = 0;   /* the first pass of the region is the song itself */
    }
    CHECK(linear);                                         /* bars 1-4 are the song: frame 1, 2, 3 ... 384 */
    CHECK_EQ(same, 2 * BAR);                               /* every repeat of bars 3-4 plays identical frames */
    CHECK_EQ(tinysid_phrase_rewinds(t), 2);
    tick_send(t);                                          /* bar 5 again */
    CHECK_EQ(tinysid_phrase_rewinds(t), 3);
    CHECK_EQ(tinysid_song_bar(t), 3);                      /* song time is back at bar 3 */
    CHECK_EQ(tinysid_play_calls(t), (uint64_t)(2 * BAR + 3 * 2 * BAR));   /* the clock never stopped: no frame skipped or doubled */

    /* Off lets the song run on past the end of the region; switched back on, it jumps back to bar 3 at the next bar line */
    tick_run(t);
    setp(t, "loop_length", "Off");
    for (int k = 1; k < 3 * BAR + 40; k++) { tick_send(t); tick_run(t); }
    CHECK_EQ(tinysid_phrase_rewinds(t), 3);
    CHECK_EQ(tinysid_song_bar(t), 6);                      /* bar 5 came and went */
    CHECK(tinysid_phrase_armed(t));                        /* the snapshot is kept */
    setp(t, "loop_length", "2 Bars");
    for (int k = 3 * BAR + 40; k < 4 * BAR; k++) { tick_send(t); tick_run(t); }
    CHECK_EQ(tinysid_phrase_rewinds(t), 3);
    tick_send(t);                                          /* the top of bar 7: past the end of the region, so back to bar 3 */
    CHECK_EQ(tinysid_phrase_rewinds(t), 4);
    CHECK_EQ(tinysid_song_bar(t), 3);

    /* a new start bar is a new phrase: it is caught on the next run, not mid-song */
    setp(t, "loop_start", "2");
    CHECK(!tinysid_phrase_armed(t));
    for (int k = 0; k < 3 * BAR; k++) { tick_send(t); tick_run(t); }
    CHECK(!tinysid_phrase_armed(t));                       /* bar 2 has gone by: nothing to catch */
    CHECK_EQ(tinysid_phrase_rewinds(t), 4);
    tinysid_midi(t, &MIDI_START, 1);                       /* a new run: bar 1 again */
    CHECK(!tinysid_phrase_armed(t));
    CHECK_EQ(tinysid_song_bar(t), 0);
    for (int k = 0; k < BAR; k++) { tick_send(t); tick_run(t); CHECK(!tinysid_phrase_armed(t)); }
    tick_send(t);                                          /* top of bar 2 */
    CHECK(tinysid_phrase_armed(t));
    CHECK_EQ(tinysid_song_bar(t), 2);
    tinysid_destroy(t);

    /* the default region starts at bar 1: switching a loop on later still jumps back to the top of the song */
    t = make_counter();
    setp(t, "loop_length", "Off");
    for (int k = 0; k < 150; k++) { tick_send(t); tick_run(t); }
    CHECK(tinysid_phrase_armed(t));                        /* bar 1 was caught although the loop was off */
    setp(t, "loop_length", "1 Bar");
    for (int k = 150; k < 2 * BAR; k++) { tick_send(t); tick_run(t); }
    tick_send(t);                                          /* bar 3's downbeat: past bar 2 (= 1 + 1), back to bar 1 */
    CHECK_EQ(tinysid_phrase_rewinds(t), 1);
    CHECK_EQ(tinysid_song_bar(t), 1);
    CHECK_EQ(tinysid_ram(t, 0x03), 0);
    tinysid_destroy(t);

    /* without a clock there are no bar lines: the song runs free */
    t = make_counter();
    setp(t, "loop_length", "1 Bar");
    setp(t, "sync_mode", "Free (PAL 50Hz)");
    for (int i = 0; i < 40; i++) tinysid_render(t, L, R, 4410);
    CHECK(!tinysid_phrase_armed(t));
    CHECK_EQ(tinysid_phrase_rewinds(t), 0);
    CHECK(tinysid_play_calls(t) > 150);
    tinysid_destroy(t);
}

/* ---- bar looper: the tune's bar, the downbeat nudge, the seam ---------------------------------- */

static void test_loop_alignment(void)
{
    SECTION("bar looper: loops follow the tune's own 16-row bar");
    enum { BAR = 96 };

    /* the bar is 16 ROWS of the tune: 96 ticks at 1x, but 192 at 0.5x and 48 at 2x. Every bar is 96 frames either way. */
    const struct { const char *mode; int ticks; } modes[] = { { "Host Lock 1x", 96 }, { "Host Lock 0.5x", 192 }, { "Host Lock 2x", 48 } };
    for (int m = 0; m < 3; m++) {
        tinysid_t *t = make_counter();
        setp(t, "sync_mode", modes[m].mode);
        setp(t, "loop_length", "1 Bar");
        for (int k = 0; k < modes[m].ticks; k++) {
            tick_send(t);
            CHECK_EQ(tinysid_phrase_rewinds(t), 0);
            tick_run(t);
        }
        CHECK_EQ(tinysid_ram(t, 0x03), BAR);               /* one whole bar of the tune has played: 16 rows x 6 frames */
        tick_send(t);                                      /* the next bar line */
        CHECK_EQ(tinysid_phrase_rewinds(t), 1);
        CHECK_EQ(tinysid_ram(t, 0x03), 0);                 /* rewound to the first row */
        CHECK_EQ(tinysid_song_bar(t), 1);
        for (int k = 0; k < modes[m].ticks - 1; k++) { tick_run(t); tick_send(t); }
        tick_run(t);
        tick_send(t);
        CHECK_EQ(tinysid_phrase_rewinds(t), 2);            /* and again a bar later, not half way */
        tinysid_destroy(t);
    }

    /* free modes do not tie rows to the clock: the host's bar (96 ticks) */
    tinysid_t *t = make_counter();
    setp(t, "sync_mode", "Free (PAL 50Hz)");
    setp(t, "loop_length", "1 Bar");
    for (int k = 0; k < BAR; k++) { tick_send(t); tick_run(t); }
    CHECK_EQ(tinysid_phrase_rewinds(t), 0);
    tick_send(t);
    CHECK_EQ(tinysid_phrase_rewinds(t), 1);
    tinysid_destroy(t);

    /* a change of sync mode is a different grid: the snapshot goes */
    t = make_counter();
    tick_send(t);
    CHECK(tinysid_phrase_armed(t));
    setp(t, "sync_mode", "Host Lock 0.5x");
    tick_run(t);
    tick_send(t);
    CHECK(!tinysid_phrase_armed(t));
    tinysid_destroy(t);

    /* the nudge: the tune's bar lines are `nudge` rows after the first tick (an intro pickup) */
    t = make_counter();
    CHECK_STR(getp(t, "loop_nudge"), "0");
    setp(t, "loop_nudge", "16");  CHECK_STR(getp(t, "loop_nudge"), "15");        /* 0..15 */
    setp(t, "loop_nudge", "-3");  CHECK_STR(getp(t, "loop_nudge"), "0");
    CHECK_EQ(tinysid_set_param(t, "loop_nudge", "x"), -1);
    setp(t, "loop_nudge", "4");   CHECK_STR(getp(t, "loop_nudge"), "4");         /* 4 rows = 24 ticks at 1x */
    setp(t, "loop_length", "1 Bar");
    for (int k = 0; k < 24; k++) {
        tick_send(t);
        CHECK(!tinysid_phrase_armed(t));                   /* the pickup: no bar yet, nothing caught at tick 0 */
        CHECK_EQ(tinysid_song_bar(t), 0);
        tick_run(t);
    }
    tick_send(t);                                          /* tick 24: the downbeat of bar 1 */
    CHECK(tinysid_phrase_armed(t));
    CHECK_EQ(tinysid_song_bar(t), 1);
    CHECK_EQ(tinysid_ram(t, 0x03), 24);                    /* the snapshot is 24 frames in */
    tick_run(t);
    for (int k = 1; k < BAR; k++) { tick_send(t); CHECK_EQ(tinysid_phrase_rewinds(t), 0); tick_run(t); }
    tick_send(t);                                          /* tick 120: a bar after the downbeat */
    CHECK_EQ(tinysid_phrase_rewinds(t), 1);
    CHECK_EQ(tinysid_ram(t, 0x03), 24);                    /* back to the downbeat, not to tick 0 */
    CHECK_EQ(tinysid_song_bar(t), 1);
    setp(t, "loop_nudge", "5");                            /* a new grid drops the snapshot */
    CHECK(!tinysid_phrase_armed(t));
    tinysid_destroy(t);

    /* at half speed a nudge row is 12 ticks (a sixteenth of the tune's bar) */
    t = make_counter();
    setp(t, "sync_mode", "Host Lock 0.5x");
    setp(t, "loop_nudge", "2");
    for (int k = 0; k < 24; k++) { tick_send(t); CHECK(!tinysid_phrase_armed(t)); tick_run(t); }
    tick_send(t);
    CHECK(tinysid_phrase_armed(t));
    tinysid_destroy(t);
}

static void test_loop_seam(void)
{
    SECTION("bar looper: a note still sounding at the seam dies by its own release");

    /* Voice 1 is set up in init with a sustained saw's envelope (sustain 14, release 15 = 24 s) but no control byte, so the
     * snapshot has it silent; from frame 8 on the play routine gates it on. At the seam the voice is mid-note. */
    fx_code_t c = { {0}, 0 };
    fx_store(&c, 0x0F, 0xD418);
    fx_store(&c, 0x10, 0xD401);
    fx_store(&c, 0x00, 0xD405);
    fx_store(&c, 0xEF, 0xD406);
    fx_byte(&c, 0x60);
    uint16_t play = (uint16_t)(FX_LOAD + c.n);
    static const uint8_t body[] = { 0xE6, 0x03, 0xA5, 0x03, 0xC9, 0x08, 0x90, 0x05, 0xA9, 0x21, 0x8D, 0x04, 0xD4, 0x60 };
    for (size_t i = 0; i < sizeof(body); i++) fx_byte(&c, body[i]);
    size_t len = fx_psid(fxbuf, c.b, c.n, FX_LOAD, FX_LOAD, play, 1, 0, 0x0004);
    fx_set_title(fxbuf, "Commando");
    tinysid_t *t = tinysid_create(SR);
    CHECK_EQ(tinysid_load(t, fxbuf, len), PSID_OK);
    setp(t, "sync_mode", "Host Lock 1x");
    setp(t, "loop_length", "1 Bar");
    tinysid_midi(t, &MIDI_START, 1);

    for (int k = 0; k < 96; k++) { tick_send(t); tick_run(t); }
    CHECK_EQ(tinysid_sid_reg(t, 0x04), 0x21);              /* the note has been sounding since frame 8 */
    CHECK(rms(L + 100, 700) > 0.02);

    tick_send(t);                                          /* the seam: the snapshot says "voice 1 silent, no waveform" */
    CHECK_EQ(tinysid_phrase_rewinds(t), 1);
    CHECK_EQ(tinysid_sid_reg(t, 0x04), 0x20);              /* the gate dropped, the saw kept: the voice goes into its release */
    CHECK_EQ(tinysid_sid_reg(t, 0x06), 0xEF);              /* with the release it was playing with */
    double tail = 0;
    for (int k = 0; k < 5; k++) {                          /* five frames of the new pass, before the song gates voice 1 again */
        tick_run(t);
        tail += rms(L + 100, 700);
        tick_send(t);
    }
    CHECK(tail / 5 > 0.02);                                /* the note rings on through the join instead of cutting out */
    CHECK_EQ(tinysid_sid_reg(t, 0x04), 0x20);

    for (int k = 0; k < 12; k++) { tick_run(t); tick_send(t); }
    CHECK_EQ(tinysid_sid_reg(t, 0x04), 0x21);              /* the song gates it on again at frame 8: it drives the voice once more */
    tinysid_destroy(t);
}

/* ---- key and BPM readouts wait for the scan ---------------------------------------- */

static void test_pending_readouts(void)
{
    SECTION("readouts: key and BPM show nothing until they are known");

    /* uncatalogued tune whose gates give a tempo (6 frames a row = 125 BPM) */
    size_t len = fx_tone(fxbuf, FX_GATER + 6, 1, 0, 0x0004);
    tinysid_t *t = tinysid_create(SR);
    tinysid_load(t, fxbuf, len);
    CHECK_STR(getp(t, "song_key"), "--");
    CHECK_STR(getp(t, "song_bpm"), "0");                   /* not the 116 fallback */
    for (int i = 0; i < 40; i++) tinysid_render(t, L, R, 128);
    CHECK(!tinysid_analysis_done(t));                      /* mid-scan */
    CHECK_STR(getp(t, "song_key"), "--");
    CHECK_STR(getp(t, "song_bpm"), "0");
    scan_to_end(t);
    CHECK_STR(getp(t, "song_bpm"), "125");                 /* the estimate, once the whole scan has voted */
    tinysid_destroy(t);

    /* a new subtune or file is pending again */
    len = fx_tone(fxbuf, FX_GATER + 6, 2, 0, 0x0004);
    t = tinysid_create(SR);
    tinysid_load(t, fxbuf, len);
    scan_to_end(t);
    CHECK_STR(getp(t, "song_bpm"), "125");
    setp(t, "subtune", "2");
    CHECK_STR(getp(t, "song_bpm"), "0");
    CHECK_STR(getp(t, "song_key"), "--");
    scan_to_end(t);
    CHECK_STR(getp(t, "song_bpm"), "125");
    tinysid_destroy(t);

    /* a tune with a key: pending, then known */
    const int c_major[] = { 60, 64, 67, 72, 62, 64, 67, 72, 65, 69, 72, 67, 71, 74, 67, 72 };
    len = fx_melody(fxbuf, c_major, 16);
    t = tinysid_create(SR);
    tinysid_load(t, fxbuf, len);
    CHECK_STR(getp(t, "song_key"), "--");
    for (int i = 0; i < 40; i++) tinysid_render(t, L, R, 128);
    CHECK_STR(getp(t, "song_key"), "--");
    scan_to_end(t);
    CHECK_STR(getp(t, "song_key"), "C Maj");
    CHECK_STR(getp(t, "song_bpm"), "0");                   /* no cadence to measure: stays unknown, not 116 */
    tinysid_destroy(t);

    /* a catalogued title shows its tempo at once, without "--" */
    len = fx_tone(fxbuf, FX_PLAIN, 2, 0, 0x0004);
    fx_set_title(fxbuf, "Commando");
    t = tinysid_create(SR);
    tinysid_load(t, fxbuf, len);
    CHECK_STR(getp(t, "song_bpm"), "125");
    setp(t, "subtune", "2");
    CHECK_STR(getp(t, "song_bpm"), "94");
    tinysid_destroy(t);

    /* a typed tempo is a known one */
    len = fx_tone(fxbuf, FX_PLAIN, 1, 0, 0x0004);
    t = tinysid_create(SR);
    tinysid_load(t, fxbuf, len);
    CHECK_STR(getp(t, "song_bpm"), "0");
    setp(t, "song_bpm_override", "133");
    CHECK_STR(getp(t, "song_bpm"), "133");
    tinysid_destroy(t);
}

/* ---- autotune ------------------------------------------------------------------ */

/* The SID word of a possibly fractional MIDI note on the given clock, the same way fx_melody builds its tables. */
static int pitch_word_clk(double note, double clk) { return (int)(440.0 * pow(2.0, (note - 69.0) / 12.0) * 16777216.0 / clk + 0.5); }
static int pitch_word(double note) { return pitch_word_clk(note, 985248.0); }

/* Advances to the next play() frame. */
static void next_frame(tinysid_t *t)
{
    uint64_t seen = tinysid_play_calls(t);
    for (int guard = 0; tinysid_play_calls(t) == seen && guard < 100; guard++) tinysid_render(t, L, R, 64);
}

static void frames(tinysid_t *t, int n) { for (int i = 0; i < n; i++) next_frame(t); }

/* A tune that holds each of `notes` on voice 1 for `hold` frames (gate on throughout, pulse 50%). */
static size_t fx_held_melody(uint8_t *out, const double *notes, int n, int hold)
{
    fx_code_t c = { {0}, 0 };
    fx_store(&c, 0x0F, 0xD418);
    fx_store(&c, 0x00, 0xD405);
    fx_store(&c, 0xF0, 0xD406);
    fx_store(&c, 0x08, 0xD403);
    fx_store(&c, 0x41, 0xD404);
    fx_byte(&c, 0x60);
    uint16_t play = (uint16_t)(FX_LOAD + c.n);
    const int code_len = 36;
    uint16_t lotab = (uint16_t)(play + code_len), hitab = (uint16_t)(lotab + n);
    const uint8_t body[] = {
        0xE6, 0x07, 0xA5, 0x07, 0xC9, (uint8_t)hold, 0x90, 0x1B,                  /* INC $07 ; LDA $07 ; CMP #hold ; BCC done */
        0xA9, 0x00, 0x85, 0x07,                                                   /* LDA #0 ; STA $07 */
        0xA6, 0x05,                                                               /* LDX $05 */
        0xBD, (uint8_t)lotab, (uint8_t)(lotab >> 8), 0x8D, 0x00, 0xD4,            /* LDA lotab,X ; STA $D400 */
        0xBD, (uint8_t)hitab, (uint8_t)(hitab >> 8), 0x8D, 0x01, 0xD4,            /* LDA hitab,X ; STA $D401 */
        0xE8, 0xE0, (uint8_t)n, 0x90, 0x02, 0xA2, 0x00, 0x86, 0x05,               /* INX ; CPX #n ; BCC +2 ; LDX #0 ; STX $05 */
        0x60
    };
    for (size_t i = 0; i < sizeof(body); i++) fx_byte(&c, body[i]);
    for (int i = 0; i < n; i++) fx_byte(&c, pitch_word(notes[i]) & 0xFF);
    for (int i = 0; i < n; i++) fx_byte(&c, pitch_word(notes[i]) >> 8);
    return fx_psid(out, c.b, c.n, FX_LOAD, FX_LOAD, play, 1, 0, 0x0004);
}

/* Three voices holding words: a loaded engine in PAL or NTSC (the PSID flags word). */
static tinysid_t *held_voices(int w1, int w2, int w3, int ctrl3, uint16_t flags)
{
    int words[3] = { w1, w2, w3 }, ctrls[3] = { 0x41, 0x41, ctrl3 };
    size_t len = fx_snoop(fxbuf, words, ctrls, flags);
    tinysid_t *t = tinysid_create(SR);
    if (!t || tinysid_load(t, fxbuf, len) != PSID_OK) { printf("fixture load failed\n"); exit(2); }
    return t;
}

static double word_cents(int w, double clk) { return 6900.0 + 1200.0 * log2(w * clk / 16777216.0 / 440.0); }

static void test_autotune(void)
{
    SECTION("autotune: a global tuning offset, a pull onto held notes, master tune");
    const int a4 = pitch_word(69.0);
    CHECK_EQ(a4, 0x1D45);                                  /* A4 = 440.0 Hz on the PAL clock */

    /* the rule that pulls a held note */
    CHECK_EQ(tinysid_snap_word(pitch_word(69.35), 985248.0, TINYSID_SNAP_CHROMATIC, 12, 2, 0), 0x1D45);
    CHECK_EQ(tinysid_snap_word(pitch_word(68.65), 985248.0, TINYSID_SNAP_CHROMATIC, 12, 2, 0), 0x1D45);
    CHECK_EQ(tinysid_snap_word(pitch_word(69.6), 985248.0, TINYSID_SNAP_CHROMATIC, 12, 2, 0), pitch_word(70.0));
    CHECK_EQ(tinysid_snap_word(0, 985248.0, TINYSID_SNAP_CHROMATIC, 0, 0, 0), 0);
    CHECK_EQ(tinysid_snap_word(1234, 985248.0, TINYSID_SNAP_OFF, 0, 0, 0), 1234);
    CHECK_EQ(tinysid_snap_word(pitch_word(69.3), 985248.0, TINYSID_SNAP_CHROMATIC, 12, 2, 20), pitch_word(69.2));   /* grid A = 440 + 20 cents */
    CHECK_EQ(tinysid_snap_word(pitch_word(69.3), 985248.0, TINYSID_SNAP_CHROMATIC, 12, 2, -30), pitch_word(69.7));    /* grid A = 440 - 30 cents */
    CHECK_EQ(tinysid_snap_word((uint16_t)(7493.0 * 985248.0 / 1022727.0 + 0.5), 1022727.0, TINYSID_SNAP_CHROMATIC, 0, 0, 0),
             pitch_word_clk(69.0, 1022727.0));             /* on the NTSC clock A4 is a different word */
    CHECK_EQ(tinysid_snap_word(pitch_word(61.2), 985248.0, TINYSID_SNAP_KEY, 0, 0, 0), pitch_word(62.0));   /* C major: C# + 20c -> D */
    CHECK_EQ(tinysid_snap_word(pitch_word(60.8), 985248.0, TINYSID_SNAP_KEY, 0, 0, 0), pitch_word(60.0));
    CHECK_EQ(tinysid_snap_word(pitch_word(68.2), 985248.0, TINYSID_SNAP_KEY, 9, 1, 0), pitch_word(69.0));   /* A minor has G, not G#: G# + 20c -> A */
    CHECK_EQ(tinysid_snap_word(pitch_word(67.8), 985248.0, TINYSID_SNAP_KEY, 9, 1, 0), pitch_word(67.0));
    CHECK_EQ(tinysid_snap_word(pitch_word(61.3), 985248.0, TINYSID_SNAP_KEY, 12, 2, 0), pitch_word(61.3));  /* key not known: unchanged */

    /* A4 + 35 cents, held on voices 1 and 2, noise on voice 3 */
    tinysid_t *t = held_voices(pitch_word(69.35), pitch_word(69.35), 0x1234, 0x81, 0x0004);
    CHECK_STR(getp(t, "pitch_snap"), "Off");
    CHECK_STR(getp(t, "master_tune"), "0");
    CHECK_EQ(sid_word(t, 0), pitch_word(69.35));           /* off: the tune's own pitch, untouched */
    CHECK_EQ(tinysid_set_param(t, "pitch_snap", "Nearest"), -1);
    setp(t, "pitch_snap", "A440 Chromatic");
    frames(t, 24);
    CHECK_EQ(sid_word(t, 0), 0x1D45);                      /* exactly A4: 440.0 Hz */
    CHECK_EQ(sid_word(t, 7), 0x1D45);
    CHECK_EQ(tinysid_sid_reg(t, 0x00), 0x45);
    CHECK_EQ(tinysid_sid_reg(t, 0x01), 0x1D);
    CHECK_NEAR(sid_word(t, 0) * 985248.0 / 16777216.0, 440.0, 0.05);
    CHECK_EQ(sid_word(t, 14), 0x1234);                     /* noise has no pitch: untouched */
    frames(t, 20);
    CHECK_EQ(sid_word(t, 0), 0x1D45);                      /* and it stays there */
    setp(t, "pitch_snap", "Off");
    CHECK_EQ(sid_word(t, 0), pitch_word(69.35));           /* off puts the tune's pitch back at once */
    tinysid_destroy(t);

    t = held_voices(pitch_word(68.65), pitch_word(68.65), 0, 0x40, 0x0004);          /* flat by 35 */
    setp(t, "pitch_snap", "A440 Chromatic");
    frames(t, 30);
    CHECK_EQ(sid_word(t, 0), 0x1D45);
    tinysid_destroy(t);

    /* the clock is the file's own: an NTSC tune lands on the NTSC word of 440 Hz, and the sync mode never changes that */
    t = held_voices(pitch_word_clk(69.35, 1022727.0), pitch_word_clk(69.35, 1022727.0), 0, 0x40, 0x0008);
    setp(t, "pitch_snap", "A440 Chromatic");
    frames(t, 30);
    CHECK_EQ(sid_word(t, 0), pitch_word_clk(69.0, 1022727.0));
    setp(t, "sync_mode", "Free (PAL 50Hz)");
    frames(t, 5);
    CHECK_EQ(sid_word(t, 0), pitch_word_clk(69.0, 1022727.0));
    tinysid_destroy(t);
    t = held_voices(pitch_word(69.35), pitch_word(69.35), 0, 0x40, 0x0004);          /* a PAL tune on an NTSC-rate play() */
    setp(t, "sync_mode", "Free (NTSC 60Hz)");
    setp(t, "pitch_snap", "A440 Chromatic");
    frames(t, 30);
    CHECK_EQ(sid_word(t, 0), 0x1D45);
    tinysid_destroy(t);

    /* the tune's own tuning is taken off every frame, so a melody of leaps (nothing is ever held) is tuned too: here it is
     * exact notes plus a master tune of +30 cents, and every frame moves by exactly that */
    const int leaps[] = { 60, 64, 67, 72, 62, 65, 69, 74, 59, 63, 66, 71 };
    size_t len = fx_melody(fxbuf, leaps, 12);
    t = tinysid_create(SR);
    CHECK_EQ(tinysid_load(t, fxbuf, len), PSID_OK);
    setp(t, "pitch_snap", "A440 Chromatic");
    setp(t, "master_tune", "30");
    CHECK_STR(getp(t, "master_tune"), "30");
    frames(t, 40);
    int off_frames = 0;
    for (int f = 0; f < 36; f++) {
        next_frame(t);
        int idx = (tinysid_ram(t, 0x05) + 12 - 1) % 12;
        int want = pitch_word(leaps[idx] + 0.30);
        if (abs(sid_word(t, 0) - want) > 1) off_frames++;
    }
    CHECK_EQ(off_frames, 0);
    setp(t, "master_tune", "500"); CHECK_STR(getp(t, "master_tune"), "100");        /* -100..100 */
    setp(t, "master_tune", "-500"); CHECK_STR(getp(t, "master_tune"), "-100");
    CHECK_EQ(tinysid_set_param(t, "master_tune", "x"), -1);
    tinysid_destroy(t);

    /* master tune offsets the grid held notes are pulled onto: A = 435 and A = 444 */
    for (int m = 0; m < 2; m++) {
        int cents = m ? 16 : -20;                          /* about A = 444 and A = 435 */
        t = held_voices(a4, a4, 0, 0x40, 0x0004);
        setp(t, "pitch_snap", "A440 Chromatic");
        char b[8];
        snprintf(b, sizeof b, "%d", cents);
        setp(t, "master_tune", b);
        frames(t, 30);
        CHECK(abs(sid_word(t, 0) - pitch_word(69.0 + cents / 100.0)) <= 1);
        tinysid_destroy(t);
    }

    /* a drum sweep (about -5 semitones a frame) keeps its shape: nothing is ever held, and the notes do not agree on a tuning */
    uint16_t pw[6] = { 0x800, 0x800, 0x800, 0x800, 0x800, 0x800 };
    uint16_t sweep[6];                                     /* 4.83 semitones down a frame, every frame a different distance from a note */
    for (int i = 0; i < 6; i++) sweep[i] = (uint16_t)pitch_word(90.0 - 4.83 * i);
    len = fx_movement(fxbuf, 6, pw, sweep);
    t = tinysid_create(SR);
    CHECK_EQ(tinysid_load(t, fxbuf, len), PSID_OK);
    setp(t, "pitch_snap", "A440 Chromatic");
    int kept = 0, moved = 0;
    for (int f = 1; f <= 60; f++) {
        next_frame(t);
        int raw = sweep[(tinysid_play_calls(t) - 1) % 6];
        if (sid_word(t, 0) == raw) kept++; else moved++;
    }
    CHECK_EQ(moved, 0);
    CHECK_EQ(kept, 60);
    tinysid_destroy(t);

    /* a vibrato (a 6-frame cycle, +-25 cents) around a note that is 30 cents sharp: it is pulled onto the note as a whole and
     * the vibrato stays, instead of being flattened or flipping between notes */
    uint16_t vib[6], vib_raw[6];
    const double vc[6] = { 0.0, 0.15, 0.25, 0.0, -0.15, -0.25 };
    for (int i = 0; i < 6; i++) { vib_raw[i] = vib[i] = (uint16_t)pitch_word(69.3 + vc[i]); }
    len = fx_movement(fxbuf, 6, pw, vib);
    t = tinysid_create(SR);
    CHECK_EQ(tinysid_load(t, fxbuf, len), PSID_OK);
    setp(t, "pitch_snap", "A440 Chromatic");
    frames(t, 60);
    double lo = 1e9, hi = -1e9, sum = 0;
    for (int f = 0; f < 24; f++) {
        next_frame(t);
        double c = word_cents(sid_word(t, 0), 985248.0) - 6900.0;
        if (c < lo) lo = c;
        if (c > hi) hi = c;
        sum += c;
    }
    CHECK_NEAR(sum / 24.0, 0.0, 4.0);                      /* centred on A4 ... */
    CHECK(hi - lo > 38.0 && hi - lo < 62.0);               /* ... and the +-25 cents of vibrato are still there */
    (void)vib_raw;
    tinysid_destroy(t);

    /* a pitch sitting between two notes (it wobbles +-8 cents either side of the line between A4 and A#4) does not flip
     * between them */
    uint16_t wob[6];
    const double wc[6] = { 0.42, 0.58, 0.42, 0.58, 0.42, 0.58 };
    for (int i = 0; i < 6; i++) wob[i] = (uint16_t)pitch_word(69.0 + wc[i]);
    len = fx_movement(fxbuf, 6, pw, wob);
    t = tinysid_create(SR);
    CHECK_EQ(tinysid_load(t, fxbuf, len), PSID_OK);
    setp(t, "pitch_snap", "A440 Chromatic");
    frames(t, 40);
    lo = 1e9; hi = -1e9;
    for (int f = 0; f < 30; f++) {
        next_frame(t);
        double c = word_cents(sid_word(t, 0), 985248.0) - 6900.0;
        if (c < lo) lo = c;
        if (c > hi) hi = c;
    }
    CHECK(hi - lo < 40.0);                                 /* it was 16 cents of wobble: never a 100-cent jump to the next note */
    tinysid_destroy(t);

    /* In-Key: held notes of a C-major tune with one F# are pulled onto the key once the scan has found it */
    const double held[] = { 60, 64, 67, 65, 69, 71, 66.2, 67, 64, 62, 60, 72 };      /* the F# is 20 cents above F#: nearer G */
    len = fx_held_melody(fxbuf, held, 12, 24);
    t = tinysid_create(SR);
    CHECK_EQ(tinysid_load(t, fxbuf, len), PSID_OK);
    setp(t, "pitch_snap", "In-Key (Scale)");
    frames(t, 60);
    int unchanged = 1;
    for (int f = 0; f < 24; f++) {                         /* no key yet: nothing to pull onto */
        next_frame(t);
        int idx = (tinysid_ram(t, 0x05) + 12 - 1) % 12;
        if (sid_word(t, 0) != pitch_word(held[idx])) unchanged = 0;
    }
    CHECK(unchanged);
    scan_to_end(t);
    CHECK(tinysid_song_key(t) < 12);
    printf("  held melody key: %s\n", getp(t, "song_key"));
    frames(t, 40);                                         /* the pull onto the key settles within a few frames of finding it */
    int checked = 0, wrong = 0, pulled = 0;
    for (int f = 0; f < 12 * 24 * 2; f++) {
        next_frame(t);
        if (tinysid_ram(t, 0x07) < 18) continue;           /* late in the hold: the pull has settled */
        int idx = (tinysid_ram(t, 0x05) + 12 - 1) % 12;
        int want = tinysid_snap_word((uint16_t)pitch_word(held[idx]), 985248.0, TINYSID_SNAP_KEY, tinysid_song_key(t), tinysid_song_scale(t), 0);
        if (abs(sid_word(t, 0) - want) > 1) wrong++;
        if (sid_word(t, 0) != pitch_word(held[idx])) pulled++;
        checked++;
    }
    CHECK(checked > 100);
    CHECK_EQ(wrong, 0);
    CHECK(pulled > 0);                                     /* the F# was pulled onto a note of the key */
    tinysid_destroy(t);

    /* Boulder Dash, when it is here: its header says NTSC and every note sits +30..+50 cents sharp of the grid. */
    FILE *fp = fopen(TINYSID_ASSET_DIR "/Boulder_Dash.sid", "rb");
    if (fp) {
        fclose(fp);
        tinysid_t *a = tinysid_create(SR), *b = tinysid_create(SR);
        CHECK_EQ(tinysid_load_file(a, TINYSID_ASSET_DIR "/Boulder_Dash.sid"), PSID_OK);
        CHECK_EQ(tinysid_load_file(b, TINYSID_ASSET_DIR "/Boulder_Dash.sid"), PSID_OK);
        setp(b, "pitch_snap", "A440 Chromatic");
        double clk = tinysid_info(a)->is_ntsc ? 1022727.0 : 985248.0;
        int total = 0, close = 0, other_semitone = 0;
        double worst = 0;
        for (int f = 0; f < 1500; f++) {
            uint64_t seen = tinysid_play_calls(a);
            for (int g = 0; tinysid_play_calls(a) == seen && g < 100; g++) { tinysid_render(a, L, R, 64); tinysid_render(b, L, R, 64); }
            if (f < 300) continue;                          /* the tuning is learned within the first seconds */
            for (int v = 0; v < 3; v++) {
                int ctrl = tinysid_sid_reg(a, v * 7 + 4), raw = sid_word(a, v * 7), out = sid_word(b, v * 7);
                if (!(ctrl & 1) || !(ctrl & 0x70) || !raw) continue;
                double mr = word_cents(raw, clk), mo = word_cents(out, clk);
                double err = mo - floor(mo / 100.0 + 0.5) * 100.0;
                total++;
                if (fabs(err) <= 20.0) close++;
                if (fabs(err) > worst) worst = fabs(err);
                if (floor(mr / 100.0 + 0.5) != floor(mo / 100.0 + 0.5)) other_semitone++;
            }
        }
        printf("  Boulder Dash (%s header): %d voice-frames, %.1f%% within 20 cents of A440, %d moved to another semitone, worst %.0f cents\n",
               clk > 1e6 && tinysid_info(a)->is_ntsc ? "NTSC" : "PAL", total, 100.0 * close / (total ? total : 1), other_semitone, worst);
        CHECK(total > 1000);
        CHECK(close >= total * 97 / 100);                   /* in tune with the Move ... */
        CHECK_EQ(other_semitone, 0);                        /* ... without a single note moved to the half-step beside it */
        tinysid_destroy(a);
        tinysid_destroy(b);
    }
}


/* ---- audio glue: DC blocker, stereo spread, ducking ------------------------------------ */

static void test_glue_dc(void)
{
    SECTION("glue: the 35 Hz DC blocker");
    tinysid_hpf_t f;
    tinysid_hpf_init(&f, SR, 35.0);
    CHECK_NEAR(f.coef, 0.995, 0.0005);                     /* R = 1 - 2 pi 35 / 44100 */
    float first = tinysid_hpf_run(&f, 1.0f), y = first;
    CHECK_NEAR(first, 1.0, 1e-6);                          /* a step passes at first ... */
    float prev = first;
    int monotone = 1;
    for (int n = 1; n < (int)(0.050 * SR); n++) {          /* ... and a steady +1.0 decays to nothing within 50 ms */
        y = tinysid_hpf_run(&f, 1.0f);
        if (y > prev + 1e-7f) monotone = 0;
        prev = y;
    }
    CHECK(monotone);
    CHECK(fabsf(y) < 0.01f);
    /* a tone well above the corner passes, and the lowest octave of rumble does not */
    tinysid_hpf_t hi, lo;
    tinysid_hpf_init(&hi, SR, 35.0);
    tinysid_hpf_init(&lo, SR, 35.0);
    double ph = 0, pl = 0, eh = 0, el = 0;
    for (int n = 0; n < 44100; n++) {
        float a = tinysid_hpf_run(&hi, sinf((float)ph)), b = tinysid_hpf_run(&lo, sinf((float)pl));
        ph += 6.2831853 * 440.0 / SR;
        pl += 6.2831853 * 10.0 / SR;
        if (n > 8000) { eh += (double)a * a; el += (double)b * b; }
    }
    CHECK(sqrt(eh / 36100.0) > 0.69);                      /* 440 Hz: 0.707 in, 0.707 out */
    CHECK(sqrt(el / 36100.0) < 0.25);                      /* 10 Hz: well down */

    /* and it is in the output path: a tune that sits off zero comes out centred, on both channels */
    tinysid_t *t = tinysid_create(48000.0);
    CHECK_EQ(tinysid_load(t, default_sid_data, default_sid_len), PSID_OK);
    tinysid_render(t, L, R, 48000);
    double ml = 0, mr = 0;
    for (int i = 16000; i < 48000; i++) { ml += L[i]; mr += R[i]; }
    CHECK(fabs(ml / 32000.0) < 0.02 && fabs(mr / 32000.0) < 0.02);
    tinysid_destroy(t);
}

/* RMS of both channels of voice `v` (0..2) playing alone, after the output has settled. */
static void voice_rms(int v, const char *spread, double *left, double *right)
{
    tinysid_t *t = make(FX_PLAIN, 1, 0, 0);
    char key[8];
    for (int i = 0; i < 3; i++) { snprintf(key, sizeof key, "v%d_vol", i + 1); setp(t, key, i == v ? "1" : "0"); }
    setp(t, "stereo_spread", spread);
    tinysid_render(t, L, R, 12000);
    tinysid_render(t, L, R, 4416);                         /* a whole number of periods of the 240 Hz sum */
    *left = rms(L, 4416);
    *right = rms(R, 4416);
    tinysid_destroy(t);
}

static void test_glue_spread(void)
{
    SECTION("glue: stereo spread");
    double l, r, mono_l, mono_r;
    tinysid_t *t0 = make(FX_PLAIN, 1, 0, 0);
    CHECK_STR(getp(t0, "stereo_spread"), "0.5");           /* the default */
    tinysid_destroy(t0);

    /* spread 0 is the mono sound: identical channels, for any mix of voices */
    tinysid_t *t = make(FX_PLAIN, 1, 0, 0);
    setp(t, "stereo_spread", "0");
    tinysid_render(t, L, R, 8192);
    CHECK(memcmp(L, R, 8192 * sizeof(float)) == 0);
    tinysid_destroy(t);

    voice_rms(1, "0", &mono_l, &mono_r);
    CHECK_NEAR(mono_l, mono_r, 1e-9);

    /* spread 1: voice 2 left, voice 3 right, voice 1 in the middle */
    voice_rms(1, "1", &l, &r);
    CHECK(l > 3.0 * r);                                    /* voice 2 is dominant on the left */
    CHECK_NEAR((l * l + r * r) / (2.0 * mono_l * mono_l), 1.0, 0.03);       /* equal power: it does not get louder for moving */
    voice_rms(2, "1", &l, &r);
    CHECK(r > 3.0 * l);                                    /* voice 3 on the right */
    CHECK_NEAR((l * l + r * r) / (2.0 * mono_l * mono_l), 1.0, 0.08);
    voice_rms(0, "1", &l, &r);
    CHECK_NEAR(l, r, l * 0.001);                           /* voice 1 stays centred */

    /* half way is half way */
    double l5, r5, l10, r10;
    voice_rms(1, "0.5", &l5, &r5);
    voice_rms(1, "1", &l10, &r10);
    CHECK(l5 / r5 > 1.2 && l5 / r5 < l10 / r10);

    /* the parameter clamps, and a bad value is refused */
    t = make(FX_PLAIN, 1, 0, 0);
    setp(t, "stereo_spread", "7");  CHECK_STR(getp(t, "stereo_spread"), "1");
    setp(t, "stereo_spread", "-1"); CHECK_STR(getp(t, "stereo_spread"), "0");
    CHECK_EQ(tinysid_set_param(t, "stereo_spread", "x"), -1);
    tinysid_destroy(t);
}

/* RMS of both channels over `n` samples: the loudness of the steady three-saw tune. */
static double glue_level(tinysid_t *t, int n)
{
    tinysid_render(t, L, R, n);
    return (rms(L, n) + rms(R, n)) * 0.5;
}

static void test_glue_ducking(void)
{
    SECTION("glue: sidechain ducking");
    const int win = 368;                                   /* two periods of the 240 Hz sum */
    tinysid_t *t = make(FX_PLAIN, 1, 0, 0);
    CHECK_STR(getp(t, "ducking_depth"), "0");
    CHECK_STR(getp(t, "ducking_release"), "80");
    setp(t, "ducking_release", "5");   CHECK_STR(getp(t, "ducking_release"), "20");     /* 20..300 ms */
    setp(t, "ducking_release", "900"); CHECK_STR(getp(t, "ducking_release"), "300");
    setp(t, "ducking_depth", "3");     CHECK_STR(getp(t, "ducking_depth"), "1");
    CHECK_EQ(tinysid_set_param(t, "ducking_depth", "x"), -1);
    tinysid_destroy(t);

    /* a kick (MIDI note 36): the level drops by the depth and recovers over the release */
    t = make(FX_PLAIN, 1, 0, 0);
    setp(t, "stereo_spread", "0");
    setp(t, "ducking_depth", "0.5");
    setp(t, "ducking_release", "100");
    tinysid_render(t, L, R, 20000);
    double base = glue_level(t, win);
    CHECK(base > 0.05);
    const uint8_t kick[3] = { 0x90, 36, 100 }, other[3] = { 0x90, 37, 100 };
    tinysid_midi(t, other, 3);                             /* not a kick: nothing happens */
    CHECK_NEAR(glue_level(t, win) / base, 1.0, 0.03);
    tinysid_midi(t, kick, 3);
    tinysid_render(t, L, R, 100);                          /* past the 2 ms attack */
    CHECK_NEAR(glue_level(t, win) / base, 0.53, 0.06);     /* dropped by the depth: 1 - 0.5, plus a little recovery */
    tinysid_render(t, L, R, 4410 - 100 - win - win / 2);
    CHECK_NEAR(glue_level(t, win) / base, 0.82, 0.06);     /* one release time on: 1 - 0.5 / e */
    tinysid_render(t, L, R, 22050);
    CHECK_NEAR(glue_level(t, win) / base, 1.0, 0.02);      /* fully recovered */
    tinysid_destroy(t);

    /* depth 0 is off: a kick changes nothing */
    t = make(FX_PLAIN, 1, 0, 0);
    setp(t, "stereo_spread", "0");
    tinysid_render(t, L, R, 20000);
    base = glue_level(t, win);
    tinysid_midi(t, kick, 3);
    tinysid_render(t, L, R, 100);
    CHECK_NEAR(glue_level(t, win) / base, 1.0, 0.02);
    tinysid_destroy(t);

    /* four on the floor: a clock tick that begins a quarter note (every 24, counted from Start) ducks, the others do not.
     * Two engines side by side, one ducking and one not, so the tune's own restart does not matter. */
    tinysid_t *d = make(FX_PLAIN, 1, 0, 0), *n = make(FX_PLAIN, 1, 0, 0);
    static float L2[1 << 16], R2[1 << 16];
    for (int k = 0; k < 2; k++) {
        tinysid_t *e = k ? n : d;
        setp(e, "stereo_spread", "0");
        setp(e, "sync_mode", "Host Lock 1x");
        setp(e, "ducking_depth", k ? "0" : "0.5");
        setp(e, "ducking_release", "80");
        tinysid_midi(e, &MIDI_START, 1);
        tinysid_render(e, L, R, 20000);
    }
    double ratio[26];
    for (int tick = 0; tick < 26; tick++) {
        tinysid_midi(d, &MIDI_CLOCK, 1);
        tinysid_midi(n, &MIDI_CLOCK, 1);
        tinysid_render(d, L, R, 100);
        tinysid_render(n, L2, R2, 100);
        tinysid_render(d, L, R, win);
        tinysid_render(n, L2, R2, win);
        ratio[tick] = (rms(L, win) + rms(R, win)) / (rms(L2, win) + rms(R2, win));
        tinysid_render(d, L, R, 882 - 100 - win);          /* the rest of the tick (125 BPM) */
        tinysid_render(n, L2, R2, 882 - 100 - win);
    }
    CHECK_NEAR(ratio[0], 0.53, 0.07);                      /* tick 0: beat 1 */
    for (int tick = 12; tick < 24; tick++) CHECK(ratio[tick] > 0.97);     /* recovered (80 ms release = 12 ticks to 97%), and no new duck between beats */
    CHECK(ratio[24] < 0.6);                                /* tick 24: beat 2 */
    CHECK(ratio[25] > 0.5 && ratio[25] < 0.95);
    tinysid_destroy(d);
    tinysid_destroy(n);
}

/* ---- performance: transpose, bend, pad controls, scale lock, filter pump, lo-fi, meters ----------------- */

static void note_on(tinysid_t *t, int note) { const uint8_t m[3] = { 0x90, (uint8_t)note, 100 }; tinysid_midi(t, m, 3); }
static void note_off(tinysid_t *t, int note) { const uint8_t m[3] = { 0x80, (uint8_t)note, 0 }; tinysid_midi(t, m, 3); }

static void test_transpose_bend(void)
{
    SECTION("transpose and pitch bend");
    const int raw = pitch_word(69.0);
    tinysid_t *t = held_voices(raw, raw, 0x1234, 0x81, 0x0004);
    CHECK_STR(getp(t, "transpose"), "0");
    frames(t, 5);
    setp(t, "transpose", "12");
    CHECK_STR(getp(t, "transpose"), "12");
    CHECK(abs(sid_word(t, 0) - raw * 2) <= 1);             /* an octave up, at once, with AutoTune off */
    CHECK(abs(sid_word(t, 7) - raw * 2) <= 1);
    CHECK_EQ(sid_word(t, 14), 0x1234);                     /* noise has no pitch */
    frames(t, 5);
    CHECK(abs(sid_word(t, 0) - raw * 2) <= 1);             /* and stays there across play() frames */
    setp(t, "transpose", "-12");
    CHECK(abs(sid_word(t, 0) - raw / 2) <= 1);
    setp(t, "transpose", "7");
    CHECK(abs(sid_word(t, 0) - pitch_word(76.0)) <= 2);    /* a fifth up: E5 */
    setp(t, "transpose", "99");  CHECK_STR(getp(t, "transpose"), "12");               /* -12..12 */
    setp(t, "transpose", "-99"); CHECK_STR(getp(t, "transpose"), "-12");
    CHECK_EQ(tinysid_set_param(t, "transpose", "x"), -1);
    setp(t, "transpose", "0");
    CHECK_EQ(sid_word(t, 0), raw);                         /* off puts the tune's pitch back exactly */

    /* pitch bend: +-2 semitones; the centre is no bend; it adds to the transpose */
    const uint8_t up[3] = { 0xE0, 0x7F, 0x7F }, down[3] = { 0xE0, 0x00, 0x00 }, centre[3] = { 0xE0, 0x00, 0x40 };
    tinysid_midi(t, up, 3);
    CHECK(abs(sid_word(t, 0) - (int)(raw * pow(2.0, 200.0 / 1200.0) + 0.5)) <= 3);
    tinysid_midi(t, down, 3);
    CHECK(abs(sid_word(t, 0) - (int)(raw * pow(2.0, -200.0 / 1200.0) + 0.5)) <= 3);
    tinysid_midi(t, centre, 3);
    CHECK_EQ(sid_word(t, 0), raw);
    setp(t, "transpose", "12");
    tinysid_midi(t, up, 3);
    CHECK(abs(sid_word(t, 0) - (int)(raw * 2 * pow(2.0, 200.0 / 1200.0) + 0.5)) <= 6);
    tinysid_midi(t, centre, 3);
    setp(t, "transpose", "0");
    tinysid_destroy(t);

    /* on top of AutoTune: a tune 35 cents sharp, tuned to A4, then a fifth up */
    t = held_voices(pitch_word(69.35), pitch_word(69.35), 0, 0x40, 0x0004);
    setp(t, "pitch_snap", "A440 Chromatic");
    setp(t, "transpose", "7");
    frames(t, 40);
    CHECK(abs(sid_word(t, 0) - pitch_word(76.0)) <= 1);
    tinysid_destroy(t);

    /* it is on a new run too, from the first frame */
    t = held_voices(raw, raw, 0, 0x40, 0x0004);
    setp(t, "transpose", "12");
    tinysid_midi(t, &MIDI_START, 1);
    frames(t, 2);
    CHECK(abs(sid_word(t, 0) - raw * 2) <= 1);
    tinysid_destroy(t);

    /* the key readout and the feed show the key as the song now plays it */
    const int cmaj[] = { 60, 64, 67, 72, 62, 64, 67, 72, 65, 69, 72, 67, 71, 74, 67, 72 };
    size_t len = fx_melody(fxbuf, cmaj, 16);
    t = tinysid_create(SR);
    CHECK_EQ(tinysid_load(t, fxbuf, len), PSID_OK);
    scan_to_end(t);
    CHECK_STR(getp(t, "song_key"), "C Maj");
    setp(t, "transpose", "2");
    CHECK_STR(getp(t, "song_key"), "D Maj");
    CHECK_EQ(tinysid_song_key(t), 0);                      /* the tune's own key is unchanged */
    int note, voice, key = -1;
    CHECK(sscanf(getp(t, "snoop_feed"), "K,%d,%d,%d", &note, &voice, &key) == 3);
    CHECK_EQ(key, 4);                                      /* D Maj = 2 * 2 */
    setp(t, "transpose", "-1");
    CHECK_STR(getp(t, "song_key"), "B Maj");
    setp(t, "transpose", "0");
    CHECK_STR(getp(t, "song_key"), "C Maj");
    tinysid_destroy(t);
}

static void test_pad_controls(void)
{
    SECTION("pad controls: mutes, transpose, last note");
    tinysid_t *t = make(FX_PLAIN, 3, 0, 0);
    setp(t, "stereo_spread", "0");
    CHECK_STR(getp(t, "pad_mode"), "Play");
    CHECK_STR(getp(t, "last_note"), "0");
    CHECK_STR(getp(t, "voice_mutes"), "0");
    note_on(t, 37);                                        /* Play mode: nothing happens, but the last note is read out */
    CHECK_STR(getp(t, "last_note"), "37");
    CHECK_STR(getp(t, "voice_mutes"), "0");
    CHECK_STR(getp(t, "subtune"), "1");
    note_on(t, 80);
    CHECK_STR(getp(t, "last_note"), "80");
    CHECK_EQ(tinysid_set_param(t, "pad_mode", "Jam"), -1);
    setp(t, "pad_mode", "Control");
    CHECK_STR(getp(t, "pad_mode"), "Control");

    /* mutes: pads 36 37 38 toggle voices 1 2 3; 39 unmutes all */
    setp(t, "v2_vol", "0");
    setp(t, "v3_vol", "0");
    tinysid_render(t, L, R, 12000);
    double all = rms(L + 6000, 4416);
    CHECK(all > 0.02);                                     /* voice 1 alone */
    note_on(t, 36);
    CHECK_STR(getp(t, "voice_mutes"), "1");
    tinysid_render(t, L, R, 12000);
    CHECK(rms(L + 6000, 4416) < 0.001);                    /* muted */
    note_off(t, 36);                                       /* releasing a pad does nothing */
    CHECK_STR(getp(t, "voice_mutes"), "1");
    note_on(t, 36);
    CHECK_STR(getp(t, "voice_mutes"), "0");
    tinysid_render(t, L, R, 12000);
    CHECK_NEAR(rms(L + 6000, 4416) / all, 1.0, 0.03);      /* back */
    note_on(t, 37); CHECK_STR(getp(t, "voice_mutes"), "2");
    note_on(t, 38); CHECK_STR(getp(t, "voice_mutes"), "6");
    note_on(t, 36); CHECK_STR(getp(t, "voice_mutes"), "7");
    note_on(t, 39); CHECK_STR(getp(t, "voice_mutes"), "0");
    CHECK_STR(getp(t, "last_note"), "39");
    setp(t, "voice_mutes", "5");
    CHECK_STR(getp(t, "voice_mutes"), "5");
    setp(t, "voice_mutes", "0");

    /* the knob levels survive a mute: unmuted, voice 1 returns to its own level */
    setp(t, "v1_vol", "0.5");
    tinysid_render(t, L, R, 12000);
    double half = rms(L + 6000, 4416);
    note_on(t, 36); tinysid_render(t, L, R, 12000);
    note_on(t, 36); tinysid_render(t, L, R, 12000);
    CHECK_NEAR(rms(L + 6000, 4416) / half, 1.0, 0.03);
    setp(t, "v1_vol", "1");

    /* a kick does not duck in Control mode: 36 is a button there */
    setp(t, "ducking_depth", "0.8");
    tinysid_render(t, L, R, 12000);
    double steady = rms(L + 6000, 4416);
    note_on(t, 36);
    note_on(t, 36);
    tinysid_render(t, L, R, 3000);
    tinysid_render(t, L, R, 4416);
    CHECK_NEAR(rms(L, 4416) / steady, 1.0, 0.05);
    setp(t, "ducking_depth", "0");

    /* the tune is not on the pads: notes past the transpose pads (and the old tune pads' numbers) change nothing */
    note_on(t, 43); note_on(t, 44); note_on(t, 45);
    CHECK_STR(getp(t, "subtune"), "1");
    CHECK_STR(getp(t, "voice_mutes"), "0");
    CHECK_STR(getp(t, "transpose"), "0");

    /* transpose: 40 down, 41 up, 42 back to nothing */
    note_on(t, 41); note_on(t, 41); CHECK_STR(getp(t, "transpose"), "2");
    note_on(t, 40); CHECK_STR(getp(t, "transpose"), "1");
    note_on(t, 42); CHECK_STR(getp(t, "transpose"), "0");
    for (int i = 0; i < 15; i++) note_on(t, 41);
    CHECK_STR(getp(t, "transpose"), "12");                 /* it stops at an octave */
    note_on(t, 42);

    /* notes above the control range do nothing, and in Control mode no pad note sounds */
    setp(t, "pad_target", "Voice 2");
    tinysid_render(t, L, R, 2048);
    note_on(t, 60);
    CHECK_EQ(tinysid_sid_reg(t, 0x0B) & 1, 0);
    CHECK_STR(getp(t, "last_note"), "60");
    setp(t, "pad_mode", "Play");
    note_on(t, 60);
    CHECK_EQ(tinysid_sid_reg(t, 0x0B) & 1, 1);             /* and in Play mode it does */
    tinysid_destroy(t);
}

/* The "brightness" of a block: the RMS of its first difference over its RMS. A low-pass closing lowers it. */
static double brightness(const float *x, int n)
{
    double d = 0, e = 0;
    for (int i = 1; i < n; i++) { d += (double)(x[i] - x[i - 1]) * (x[i] - x[i - 1]); e += (double)x[i] * x[i]; }
    return e > 0 ? sqrt(d / e) : 0.0;
}

static void test_filter_pump(void)
{
    SECTION("filter pump: the output's low-pass closes on a beat, on any tune");
    tinysid_t *t = make(FX_PLAIN, 1, 0, 0);                /* no filter in use at all: $D417 = $80, $D418 = $0F */
    setp(t, "stereo_spread", "0");
    CHECK_STR(getp(t, "filter_pump"), "0");
    setp(t, "filter_pump", "7");  CHECK_STR(getp(t, "filter_pump"), "1");
    setp(t, "filter_pump", "-1"); CHECK_STR(getp(t, "filter_pump"), "0");
    CHECK_EQ(tinysid_set_param(t, "filter_pump", "x"), -1);
    tinysid_render(t, L, R, 12000);
    double bright = brightness(L + 6000, 4416), level = rms(L + 6000, 4416);

    note_on(t, 36);                                        /* pump 0: nothing */
    tinysid_render(t, L, R, 200);
    tinysid_render(t, L, R, 4416);
    CHECK_NEAR(brightness(L, 4416) / bright, 1.0, 0.02);

    setp(t, "filter_pump", "0.8");
    tinysid_render(t, L, R, 12000);                        /* idle: transparent, in level and in brightness */
    CHECK_NEAR(rms(L + 6000, 4416) / level, 1.0, 0.02);
    CHECK_NEAR(brightness(L + 6000, 4416) / bright, 1.0, 0.06);
    CHECK_EQ(tinysid_sid_reg(t, 0x17), 0x80);              /* and the tune's filter registers are not touched */
    CHECK_EQ(tinysid_sid_reg(t, 0x18), 0x0F);
    setp(t, "ducking_release", "100");
    note_on(t, 36);                                        /* a beat */
    tinysid_render(t, L, R, 400);
    tinysid_render(t, L, R, 1104);
    double closed = brightness(L, 1104) / bright;
    CHECK(closed < 0.4);                                   /* closed: most of the highs are gone */
    tinysid_render(t, L, R, 3000);
    tinysid_render(t, L, R, 4416);
    double opening = brightness(L, 4416) / bright;
    CHECK(opening > closed && opening < 0.9);              /* part way back after about one release time */
    tinysid_render(t, L, R, 44100);
    tinysid_render(t, L, R, 4416);
    CHECK_NEAR(brightness(L, 4416) / bright, 1.0, 0.03);   /* open again */
    CHECK_NEAR(rms(L, 4416) / level, 1.0, 0.03);

    /* a deeper setting closes it further; the release time is the ducker's */
    setp(t, "filter_pump", "0.3");
    note_on(t, 36);
    tinysid_render(t, L, R, 400);
    tinysid_render(t, L, R, 1104);
    double shallow = brightness(L, 1104) / bright;
    CHECK(shallow > closed && shallow < 0.95);
    tinysid_render(t, L, R, 44100);

    /* it follows the clock too: the first tick after Start is a quarter note */
    setp(t, "filter_pump", "0.8");
    tinysid_midi(t, &MIDI_START, 1);
    tinysid_render(t, L, R, 20000);
    double settled = brightness(L + 15000, 2000);
    tinysid_midi(t, &MIDI_CLOCK, 1);
    tinysid_render(t, L, R, 400);
    tinysid_render(t, L, R, 1104);
    CHECK(brightness(L, 1104) < 0.6 * settled);
    /* switching it off lets it go, with no loud click */
    setp(t, "filter_pump", "0");
    tinysid_render(t, L, R, 44100);
    double peak = 0;
    for (int i = 1; i < 4416; i++) { double d = fabs((double)L[i] - L[i - 1]); if (d > peak) peak = d; }
    CHECK(all_finite_bounded(L, 4416));
    CHECK(peak < 0.5);
    tinysid_destroy(t);
}

static void test_lofi(void)
{
    SECTION("lo-fi: fewer bits, a lower sample rate");
    tinysid_t *a = make(FX_PLAIN, 1, 0, 0), *b = make(FX_PLAIN, 1, 0, 0);
    static float La[8192], Ra[8192];
    CHECK_STR(getp(b, "lofi"), "0");
    setp(b, "lofi", "7");  CHECK_STR(getp(b, "lofi"), "1");
    setp(b, "lofi", "0");
    tinysid_render(a, La, Ra, 8192);
    tinysid_render(b, L, R, 8192);
    CHECK(memcmp(La, L, 8192 * sizeof(float)) == 0);       /* 0 is the clean sound, bit for bit */
    setp(b, "lofi", "1");
    tinysid_render(a, La, Ra, 8192);
    tinysid_render(b, L, R, 8192);
    int same = 0;
    float seen[256];
    int distinct = 0;
    for (int i = 4096; i < 8192; i++) {
        if (L[i] == L[i - 1]) same++;
        int known = 0;
        for (int k = 0; k < distinct && !known; k++) if (fabsf(seen[k] - L[i]) < 1e-6f) known = 1;
        if (!known && distinct < 256) seen[distinct++] = L[i];
    }
    CHECK(same > 4096 * 80 / 100);                         /* held for 8 samples at a time: 7 in 8 repeat */
    CHECK(distinct < 40);                                  /* a handful of levels, where the clean sound has thousands */
    CHECK(rms(L + 4096, 4096) > 0.5 * rms(La + 4096, 4096) && rms(L + 4096, 4096) < 1.6 * rms(La + 4096, 4096));
    CHECK(all_finite_bounded(L, 8192));
    setp(b, "lofi", "0.5");
    tinysid_render(b, L, R, 8192);
    same = 0;
    for (int i = 4096; i < 8192; i++) if (L[i] == L[i - 1]) same++;
    CHECK(same > 4096 * 60 / 100 && same < 4096 * 85 / 100);   /* held 5 at a time: 4 in 5 */
    CHECK_EQ(memcmp(L, R, 8192 * sizeof(float)) != 0, 1);  /* still the stereo sound underneath */
    tinysid_destroy(a);
    tinysid_destroy(b);
}

static void test_mix_feed(void)
{
    SECTION("the mix feed: meters and mutes");
    tinysid_t *t = make(FX_PLAIN, 1, 0, 0);
    int l1, l2, l3, mask, trans;
    tinysid_render(t, L, R, 8192);
    CHECK(sscanf(getp(t, "mix_feed"), "M,%d,%d,%d,%d,%d", &l1, &l2, &l3, &mask, &trans) == 5);
    CHECK(l1 > 20 && l2 > 20 && l3 > 20 && l1 <= 100);     /* three sustained voices */
    CHECK_EQ(mask, 0);
    CHECK_EQ(trans, 0);
    setp(t, "voice_mutes", "2");
    setp(t, "transpose", "-3");
    CHECK(sscanf(getp(t, "mix_feed"), "M,%d,%d,%d,%d,%d", &l1, &l2, &l3, &mask, &trans) == 5);
    CHECK(l1 > 20 && l2 == 0 && l3 > 20);                  /* a muted voice shows nothing */
    CHECK_EQ(mask, 2);
    CHECK_EQ(trans, -3);
    setp(t, "mix_feed", "junk");                           /* hosts write every param's default: accepted and dropped */
    CHECK(sscanf(getp(t, "mix_feed"), "M,%d", &l1) == 1);
    tinysid_destroy(t);
}

/* ---- the pad is in tune with the song ------------------------------------------------- */

/* A seven-note melody (A3 to C5, four frames a note) all `cents` sharp: enough different notes for the tuning to be learned. */
static tinysid_t *detuned_engine(double cents)
{
    double notes[7] = { 57, 60, 62, 64, 67, 69, 72 };
    for (int i = 0; i < 7; i++) notes[i] += cents / 100.0;
    size_t len = fx_held_melody(fxbuf, notes, 7, 4);
    tinysid_t *t = tinysid_create(SR);
    if (!t || tinysid_load(t, fxbuf, len) != PSID_OK) { printf("fixture load failed\n"); exit(2); }
    return t;
}

static void test_pad_in_tune(void)
{
    SECTION("pad voice: in tune with the song as it sounds");
    const int sharp = pitch_word(69.35);                   /* a song 35 cents sharp of A = 440 Hz */

    /* AutoTune off: the pad plays at the song's own tuning, so the two are in tune with each other */
    tinysid_t *t = detuned_engine(35.0);
    setp(t, "pad_target", "Voice 3");
    frames(t, 80);                                         /* the tuning is learned from the song's notes */
    note_on(t, 69);
    CHECK(abs(sid_word(t, 14) - sharp) <= 2);              /* A4 sits where the song's A4 does */
    note_off(t, 69);
    frames(t, 5);

    /* AutoTune on: the song is brought onto A440, and the pad is on it too */
    setp(t, "pitch_snap", "A440 Chromatic");
    frames(t, 40);
    note_on(t, 69);
    CHECK(abs(sid_word(t, 14) - 0x1D45) <= 1);
    note_off(t, 69);
    setp(t, "master_tune", "-20");                         /* and on the master-tuned grid when that is set */
    frames(t, 40);
    note_on(t, 69);
    CHECK(abs(sid_word(t, 14) - pitch_word(68.8)) <= 1);
    note_off(t, 69);
    setp(t, "master_tune", "0");
    setp(t, "pitch_snap", "Off");
    frames(t, 5);
    note_on(t, 69);
    CHECK(abs(sid_word(t, 14) - sharp) <= 2);              /* back to following the song */
    tinysid_destroy(t);

    /* a note held while the tuning is still being learned moves onto it once it is known */
    t = detuned_engine(35.0);
    setp(t, "pad_target", "Voice 3");
    tinysid_render(t, L, R, 2048);
    note_on(t, 69);
    CHECK(abs(sid_word(t, 14) - 0x1D45) <= 1);             /* nothing learned yet: the note as played */
    frames(t, 40);
    CHECK(abs(sid_word(t, 14) - sharp) <= 2);              /* the held note followed */
    tinysid_destroy(t);

    /* a sound effect that repeats one pitch says nothing about a tuning (Commando's subtune 9 once "learned" -45 cents from one):
     * however sharp it is, the pad stays on A440 */
    t = held_voices(pitch_word(69.45), pitch_word(69.45), pitch_word(69.45), 0x41, 0x0004);
    setp(t, "pad_target", "Voice 3");
    frames(t, 200);
    note_on(t, 69);
    CHECK(abs(sid_word(t, 14) - 0x1D45) <= 1);
    tinysid_destroy(t);

    /* Cybernoid II, when it is here: its subtune 1 is in C minor and its notes sit about 37 cents sharp of A440, which is what
     * made a pad played in C minor sound distinctly off key. */
    FILE *fp = fopen(TINYSID_ASSET_DIR "/Cybernoid_II.sid", "rb");
    if (fp) {
        fclose(fp);
        double err[2];
        for (int snap = 0; snap < 2; snap++) {
            t = tinysid_create(SR);
            CHECK_EQ(tinysid_load_file(t, TINYSID_ASSET_DIR "/Cybernoid_II.sid"), PSID_OK);
            setp(t, "subtune", "1");
            if (snap) setp(t, "pitch_snap", "A440 Chromatic");
            scan_to_end(t);
            if (!snap) CHECK_STR(getp(t, "song_key"), "C Min");
            frames(t, 600);
            setp(t, "pad_target", "Voice 2");
            tinysid_render(t, L, R, 2048);
            note_on(t, 60);
            double m = word_cents(sid_word(t, 7), 985248.0);
            err[snap] = m - 6000.0;                        /* the first frame: before the instrument's own movement */
            tinysid_destroy(t);
        }
        printf("  Cybernoid II, pad C4: %+.1f cents from A440 with AutoTune off (the song's own tuning), %+.1f with it on\n", err[0], err[1]);
        CHECK(err[0] > 25.0 && err[0] < 50.0);             /* in tune with the song, which is sharp */
        CHECK(fabs(err[1]) < 3.0);                         /* and on A440 when AutoTune has brought the song there */
    }
}

/* The ducker (and the pump) beat on the host's MIDI clock; with none arriving they keep the song's own beat. */
static void test_song_beat(void)
{
    SECTION("glue: with no host clock the ducker keeps the song's own beat");
    const int win = 368, step = 2205;                      /* a window of the tone every 50 ms, for 4 s */
    for (int k = 0; k < 2; k++) {
        tinysid_t *t = make(FX_PLAIN, 1, 0, 0);
        setp(t, "stereo_spread", "0");
        setp(t, "ducking_depth", k ? "0" : "0.5");
        setp(t, "ducking_release", "100");
        tinysid_render(t, L, R, 20000);
        double lo = 1e9, hi = 0;
        for (int i = 0; i < 80; i++) {
            tinysid_render(t, L, R, win);
            double lv = rms(L, win);
            if (lv < lo) lo = lv;
            if (lv > hi) hi = lv;
            tinysid_render(t, L, R, step - win);
        }
        if (k) CHECK(lo / hi > 0.97);                      /* depth 0: no dips */
        else CHECK(lo / hi < 0.75);                        /* depth 0.5: a dip on every quarter note of the song's tempo */
        tinysid_destroy(t);
    }

    /* the pump closes on the same beats */
    tinysid_t *t = make(FX_PLAIN, 1, 0, 0);
    setp(t, "stereo_spread", "0");
    setp(t, "filter_pump", "1");
    setp(t, "ducking_release", "100");
    tinysid_render(t, L, R, 20000);
    double lo = 1e9, hi = 0;
    for (int i = 0; i < 80; i++) {
        tinysid_render(t, L, R, 1104);
        double b = brightness(L, 1104);
        if (b < lo) lo = b;
        if (b > hi) hi = b;
        tinysid_render(t, L, R, step - 1104);
    }
    CHECK(lo / hi < 0.6);
    tinysid_destroy(t);

    /* a host clock that is running keeps the beat: ticks every 882 samples, a quarter note each 24, and no extra duck between them */
    tinysid_t *d = make(FX_PLAIN, 1, 0, 0), *n = make(FX_PLAIN, 1, 0, 0);
    static float L2[1 << 16], R2[1 << 16];
    for (int k = 0; k < 2; k++) {
        tinysid_t *e = k ? n : d;
        setp(e, "stereo_spread", "0");
        setp(e, "sync_mode", "Host Lock 1x");
        setp(e, "ducking_depth", k ? "0" : "0.5");
        setp(e, "ducking_release", "80");
        tinysid_midi(e, &MIDI_START, 1);
        tinysid_render(e, L, R, 20000);
    }
    double worst = 2.0;
    for (int tick = 1; tick < 24; tick++) {                /* between the beats: the song's own beat must stay out of it */
        tinysid_midi(d, &MIDI_CLOCK, 1);
        tinysid_midi(n, &MIDI_CLOCK, 1);
        tinysid_render(d, L, R, 882);
        tinysid_render(n, L2, R2, 882);
        if (tick > 14) {
            double r = rms(L, 882) / rms(L2, 882);
            if (r < worst) worst = r;
        }
    }
    CHECK(worst > 0.95);
    tinysid_destroy(d);
    tinysid_destroy(n);
}

/* Renders `n` samples in pieces that fit the shared buffers. */
static void fx_wait(tinysid_t *t, int n)
{
    for (; n > 0; n -= 20000) tinysid_render(t, L, R, n < 20000 ? n : 20000);
}

/* The tune's other subtunes that sound once and stop are offered as sound effects and played on spare engines over the song. */
static tinysid_t *fx_tune(void)
{
    size_t len = fx_effects(fxbuf, 4);
    tinysid_t *t = tinysid_create(SR);
    if (!t || tinysid_load(t, fxbuf, len) != PSID_OK) { printf("fixture load failed\n"); exit(2); }
    setp(t, "stereo_spread", "0");
    return t;
}

static void test_sound_effects(void)
{
    SECTION("sound effects: subtunes that sound once and stop");
    const int win = 2000;
    tinysid_t *t = fx_tune();
    CHECK_STR(getp(t, "fx_count"), "0");
    CHECK_STR(getp(t, "fx_ready"), "0");
    CHECK_STR(getp(t, "fx_duck"), "0.4");
    tinysid_fx_scan_advance(t, 1000000);
    CHECK_STR(getp(t, "fx_ready"), "1");
    CHECK_STR(getp(t, "fx_count"), "2");                   /* subtune 1 is music, 4 is silent: 2 and 3 are the effects */
    CHECK_STR(getp(t, "fx_list"), "2,3");
    CHECK_EQ(tinysid_fx_count(t), 2);
    tinysid_destroy(t);

    /* found by the audio thread's own slices, 8 frames a block */
    t = fx_tune();
    int blocks = 0;
    for (; blocks < 2000 && !tinysid_fx_ready(t); blocks++) tinysid_render(t, L, R, 128);
    CHECK(blocks > 100 && blocks < 1000);                  /* 4 subtunes x 600 frames at 8 a block is 300 blocks */
    CHECK_STR(getp(t, "fx_list"), "2,3");
    tinysid_destroy(t);

    /* a tune that is all music has none */
    t = make(FX_PLAIN, 3, 0, 0);
    tinysid_fx_scan_advance(t, 1000000);
    CHECK_EQ(tinysid_fx_count(t), 0);
    CHECK_STR(getp(t, "fx_ready"), "1");
    tinysid_destroy(t);

    /* the effect alone, for reference */
    tinysid_t *r = fx_tune();
    setp(r, "subtune", "2");
    tinysid_render(r, L, R, 4000);
    tinysid_render(r, L, R, win);
    double effect = rms(L, win);
    CHECK(effect > 0.05);
    tinysid_destroy(r);

    /* a hit adds the effect to the song; with the duck off the song carries on at its level */
    t = fx_tune();
    tinysid_fx_scan_advance(t, 1000000);
    setp(t, "fx_duck", "0");
    tinysid_render(t, L, R, 20000);
    tinysid_render(t, L, R, win);
    double song = rms(L, win);
    CHECK(song > 0.05);
    CHECK_EQ(tinysid_set_param(t, "fx_trigger", "0"), 0);
    tinysid_render(t, L, R, 4000);
    tinysid_render(t, L, R, win);
    double both = rms(L, win);
    CHECK(both > song * 1.25);                             /* two tones: the power adds */
    fx_wait(t, 80000);                        /* the effect (20 frames and a 1 s tail) is over ... */
    tinysid_render(t, L, R, win);
    CHECK_NEAR(rms(L, win) / song, 1.0, 0.03);             /* ... and the song is as it was */
    CHECK(all_finite_bounded(L, win));

    /* the velocity sets its level */
    CHECK_EQ(tinysid_set_param(t, "fx_trigger", "0,1"), 0);
    tinysid_render(t, L, R, 4000);
    tinysid_render(t, L, R, win);
    double soft = rms(L, win);
    CHECK(soft > song * 0.97 && soft < both * 0.9);
    fx_wait(t, 80000);

    /* the second effect is its own subtune */
    CHECK_EQ(tinysid_set_param(t, "fx_trigger", "1,127"), 0);
    tinysid_render(t, L, R, 4000);
    tinysid_render(t, L, R, win);
    CHECK(rms(L, win) > song * 1.25);
    fx_wait(t, 80000);

    /* nothing there, or not a number */
    CHECK_EQ(tinysid_set_param(t, "fx_trigger", "7"), 0);
    CHECK_EQ(tinysid_set_param(t, "fx_trigger", "-1"), 0);
    CHECK_EQ(tinysid_set_param(t, "fx_trigger", "x"), -1);
    tinysid_render(t, L, R, 4000);
    tinysid_render(t, L, R, win);
    CHECK_NEAR(rms(L, win) / song, 1.0, 0.03);

    /* several at once, and a fourth takes the slot nearest to finishing */
    for (int i = 0; i < 4; i++) { tinysid_set_param(t, "fx_trigger", i & 1 ? "1" : "0"); tinysid_render(t, L, R, 300); }
    tinysid_render(t, L, R, 4000);
    CHECK(all_finite_bounded(L, 4000));
    tinysid_destroy(t);

    /* the song dips for the effect, and comes back with the ducker's release */
    t = fx_tune();
    tinysid_fx_scan_advance(t, 1000000);
    setp(t, "fx_duck", "1");
    setp(t, "ducking_release", "100");
    CHECK_STR(getp(t, "fx_duck"), "1");
    tinysid_render(t, L, R, 20000);
    tinysid_set_param(t, "fx_trigger", "0");
    tinysid_render(t, L, R, 400);                          /* past the 2 ms attack and the first moments */
    tinysid_render(t, L, R, win);
    CHECK_NEAR(rms(L, win) / effect, 1.0, 0.25);           /* the song is gone: only the effect is heard */
    tinysid_destroy(t);

    /* a new file clears them */
    t = fx_tune();
    tinysid_fx_scan_advance(t, 1000000);
    CHECK_EQ(tinysid_fx_count(t), 2);
    size_t len = fx_tone(fxbuf, FX_PLAIN, 1, 0, 0);
    CHECK_EQ(tinysid_load(t, fxbuf, len), PSID_OK);
    CHECK_EQ(tinysid_fx_count(t), 0);
    CHECK_STR(getp(t, "fx_ready"), "0");
    tinysid_destroy(t);
}

/* Notes out: the chosen voices' notes as MIDI events, with the tune's own tuning on pitch bend. */
typedef struct { int type, note, value, channel; } mev_t;

static int mout_drain(tinysid_t *t, mev_t *ev, int max)
{
    int n = 0, ty, nt, va, ch;
    while (tinysid_midi_out_peek(t, &ty, &nt, &va, &ch)) {
        if (n < max) { ev[n].type = ty; ev[n].note = nt; ev[n].value = va; ev[n].channel = ch; n++; }
        tinysid_midi_out_drop(t);
    }
    return n;
}

#define MOUT_Q_TEST 64                                     /* the engine's queue length */

static int word_of(double midi) { return (int)lround(440.0 * pow(2.0, (midi - 69.0) / 12.0) * 16777216.0 / 985248.0); }

/* the last event of `type` on `channel` in ev[0..n), or -1 */
static int mout_find(const mev_t *ev, int n, int type, int channel)
{
    for (int i = n - 1; i >= 0; i--) if (ev[i].type == type && ev[i].channel == channel) return i;
    return -1;
}

static void test_notes_out(void)
{
    SECTION("notes out: voices as MIDI, the tuning on pitch bend");
    mev_t ev[64];
    const int mid = 8192, per_cent = 41;                   /* a bend of +-2 semitones is 8192 steps: about 41 a cent */

    /* voice 1 holds A4 + 37 cents: the tune that sits sharp, as Cybernoid does */
    tinysid_t *t = held_voices(word_of(69.37), word_of(60), word_of(64), 0x41, 0);
    CHECK_STR(getp(t, "midi_out_v1"), "Off");
    CHECK_STR(getp(t, "midi_out_v2"), "Off");
    CHECK_STR(getp(t, "midi_out_v3"), "Off");
    CHECK_STR(getp(t, "midi_out_ch1"), "1");               /* voices 1, 2 and 3 to channels 1, 2 and 3 ... */
    CHECK_STR(getp(t, "midi_out_ch2"), "2");
    CHECK_STR(getp(t, "midi_out_ch3"), "3");               /* ... one channel each, so each can bend on its own */
    tinysid_render(t, L, R, 4410);
    CHECK_EQ(mout_drain(t, ev, 64), 0);                    /* off by default: nothing is sent */

    setp(t, "midi_out_v1", "On");
    CHECK_STR(getp(t, "midi_out_v1"), "On");
    tinysid_render(t, L, R, 4410);
    int n = mout_drain(t, ev, 64);
    CHECK_EQ(n, 2);
    CHECK(ev[0].type == 2 && ev[0].channel == 0);          /* the bend first, so the note starts at the right pitch ... */
    CHECK(abs(ev[0].value - (mid + 37 * per_cent)) < 30);  /* ... 37 cents sharp, to within a fraction of a cent */
    CHECK(ev[1].type == 1 && ev[1].channel == 0);          /* then the note: A4 */
    CHECK_EQ(ev[1].note, 69);
    CHECK_EQ(ev[1].value, 101);                            /* the velocity that marks our notes when they come back as pad presses */
    CHECK_STR(getp(t, "mout_echo"), "69");                 /* the note just sent on channel 1 is listed, for the screen to ignore ... */
    tinysid_render(t, L, R, 44100);
    CHECK_STR(getp(t, "mout_echo"), "");                   /* ... for a moment only */
    CHECK_EQ(mout_drain(t, ev, 64), 0);                    /* a held note sends nothing more */

    /* voices 2 and 3 join, each on its own channel */
    setp(t, "midi_out_v2", "On");
    setp(t, "midi_out_v3", "On");
    tinysid_render(t, L, R, 4410);
    n = mout_drain(t, ev, 64);
    int on2 = mout_find(ev, n, 1, 1), on3 = mout_find(ev, n, 1, 2);
    CHECK(on2 >= 0 && ev[on2].note == 60);
    CHECK(on3 >= 0 && ev[on3].note == 64);
    int b2 = mout_find(ev, n, 2, 1);
    CHECK(b2 >= 0 && abs(ev[b2].value - mid) < 10);        /* in tune with the grid: no bend to speak of */
    CHECK(tinysid_midi_out_listens(t, 0) && tinysid_midi_out_listens(t, 1) && tinysid_midi_out_listens(t, 2) && !tinysid_midi_out_listens(t, 3));

    /* muting a voice in the audio does not touch its MIDI: the two are muted apart */
    setp(t, "voice_mutes", "2");
    tinysid_render(t, L, R, 4410);
    CHECK_EQ(mout_drain(t, ev, 64), 0);
    setp(t, "voice_mutes", "0");

    /* switching one off lets go of its note, on its channel, and the others carry on */
    setp(t, "midi_out_v1", "Off");
    n = mout_drain(t, ev, 64);
    CHECK(n >= 1 && ev[0].type == 0 && ev[0].note == 69 && ev[0].channel == 0);
    CHECK(n == 1 || (ev[1].type == 2 && ev[1].value == mid));      /* and the bend goes back to the middle */
    tinysid_render(t, L, R, 4410);
    CHECK_EQ(mout_drain(t, ev, 64), 0);
    CHECK(!tinysid_midi_out_listens(t, 0));

    /* two voices to the same channel: each is let go where it was, then they come back together on the new channel */
    setp(t, "midi_out_ch2", "5");
    setp(t, "midi_out_ch3", "5");
    n = mout_drain(t, ev, 64);
    CHECK(mout_find(ev, n, 0, 1) >= 0 && ev[mout_find(ev, n, 0, 1)].note == 60);
    CHECK(mout_find(ev, n, 0, 2) >= 0 && ev[mout_find(ev, n, 0, 2)].note == 64);
    tinysid_render(t, L, R, 4410);
    n = mout_drain(t, ev, 64);
    int notes_on_5 = 0;
    for (int i = 0; i < n; i++) if (ev[i].type == 1 && ev[i].channel == 4) notes_on_5++;
    CHECK_EQ(notes_on_5, 2);
    CHECK(tinysid_midi_out_listens(t, 4) && !tinysid_midi_out_listens(t, 1) && !tinysid_midi_out_listens(t, 2));

    /* bends are merged while they wait: a vibrato never queues up behind itself, and a note keeps its bend in front of it */
    tinysid_destroy(t);
    t = held_voices(word_of(69), word_of(60), word_of(64), 0x41, 0);
    setp(t, "midi_out_v1", "On");
    tinysid_render(t, L, R, 4410);
    mout_drain(t, ev, 64);
    for (int k = 0; k < 20; k++) { setp(t, "transpose", k & 1 ? "1" : "0"); tinysid_render(t, L, R, 882); }   /* the pitch moves every frame */
    CHECK(tinysid_midi_out_pending(t) < 10);               /* not twenty bends (and note changes) piled up */
    n = mout_drain(t, ev, 64);
    int bends_in_a_row = 0, worst_row = 0;
    for (int i = 0; i < n; i++) {
        bends_in_a_row = ev[i].type == 2 ? bends_in_a_row + 1 : 0;
        if (bends_in_a_row > worst_row) worst_row = bends_in_a_row;
    }
    CHECK(worst_row <= 1);                                 /* never two waiting bends for one channel in a row */
    CHECK(tinysid_midi_out_longest(t) < MOUT_Q_TEST);

    /* the channels are 1..16, and another number is an error */
    setp(t, "midi_out_ch1", "0");  CHECK_STR(getp(t, "midi_out_ch1"), "1");
    setp(t, "midi_out_ch3", "99"); CHECK_STR(getp(t, "midi_out_ch3"), "16");
    CHECK_EQ(tinysid_set_param(t, "midi_out_ch2", "x"), -1);
    CHECK_EQ(tinysid_set_param(t, "midi_out_ch4", "2"), -1);
    CHECK_EQ(tinysid_set_param(t, "midi_out_v1", "Maybe"), -1);
    CHECK_EQ(tinysid_set_param(t, "midi_out_v4", "On"), -1);
    mout_drain(t, ev, 64);
    tinysid_destroy(t);

    /* a noise voice has no pitch to send */
    t = held_voices(word_of(60), word_of(62), word_of(64), 0x81, 0);
    setp(t, "midi_out_v3", "On");
    tinysid_render(t, L, R, 4410);
    CHECK_EQ(mout_drain(t, ev, 64), 0);
    tinysid_destroy(t);

    /* AutoTune brings the sound onto the grid, and the notes out follow what is heard */
    t = held_voices(word_of(69.37), word_of(60), word_of(64), 0x41, 0);
    setp(t, "pitch_snap", "A440 Chromatic");
    setp(t, "midi_out_v1", "On");
    tinysid_render(t, L, R, 30000);
    n = mout_drain(t, ev, 64);
    int bend = mid, last_note = -1;
    for (int i = 0; i < n; i++) { if (ev[i].type == 2) bend = ev[i].value; if (ev[i].type == 1) last_note = ev[i].note; }
    CHECK_EQ(last_note, 69);
    CHECK(abs(bend - mid) < 15 * per_cent);                /* within a few cents of the grid */
    tinysid_destroy(t);

    /* a new gate is a new note, even on the same pitch; a melody comes out note for note */
    const int notes[3] = { 60, 64, 67 };
    size_t len = fx_melody(fxbuf, notes, 3);
    t = tinysid_create(SR);
    CHECK_EQ(tinysid_load(t, fxbuf, len), PSID_OK);
    setp(t, "midi_out_v1", "On");
    tinysid_render(t, L, R, 8820);                         /* 10 frames at 50 Hz */
    n = mout_drain(t, ev, 64);
    int ons[16], on_n = 0, offs = 0;
    for (int i = 0; i < n; i++) {
        if (ev[i].type == 1 && on_n < 16) ons[on_n++] = ev[i].note;
        if (ev[i].type == 0) offs++;
    }
    CHECK(on_n >= 6);
    for (int i = 0; i < on_n && i < 6; i++) CHECK_EQ(ons[i], notes[i % 3]);
    CHECK(offs >= on_n - 1);                               /* each note is let go before the next */
    tinysid_destroy(t);

    /* the transport stopping lets go of the notes */
    t = held_voices(word_of(69), word_of(60), word_of(64), 0x41, 0);
    setp(t, "sync_mode", "Host Lock 1x");
    setp(t, "midi_out_v1", "On");
    tinysid_midi(t, &MIDI_START, 1);
    for (int i = 0; i < 8; i++) { tinysid_midi(t, &MIDI_CLOCK, 1); tinysid_render(t, L, R, 882); }
    n = mout_drain(t, ev, 64);
    CHECK(n >= 1 && ev[n - 1].type == 1);
    tinysid_midi(t, &MIDI_STOP, 1);
    n = mout_drain(t, ev, 64);
    CHECK(n >= 1 && ev[0].type == 0 && ev[0].note == 69);
    tinysid_destroy(t);
}

/* The module's screen hands every pad press to the engine, which carries it out, unless it is one of our own notes coming back. */
static void test_pad_press(void)
{
    SECTION("pad presses from the module's screen, and our own notes coming back");
    mev_t ev[64];
    unsigned seq, echoes;
    int last;
    tinysid_t *t = held_voices(word_of(69), word_of(60), word_of(64), 0x41, 0);
    CHECK(sscanf(getp(t, "pad_ack"), "%u,%d,%u", &seq, &last, &echoes) == 3 && seq == 0 && echoes == 0);
    setp(t, "pad_press", "68,100"); CHECK_STR(getp(t, "voice_mutes"), "1");      /* bottom row: mute voice 1 ... */
    setp(t, "pad_press", "68,100"); CHECK_STR(getp(t, "voice_mutes"), "0");
    setp(t, "pad_press", "70,90");  CHECK_STR(getp(t, "voice_mutes"), "4");      /* ... voice 3 */
    setp(t, "pad_press", "71,90");  CHECK_STR(getp(t, "voice_mutes"), "0");      /* all on */
    setp(t, "pad_press", "73,90");  CHECK_STR(getp(t, "transpose"), "1");        /* up */
    setp(t, "pad_press", "72,90");  setp(t, "pad_press", "72,90");
    CHECK_STR(getp(t, "transpose"), "-1");                                       /* down twice */
    setp(t, "pad_press", "74,90");  CHECK_STR(getp(t, "transpose"), "0");        /* back to 0 */
    setp(t, "pad_press", "77,90");  CHECK_STR(getp(t, "midi_out_v2"), "On");     /* second row: Notes Out for voice 2 */
    setp(t, "pad_press", "77,90");  CHECK_STR(getp(t, "midi_out_v2"), "Off");
    CHECK(sscanf(getp(t, "pad_ack"), "%u,%d,%u", &seq, &last, &echoes) == 3 && seq == 10 && last == 77 && echoes == 0);
    setp(t, "pad_press", "79,90");                                               /* an unused pad, an empty effect pad, a pad off the grid */
    setp(t, "pad_press", "92,90");
    setp(t, "pad_press", "60,90");
    CHECK(sscanf(getp(t, "pad_ack"), "%u,%d,%u", &seq, &last, &echoes) == 3 && seq == 10);
    CHECK_EQ(tinysid_set_param(t, "pad_press", "x"), -1);

    /* a pad whose note Notes Out has just sent is pressed like any other: on the device the screen's feed does not carry our notes */
    setp(t, "midi_out_v1", "On");
    tinysid_render(t, L, R, 4410);
    mout_drain(t, ev, 64);                                                       /* A4 (69) has gone out */
    setp(t, "pad_press", "69,100");
    CHECK_STR(getp(t, "voice_mutes"), "2");
    tinysid_destroy(t);


    /* the effect pads play the effects, at the press's velocity */
    t = fx_tune();
    tinysid_fx_scan_advance(t, 1000000);
    setp(t, "fx_duck", "0");
    tinysid_render(t, L, R, 20000);
    tinysid_render(t, L, R, 2000);
    double song = rms(L, 2000);
    setp(t, "pad_press", "93,127");                                              /* the second effect */
    tinysid_render(t, L, R, 4000);
    tinysid_render(t, L, R, 2000);
    CHECK(rms(L, 2000) > song * 1.25);
    CHECK(sscanf(getp(t, "pad_ack"), "%u,%d,%u", &seq, &last, &echoes) == 3 && seq == 1 && last == 93);
    setp(t, "pad_press", "94,127");                                              /* there is no third */
    CHECK(sscanf(getp(t, "pad_ack"), "%u,%d,%u", &seq, &last, &echoes) == 3 && seq == 1);
    tinysid_destroy(t);

    /* the same notes come back into the slot as MIDI: voice 1 sends C2 (note 36, a "mute voice 1" button in Control mode) */
    t = held_voices(word_of(36), word_of(60), word_of(64), 0x41, 0);
    setp(t, "pad_mode", "Control");
    setp(t, "midi_out_v1", "On");
    tinysid_render(t, L, R, 4410);
    mout_drain(t, ev, 64);
    const uint8_t c2_on[3] = { 0x90, 36, 100 }, c2_off[3] = { 0x80, 36, 0 }, c2_on_ch3[3] = { 0x92, 36, 100 };
    tinysid_midi(t, c2_on_ch3, 3);                         /* it comes back, on whatever channel ... */
    tinysid_midi(t, c2_off, 3);                            /* ... and its note-off goes with it */
    CHECK_STR(getp(t, "voice_mutes"), "0");                /* our own note: not a button */
    CHECK_STR(getp(t, "midi_echoes"), "1");
    tinysid_midi(t, c2_on, 3);                             /* the next C2 is a C2 again: the stock editor's button */
    CHECK_STR(getp(t, "voice_mutes"), "1");
    tinysid_midi(t, c2_off, 3);
    /* an echo that never comes is not owed for long: a C2 played half a second after one was sent is a C2 */
    setp(t, "midi_out_v1", "Off");
    setp(t, "midi_out_v1", "On");
    tinysid_render(t, L, R, 4410);
    mout_drain(t, ev, 64);
    tinysid_render(t, L, R, 22050);
    tinysid_midi(t, c2_on, 3);
    CHECK_STR(getp(t, "voice_mutes"), "0");
    tinysid_midi(t, c2_off, 3);

    /* with the module's own screen, incoming notes are never buttons: its pads go through pad_press */
    CHECK_STR(getp(t, "note_controls"), "1");
    setp(t, "note_controls", "0");
    CHECK_STR(getp(t, "note_controls"), "0");
    tinysid_render(t, L, R, 22050);
    tinysid_midi(t, c2_on, 3);
    tinysid_midi(t, c2_off, 3);
    CHECK_STR(getp(t, "voice_mutes"), "0");                /* the C2 changed nothing */
    setp(t, "pad_press", "68,100");                        /* while the screen's pad press still works */
    CHECK_STR(getp(t, "voice_mutes"), "1");
    tinysid_destroy(t);

    /* in Play mode an echo does not play the pad voice either */
    t = held_voices(word_of(60), word_of(62), word_of(64), 0x41, 0);
    setp(t, "midi_out_v2", "On");
    setp(t, "pad_target", "Voice 3");
    tinysid_render(t, L, R, 4410);
    mout_drain(t, ev, 64);
    const uint8_t d4_on[3] = { 0x90, 62, 100 };
    tinysid_midi(t, d4_on, 3);
    CHECK_STR(getp(t, "last_note"), "0");                  /* not even seen */
    tinysid_destroy(t);
}

/* The output delay: TinySID's own sound waits for tracks played by Notes Out, while it sends a voice. */
static void test_audio_delay(void)
{
    SECTION("audio delay: the output waits for tracks fed by Notes Out, only while a voice is sent");
    static float L2[1 << 16], R2[1 << 16];
    tinysid_t *a = make(FX_PLAIN, 1, 0, 0), *b = make(FX_PLAIN, 1, 0, 0);
    CHECK_STR(getp(a, "audio_delay"), "0");
    setp(b, "audio_delay", "20");
    CHECK_STR(getp(b, "audio_delay"), "20");
    tinysid_render(a, L, R, 8000);
    tinysid_render(b, L2, R2, 8000);
    CHECK(memcmp(L, L2, 8000 * sizeof(float)) == 0);      /* nothing sent: no delay at all */

    /* a voice sent: after the switch (a 5 ms fade, a 20 ms gap while the line fills, a fade back in), the same sound 20 ms later */
    setp(a, "midi_out_v1", "On");                          /* the reference sends too, so the two engines stay alike */
    setp(b, "midi_out_v1", "On");
    tinysid_render(a, L, R, 8000);
    tinysid_render(b, L2, R2, 8000);
    const int lag = 882;                                    /* 20 ms at 44.1 kHz */
    CHECK(rms(L2 + 300, 500) < 1e-6);                      /* the gap */
    int same = 1;
    for (int i = 2000; i < 8000; i++) if (fabsf(L2[i] - L[i - lag]) > 1e-5f) { same = 0; break; }
    CHECK(same);
    CHECK(all_finite_bounded(L2, 8000) && all_finite_bounded(R2, 8000));

    /* nothing sent any more: the delay goes, with the same fade */
    setp(a, "midi_out_v1", "Off");
    setp(b, "midi_out_v1", "Off");
    tinysid_render(a, L, R, 8000);
    tinysid_render(b, L2, R2, 8000);
    CHECK(memcmp(L + 2000, L2 + 2000, 6000 * sizeof(float)) == 0);
    double jump = 0, ref = 0;
    for (int i = 1; i < 8000; i++) {
        if (fabs(L2[i] - L2[i - 1]) > jump) jump = fabs(L2[i] - L2[i - 1]);
        if (fabs(L[i] - L[i - 1]) > ref) ref = fabs(L[i] - L[i - 1]);
    }
    CHECK(jump <= ref + 0.01);                             /* no step bigger than the tune's own saws make: no click on the way out */

    setp(b, "audio_delay", "900"); CHECK_STR(getp(b, "audio_delay"), "200");
    setp(b, "audio_delay", "-3");  CHECK_STR(getp(b, "audio_delay"), "0");
    CHECK_EQ(tinysid_set_param(b, "audio_delay", "x"), -1);
    tinysid_destroy(a);
    tinysid_destroy(b);
}

/* Loop capture: start a loop on the bar playing, stop it on the bar playing, and the dials follow. */
static void test_loop_capture(void)
{
    SECTION("loop capture: from the bar playing to the bar playing at the stop");
    enum { BAR = 96 };
    tinysid_t *t = make_counter();
    CHECK_STR(getp(t, "loop_capture"), "idle");
    setp(t, "loop_capture", "start");                      /* no tick yet: there is no bar to start on */
    CHECK_STR(getp(t, "loop_capture"), "idle");
    CHECK_EQ(tinysid_set_param(t, "loop_capture", "maybe"), -1);

    /* into bar 3, remembering the zero page at its downbeat */
    uint8_t zp3[256];
    for (int k = 0; k < 2 * BAR + 40; k++) {
        tick_send(t);
        if (k == 2 * BAR) for (int i = 0; i < 256; i++) zp3[i] = tinysid_ram(t, (uint16_t)i);
        tick_run(t);
    }
    CHECK_EQ(tinysid_song_bar(t), 3);
    setp(t, "pad_press", "75,100");                        /* the eighth pad of the bottom row: start */
    CHECK_STR(getp(t, "loop_capture"), "recording");
    CHECK_STR(getp(t, "loop_start"), "3");                 /* the bar that was playing, though its downbeat had passed */
    CHECK_STR(getp(t, "loop_length"), "Off");              /* the song plays on while it is captured */

    /* on through bar 4 into bar 5, then stop: bars 3, 4 and 5 */
    for (int k = 0; k < 2 * BAR; k++) { tick_send(t); tick_run(t); }
    CHECK_EQ(tinysid_song_bar(t), 5);
    setp(t, "pad_press", "75,100");
    CHECK_STR(getp(t, "loop_capture"), "looping");
    CHECK_STR(getp(t, "loop_length"), "3 Bars");
    CHECK_STR(getp(t, "loop_start"), "3");

    /* the downbeat of bar 6 goes back to bar 3, as bar 3 began */
    uint64_t rewinds = tinysid_phrase_rewinds(t);
    for (int k = 0; k < BAR - 40; k++) { tick_send(t); tick_run(t); }
    tick_send(t);
    CHECK_EQ(tinysid_song_bar(t), 3);
    CHECK_EQ(tinysid_phrase_rewinds(t), rewinds + 1);
    CHECK(zero_page_equals(t, zp3));
    tick_run(t);

    /* a new capture while looping: the old loop stops, the song plays on from where it is */
    for (int k = 0; k < BAR; k++) { tick_send(t); tick_run(t); }   /* now in bar 4 of the loop */
    setp(t, "loop_capture", "toggle");
    CHECK_STR(getp(t, "loop_capture"), "recording");
    CHECK_STR(getp(t, "loop_start"), "4");
    for (int k = 0; k < 12 * BAR; k++) { tick_send(t); tick_run(t); }
    CHECK_EQ(tinysid_song_bar(t), 16);
    setp(t, "loop_capture", "toggle");
    CHECK_STR(getp(t, "loop_length"), "8 Bars");           /* longer than 8 bars keeps the first 8 */
    CHECK_STR(getp(t, "loop_capture"), "looping");
    tick_send(t);
    tick_run(t);
    for (int k = 0; k < BAR; k++) { tick_send(t); tick_run(t); }
    CHECK(tinysid_song_bar(t) >= 4 && tinysid_song_bar(t) <= 11);  /* it went back into the loop */

    /* cancel, and the readout when the loop is switched off by its dial */
    setp(t, "loop_capture", "start");
    setp(t, "loop_capture", "cancel");
    CHECK_STR(getp(t, "loop_capture"), "idle");            /* the old loop had stopped at the start, and nothing was set */
    tinysid_destroy(t);
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
    test_pad_filter_route();
    test_pad_isolation();
    test_timbre_lock();
    test_timbre_hard_restart();
    test_autotune();
    test_glue_dc();
    test_glue_spread();
    test_glue_ducking();
    test_transpose_bend();
    test_pad_controls();
    test_pad_in_tune();
    test_pad_release_and_bend();
    test_filter_pump();
    test_song_beat();
    test_sound_effects();
    test_notes_out();
    test_pad_press();
    test_audio_delay();
    test_lofi();
    test_mix_feed();
    test_bar_loop();
    test_loop_alignment();
    test_loop_capture();
    test_loop_seam();
    test_pending_readouts();
    test_scan_headless();
    test_robustness();
    test_leaks();
    return test_finish("test_engine");
}
