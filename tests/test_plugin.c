/* Exercises the exported plugin ABI exactly as a host would. */
#include <stdlib.h>

#include "schwung_api.h"
#include "test_util.h"

plugin_api_v2_t *SCHWUNG_INIT_SYMBOL(const host_api_v1_t *host);

/* Brace / bracket balance outside strings: a cheap check that a document was not truncated. */
static int json_balanced(const char *s)
{
    int depth = 0, in_str = 0;
    for (; *s; s++) {
        if (in_str) { if (*s == '\\') s++; else if (*s == '"') in_str = 0; continue; }
        if (*s == '"') in_str = 1;
        else if (*s == '{' || *s == '[') depth++;
        else if (*s == '}' || *s == ']') depth--;
        if (depth < 0) return 0;
    }
    return depth == 0 && !in_str;
}

int main(void)
{
    SECTION("plugin ABI");
    plugin_api_v2_t *api = SCHWUNG_INIT_SYMBOL(NULL);
    CHECK(api != NULL);
    CHECK_EQ(api->api_version, 2);
    CHECK(api->create_instance && api->destroy_instance && api->on_midi && api->set_param &&
          api->get_param && api->get_error && api->render_block);

    void *inst = api->create_instance(".", "{}");
    CHECK(inst != NULL);

    char buf[64], err[64];
    CHECK(api->get_param(inst, "subtune_count", buf, sizeof(buf)) > 0);
    CHECK(atoi(buf) >= 1);
    CHECK(api->get_param(inst, "title", buf, sizeof(buf)) > 0);
    printf("  loaded: %s\n", buf);

    static int16_t pcm[128 * 2 * 64];
    int peak = 0;
    for (int b = 0; b < 64; b++) {
        api->render_block(inst, pcm + b * 256, 128);
    }
    for (int i = 0; i < 128 * 2 * 64; i++) {
        int a = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (a > peak) peak = a;
    }
    CHECK(peak > 500);                                      /* audible straight after insertion */
    CHECK(peak < 32768);
    int mismatch = 0;
    for (int i = 0; i < 128 * 64; i++) if (pcm[i * 2] != pcm[i * 2 + 1]) mismatch++;
    CHECK_EQ(mismatch, 0);

    /* params round-trip through the string API */
    api->set_param(inst, "v1_vol", "0.5");
    CHECK(api->get_param(inst, "v1_vol", buf, sizeof(buf)) > 0);
    CHECK_STR(buf, "0.5");
    api->set_param(inst, "chip_model", "8580");
    api->get_param(inst, "chip_model", buf, sizeof(buf));
    CHECK_STR(buf, "8580");
    CHECK_EQ(api->get_param(inst, "bogus", buf, sizeof(buf)), -1);

    /* keys the host writes to every module are ignored without a warning; our own parameters with a bad value are not */
    api->set_param(inst, "preset", "0");
    api->set_param(inst, "some_future_host_key", "1");
    CHECK_EQ(api->get_error(inst, err, sizeof(err)), 0);
    api->set_param(inst, "subtune", "garbage");
    CHECK(api->get_error(inst, err, sizeof(err)) > 0);
    CHECK(strstr(err, "subtune") != NULL);

    /* errors are reported once through get_error */
    CHECK_EQ(api->get_error(inst, err, sizeof(err)), 0);
    api->set_param(inst, "out_trim", "loud");
    CHECK(api->get_error(inst, err, sizeof(err)) > 0);
    CHECK_EQ(api->get_error(inst, err, sizeof(err)), 0);

    /* MIDI path: pad jam on voice 1 */
    api->set_param(inst, "pad_target", "Voice 1");
    uint8_t on[3] = { 0x90, 60, 100 };
    api->on_midi(inst, on, 3, 0);
    api->render_block(inst, pcm, 128);

    /* blocks larger than the internal scratch are split transparently */
    static int16_t big[2048 * 2];
    api->render_block(inst, big, 2048);

    api->destroy_instance(inst);

    /* ---- documents the host reads through get_param ---- */
    SECTION("plugin documents and state");
    inst = api->create_instance(".", "{}");
    static char doc[8192];

    int n = api->get_param(inst, "ui_hierarchy", doc, sizeof(doc));
    CHECK(n > 100 && n < 3600);
    CHECK_EQ(n, (int)strlen(doc));
    CHECK(json_balanced(doc));
    CHECK(strstr(doc, "\"levels\"") && strstr(doc, "\"root\"") && strstr(doc, "\"knobs\""));
    CHECK(strstr(doc, "\"pad_target\"") && strstr(doc, "\"sync_mode\"") && strstr(doc, "\"song_bpm\""));
    CHECK(strstr(doc, "\"level\":\"song\"") && strstr(doc, "\"level\":\"pad\""));   /* the Song and Pad + Filter pages are linked from the mixer */
    CHECK(strstr(doc, "\"snoop_voice\"") && strstr(doc, "\"piano_roll\""));           /* the snooper and its piano roll live on the Song page */
    CHECK(!strstr(doc, "\"level\":\"snoop\"") && !strstr(doc, "\"level\":\"filter\""));
    printf("  ui_hierarchy %d bytes\n", n);

    n = api->get_param(inst, "chain_params", doc, sizeof(doc));
    CHECK(n > 100 && n < 3600);
    CHECK(json_balanced(doc) && doc[0] == '[');
    CHECK(strstr(doc, "\"Host Lock 0.5x\""));
    CHECK(strstr(doc, "\"access\":\"read\""));                     /* song_bpm is a readout */
    CHECK(strstr(doc, "\"max\":19") == NULL);                    /* the demo tune has 2 subtunes, not 19 */
    CHECK(strstr(doc, "\"key\":\"pad_sound\",\"name\":\"Pad Sound\",\"type\":\"enum\",\"options\":[\"Lead\"]") != NULL);
    CHECK(strstr(doc, "run_mode") == NULL);
    CHECK(strstr(doc, "\"key\":\"subtune\",\"name\":\"Subtune\",\"type\":\"int\",\"min\":1,\"max\":2,"));
    CHECK(strstr(doc, "@") == NULL);                               /* no placeholder leaks out */
    api->set_param(inst, "load_path", TINYSID_ASSET_DIR "/Commando.sid");
    if (api->get_error(inst, err, sizeof(err)) == 0) {              /* only when the local file is present */
        api->get_param(inst, "chain_params", doc, sizeof(doc));
        CHECK(strstr(doc, "\"min\":1,\"max\":19,"));
        api->set_param(inst, "load_path", TINYSID_ASSET_DIR "/demo.sid");
    }
    printf("  chain_params %d bytes\n", n);
    /* the canvas parameter carries what the host needs to open its screen and feed it live values */
    CHECK(strstr(doc, "\"key\":\"piano_roll\"") != NULL);
    CHECK(strstr(doc, "\"canvas_script\":\"canvas.js\"") != NULL);
    CHECK(strstr(doc, "\"as_page\":true,\"page_first\":true") != NULL);          /* a page in the rotation, not a click-to-open cell */
    CHECK(strstr(doc, "\"extra_keys\":[\"snoop_feed\"]") != NULL);
    CHECK(strstr(doc, "\"key\":\"snoop_feed\",\"name\":\"Snoop feed\",\"type\":\"string\",\"access\":\"read\"") != NULL);
    CHECK(strstr(doc, "\"D Min\"") != NULL && strstr(doc, "\"Voice 3 (Arp/Drm)\"") != NULL);
    CHECK(strstr(doc, "\"key\":\"snoop_note\",\"name\":\"Note\",\"type\":\"int\",\"min\":0,\"max\":127") != NULL);

    CHECK_EQ(api->get_param(inst, "ui_hierarchy", doc, 100), -1);   /* never a truncated document */
    CHECK_EQ(api->get_param(inst, "preset_count", doc, sizeof(doc)), 1);
    CHECK_STR(doc, "0");
    api->get_param(inst, "name", doc, sizeof(doc));
    CHECK_STR(doc, "TinySID");

    /* a host writing every default at load must not disturb the tempo readout */
    char bpm[16];
    api->get_param(inst, "song_bpm", bpm, sizeof(bpm));
    api->set_param(inst, "song_bpm", "116");
    char bpm2[16];
    api->get_param(inst, "song_bpm", bpm2, sizeof(bpm2));
    CHECK_STR(bpm2, bpm);

    /* once the tune has been scanned, the Pad Sound list names its instruments and is_loading says so */
    api->get_param(inst, "is_loading", bpm, sizeof(bpm));
    CHECK_STR(bpm, "1");
    for (int b = 0; b < 2000; b++) api->render_block(inst, pcm, 128);
    api->get_param(inst, "is_loading", bpm, sizeof(bpm));
    CHECK_STR(bpm, "0");
    n = api->get_param(inst, "chain_params", doc, sizeof(doc));
    CHECK(n > 0 && json_balanced(doc));
    CHECK(strstr(doc, "\"Pad Sound\",\"type\":\"enum\",\"options\":[\"1 ") != NULL);   /* the song's own instruments */
    CHECK(strstr(doc, "@") == NULL);
    printf("  chain_params after the scan: %d bytes\n", n);

    /* state round trip into a fresh instance */
    api->set_param(inst, "subtune", "2");
    api->set_param(inst, "v2_vol", "0.25");
    api->set_param(inst, "out_trim", "-4.5");
    api->set_param(inst, "sync_mode", "Host Lock 2x");
    api->set_param(inst, "tempo_trim", "1.25");
    api->set_param(inst, "chip_model", "8580");
    api->set_param(inst, "cutoff_bias", "-20");
    api->set_param(inst, "res_bias", "7");
    api->set_param(inst, "pad_target", "Voice 3");
    api->set_param(inst, "pad_sound", "3");
    n = api->get_param(inst, "state", doc, sizeof(doc));
    CHECK(strstr(doc, "\"pad_sound\":3") != NULL);                /* saved as its list position */
    CHECK(strstr(doc, "run_mode") == NULL);
    CHECK(n > 50 && json_balanced(doc));
    printf("  state: %s\n", doc);
    CHECK_EQ(api->get_param(inst, "state", doc + 8000, 20), -1);

    void *inst2 = api->create_instance(".", "{}");
    api->set_param(inst2, "state", doc);
    const char *keys[] = { "subtune", "v2_vol", "out_trim", "sync_mode", "tempo_trim", "chip_model",
                           "cutoff_bias", "res_bias", "pad_target", "pad_sound_index" };
    const char *want[] = { "2", "0.25", "-4.5", "Host Lock 2x", "1.25", "8580", "-20", "7", "Voice 3", "3" };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        api->get_param(inst2, keys[i], bpm, sizeof(bpm));
        CHECK_STR(bpm, want[i]);
    }
    CHECK_EQ(api->get_error(inst2, err, sizeof(err)), 0);

    api->set_param(inst2, "state", "{\"v1_vol\":0.5,\"bogus\":3,\"subtune\":\"1\"}");   /* unknown keys are ignored */
    api->get_param(inst2, "v1_vol", bpm, sizeof(bpm));
    CHECK_STR(bpm, "0.5");
    api->get_param(inst2, "subtune", bpm, sizeof(bpm));
    CHECK_STR(bpm, "1");
    api->set_param(inst2, "state", "garbage{{");
    api->set_param(inst2, "state", "");

    /* a custom file survives in the state */
    api->set_param(inst2, "sid_file", TINYSID_ASSET_DIR "/demo.sid");
    n = api->get_param(inst2, "state", doc, sizeof(doc));
    CHECK(strstr(doc, "\"sid_file\""));
    api->get_param(inst2, "sid_file", doc, sizeof(doc));
    CHECK(strstr(doc, "demo.sid") != NULL);                         /* the picker reads back the chosen file */
    api->set_param(inst2, "sid_file", "");                          /* the picker's empty default changes nothing */
    api->get_param(inst2, "sid_file", doc, sizeof(doc));
    CHECK(strstr(doc, "demo.sid") != NULL);
    api->set_param(inst2, "sid_file", "/no/such/file.sid");
    CHECK(api->get_error(inst2, err, sizeof(err)) > 0);
    api->get_param(inst2, "sid_file", doc, sizeof(doc));
    CHECK(strstr(doc, "demo.sid") != NULL);                         /* a failed pick keeps the old one */
    api->destroy_instance(inst);
    api->destroy_instance(inst2);
    api->destroy_instance(NULL);
    return test_finish("test_plugin");
}
