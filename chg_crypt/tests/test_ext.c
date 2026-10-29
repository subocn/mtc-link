/* 作者：苏勃(subo)
 * SPDX-License-Identifier: Apache-2.0
 * 令牌外壳接到已经组好的明文帧。明文相位机不因 0x05 / 0x07 前进。
 */
#include "chg_crypt.h"
#include "chg_bms.h"
#include "chg_charger.h"

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
    if (memcmp(got, want, n) != 0) {
        size_t i;
        fprintf(stderr, "FAIL %s\n", name);
        for (i = 0; i < n; i++) {
            fprintf(stderr, " %02X", got[i]);
        }
        fprintf(stderr, "\n");
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

static int hello_485(ChgCrypt *bms, ChgCrypt *chr, ChgMacWork *w, uint8_t *hello, int *hello_n)
{
    uint8_t f[40];
    int n = chg_crypt_pack_hello_bms(bms, f, sizeof f);
    int rc;
    if (n < 0) {
        return -1;
    }
    if (hello != 0 && hello_n != 0) {
        memcpy(hello, f, (size_t)n);
        *hello_n = n;
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

static void test_install_gate(void)
{
    uint8_t suites[4];
    uint8_t flags = 0;
    expect_eq("bare n", chg_crypt_level_suites(CHG_CRYPT_LEVEL_HMAC, suites, 4, &flags), 1);
    expect_eq("bare suite", suites[0], CHG_SUITE_PLAIN);
    chg_crypt_install_hmac();
    expect_eq("inst n", chg_crypt_level_suites(CHG_CRYPT_LEVEL_HMAC, suites, 4, &flags), 1);
    expect_eq("inst suite", suites[0], CHG_SUITE_HMAC);
    chg_crypt_set_mac(0);
    expect_eq("cleared suite", chg_crypt_level_suites(CHG_CRYPT_LEVEL_HMAC, suites, 4, &flags), 1);
    expect_eq("cleared wrote plain", suites[0], CHG_SUITE_PLAIN);
    chg_crypt_install_hmac();
}

static void fill_req(ChgCtrl *req)
{
    memset(req, 0, sizeof *req);
    req->allow = 1;
    req->v_req_01v = 840;
    req->i_req_01a = 300;
}

static void test_cover_hmac(void)
{
    ChgCrypt bms, chr;
    ChgMacWork w;
    ChgCtrl req;
    uint8_t packed[16];
    uint8_t ctrl[6];
    uint8_t auth[32];
    uint8_t hello[40];
    uint8_t bad[16];
    int hello_n = 0;
    int pn;
    int n;
    uint16_t i;
    uint8_t en;

    init_level(&bms, CHG_CRYPT_ROLE_BMS, ID_BMS, CHG_CRYPT_LEVEL_HMAC);
    init_level(&chr, CHG_CRYPT_ROLE_CHR, ID_CHR, CHG_CRYPT_LEVEL_HMAC);
    expect_eq("hmac hello", (unsigned)hello_485(&bms, &chr, &w, hello, &hello_n), 1);
    expect_eq("hmac suite", chr.suite, CHG_SUITE_HMAC);
    expect_eq("hello len", hello_n > 0, 1);
    expect_eq("hello msg", hello[3], 0x05);

    fill_req(&req);
    pn = chg_ctrl_pack(&req, packed, sizeof packed);
    expect_eq("pack 01", pn, 13);
    memcpy(ctrl, packed + 5, 6);
    n = chg_crypt_cover_ctrl(&bms, &w, ctrl, 0, auth, sizeof auth);
    expect_eq("cover token", n > 0, 1);
    expect_eq("cover msg", auth[3], 0x07);
    expect_mem("cover bytes", ctrl, packed + 5, 6);

    i = 300;
    en = 1;
    chg_crypt_bind_output(&chr, &i, &en);
    expect_eq("before amps", i, 0);
    expect_eq("before en", en, 0);
    expect_eq("observe 01", (unsigned)chg_crypt_observe(&chr, packed, (size_t)pn), 1);
    memcpy(bad, packed, (size_t)pn);
    bad[5] ^= 0x01u;
    expect_eq("observe bad crc", (unsigned)chg_crypt_observe(&chr, bad, (size_t)pn), 0);
    expect_eq("observe auth", (unsigned)chg_crypt_observe(&chr, auth, (size_t)n), 0);
    expect_eq("rx token", (unsigned)chg_crypt_rx_frame(&chr, &w, auth, (size_t)n), 1);
    i = 300;
    en = 1;
    chg_crypt_bind_output(&chr, &i, &en);
    expect_eq("after amps", i, 300);
    expect_eq("after en", en, 1);
}

static void test_cover_plain(void)
{
    ChgCrypt bms, chr;
    ChgMacWork w;
    ChgCtrl req;
    uint8_t packed[16];
    uint8_t ctrl[6];
    uint8_t auth[32];
    int pn;
    int n;
    uint16_t i;
    uint8_t en;

    init_level(&bms, CHG_CRYPT_ROLE_BMS, ID_BMS, CHG_CRYPT_LEVEL_PLAIN);
    init_level(&chr, CHG_CRYPT_ROLE_CHR, ID_CHR, CHG_CRYPT_LEVEL_PLAIN);
    expect_eq("plain hello", (unsigned)hello_485(&bms, &chr, &w, 0, 0), 1);
    expect_eq("plain suite", bms.suite, CHG_SUITE_PLAIN);
    fill_req(&req);
    pn = chg_ctrl_pack(&req, packed, sizeof packed);
    memcpy(ctrl, packed + 5, 6);
    n = chg_crypt_cover_ctrl(&bms, &w, ctrl, 0, auth, sizeof auth);
    expect_eq("plain cover", n, 0);
    expect_mem("plain same 01", ctrl, packed + 5, 6);
    i = 300;
    en = 1;
    chg_crypt_bind_output(&chr, &i, &en);
    expect_eq("plain amps", i, 300);

    chg_crypt_factory_activate(&bms, KEY, ID_CHR);
    chg_crypt_factory_activate(&chr, KEY, ID_BMS);
    chg_crypt_set_salt_bms(&bms, SALT_BMS);
    chg_crypt_set_salt_chr(&chr, SALT_CHR);
    expect_eq("bound hello", (unsigned)hello_485(&bms, &chr, &w, 0, 0), 1);
    expect_eq("bound suite", bms.suite, CHG_SUITE_PLAIN);
    memcpy(ctrl, packed + 5, 6);
    n = chg_crypt_cover_ctrl(&bms, &w, ctrl, 0, auth, sizeof auth);
    expect_eq("bound cover", n, 0);
    expect_eq("bound allow cleared", ctrl[0], (unsigned)(packed[5] & 0xFEu));
    i = 300;
    en = 1;
    chg_crypt_bind_output(&chr, &i, &en);
    expect_eq("bound amps", i, 0);
    expect_eq("bound en", en, 0);
    (void)pn;
}

static void feed_bms(BmsLink *l, const ChgCtrl *c, const uint8_t *frame, size_t n, BmsTx *tx)
{
    size_t i;
    for (i = 0; i < n; i++) {
        bms_link_rx_byte(l, frame[i], 1, c, tx);
    }
}

static void feed_chr(ChrLink *l, const ChgStat *s, const uint8_t *frame, size_t n, ChrTx *tx)
{
    size_t i;
    for (i = 0; i < n; i++) {
        chr_link_rx_byte(l, frame[i], 1, s, tx);
    }
}

static void test_share_wire(void)
{
    ChgCrypt bms, chr;
    ChgMacWork w;
    ChgCtrl req;
    BmsLink blink;
    BmsTx btx;
    ChrLink clink;
    ChrTx ctx;
    ChgCap cap;
    ChgStat st;
    uint8_t packed[16];
    uint8_t ctrl[6];
    uint8_t auth[32];
    uint8_t hello[40];
    int hello_n = 0;
    int pn;
    int n;
    uint8_t phase;
    static const uint8_t cap1[] = {0xA5, 0x5A, 0x01, 0x02, 0x09, 0x01, 0xF4, 0x01, 0xE8, 0x03,
                                   0x0A, 0x00, 0x20, 0x03, 0xEA, 0x7C};
    static const uint8_t hs[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x00, 0x48, 0x03, 0x00, 0x00, 0x00, 0x7C, 0x5D};
    static const uint8_t run[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x01, 0x48, 0x03, 0x90, 0x01, 0x00, 0x7C, 0x31};

    init_level(&bms, CHG_CRYPT_ROLE_BMS, ID_BMS, CHG_CRYPT_LEVEL_HMAC);
    init_level(&chr, CHG_CRYPT_ROLE_CHR, ID_CHR, CHG_CRYPT_LEVEL_HMAC);
    expect_eq("wire hello", (unsigned)hello_485(&bms, &chr, &w, hello, &hello_n), 1);
    fill_req(&req);
    pn = chg_ctrl_pack(&req, packed, sizeof packed);
    memcpy(ctrl, packed + 5, 6);
    n = chg_crypt_cover_ctrl(&bms, &w, ctrl, 0, auth, sizeof auth);
    expect_eq("wire token", n > 0, 1);
    (void)pn;

    memset(&req, 0, sizeof req);
    req.allow = 1;
    req.v_req_01v = 840;
    req.i_req_01a = 400;
    bms_link_init(&blink, CHG_PORT_485);
    bms_link_tick(&blink, &req, 0, &btx);
    phase = blink.phase;
    feed_bms(&blink, &req, auth, (size_t)n, &btx);
    feed_bms(&blink, &req, hello, (size_t)hello_n, &btx);
    expect_eq("bms phase held", blink.phase, phase);
    expect_eq("bms seen held", blink.seen_ready, 0);
    expect_eq("bms raw clear", blink.raw_n, 0);
    feed_bms(&blink, &req, cap1, sizeof cap1, &btx);
    expect_eq("bms cap after auth", blink.phase, CHG_PHASE_RUN);

    memset(&cap, 0, sizeof cap);
    cap.v_min_01v = 500;
    cap.v_max_01v = 1000;
    cap.i_min_01a = 10;
    cap.i_max_01a = 800;
    memset(&st, 0, sizeof st);
    st.outputting = 1;
    st.dc_path = 1;
    st.v_out_01v = 836;
    st.i_out_01a = 398;
    chr_link_init(&clink, CHG_PORT_485, &cap);
    chr_link_tick(&clink, &st, 0, &ctx);
    phase = clink.phase;
    feed_chr(&clink, &st, hello, (size_t)hello_n, &ctx);
    feed_chr(&clink, &st, auth, (size_t)n, &ctx);
    expect_eq("chr phase held", clink.phase, phase);
    expect_eq("chr no reply", ctx.frame_len, 0);
    expect_eq("chr raw clear", clink.raw_n, 0);
    feed_chr(&clink, &st, hs, sizeof hs, &ctx);
    feed_chr(&clink, &st, hs, sizeof hs, &ctx);
    feed_chr(&clink, &st, run, sizeof run, &ctx);
    expect_eq("chr run", clink.phase, CHG_PHASE_RUN);
    expect_eq("chr enable", ctx.act.enable, 1);
    expect_eq("chr amps", ctx.act.i_out_01a, 400);
}

int main(void)
{
    test_install_gate();
    test_cover_hmac();
    test_cover_plain();
    test_share_wire();
    if (g_fail) {
        fprintf(stderr, "FAILED\n");
        return 1;
    }
    printf("ok\n");
    return 0;
}
