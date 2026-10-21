/* How expensive is the headless pre-scan? It runs the 6502 alone (no audio synthesis) for 1,500 frames, i.e. 30 s of
 * music at 50 Hz. Built WITHOUT sanitizers by scripts/test.sh so the numbers mean something.
 *
 * Two limits are enforced:
 *   - the built-in demo scans 1,500 frames in under 15 ms (headless is cheap for ordinary tunes);
 *   - on EVERY tune one audio block's share of the scan (ANA_SLICE = 8 frames) stays under 400 us. This is what keeps
 *     the audio thread safe, because real players differ a lot: some use most of a frame's 20,000 cycles, and a
 *     scan that ran all 1,500 frames at once would block the audio thread for as long as the total printed below. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "default_sid.h"
#include "tinysid.h"

#define FRAMES 1500
#define REPS 25
#define SLICE_FRAMES 8
#define DEMO_BUDGET_MS 15.0
#define SLICE_BUDGET_US 400.0

static double now_ms(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void bench(const unsigned char *data, size_t len, const char *name, int is_demo, int *ok)
{
    double total = 0, worst = 0;
    for (int i = 0; i < REPS; i++) {
        tinysid_t *t = tinysid_create(44100.0);
        if (tinysid_load(t, data, len) != 0) { printf("  %s: load failed\n", name); *ok = 0; tinysid_destroy(t); return; }
        double t0 = now_ms();
        tinysid_scan_advance(t, FRAMES);
        double ms = now_ms() - t0;
        total += ms;
        if (ms > worst) worst = ms;
        if (tinysid_scan_frames(t) != FRAMES) { printf("  %s: scan stopped at %d frames\n", name, tinysid_scan_frames(t)); *ok = 0; }
        tinysid_destroy(t);
    }
    double avg = total / REPS, slice_us = avg * 1000.0 / FRAMES * SLICE_FRAMES;
    int slice_ok = slice_us < SLICE_BUDGET_US, total_ok = !is_demo || avg < DEMO_BUDGET_MS;
    printf("  %-18s 1500 frames: avg %6.2f ms, worst %6.2f ms | per audio block (8 frames): %6.1f us  %s%s%s\n", name, avg, worst,
           slice_us, slice_ok && total_ok ? "ok" : "OVER BUDGET",
           !is_demo && avg >= DEMO_BUDGET_MS ? "  (all at once would block the audio thread: that is why it is sliced)" : "",
           "");
    if (!slice_ok || !total_ok) *ok = 0;
}

int main(int argc, char **argv)
{
    int ok = 1;
    printf("headless pre-scan benchmark (no sanitizers); limits: demo < %.0f ms per 1500 frames, every tune < %.0f us per block\n",
           DEMO_BUDGET_MS, SLICE_BUDGET_US);
    bench(default_sid_data, default_sid_len, "built-in demo", 1, &ok);
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) continue;
        static unsigned char buf[0x10400];
        size_t n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        const char *base = argv[i];
        for (const char *p = argv[i]; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
        bench(buf, n, base, 0, &ok);
    }
    return ok ? 0 : 1;
}
