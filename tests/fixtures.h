/* Hand-assembled PSID fixtures shared by the tests. */
#ifndef FIXTURES_H
#define FIXTURES_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define FX_LOAD 0x1000

/* Builds a PSID v2 file with a non-zero load address in the header. */
static size_t fx_psid(uint8_t *out, const uint8_t *code, size_t code_len, uint16_t load,
                      uint16_t init, uint16_t play, int songs, uint32_t speed, uint16_t flags)
{
    memset(out, 0, 0x7C);
    memcpy(out, "PSID", 4);
    out[5] = 2;
    out[7] = 0x7C;
    out[8] = (uint8_t)(load >> 8);  out[9] = (uint8_t)load;
    out[10] = (uint8_t)(init >> 8); out[11] = (uint8_t)init;
    out[12] = (uint8_t)(play >> 8); out[13] = (uint8_t)play;
    out[14] = (uint8_t)(songs >> 8); out[15] = (uint8_t)songs;
    out[17] = 1;
    out[18] = (uint8_t)(speed >> 24); out[19] = (uint8_t)(speed >> 16);
    out[20] = (uint8_t)(speed >> 8);  out[21] = (uint8_t)speed;
    memcpy(out + 0x16, "Fixture", 7);
    memcpy(out + 0x36, "Tester", 6);
    memcpy(out + 0x56, "2026", 4);
    out[0x76] = (uint8_t)(flags >> 8);
    out[0x77] = (uint8_t)flags;
    memcpy(out + 0x7C, code, code_len);
    return 0x7C + code_len;
}

/* tiny emitter */
typedef struct { uint8_t b[768]; size_t n; } fx_code_t;
static void fx_byte(fx_code_t *c, int v) { c->b[c->n++] = (uint8_t)v; }
static void fx_store(fx_code_t *c, int val, int addr)   /* LDA #val ; STA addr */
{
    fx_byte(c, 0xA9); fx_byte(c, val);
    fx_byte(c, 0x8D); fx_byte(c, addr & 0xFF); fx_byte(c, addr >> 8);
}

enum {
    FX_PLAIN = 0,      /* init sets up three saw voices, play is just RTS */
    FX_WRITER = 1,     /* play rewrites $D401 (voice 1 freq hi) and $D416 (cutoff) every frame */
    FX_HANG = 2,       /* play is an endless JMP loop */
    FX_CIA = 3,        /* init programs CIA1 timer A to 16421 cycles (~60 Hz) */
    FX_COUNTER = 4,    /* play bumps zero-page $03 and mirrors it into every voice's freq hi ($D401/$D408/$D40F) */
    FX_KILLV2 = 5,     /* play silences voice 2: control, AD and SR all written to 0 */
    FX_LONGREL = 6,    /* voice 2 has release setting 15 (24 s) */
    FX_GATER = 100     /* FX_GATER + k: voice 1 gates on every k frames (k = 3..99) */
};

/* Init: STA $02 (stores A = subtune-1), then voices at 240/480/720 Hz, $D417 = $80, volume 15.
 * The three voices play near-full-sustain saws (AD $20, SR $E8); there is no filter routing. */
static size_t fx_tone(uint8_t *out, int variant, int songs, uint32_t speed, uint16_t flags)
{
    fx_code_t c = { {0}, 0 };
    fx_byte(&c, 0x85); fx_byte(&c, 0x02);
    fx_store(&c, 0x0F, 0xD418);
    fx_store(&c, 0x80, 0xD417);
    for (int v = 0; v < 3; v++) {
        int base = 0xD400 + v * 7;
        fx_store(&c, 0x10 * (v + 1), base + 1);
        fx_store(&c, 0x20, base + 5);                /* AD: distinct from the idle-fallback patch */
        fx_store(&c, 0xE8, base + 6);                /* SR: sustain 14, release 8 (300 ms) */
        fx_store(&c, 0x21, base + 4);
    }
    if (variant == FX_LONGREL) fx_store(&c, 0xEF, 0xD40D);
    if (variant == FX_CIA) {
        fx_store(&c, 0x25, 0xDC04);
        fx_store(&c, 0x40, 0xDC05);
    }
    fx_byte(&c, 0x60);                                   /* RTS */

    uint16_t play = (uint16_t)(FX_LOAD + c.n);
    if (variant == FX_WRITER) {
        fx_store(&c, 0x99, 0xD401);
        fx_store(&c, 0x40, 0xD416);
        fx_byte(&c, 0x60);
    } else if (variant == FX_COUNTER) {
        fx_byte(&c, 0xE6); fx_byte(&c, 0x03);                     /* INC $03 */
        fx_byte(&c, 0xA5); fx_byte(&c, 0x03);                     /* LDA $03 */
        for (int a = 0xD401; a <= 0xD40F; a += 7) {               /* STA $D401 / $D408 / $D40F */
            fx_byte(&c, 0x8D); fx_byte(&c, a & 0xFF); fx_byte(&c, a >> 8);
        }
        fx_byte(&c, 0x60);
    } else if (variant == FX_KILLV2) {
        fx_store(&c, 0x00, 0xD40B);
        fx_byte(&c, 0x8D); fx_byte(&c, 0x0C); fx_byte(&c, 0xD4);   /* STA $D40C */
        fx_byte(&c, 0x8D); fx_byte(&c, 0x0D); fx_byte(&c, 0xD4);   /* STA $D40D */
        fx_byte(&c, 0x60);
    } else if (variant >= FX_GATER) {
        static const uint8_t body[] = {
            0xA9, 0x20, 0x8D, 0x04, 0xD4,        /* LDA #$20 ; STA $D404   gate off */
            0xE6, 0x04, 0xA5, 0x04,              /* INC $04  ; LDA $04     frame counter */
            0xC9, 0x00,                          /* CMP #k (patched)       */
            0x90, 0x09,                          /* BCC done               */
            0xA9, 0x00, 0x85, 0x04,              /* LDA #0   ; STA $04     */
            0xA9, 0x21, 0x8D, 0x04, 0xD4,        /* LDA #$21 ; STA $D404   gate on */
            0x60                                 /* done: RTS */
        };
        for (size_t i = 0; i < sizeof(body); i++) fx_byte(&c, i == 10 ? variant - FX_GATER : body[i]);
    } else if (variant == FX_HANG) {
        fx_byte(&c, 0x4C); fx_byte(&c, play & 0xFF); fx_byte(&c, play >> 8);
    } else {
        fx_byte(&c, 0x60);
    }
    return fx_psid(out, c.b, c.n, FX_LOAD, FX_LOAD, play, songs, speed, flags);
}

/* A tune that plays `notes` (MIDI numbers) in a loop on voice 1, re-gating every frame. Used for key detection. */
static inline size_t fx_melody(uint8_t *out, const int *notes, int n)
{
    fx_code_t c = { {0}, 0 };
    fx_store(&c, 0x0F, 0xD418);
    fx_store(&c, 0x00, 0xD405);
    fx_store(&c, 0xF0, 0xD406);
    fx_byte(&c, 0x60);                                           /* init: RTS */

    uint16_t play = (uint16_t)(FX_LOAD + c.n);
    const int code_len = 34;
    uint16_t lotab = (uint16_t)(play + code_len), hitab = (uint16_t)(lotab + n);
    const uint8_t body[] = {
        0xA9, 0x20, 0x8D, 0x04, 0xD4,                            /* gate off */
        0xA6, 0x05,                                              /* LDX $05 */
        0xBD, (uint8_t)lotab, (uint8_t)(lotab >> 8),             /* LDA lotab,X */
        0x8D, 0x00, 0xD4,                                        /* STA $D400 */
        0xBD, (uint8_t)hitab, (uint8_t)(hitab >> 8),             /* LDA hitab,X */
        0x8D, 0x01, 0xD4,                                        /* STA $D401 */
        0xA9, 0x21, 0x8D, 0x04, 0xD4,                            /* gate on */
        0xE8,                                                    /* INX */
        0xE0, (uint8_t)n,                                        /* CPX #n */
        0x90, 0x02,                                              /* BCC +2 */
        0xA2, 0x00,                                              /* LDX #0 */
        0x86, 0x05,                                              /* STX $05 */
        0x60
    };
    for (size_t i = 0; i < sizeof(body); i++) fx_byte(&c, body[i]);
    for (int i = 0; i < n; i++) fx_byte(&c, (int)(440.0 * pow(2.0, (notes[i] - 69) / 12.0) * 16777216.0 / 985248.0 + 0.5) & 0xFF);
    for (int i = 0; i < n; i++) fx_byte(&c, (int)(440.0 * pow(2.0, (notes[i] - 69) / 12.0) * 16777216.0 / 985248.0 + 0.5) >> 8);
    return fx_psid(out, c.b, c.n, FX_LOAD, FX_LOAD, play, 1, 0, 0x0004);
}

/* A tune whose voice 1 plays through `pattern` (indexes into the instrument tables), one gate-on per frame:
 * every `hold` frames it hard-restarts the voice (AD/SR = 0), gates it on, then writes the real AD and SR. PW is $0523 throughout. */
static inline size_t fx_instruments(uint8_t *out, const int *pattern, int n, const uint8_t *wave, const uint8_t *ad,
                                    const uint8_t *sr, int kinds, int hold)
{
    fx_code_t c = { {0}, 0 };
    fx_store(&c, 0x0F, 0xD418);
    fx_store(&c, 0x23, 0xD402);
    fx_store(&c, 0x05, 0xD403);
    fx_store(&c, 0x10, 0xD401);
    fx_byte(&c, 0x60);                                           /* init: RTS */

    uint16_t play = (uint16_t)(FX_LOAD + c.n);
    const int code_len = 58;
    uint16_t patt = (uint16_t)(play + code_len), adt = (uint16_t)(patt + n);
    uint16_t srt = (uint16_t)(adt + kinds), wvt = (uint16_t)(srt + kinds);
    /* "hard restart" like real drivers: gate off, AD/SR = 0, gate on with the waveform, and only then the real AD/SR */
    const uint8_t body[] = {
        0xE6, 0x06, 0xA5, 0x06, 0xC9, (uint8_t)hold,             /* INC $06 ; LDA $06 ; CMP #hold */
        0x90, 0x31,                                              /* BCC done: hold the current instrument */
        0xA9, 0x00, 0x85, 0x06,                                  /* LDA #0 ; STA $06 */
        0xA9, 0x00, 0x8D, 0x04, 0xD4,                            /* gate off */
        0xA9, 0x00, 0x8D, 0x05, 0xD4,                            /* AD = 0 */
        0x8D, 0x06, 0xD4,                                        /* SR = 0 */
        0xA6, 0x05,                                              /* LDX $05 */
        0xBC, (uint8_t)patt, (uint8_t)(patt >> 8),               /* LDY pattern,X */
        0xB9, (uint8_t)wvt, (uint8_t)(wvt >> 8),                 /* LDA wavetab,Y */
        0x8D, 0x04, 0xD4,                                        /* STA $D404 (waveform + gate) */
        0xB9, (uint8_t)adt, (uint8_t)(adt >> 8),                 /* LDA adtab,Y */
        0x8D, 0x05, 0xD4,                                        /* STA $D405 */
        0xB9, (uint8_t)srt, (uint8_t)(srt >> 8),                 /* LDA srtab,Y */
        0x8D, 0x06, 0xD4,                                        /* STA $D406 */
        0xE8,                                                    /* INX */
        0xE0, (uint8_t)n,                                        /* CPX #n */
        0x90, 0x02,                                              /* BCC +2 */
        0xA2, 0x00,                                              /* LDX #0 */
        0x86, 0x05,                                              /* STX $05 */
        0x60
    };
    for (size_t i = 0; i < sizeof(body); i++) fx_byte(&c, body[i]);
    for (int i = 0; i < n; i++) fx_byte(&c, pattern[i]);
    for (int i = 0; i < kinds; i++) fx_byte(&c, ad[i]);
    for (int i = 0; i < kinds; i++) fx_byte(&c, sr[i]);
    for (int i = 0; i < kinds; i++) fx_byte(&c, wave[i]);
    return fx_psid(out, c.b, c.n, FX_LOAD, FX_LOAD, play, 1, 0, 0x0004);
}

/* A held voice-1 note (pulse, AD $38, SR $7A) whose driver moves it every frame, like a real lead: the pulse width
 * sweeps through `pw[]` and the pitch follows `freq[]`, both n entries long and looping. */
static inline size_t fx_movement(uint8_t *out, int n, const uint16_t *pw, const uint16_t *freq)
{
    fx_code_t c = { {0}, 0 };
    fx_store(&c, 0x0F, 0xD418);
    fx_store(&c, 0x38, 0xD405);
    fx_store(&c, 0x7A, 0xD406);
    fx_store(&c, 0x41, 0xD404);                                  /* gate on once, held */
    fx_byte(&c, 0x60);

    uint16_t play = (uint16_t)(FX_LOAD + c.n);
    const int code_len = 36;
    uint16_t t0 = (uint16_t)(play + code_len);                   /* pwlo, pwhi, flo, fhi tables, n entries each */
    uint16_t pwl = t0, pwh = (uint16_t)(t0 + n), fl = (uint16_t)(t0 + 2 * n), fh = (uint16_t)(t0 + 3 * n);
    const uint8_t body[] = {
        0xA6, 0x05,                                              /* LDX $05 */
        0xBD, (uint8_t)pwl, (uint8_t)(pwl >> 8), 0x8D, 0x02, 0xD4,   /* LDA pwlo,X ; STA $D402 */
        0xBD, (uint8_t)pwh, (uint8_t)(pwh >> 8), 0x8D, 0x03, 0xD4,   /* LDA pwhi,X ; STA $D403 */
        0xBD, (uint8_t)fl, (uint8_t)(fl >> 8), 0x8D, 0x00, 0xD4,     /* LDA flo,X  ; STA $D400 */
        0xBD, (uint8_t)fh, (uint8_t)(fh >> 8), 0x8D, 0x01, 0xD4,     /* LDA fhi,X  ; STA $D401 */
        0xE8, 0xE0, (uint8_t)n, 0x90, 0x02, 0xA2, 0x00, 0x86, 0x05, 0x60
    };
    for (size_t i = 0; i < sizeof(body); i++) fx_byte(&c, body[i]);
    for (int i = 0; i < n; i++) fx_byte(&c, pw[i] & 0xFF);
    for (int i = 0; i < n; i++) fx_byte(&c, pw[i] >> 8);
    for (int i = 0; i < n; i++) fx_byte(&c, freq[i] & 0xFF);
    for (int i = 0; i < n; i++) fx_byte(&c, freq[i] >> 8);
    return fx_psid(out, c.b, c.n, FX_LOAD, FX_LOAD, play, 1, 0, 0x0004);
}

/* Three voices set up once in init: frequency word and control byte each. `flags` is the PSID flags word. */
static inline size_t fx_snoop(uint8_t *out, const int words[3], const int ctrls[3], uint16_t flags)
{
    fx_code_t c = { {0}, 0 };
    fx_store(&c, 0x0F, 0xD418);
    for (int v = 0; v < 3; v++) {
        int base = 0xD400 + v * 7;
        fx_store(&c, words[v] & 0xFF, base);
        fx_store(&c, words[v] >> 8, base + 1);
        fx_store(&c, 0x00, base + 5);
        fx_store(&c, 0xF0, base + 6);
        fx_store(&c, ctrls[v], base + 4);
    }
    fx_byte(&c, 0x60);
    uint16_t play = (uint16_t)(FX_LOAD + c.n);
    fx_byte(&c, 0x60);
    return fx_psid(out, c.b, c.n, FX_LOAD, FX_LOAD, play, 1, 0, flags);
}

/* Overwrites the 32-byte title field of a built PSID. */
static inline void fx_set_title(uint8_t *psid, const char *title)
{
    memset(psid + 0x16, 0, 32);
    memcpy(psid + 0x16, title, strlen(title) > 31 ? 31 : strlen(title));
}

#endif
