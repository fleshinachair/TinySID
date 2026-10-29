/* Exercises the exported plugin ABI exactly as a host would. */
#include <stdlib.h>

#include "schwung_api.h"
#include "test_util.h"

plugin_api_v2_t *SCHWUNG_INIT_SYMBOL(const host_api_v1_t *host);

/* A host that takes MIDI: it records every 4-byte packet, and can be told to refuse them (its buffer is full). */
static uint8_t g_pkts[256][4];
static int g_pkt_n, g_accept = 1;
static int g_slot_channel = -1;
static int mock_slot_channel(void *instance) { (void)instance; return g_slot_channel; }
static int mock_inject(const uint8_t *msg, int len)
{
    if (!g_accept || len != 4 || g_pkt_n >= 256) return 0;
    memcpy(g_pkts[g_pkt_n++], msg, 4);
    return 4;
}

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
    CHECK(mismatch > 0);                                    /* the default spread is stereo */

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
    api->set_param(inst, "v1_vol", "loud");
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
    CHECK(strstr(doc, "\"level\":\"tune\"") && strstr(doc, "\"level\":\"glue\"") && strstr(doc, "\"level\":\"pad\"") && !strstr(doc, "\"level\":\"song\""));   /* the Song and Pad + Filter pages are linked from the mixer */
    CHECK(strstr(doc, "\"piano_roll\"") && !strstr(doc, "\"snoop_voice\""));           /* the snooper and its piano roll live on the Song page */
    CHECK(!strstr(doc, "\"level\":\"snoop\"") && !strstr(doc, "\"level\":\"filter\""));
    printf("  ui_hierarchy %d bytes\n", n);

    n = api->get_param(inst, "chain_params", doc, sizeof(doc));
    CHECK(n > 100 && n < 3800);
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
    CHECK(strstr(doc, "\"extra_keys\":[\"snoop_feed\",\"mix_feed\"]") != NULL);
    CHECK(strstr(doc, "\"key\":\"snoop_feed\",\"name\":\"Snoop feed\",\"type\":\"string\",\"access\":\"read\"") != NULL);
    CHECK(strstr(doc, "\"D Min\"") != NULL);
    CHECK(strstr(doc, "\"snoop_note\"") == NULL && strstr(doc, "\"snoop_level\"") == NULL);   /* the roll draws them from the feed: no dials */
    CHECK(api->get_param(inst, "snoop_note", buf, sizeof(buf)) > 0);                          /* the engine still answers */

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
    api->set_param(inst, "out_trim", "-4.5");                     /* the trim is not on any page any more, and not saved */
    api->set_param(inst, "sync_mode", "Host Lock 2x");
    api->set_param(inst, "tempo_trim", "1.25");
    api->set_param(inst, "chip_model", "8580");
    api->set_param(inst, "cutoff_bias", "-20");
    api->set_param(inst, "res_bias", "7");
    api->set_param(inst, "pad_target", "Voice 3");
    api->set_param(inst, "pad_sound", "3");
    api->set_param(inst, "pitch_snap", "In-Key (Scale)");
    api->set_param(inst, "master_tune", "-12");
    api->set_param(inst, "stereo_spread", "0.8");
    api->set_param(inst, "ducking_depth", "0.4");
    api->set_param(inst, "ducking_release", "150");
    api->set_param(inst, "transpose", "5");
    api->set_param(inst, "filter_pump", "0.4");
    api->set_param(inst, "lofi", "0.25");
    api->set_param(inst, "fx_duck", "0.7");
    api->set_param(inst, "pad_mode", "Control");                  /* a mode, not a setting: it is not saved */
    api->set_param(inst, "timbre_lock", "Lock Timbre");           /* neither: restored it would lock whatever is playing then */
    n = api->get_param(inst, "state", doc, sizeof(doc));
    CHECK(strstr(doc, "\"pad_sound\":3") != NULL);                /* saved as its list position */
    CHECK(strstr(doc, "out_trim") == NULL);
    CHECK(strstr(doc, "run_mode") == NULL);
    CHECK(strstr(doc, "pad_mode") == NULL && strstr(doc, "timbre_lock") == NULL);
    CHECK(n > 50 && json_balanced(doc));
    printf("  state: %s\n", doc);
    CHECK_EQ(api->get_param(inst, "state", doc + 8000, 20), -1);

    void *inst2 = api->create_instance(".", "{}");
    api->set_param(inst2, "state", doc);
    const char *keys[] = { "subtune", "v2_vol", "sync_mode", "tempo_trim", "chip_model",
                           "cutoff_bias", "res_bias", "pad_target", "pad_sound_index", "pitch_snap", "master_tune",
                           "stereo_spread", "ducking_depth", "ducking_release", "transpose", "filter_pump", "lofi",
                           "fx_duck", "pad_mode", "timbre_lock" };
    const char *want[] = { "2", "0.25", "Host Lock 2x", "1.25", "8580", "-20", "7", "Voice 3", "3", "In-Key (Scale)", "-12",
                           "0.8", "0.4", "150", "5", "0.4", "0.25", "0.7", "Play", "Unlocked" };
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

    /* ui_chain.js beside the module: it supplies the pages itself, so the DSP refuses the hierarchy (the host loads a component's own
     * UI only when none is served) while still serving chain_params; without the file the hierarchy is served as before */
    SECTION("ui_chain.js");
    void *plain = api->create_instance(".", "{}");
    CHECK(api->get_param(plain, "ui_hierarchy", doc, sizeof(doc)) > 100);
    api->destroy_instance(plain);
    void *chain = api->create_instance(TINYSID_ASSET_DIR "/../tests/fixture_ui", "{}");
    CHECK_EQ(api->get_param(chain, "ui_hierarchy", doc, sizeof(doc)), -1);
    CHECK(api->get_param(chain, "chain_params", doc, sizeof(doc)) > 100);
    CHECK(strstr(doc, "\"piano_roll\"") != NULL);
    CHECK(api->get_param(chain, "pad_mode", doc, sizeof(doc)) > 0);
    api->get_param(chain, "audio_delay", doc, sizeof(doc));
    CHECK_STR(doc, "50");                                      /* the module starts near the delay of tracks fed by Notes Out */
    api->get_param(chain, "note_controls", doc, sizeof(doc));
    CHECK_STR(doc, "0");                                       /* its own screen runs the pads: incoming notes are not buttons */
    api->destroy_instance(chain);
    api->destroy_instance(NULL);

    /* Notes out: the engine's events go to the host as USB-MIDI packets on cable 2, on the chosen channel */
    SECTION("notes out");
    static host_api_v1_t host;
    memset(&host, 0, sizeof(host));
    host.midi_inject_to_move = mock_inject;
    SCHWUNG_INIT_SYMBOL(&host);
    void *mo = api->create_instance(".", "{}");
    static int16_t pcm_mo[128 * 2];
    api->set_param(mo, "midi_out_v1", "On");
    api->set_param(mo, "midi_out_ch1", "3");
    g_pkt_n = 0;
    for (int b = 0; b < 400; b++) api->render_block(mo, pcm_mo, 128);          /* about 1.2 s of the demo tune */
    int ons = 0, offs = 0, bends = 0, bad = 0;
    for (int i = 0; i < g_pkt_n; i++) {
        uint8_t *k = g_pkts[i];
        if (k[0] == 0x29 && k[1] == 0x92 && k[2] < 128 && k[3] > 0) ons++;
        else if (k[0] == 0x28 && k[1] == 0x82) offs++;
        else if (k[0] == 0x2E && k[1] == 0xE2 && k[2] < 128 && k[3] < 128) bends++;
        else bad++;
    }
    CHECK(ons > 3);                                                              /* the demo plays notes on voice 1 */
    api->get_param(mo, "mout_stats", bpm, sizeof(bpm));
    unsigned st_sent = 0, st_refused = 0;
    int st_longest = -1, st_now = -1;
    CHECK(sscanf(bpm, "%u,%u,%d,%d", &st_sent, &st_refused, &st_longest, &st_now) == 4);
    CHECK((int)st_sent == g_pkt_n && st_refused == 0 && st_longest >= 1 && st_now == 0);   /* the counts behind the debug log */
    CHECK(offs >= ons - 1);
    CHECK(bends > 0);
    CHECK_EQ(bad, 0);                                                            /* nothing but notes and bends, on channel 3 */

    /* a host that refuses keeps the events: nothing is lost, only late */
    int before = g_pkt_n;
    g_accept = 0;
    for (int b = 0; b < 200; b++) api->render_block(mo, pcm_mo, 128);
    CHECK_EQ(g_pkt_n, before);
    g_accept = 1;
    for (int b = 0; b < 400; b++) api->render_block(mo, pcm_mo, 128);
    CHECK(g_pkt_n > before);
    int ons2 = 0, offs2 = 0;
    for (int i = 0; i < g_pkt_n; i++) { if (g_pkts[i][0] == 0x29) ons2++; if (g_pkts[i][0] == 0x28) offs2++; }
    CHECK(ons2 - offs2 <= 1);                                                    /* at most the note sounding now is open */

    /* our own echo (cable 2 comes back to every slot listening on that channel) is not played: channel 3 here */
    api->set_param(mo, "pad_mode", "Control");
    const uint8_t echo[3] = { 0x92, 36, 100 }, mine[3] = { 0x90, 36, 100 };
    api->on_midi(mo, echo, 3, 0);
    api->get_param(mo, "voice_mutes", bpm, sizeof(bpm));
    CHECK_STR(bpm, "0");
    api->on_midi(mo, mine, 3, 0);                                                /* another channel still works */
    api->get_param(mo, "voice_mutes", bpm, sizeof(bpm));
    CHECK_STR(bpm, "1");

    /* destroying the instance lets go of the note that is sounding */
    api->set_param(mo, "voice_mutes", "0");
    for (int b = 0; b < 100; b++) api->render_block(mo, pcm_mo, 128);
    int total_before = g_pkt_n;
    api->destroy_instance(mo);
    int opens = 0;
    for (int i = 0; i < g_pkt_n; i++) { if (g_pkts[i][0] == 0x29) opens++; if (g_pkts[i][0] == 0x28) opens--; }
    CHECK(g_pkt_n >= total_before);
    CHECK(opens <= 0);

    /* with no inject, the external send is used; with neither, events are dropped and the queue does not back up */
    memset(&host, 0, sizeof(host));
    host.midi_send_external = mock_inject;
    SCHWUNG_INIT_SYMBOL(&host);
    mo = api->create_instance(".", "{}");
    api->set_param(mo, "midi_out_v1", "On");
    g_pkt_n = 0;
    for (int b = 0; b < 200; b++) api->render_block(mo, pcm_mo, 128);
    CHECK(g_pkt_n > 0);
    api->destroy_instance(mo);
    memset(&host, 0, sizeof(host));
    SCHWUNG_INIT_SYMBOL(&host);
    mo = api->create_instance(".", "{}");
    api->set_param(mo, "midi_out_v1", "On");
    g_pkt_n = 0;
    for (int b = 0; b < 400; b++) api->render_block(mo, pcm_mo, 128);
    CHECK_EQ(g_pkt_n, 0);
    api->destroy_instance(mo);

    /* a slot never sends on the channel it listens on (it would come straight back in); the other channels still go */
    memset(&host, 0, sizeof(host));
    host.midi_inject_to_move = mock_inject;
    host.slot_recv_channel = mock_slot_channel;
    SCHWUNG_INIT_SYMBOL(&host);
    g_slot_channel = 0;                                                          /* listens on MIDI channel 1 */
    mo = api->create_instance(".", "{}");
    api->set_param(mo, "midi_out_v1", "On");                                     /* voice 1 -> channel 1: ours */
    api->set_param(mo, "midi_out_v2", "On");                                     /* voice 2 -> channel 2 */
    g_pkt_n = 0;
    for (int b = 0; b < 400; b++) api->render_block(mo, pcm_mo, 128);
    int ch1 = 0, ch2 = 0;
    for (int i = 0; i < g_pkt_n; i++) { if ((g_pkts[i][1] & 0x0F) == 0) ch1++; if ((g_pkts[i][1] & 0x0F) == 1) ch2++; }
    CHECK_EQ(ch1, 0);
    CHECK(ch2 > 0);
    api->destroy_instance(mo);

    /* the switches are remembered with the set (they start Off on a fresh module) */
    void *so = api->create_instance(".", "{}");
    api->get_param(so, "midi_out_v2", bpm, sizeof(bpm));
    CHECK_STR(bpm, "Off");
    api->set_param(so, "midi_out_v2", "On");
    api->set_param(so, "midi_out_ch2", "7");
    api->get_param(so, "state", doc, sizeof(doc));
    CHECK(strstr(doc, "\"midi_out_v2\":\"On\"") != NULL);
    void *so2 = api->create_instance(".", "{}");
    api->set_param(so2, "state", doc);
    api->get_param(so2, "midi_out_v2", bpm, sizeof(bpm));
    CHECK_STR(bpm, "On");
    api->get_param(so2, "midi_out_v1", bpm, sizeof(bpm));
    CHECK_STR(bpm, "Off");
    api->get_param(so2, "midi_out_ch2", bpm, sizeof(bpm));
    CHECK_STR(bpm, "7");
    api->get_param(so2, "midi_out_ch3", bpm, sizeof(bpm));
    CHECK_STR(bpm, "3");
    api->destroy_instance(so);
    api->destroy_instance(so2);
    SCHWUNG_INIT_SYMBOL(NULL);
    return test_finish("test_plugin");
}
