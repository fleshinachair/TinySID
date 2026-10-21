/* PSID header parsing and memory layout after load. */
#include "fixtures.h"
#include "psid.h"
#include "test_util.h"
#include "tinysid.h"

#include "default_sid.h"

static uint8_t buf[0x10400];

static void test_header_fields(void)
{
    SECTION("header fields");
    size_t len = fx_tone(buf, FX_PLAIN, 3, 0x00000005, 0x0008 /* NTSC */);
    psid_info_t i;
    CHECK_EQ(psid_parse(buf, len, &i), PSID_OK);
    CHECK_EQ(i.version, 2);
    CHECK_EQ(i.data_offset, 0x7C);
    CHECK_EQ(i.load_addr, FX_LOAD);
    CHECK_EQ(i.init_addr, FX_LOAD);
    CHECK_EQ(i.songs, 3);
    CHECK_EQ(i.start_song, 1);
    CHECK_EQ(i.speed, 5);
    CHECK_EQ(i.is_ntsc, 1);
    CHECK_STR(i.name, "Fixture");
    CHECK_STR(i.author, "Tester");
    CHECK_STR(i.released, "2026");
    CHECK_EQ(i.data_len, len - 0x7C);
    CHECK(i.data == buf + 0x7C);

    CHECK_EQ(psid_subtune_uses_cia(&i, 1), 1);
    CHECK_EQ(psid_subtune_uses_cia(&i, 2), 0);
    CHECK_EQ(psid_subtune_uses_cia(&i, 3), 1);
    CHECK_EQ(psid_subtune_uses_cia(&i, 200), 0);          /* bits beyond 32 reuse bit 31 */

    len = fx_tone(buf, FX_PLAIN, 1, 0, 0x0004 /* PAL */);
    CHECK_EQ(psid_parse(buf, len, &i), PSID_OK);
    CHECK_EQ(i.is_ntsc, 0);
    len = fx_tone(buf, FX_PLAIN, 1, 0, 0x000C /* both */);
    CHECK_EQ(psid_parse(buf, len, &i), PSID_OK);
    CHECK_EQ(i.is_ntsc, 0);
}

static void test_load_address_in_data(void)
{
    SECTION("load address stored in data");
    uint8_t code[] = { 0x00, 0x20, 0x60, 0xEA };          /* load $2000, then RTS + NOP */
    size_t len = fx_psid(buf, code, sizeof(code), 0, 0, 0, 1, 0, 0);
    psid_info_t i;
    CHECK_EQ(psid_parse(buf, len, &i), PSID_OK);
    CHECK_EQ(i.load_addr, 0x2000);
    CHECK_EQ(i.init_addr, 0x2000);                        /* init 0 means load address */
    CHECK_EQ(i.data_len, 2);
    CHECK_EQ(i.data[0], 0x60);
}

static void test_rejections(void)
{
    SECTION("rejections");
    psid_info_t i;
    size_t len = fx_tone(buf, FX_PLAIN, 1, 0, 0);

    CHECK_EQ(psid_parse(buf, 10, &i), PSID_ERR_TOO_SHORT);
    CHECK_EQ(psid_parse(NULL, 100, &i), PSID_ERR_TOO_SHORT);

    uint8_t t[sizeof(buf)];
    memcpy(t, buf, len);
    memcpy(t, "XSID", 4);
    CHECK_EQ(psid_parse(t, len, &i), PSID_ERR_MAGIC);
    memcpy(t, "RSID", 4);
    CHECK_EQ(psid_parse(t, len, &i), PSID_ERR_RSID);

    memcpy(t, buf, len);
    t[5] = 9;
    CHECK_EQ(psid_parse(t, len, &i), PSID_ERR_VERSION);

    memcpy(t, buf, len);
    t[6] = 0x7F; t[7] = 0xFF;                             /* data offset beyond the file */
    CHECK_EQ(psid_parse(t, len, &i), PSID_ERR_OFFSET);
    memcpy(t, buf, len);
    t[6] = 0; t[7] = 0x10;                                /* offset inside the header */
    CHECK_EQ(psid_parse(t, len, &i), PSID_ERR_OFFSET);

    memcpy(t, buf, len);
    t[14] = 0; t[15] = 0;
    CHECK_EQ(psid_parse(t, len, &i), PSID_ERR_SONGS);
    t[14] = 0x01; t[15] = 0x01;                           /* 257 */
    CHECK_EQ(psid_parse(t, len, &i), PSID_ERR_SONGS);

    CHECK_EQ(psid_parse(buf, 0x7C, &i), PSID_ERR_LOAD);   /* header only, no program */

    memcpy(t, buf, len);
    t[8] = 0xFF; t[9] = 0xF0;                             /* load at $FFF0 with ~600 bytes */
    CHECK_EQ(psid_parse(t, len, &i), PSID_ERR_TOO_BIG);

    memcpy(t, buf, len);
    t[16] = 0; t[17] = 9;                                 /* start song past the end */
    CHECK_EQ(psid_parse(t, len, &i), PSID_OK);
    CHECK_EQ(i.start_song, 1);

    CHECK(strlen(psid_strerror(PSID_ERR_RSID)) > 0);
}

static void test_memory_layout(void)
{
    SECTION("memory layout after load");
    size_t len = fx_tone(buf, FX_WRITER, 2, 0, 0);
    tinysid_t *t = tinysid_create(44100.0);
    CHECK(t != NULL);
    CHECK(tinysid_info(t) == NULL);
    CHECK_EQ(tinysid_load(t, buf, len), PSID_OK);

    const psid_info_t *i = tinysid_info(t);
    CHECK(i != NULL);
    CHECK_EQ(i->load_addr, FX_LOAD);
    int same = 1;
    for (size_t k = 0; k < i->data_len; k++)
        if (tinysid_ram(t, (uint16_t)(FX_LOAD + k)) != buf[0x7C + k]) same = 0;
    CHECK(same);
    CHECK_EQ(tinysid_ram(t, FX_LOAD + (uint16_t)i->data_len), 0);   /* nothing written past the image */
    CHECK_EQ(tinysid_ram(t, FX_LOAD - 1), 0);
    CHECK_EQ(tinysid_ram(t, 0x0314), 0x31);                       /* default IRQ vector */
    CHECK_EQ(tinysid_ram(t, 0x0315), 0xEA);
    CHECK_EQ(tinysid_ram(t, 0x02), 0);                            /* init saw A = subtune - 1 = 0 */

    /* init ran: the SID chip holds what the init routine wrote */
    CHECK_EQ(tinysid_sid_reg(t, 0x18), 0x0F);
    CHECK_EQ(tinysid_sid_reg(t, 1), 0x10);
    CHECK_EQ(tinysid_sid_reg(t, 8), 0x20);
    CHECK_EQ(tinysid_sid_reg(t, 15), 0x30);
    CHECK_EQ(tinysid_sid_reg(t, 4), 0x21);

    /* a rejected file leaves the loaded tune untouched */
    uint8_t junk[64] = { 'P', 'S', 'I', 'D' };
    CHECK_EQ(tinysid_load(t, junk, sizeof(junk)), PSID_ERR_TOO_SHORT);
    CHECK(tinysid_info(t) == i);
    CHECK_EQ(tinysid_info(t)->songs, 2);

    tinysid_destroy(t);
}

static void test_embedded_default(void)
{
    SECTION("embedded default tune");
    psid_info_t i;
    CHECK_EQ(psid_parse(default_sid_data, default_sid_len, &i), PSID_OK);
    CHECK(i.songs >= 1);
    CHECK(i.play_addr != 0);
    CHECK(i.load_addr >= 0x0400);
    printf("  default tune: \"%s\" by %s, %u subtune(s), load $%04X init $%04X play $%04X\n",
           i.name, i.author, i.songs, i.load_addr, i.init_addr, i.play_addr);
}

int main(void)
{
    test_header_fields();
    test_load_address_in_data();
    test_rejections();
    test_memory_layout();
    test_embedded_default();
    return test_finish("test_psid");
}
