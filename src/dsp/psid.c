#include "psid.h"

#include <string.h>

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void copy_str(char dst[33], const uint8_t *src)
{
    memcpy(dst, src, 32);
    dst[32] = '\0';
}

int psid_parse(const uint8_t *buf, size_t len, psid_info_t *out)
{
    if (!buf || !out) return PSID_ERR_TOO_SHORT;
    if (len < PSID_V1_HEADER_SIZE) return PSID_ERR_TOO_SHORT;
    if (memcmp(buf, "RSID", 4) == 0) return PSID_ERR_RSID;
    if (memcmp(buf, "PSID", 4) != 0) return PSID_ERR_MAGIC;

    memset(out, 0, sizeof(*out));
    out->version = be16(buf + 4);
    if (out->version < 1 || out->version > 4) return PSID_ERR_VERSION;

    out->data_offset = be16(buf + 6);
    size_t min_off = out->version == 1 ? PSID_V1_HEADER_SIZE : PSID_V2_HEADER_SIZE;
    if (out->data_offset < min_off || out->data_offset > len || len < min_off)
        return PSID_ERR_OFFSET;

    out->load_addr  = be16(buf + 8);
    out->init_addr  = be16(buf + 10);
    out->play_addr  = be16(buf + 12);
    out->songs      = be16(buf + 14);
    out->start_song = be16(buf + 16);
    out->speed      = be32(buf + 18);
    copy_str(out->name, buf + 0x16);
    copy_str(out->author, buf + 0x36);
    copy_str(out->released, buf + 0x56);

    if (out->songs < 1 || out->songs > 256) return PSID_ERR_SONGS;
    if (out->start_song < 1 || out->start_song > out->songs) out->start_song = 1;

    if (out->version >= 2) {
        out->flags = be16(buf + 0x76);
        out->start_page = buf[0x78];
        out->page_length = buf[0x79];
        out->is_ntsc = ((out->flags >> 2) & 3) == 2;
    }

    const uint8_t *data = buf + out->data_offset;
    size_t data_len = len - out->data_offset;
    if (out->load_addr == 0) {
        if (data_len < 2) return PSID_ERR_LOAD;
        out->load_addr = (uint16_t)(data[0] | (data[1] << 8));
        data += 2;
        data_len -= 2;
    }
    if (data_len == 0) return PSID_ERR_LOAD;
    if ((size_t)out->load_addr + data_len > 0x10000) return PSID_ERR_TOO_BIG;
    if (out->init_addr == 0) out->init_addr = out->load_addr;

    out->data = data;
    out->data_len = data_len;
    return PSID_OK;
}

int psid_subtune_uses_cia(const psid_info_t *info, int n)
{
    int bit = n - 1;
    if (bit < 0) bit = 0;
    if (bit > 31) bit = 31;
    return (int)((info->speed >> bit) & 1u);
}

const char *psid_strerror(int err)
{
    switch (err) {
    case PSID_OK:            return "ok";
    case PSID_ERR_TOO_SHORT: return "file too short for a PSID header";
    case PSID_ERR_MAGIC:     return "not a PSID file";
    case PSID_ERR_RSID:      return "RSID files are not supported";
    case PSID_ERR_VERSION:   return "unsupported PSID version";
    case PSID_ERR_OFFSET:    return "bad data offset";
    case PSID_ERR_SONGS:     return "bad subtune count";
    case PSID_ERR_LOAD:      return "no program data";
    case PSID_ERR_TOO_BIG:   return "program does not fit in 64K";
    default:                 return "unknown error";
    }
}
