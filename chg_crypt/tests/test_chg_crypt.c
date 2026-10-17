/* 作者：苏勃(subo) */
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

static void expect_mem(const char *name, const uint8_t *got, const uint8_t *want, size_t n)
{
    if (got == 0 || want == 0 || memcmp(got, want, n) != 0) {
        size_t i;
        fprintf(stderr, "FAIL %s\n", name);
        for (i = 0; i < n && got != 0; i++) {
            fprintf(stderr, " %02X", got[i]);
        }
        fprintf(stderr, "\n");
        g_fail = 1;
    }
}

typedef char chg_mac_work_small[(sizeof(ChgMacWork) <= 400) ? 1 : -1];
typedef char chg_crypt_small[(sizeof(ChgCrypt) <= 256) ? 1 : -1];

static const uint8_t KEY[16] = {
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F
};
static const uint8_t SALT_BMS[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
static const uint8_t SALT_CHR[8] = {0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18};
static const uint8_t ID_BMS[8] = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8};
static const uint8_t ID_CHR[8] = {0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8};
/* 84.0 V / 30.0 A，允许 = 1。与公开算例同一组请求。 */
static const uint8_t CTRL[6] = {0x01, 0x48, 0x03, 0x2C, 0x01, 0x00};

static void test_sha(void)
{
    ChgMacWork w;
    uint8_t out[32];
    static const uint8_t abc[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    static const uint8_t empty[32] = {
        0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
        0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55
    };
    static const uint8_t mac[32] = {
        0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53, 0x5c, 0xa8, 0xaf, 0xce, 0xaf, 0x0b, 0xf1, 0x2b,
        0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7, 0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7
    };
    uint8_t k[20];
    memset(k, 0x0b, sizeof k);
    chg_sha256(&w, (const uint8_t *)"abc", 3, out);
    expect_mem("sha256 abc", out, abc, 32);
    chg_sha256(&w, 0, 0, out);
    expect_mem("sha256 empty", out, empty, 32);
    chg_hmac_sha256(&w, k, sizeof k, (const uint8_t *)"Hi There", 8, out);
    expect_mem("hmac rfc4231", out, mac, 32);
}

static void arm(ChgCrypt *bms, ChgCrypt *chr, uint8_t port, const uint8_t *suites, uint8_t ns,
                uint8_t flags)
{
    chg_crypt_init(bms, CHG_CRYPT_ROLE_BMS, port, KEY, ID_BMS, suites, ns, flags);
    chg_crypt_init(chr, CHG_CRYPT_ROLE_CHR, port, KEY, ID_CHR, suites, ns, flags);
    chg_crypt_set_salt_bms(bms, SALT_BMS);
    chg_crypt_set_salt_chr(chr, SALT_CHR);
}

static int hello_485(ChgCrypt *bms, ChgCrypt *chr, ChgMacWork *w)
{
    uint8_t f[40];
    int n = chg_crypt_pack_hello_bms(bms, f, sizeof f);
    int rc;
    if (n < 0) {
        return -1;
    }
    rc = chg_crypt_rx_frame(chr, w, f, (size_t)n);
    if (rc != 1) {
        return -1;
    }
    n = chg_crypt_pack_hello_chr(chr, f, sizeof f);
    if (n < 0) {
        return -1;
    }
    return chg_crypt_rx_frame(bms, w, f, (size_t)n);
}

static void test_plain(void)
{
    ChgCrypt bms, chr;
    ChgMacWork w;
    uint8_t suites[] = {CHG_SUITE_PLAIN, CHG_SUITE_HMAC};
    uint16_t i = 300;
    uint8_t en = 1;
    uint8_t only_hmac[] = {CHG_SUITE_HMAC};
    arm(&bms, &chr, CHG_CRYPT_PORT_485, suites, 2, CHG_CRYPT_F_PLAIN);
    /* 双方都有 0x01，应选 0x01，而不是因为带了明文许可就退回明文。 */
    expect_eq("prefer hmac hello", (unsigned)hello_485(&bms, &chr, &w), 1);
    expect_eq("prefer suite", chr.suite, CHG_SUITE_HMAC);
    expect_eq("prefer not yet", (unsigned)chg_crypt_output_allowed(&chr), 0);

    arm(&bms, &chr, CHG_CRYPT_PORT_485, suites, 1, CHG_CRYPT_F_PLAIN); /* only plain in list: first byte */
    suites[0] = CHG_SUITE_PLAIN;
    arm(&bms, &chr, CHG_CRYPT_PORT_485, suites, 1, CHG_CRYPT_F_PLAIN);
    expect_eq("plain hello", (unsigned)hello_485(&bms, &chr, &w), 1);
    expect_eq("plain suite", bms.suite, CHG_SUITE_PLAIN);
    expect_eq("plain open", (unsigned)chg_crypt_output_allowed(&chr), 1);
    expect_eq("plain bms allow", (unsigned)chg_crypt_bms_may_allow(&bms), 1);
    chg_crypt_bind_output(&chr, &i, &en);
    expect_eq("plain keeps amps", i, 300);
    expect_eq("plain keeps en", en, 1);

    arm(&bms, &chr, CHG_CRYPT_PORT_485, only_hmac, 1, 0);
    chg_crypt_init(&chr, CHG_CRYPT_ROLE_CHR, CHG_CRYPT_PORT_485, KEY, ID_CHR, only_hmac, 0, 0);
    chg_crypt_set_salt_chr(&chr, SALT_CHR);
    expect_eq("no intersect hello", (unsigned)hello_485(&bms, &chr, &w), 1);
    expect_eq("no intersect suite", chr.suite, CHG_SUITE_NONE);
    i = 300;
    en = 1;
    chg_crypt_bind_output(&chr, &i, &en);
    expect_eq("no intersect amps", i, 0);
    expect_eq("no intersect en", en, 0);
    expect_eq("no intersect bms", (unsigned)chg_crypt_bms_may_allow(&bms), 0);
}

static void test_hmac_485(void)
{
    ChgCrypt bms, chr;
    ChgMacWork wb, wc;
    uint8_t suites[] = {CHG_SUITE_HMAC};
    uint8_t f[40];
    uint8_t cover[57];
    uint8_t full[32];
    uint16_t i = 300;
    uint8_t en = 1;
    int n;
    arm(&bms, &chr, CHG_CRYPT_PORT_485, suites, 1, CHG_CRYPT_F_BIDI | CHG_CRYPT_F_PLAIN);
    expect_eq("hmac hello", (unsigned)hello_485(&bms, &chr, &wb), 1);
    expect_eq("hmac suite", bms.suite, CHG_SUITE_HMAC);
    expect_eq("hmac phase", bms.phase, CHG_CRYPT_PH_RUN);
    expect_eq("bidi holds bms", (unsigned)chg_crypt_bms_may_allow(&bms), 0);
    chg_crypt_set_ctrl(&bms, CTRL);
    chg_crypt_set_ctrl(&chr, CTRL);
    n = chg_crypt_pack_token(&bms, &wb, f, sizeof f);
    expect_eq("token len", (unsigned)n, 20);
    expect_eq("token msg", f[3], CHG_MSG_TOKEN);
    expect_eq("token dlen", f[4], 13);
    memset(cover, 0, sizeof cover);
    cover[0] = CHG_SUITE_HMAC;
    memcpy(cover + 1, SALT_BMS, 8);
    memcpy(cover + 9, SALT_CHR, 8);
    cover[17] = 1;
    cover[21] = CHG_DIR_BMS;
    memcpy(cover + 22, CTRL, 6);
    chg_hmac_sha256(&wc, KEY, 16, cover, 57, full);
    expect_mem("tag layout", f + 10, full, 8);

    expect_eq("rx token", (unsigned)chg_crypt_rx_frame(&chr, &wc, f, (size_t)n), 1);
    expect_eq("out open", (unsigned)chg_crypt_output_allowed(&chr), 1);
    chg_crypt_bind_output(&chr, &i, &en);
    expect_eq("hmac keeps amps", i, 300);

    n = chg_crypt_pack_token(&chr, &wc, f, sizeof f);
    expect_eq("chr token", (unsigned)n, 20);
    expect_eq("rx chr token", (unsigned)chg_crypt_rx_frame(&bms, &wb, f, (size_t)n), 1);
    expect_eq("bidi open", (unsigned)chg_crypt_bms_may_allow(&bms), 1);

    /* 盐变一个字节后再发下一枚令牌。充电机仍用原盐，标签对不上，出流回到 0。 */
    {
        uint8_t salt[8];
        memcpy(salt, SALT_BMS, 8);
        salt[0] ^= 0x01u;
        chg_crypt_set_salt_bms(&bms, salt);
        chg_crypt_set_ctrl(&bms, CTRL);
        chg_crypt_set_ctrl(&chr, CTRL);
        n = chg_crypt_pack_token(&bms, &wb, f, sizeof f);
        i = 300;
        en = 1;
        expect_eq("bad tag rx", (unsigned)chg_crypt_rx_frame(&chr, &wc, f, (size_t)n), 1);
        chg_crypt_bind_output(&chr, &i, &en);
        expect_eq("bad tag amps", i, 0);
        expect_eq("fails 1", chr.fails, 1);
    }
}

static void test_lock(void)
{
    ChgCrypt bms, chr;
    ChgMacWork w;
    uint8_t suites[] = {CHG_SUITE_HMAC};
    uint8_t f[40];
    int n;
    int k;
    arm(&bms, &chr, CHG_CRYPT_PORT_485, suites, 1, 0);
    hello_485(&bms, &chr, &w);
    chg_crypt_set_ctrl(&bms, CTRL);
    chg_crypt_set_ctrl(&chr, CTRL);
    n = chg_crypt_pack_token(&bms, &w, f, sizeof f);
    /* 充电机覆盖的 0x01 与令牌不一致，CRC 仍正确。同一帧连错 3 次。 */
    chr.ctrl[5] ^= 0x01u;
    for (k = 0; k < 3; k++) {
        chg_crypt_rx_frame(&chr, &w, f, (size_t)n);
    }
    expect_eq("locked", chr.phase, CHG_CRYPT_PH_LOCK);
    expect_eq("lock ms", chr.lock_ms, CHG_CRYPT_LOCK_MS);
    chg_crypt_tick(&chr, 9999);
    expect_eq("still locked", chr.phase, CHG_CRYPT_PH_LOCK);
    chr.ctrl[5] ^= 0x01u;
    chg_crypt_rx_frame(&chr, &w, f, (size_t)n);
    expect_eq("lock ignores", chr.phase, CHG_CRYPT_PH_LOCK);
    chg_crypt_tick(&chr, 1);
    expect_eq("lock done", chr.phase, CHG_CRYPT_PH_IDLE);
    expect_eq("salt cleared", chr.salt_bms_set, 0);
    expect_eq("still blocked", (unsigned)chg_crypt_output_allowed(&chr), 0);
}

static void test_sif(void)
{
    ChgCrypt bms, chr;
    ChgMacWork w;
    uint8_t suites[] = {CHG_SUITE_HMAC};
    uint8_t f[48];
    uint16_t i = 200;
    uint8_t en = 1;
    int n;
    arm(&bms, &chr, CHG_CRYPT_PORT_SIF, suites, 1, CHG_CRYPT_F_BIDI);
    expect_eq("sif bidi pack", (unsigned)chg_crypt_pack_hello_bms(&bms, f, sizeof f), (unsigned)-1);
    expect_eq("sif bidi blocked", (unsigned)chg_crypt_output_allowed(&chr), 0);

    arm(&bms, &chr, CHG_CRYPT_PORT_SIF, suites, 1, 0);
    n = chg_crypt_pack_hello_bms(&bms, f, sizeof f);
    expect_eq("sif hello n", n > 0, 1);
    expect_eq("sif bms run", bms.phase, CHG_CRYPT_PH_RUN);
    expect_eq("sif rx", (unsigned)chg_crypt_rx_frame(&chr, &w, f, (size_t)n), 1);
    expect_eq("sif chr run", chr.phase, CHG_CRYPT_PH_RUN);
    expect_eq("sif no reply", (unsigned)chg_crypt_pack_hello_chr(&chr, f, sizeof f), (unsigned)-1);
    chg_crypt_set_ctrl(&bms, CTRL);
    chg_crypt_set_ctrl(&chr, CTRL);
    n = chg_crypt_pack_token(&bms, &w, f, sizeof f);
    expect_eq("sif token rx", (unsigned)chg_crypt_rx_frame(&chr, &w, f, (size_t)n), 1);
    chg_crypt_bind_output(&chr, &i, &en);
    expect_eq("sif amps", i, 200);
    expect_eq("sif bms may", (unsigned)chg_crypt_bms_may_allow(&bms), 1);
}

static void test_id(void)
{
    ChgCrypt bms, chr;
    ChgMacWork w;
    uint8_t suites[] = {CHG_SUITE_HMAC};
    uint8_t plain[] = {CHG_SUITE_PLAIN};
    uint8_t f[40];
    uint8_t other[8] = {9, 9, 9, 9, 9, 9, 9, 9};
    int n;
    arm(&bms, &chr, CHG_CRYPT_PORT_485, suites, 1, 0);
    chg_crypt_set_allow(&chr, ID_BMS, 1, 1);
    hello_485(&bms, &chr, &w);
    chg_crypt_set_ctrl(&bms, CTRL);
    chg_crypt_set_ctrl(&chr, CTRL);
    n = chg_crypt_pack_token(&bms, &w, f, sizeof f);
    chg_crypt_rx_frame(&chr, &w, f, (size_t)n);
    expect_eq("id listed", (unsigned)chg_crypt_output_allowed(&chr), 1);

    arm(&bms, &chr, CHG_CRYPT_PORT_485, suites, 1, 0);
    chg_crypt_set_allow(&chr, 0, 0, 1);
    hello_485(&bms, &chr, &w);
    chg_crypt_set_ctrl(&bms, CTRL);
    chg_crypt_set_ctrl(&chr, CTRL);
    n = chg_crypt_pack_token(&bms, &w, f, sizeof f);
    chg_crypt_rx_frame(&chr, &w, f, (size_t)n);
    expect_eq("id empty", (unsigned)chg_crypt_output_allowed(&chr), 0);

    arm(&bms, &chr, CHG_CRYPT_PORT_485, suites, 1, 0);
    chg_crypt_set_allow(&chr, other, 1, 1);
    hello_485(&bms, &chr, &w);
    chg_crypt_set_ctrl(&bms, CTRL);
    chg_crypt_set_ctrl(&chr, CTRL);
    n = chg_crypt_pack_token(&bms, &w, f, sizeof f);
    chg_crypt_rx_frame(&chr, &w, f, (size_t)n);
    expect_eq("id other", (unsigned)chg_crypt_output_allowed(&chr), 0);

    arm(&bms, &chr, CHG_CRYPT_PORT_485, plain, 1, CHG_CRYPT_F_PLAIN);
    chg_crypt_set_allow(&chr, ID_BMS, 1, 1);
    hello_485(&bms, &chr, &w);
    expect_eq("plain id not proof", (unsigned)chg_crypt_output_allowed(&chr), 0);
}

static int vendor_ok(uint8_t suite, const uint8_t *body, uint8_t body_len,
                     uint8_t *msg, uint8_t *data, uint8_t *data_len, void *user)
{
    (void)user;
    if (suite != 0x90u || body_len != 6u) {
        return -1;
    }
    *msg = 0x01u;
    memcpy(data, body, 6u);
    *data_len = 6u;
    return 0;
}

static int vendor_bad(uint8_t suite, const uint8_t *body, uint8_t body_len,
                      uint8_t *msg, uint8_t *data, uint8_t *data_len, void *user)
{
    (void)suite;
    (void)body;
    (void)body_len;
    (void)msg;
    (void)data;
    (void)data_len;
    (void)user;
    return -1;
}

static void test_vendor(void)
{
    ChgCrypt bms, chr;
    ChgMacWork w;
    uint8_t suites[] = {CHG_SUITE_HMAC, 0x90};
    uint8_t f[48];
    uint16_t i = 300;
    uint8_t en = 1;
    int n;
    int k;
    arm(&bms, &chr, CHG_CRYPT_PORT_485, suites, 2, 0);
    chg_crypt_set_vendor(&bms, vendor_ok, 0);
    chg_crypt_set_vendor(&chr, vendor_ok, 0);
    expect_eq("vendor hello", (unsigned)hello_485(&bms, &chr, &w), 1);
    expect_eq("vendor suite", chr.suite, 0x90);
    n = chg_crypt_pack_opaque(&bms, CTRL, 6, f, sizeof f);
    expect_eq("opaque pack", n > 0, 1);
    expect_eq("opaque rx", (unsigned)chg_crypt_rx_frame(&chr, &w, f, (size_t)n), 1);
    chg_crypt_bind_output(&chr, &i, &en);
    expect_eq("vendor amps", i, 300);
    expect_mem("vendor ctrl", chr.ctrl, CTRL, 6);

    arm(&bms, &chr, CHG_CRYPT_PORT_485, suites, 2, 0);
    chg_crypt_set_vendor(&bms, vendor_bad, 0);
    chg_crypt_set_vendor(&chr, vendor_bad, 0);
    hello_485(&bms, &chr, &w);
    n = chg_crypt_pack_opaque(&bms, CTRL, 6, f, sizeof f);
    i = 300;
    en = 1;
    for (k = 0; k < 3; k++) {
        chg_crypt_rx_frame(&chr, &w, f, (size_t)n);
    }
    chg_crypt_bind_output(&chr, &i, &en);
    expect_eq("vendor fail amps", i, 0);
    expect_eq("vendor locked", chr.phase, CHG_CRYPT_PH_LOCK);

    /* 没有钩子时，0x90 不能被选上。只剩明文许可才退回明文。 */
    arm(&bms, &chr, CHG_CRYPT_PORT_485, suites, 2, CHG_CRYPT_F_PLAIN);
    hello_485(&bms, &chr, &w);
    expect_eq("no hook falls to hmac", chr.suite, CHG_SUITE_HMAC);
}

static void test_can_and_ignore(void)
{
    ChgCrypt bms, chr;
    ChgMacWork w;
    uint8_t suites[] = {CHG_SUITE_HMAC};
    uint8_t f[40];
    ChgCan can[4];
    int n;
    int frames;
    int i;
    static const uint8_t ctrl_frame[] = {
        0xA5, 0x5A, 0x01, 0x01, 0x06, 0x01, 0x48, 0x03, 0x2C, 0x01, 0x00, 0xBD, 0xD5
    };
    arm(&bms, &chr, CHG_CRYPT_PORT_CAN, suites, 1, 0);
    hello_485(&bms, &chr, &w);
    chg_crypt_set_ctrl(&bms, CTRL);
    chg_crypt_set_ctrl(&chr, CTRL);
    n = chg_crypt_pack_token(&bms, &w, f, sizeof f);
    frames = chg_crypt_can_split(CHG_CAN_TOK_BMS, f[3], f + 5, f[4], can, 4);
    expect_eq("can frames", (unsigned)frames, 3);
    expect_eq("can seq0", can[0].data[0], 0);
    for (i = 0; i < frames; i++) {
        expect_eq("can rx", (unsigned)chg_crypt_can_rx(&chr, &w, &can[i]), 1);
    }
    expect_eq("can open", (unsigned)chg_crypt_output_allowed(&chr), 1);

    expect_eq("ctrl not crypt", (unsigned)chg_crypt_rx_frame(&chr, &w, ctrl_frame, sizeof ctrl_frame), 0);
    expect_eq("ctrl still open", (unsigned)chg_crypt_output_allowed(&chr), 1);

    f[n - 1] ^= 0xFFu;
    expect_eq("bad crc", (unsigned)chg_crypt_rx_frame(&chr, &w, f, (size_t)n), (unsigned)-1);
    expect_eq("bad crc keeps", (unsigned)chg_crypt_output_allowed(&chr), 1);
}

static void test_replay_and_bat(void)
{
    ChgCrypt bms, chr;
    ChgMacWork w;
    uint8_t suites[] = {CHG_SUITE_HMAC};
    uint8_t f[40];
    uint8_t f2[40];
    uint8_t bat[28];
    int n;
    int n2;
    memset(bat, 0x5A, sizeof bat);
    arm(&bms, &chr, CHG_CRYPT_PORT_485, suites, 1, 0);
    hello_485(&bms, &chr, &w);
    chg_crypt_set_ctrl(&bms, CTRL);
    chg_crypt_set_ctrl(&chr, CTRL);
    n = chg_crypt_pack_token(&bms, &w, f, sizeof f);
    chg_crypt_rx_frame(&chr, &w, f, (size_t)n);
    expect_eq("first ok", (unsigned)chg_crypt_output_allowed(&chr), 1);
    chg_crypt_set_ctrl(&chr, CTRL);
    expect_eq("new ctrl closes", (unsigned)chg_crypt_output_allowed(&chr), 0);
    expect_eq("replay", (unsigned)chg_crypt_rx_frame(&chr, &w, f, (size_t)n), 1);
    expect_eq("replay closed", (unsigned)chg_crypt_output_allowed(&chr), 0);

    arm(&bms, &chr, CHG_CRYPT_PORT_485, suites, 1, 0);
    hello_485(&bms, &chr, &w);
    chg_crypt_set_ctrl(&bms, CTRL);
    n = chg_crypt_pack_token(&bms, &w, f, sizeof f);
    /* 重新握手，使两枚令牌的计数都是 1，只让 0x04 不同。 */
    arm(&bms, &chr, CHG_CRYPT_PORT_485, suites, 1, 0);
    hello_485(&bms, &chr, &w);
    chg_crypt_set_ctrl(&bms, CTRL);
    chg_crypt_set_bat(&bms, bat);
    n2 = chg_crypt_pack_token(&bms, &w, f2, sizeof f2);
    expect_eq("bat token len", (unsigned)n, 20);
    expect_eq("bat token len 2", (unsigned)n2, 20);
    expect_eq("bat changes tag", memcmp(f + 10, f2 + 10, 8) != 0, 1);
}

int main(void)
{
    printf("sizeof ChgSha256 %lu\n", (unsigned long)sizeof(ChgSha256));
    printf("sizeof ChgMacWork %lu\n", (unsigned long)sizeof(ChgMacWork));
    printf("sizeof ChgCrypt %lu\n", (unsigned long)sizeof(ChgCrypt));
    test_sha();
    test_plain();
    test_hmac_485();
    test_lock();
    test_sif();
    test_id();
    test_vendor();
    test_can_and_ignore();
    test_replay_and_bat();
    if (g_fail) {
        fprintf(stderr, "FAILED\n");
        return 1;
    }
    printf("ok\n");
    return 0;
}
