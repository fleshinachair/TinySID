/* PSID (v1-v4) header parser. RSID files need a real C64 environment (CIA
 * timers, interrupts, ROMs) and are rejected with PSID_ERR_RSID. */
#ifndef PSID_H
#define PSID_H

#include <stddef.h>
#include <stdint.h>

enum {
    PSID_OK = 0,
    PSID_ERR_TOO_SHORT,
    PSID_ERR_MAGIC,
    PSID_ERR_RSID,
    PSID_ERR_VERSION,
    PSID_ERR_OFFSET,
    PSID_ERR_SONGS,
    PSID_ERR_LOAD,
    PSID_ERR_TOO_BIG
};

#define PSID_V1_HEADER_SIZE 0x76
#define PSID_V2_HEADER_SIZE 0x7C

typedef struct {
    uint16_t version;
    uint16_t data_offset;
    uint16_t load_addr;      /* resolved: taken from the first two data bytes when the header says 0 */
    uint16_t init_addr;      /* resolved: equals load_addr when the header says 0 */
    uint16_t play_addr;      /* 0 = tune installs its own IRQ handler */
    uint16_t songs;          /* 1..256 */
    uint16_t start_song;     /* 1-based */
    uint32_t speed;          /* bit n set = subtune n+1 is CIA-timed instead of vblank */
    uint16_t flags;          /* v2+: bits 2-3 video standard */
    uint8_t  start_page, page_length;
    int      is_ntsc;        /* video standard resolved to NTSC (PAL when unknown or both) */
    char     name[33], author[33], released[33];
    const uint8_t *data;     /* points into the caller's buffer; load image */
    size_t   data_len;
} psid_info_t;

int psid_parse(const uint8_t *buf, size_t len, psid_info_t *out);
const char *psid_strerror(int err);

/* 1 when the speed flag for 1-based subtune `n` selects CIA timing. */
int psid_subtune_uses_cia(const psid_info_t *info, int n);

#endif
