/* 作者：苏勃(subo)
 * SPDX-License-Identifier: Apache-2.0
 * 去掉 HMAC 的映像。不链接 chg_sha256.c。
 */
#include "chg_crypt.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

static void expect_eq(const char *name, unsigned long got, unsigned long want)
{
    if (got != want) {
        fprintf(stderr, "FAIL %s got %lu want %lu\n", name, got, want);
        g_fail = 1;
    }
}

static const uint8_t KEY[16] = {
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F
};
static const uint8_t SALT_BMS[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
static const uint8_t SALT_CHR[8] = {0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18};
static const uint8_t ID_BMS[8] = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8};
static const uint8_t ID_CHR[8] = {0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8};

static void init_level(ChgCrypt *c, uint8_t role, const uint8_t *id, uint8_t level)
{
    uint8_t suites[4];
    uint8_t flags = 0;
    uint8_t n = chg_crypt_level_suites(level, suites, 4, &flags);
    chg_crypt_init(c, role, CHG_CRYPT_PORT_485, KEY, id, suites, n, flags);
    chg_crypt_set_salt_bms(c, SALT_BMS);
    chg_crypt_set_salt_chr(c, SALT_CHR);
}

static int hello(ChgCrypt *bms, ChgCrypt *chr)
{
    ChgMacWork w;
    uint8_t f[40];
    int n = chg_crypt_pack_hello_bms(bms, f, sizeof f);
    int rc;
    if (n < 0) {
        return -1;
    }
    rc = chg_crypt_rx_frame(chr, &w, f, (size_t)n);
    if (rc != 1) {
        return -1;
    }
    n = chg_crypt_pack_hello_chr(chr, f, sizeof f);
    if (n < 0) {
        return -1;
    }
    return chg_crypt_rx_frame(bms, &w, f, (size_t)n);
}

int main(void)
{
    ChgCrypt bms, chr;
    ChgMacWork w;
    uint8_t suites[4];
    uint8_t flags = 0;
    uint8_t only[1];
    uint8_t f[40];
    uint16_t i = 300;
    uint8_t en = 1;
    int n;

    expect_eq("omit on", CHG_CRYPT_OMIT_HMAC, 1u);
    n = (int)chg_crypt_level_suites(CHG_CRYPT_LEVEL_HMAC, suites, 4, &flags);
    expect_eq("asked hmac n", (unsigned)n, 1);
    expect_eq("asked hmac is plain", suites[0], CHG_SUITE_PLAIN);
    expect_eq("asked hmac flags", flags, CHG_CRYPT_F_PLAIN);
    n = (int)chg_crypt_level_suites(CHG_CRYPT_LEVEL_PLAIN, suites, 4, &flags);
    expect_eq("plain n", (unsigned)n, 1);
    expect_eq("plain suite", suites[0], CHG_SUITE_PLAIN);

    init_level(&bms, CHG_CRYPT_ROLE_BMS, ID_BMS, CHG_CRYPT_LEVEL_HMAC);
    init_level(&chr, CHG_CRYPT_ROLE_CHR, ID_CHR, CHG_CRYPT_LEVEL_HMAC);
    expect_eq("omit hello", (unsigned)hello(&bms, &chr), 1);
    expect_eq("omit suite", chr.suite, CHG_SUITE_PLAIN);
    chg_crypt_bind_output(&chr, &i, &en);
    expect_eq("omit amps", i, 300);
    expect_eq("omit token", (unsigned)chg_crypt_pack_token(&bms, &w, f, sizeof f), (unsigned)-1);

    only[0] = CHG_SUITE_HMAC;
    chg_crypt_init(&bms, CHG_CRYPT_ROLE_BMS, CHG_CRYPT_PORT_485, KEY, ID_BMS, only, 1, 0);
    chg_crypt_init(&chr, CHG_CRYPT_ROLE_CHR, CHG_CRYPT_PORT_485, KEY, ID_CHR, only, 1, 0);
    chg_crypt_set_salt_bms(&bms, SALT_BMS);
    chg_crypt_set_salt_chr(&chr, SALT_CHR);
    expect_eq("forced hello", (unsigned)hello(&bms, &chr), 1);
    expect_eq("forced suite", chr.suite, CHG_SUITE_NONE);
    i = 300;
    en = 1;
    chg_crypt_bind_output(&chr, &i, &en);
    expect_eq("forced amps", i, 0);
    expect_eq("state", (unsigned)(sizeof(ChgCrypt) + sizeof(ChgMacWork) <= CHG_CRYPT_STATE_MAX), 1);

    if (g_fail) {
        fprintf(stderr, "FAILED\n");
        return 1;
    }
    printf("ok\n");
    return 0;
}
