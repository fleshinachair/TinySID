/* Declaration of the Schwung v2 DSP plugin ABI that plugin.c targets.
 *
 * Checked against plugin_api_v1.h as shipped on a Move (modules/overtake/twinsampler): the
 * plugin_api_v2_t layout, the move_plugin_init_v2 symbol, 44.1 kHz and 128-frame blocks all match.
 * host_api_v1_t is the real table; TinySID uses get_clock_status() to follow the transport in Transport mode. */
#ifndef SCHWUNG_API_H
#define SCHWUNG_API_H

#include <stdint.h>

#define SCHWUNG_PLUGIN_API_VERSION 2

/* Host services table passed to the init function (layout copied from the device's plugin_api_v1.h).
 * TinySID only uses get_clock_status. */
typedef int (*move_mod_emit_value_fn)(void *ctx, const char *source_id, const char *target, const char *param,
                                      float signal, float depth, float offset, int bipolar, int enabled);
typedef void (*move_mod_clear_source_fn)(void *ctx, const char *source_id);

#define MOVE_CLOCK_STATUS_UNAVAILABLE 0
#define MOVE_CLOCK_STATUS_STOPPED 1
#define MOVE_CLOCK_STATUS_RUNNING 2

typedef struct host_api_v1 {
    uint32_t api_version;
    int sample_rate;
    int frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;
    void (*log)(const char *msg);
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);
    int (*get_clock_status)(void);
    move_mod_emit_value_fn mod_emit_value;
    move_mod_clear_source_fn mod_clear_source;
    void *mod_host_ctx;
    float (*get_bpm)(void);
} host_api_v1_t;

typedef struct plugin_api_v2 {
    uint32_t api_version;
    void *(*create_instance)(const char *module_dir, const char *json_defaults);
    void (*destroy_instance)(void *instance);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    int (*get_error)(void *instance, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;

#define SCHWUNG_INIT_SYMBOL move_plugin_init_v2

#endif
