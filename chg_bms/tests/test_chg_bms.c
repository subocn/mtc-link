/* 作者：苏勃(subo)
 * SPDX-License-Identifier: Apache-2.0
 */
#include "chg_bms.h"

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

static ChgCtrl energy(void)
{
    ChgCtrl c;
    memset(&c, 0, sizeof c);
    c.allow = 1;
    c.v_req_01v = 840;
    c.i_req_01a = 400;
    return c;
}

static int find_can(const BmsTx *tx, uint32_t id)
{
    uint8_t i;
    for (i = 0; i < tx->can_n; i++) {
        if (tx->can[i].id == id) {
            return (int)i;
        }
    }
    return -1;
}

static void feed(BmsLink *l, const ChgCtrl *c, const uint8_t *frame, size_t n, BmsTx *tx)
{
    size_t i;
    for (i = 0; i < n; i++) {
        bms_link_rx_byte(l, frame[i], 1, c, tx);
    }
}


static void test_sif(void)
{
    BmsLink link;
    BmsTx tx;
    ChgCtrl c = energy();
    static const uint8_t hs[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x00, 0x48, 0x03, 0x00, 0x00, 0x00, 0x7C, 0x5D};
    static const uint8_t run[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x01, 0x48, 0x03, 0x90, 0x01, 0x00, 0x7C, 0x31};
    static const uint8_t pause[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x03, 0x48, 0x03, 0x90, 0x01, 0x00, 0x7D, 0xD3};
    static const uint8_t stop[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x04, 0x48, 0x03, 0x00, 0x00, 0x00, 0x7D, 0xD9};
    uint8_t noise = 0x11;

    bms_link_init(&link, CHG_PORT_SIF);
    expect_eq("sif init off", link.phase, CHG_PHASE_OFF);
    bms_link_tick(&link, &c, 0, &tx);
    expect_mem("sif hs1", tx.frame, hs, sizeof hs);
    expect_eq("sif hs phase", tx.phase, CHG_PHASE_HS);
    bms_link_tick(&link, &c, 250, &tx);
    expect_mem("sif hs2", tx.frame, hs, sizeof hs);
    bms_link_tick(&link, &c, 250, &tx);
    expect_mem("sif run", tx.frame, run, sizeof run);
    expect_eq("sif run phase", tx.phase, CHG_PHASE_RUN);
    bms_link_tick(&link, &c, 100, &tx);
    expect_eq("sif not yet", tx.frame_len, 0);
    bms_link_tick(&link, &c, 1000, &tx);
    expect_eq("sif stays run without reply", tx.phase, CHG_PHASE_RUN);

    bms_link_rx_byte(&link, noise, 1, &c, &tx);
    expect_eq("sif ignores rx", tx.frame_len, 0);

    c.pause = 1;
    bms_link_tick(&link, &c, 250, &tx);
    expect_mem("sif pause", tx.frame, pause, sizeof pause);

    c.pause = 0;
    c.i_req_01a = 0;
    bms_link_tick(&link, &c, 0, &tx);
    expect_mem("sif stop", tx.frame, stop, sizeof stop);
    expect_eq("sif stopped", tx.phase, CHG_PHASE_STOP);
}

/* v31 新增：chg_can_bsm 把 BMS 0x04 flags 低 4 位写入 byte 6，
 * bit4=1 表示未暂停（GB/T 27930-2023 §6 表 17）。 */
static void test_bsm_flags(void)
{
    ChgCan f;
    chg_can_bsm(/*pause*/ 0u, /*flags*/ 0x04u, &f); /* 未暂停 + 绝缘告警 */
    expect_eq("bsm flags byte6", f.data[6], 0x14u);
    expect_eq("bsm flags id",    f.id,    CHG_CAN_BSM);
    expect_eq("bsm flags dlc",   f.dlc,   7u);

    chg_can_bsm(/*pause*/ 1u, /*flags*/ 0x04u, &f); /* 暂停 + 绝缘告警 */
    expect_eq("bsm paused",      f.data[6], 0x04u);

    chg_can_bsm(/*pause*/ 0u, /*flags*/ 0xF0u, &f); /* 高 4 位 mask */
    expect_eq("bsm flags mask",  f.data[6], 0x10u);
}

static void test_can(void)
{
    BmsLink link;
    BmsTx tx;
    ChgCtrl c = energy();
    ChgCan rx;
    int bcl;
    int bro;
    int bsm;
    int bst;

    bms_link_init(&link, CHG_PORT_CAN);
    expect_eq("can init off", link.phase, CHG_PHASE_OFF);
    bms_link_tick(&link, &c, 0, &tx);
    bcl = find_can(&tx, CHG_CAN_BCL);
    bro = find_can(&tx, CHG_CAN_BRO);
    expect_eq("can hs has bcl", bcl >= 0, 1);
    expect_eq("can hs current", tx.can[bcl].data[2], 0xA0);
    expect_eq("can hs bro", tx.can[bro].data[0], 0x00);
    expect_eq("can hs phase", tx.phase, CHG_PHASE_HS);

    memset(&rx, 0, sizeof rx);
    rx.id = CHG_CAN_CRO;
    rx.dlc = 1;
    rx.data[0] = 0xAA;
    bms_link_can_rx(&link, &rx);
    bms_link_tick(&link, &c, 250, &tx);
    bcl = find_can(&tx, CHG_CAN_BCL);
    bro = find_can(&tx, CHG_CAN_BRO);
    bsm = find_can(&tx, CHG_CAN_BSM);
    expect_eq("can run phase", tx.phase, CHG_PHASE_RUN);
    expect_mem("can bcl", tx.can[bcl].data, (const uint8_t *)"\x48\x03\x30\x11\x02", 5);
    {
        ChgCan big;
        ChgCtrl hi = c;
        hi.i_req_01a = 62000;
        chg_can_bcl(&hi, &big);
        expect_eq("bcl raw cap lo", big.data[2], 0xFFu);
        expect_eq("bcl raw cap hi", big.data[3], 0xFFu);
    }
    memset(&rx, 0, sizeof rx);
    rx.id = CHG_CAN_CCS;
    rx.dlc = 8;
    rx.data[2] = 0xFF;
    rx.data[3] = 0xFF;
    rx.data[6] = 0x01;
    bms_link_can_rx(&link, &rx);
    expect_eq("ccs raw max amps", link.stat.i_out_01a, 61535u);
    expect_eq("can bro aa", tx.can[bro].data[0], 0xAA);
    expect_eq("can bsm run", tx.can[bsm].data[6], 0x10);

    c.pause = 1;
    /* 不满 200 ms 的暂停后，上一拍 39.8 A 仍落在残余的陈旧窗内。 */
    bms_link_tick(&link, &c, 150, &tx);
    bsm = find_can(&tx, CHG_CAN_BSM);
    expect_eq("can bsm pause", tx.can[bsm].data[6], 0x00);
    expect_eq("can pause stays", tx.phase, CHG_PHASE_RUN);

    memset(&rx, 0, sizeof rx);
    rx.id = CHG_CAN_CCS;
    rx.dlc = 8;
    rx.data[0] = 0x44;
    rx.data[1] = 0x03;
    rx.data[2] = 0x2E;
    rx.data[3] = 0x11;
    rx.data[6] = 0x01;
    rx.data[7] = 0x00;
    bms_link_can_rx(&link, &rx);
    expect_eq("ccs volts", link.stat.v_out_01v, 836);
    expect_eq("ccs amps", link.stat.i_out_01a, 398);
    expect_eq("ccs stale not residual", link.i_alarm, CHG_I_ALARM_NONE);
    expect_eq("ccs stale stays", link.phase, CHG_PHASE_RUN);
    bms_link_tick(&link, &c, 900, &tx);
    expect_eq("ccs keeps run", tx.phase, CHG_PHASE_RUN);
    bms_link_tick(&link, &c, 1000, &tx);
    bst = find_can(&tx, CHG_CAN_BST);
    expect_eq("loss stops", tx.phase, CHG_PHASE_STOP);
    expect_eq("loss bst", bst >= 0, 1);
    expect_eq("loss link", tx.link_ok, 0);
    c.pause = 0;
    bms_link_tick(&link, &c, 0, &tx);
    expect_eq("loss holds", tx.phase, CHG_PHASE_STOP);
    bms_link_tick(&link, &c, 10, &tx);
    bst = find_can(&tx, CHG_CAN_BST);
    expect_eq("bst 10ms", bst >= 0, 1);
    expect_eq("bst no bcl", find_can(&tx, CHG_CAN_BCL) < 0, 1);

    c.i_req_01a = 0;
    bms_link_tick(&link, &c, 250, &tx);
    c = energy();
    bms_link_tick(&link, &c, 250, &tx);
    expect_eq("rehs", tx.phase, CHG_PHASE_HS);
}

static void test_can_cml(void)
{
    BmsLink link;
    BmsTx tx;
    ChgCtrl c = energy();
    ChgCan rx;

    /* 起点：BMS 收到 CRO=0xAA 后 cap.ready=1；CML 应保留 ready，不清零。 */
    bms_link_init(&link, CHG_PORT_CAN);
    memset(&rx, 0, sizeof rx);
    rx.id = CHG_CAN_CRO;
    rx.dlc = 1;
    rx.data[0] = 0xAA;
    bms_link_can_rx(&link, &rx);
    expect_eq("can cml seed ready", link.cap.ready, 1);

    /* CML（v_max=840, v_min=500, i_max=400+4000, i_min=10+4000）。
     * 物理量：i_max=400A → 编码 4400 → LE 0x30 0x11
     *        i_min=10A  → 编码 4010 → LE 0xAA 0x0F
     * ready 必须保持 1（CML 不该清掉它）。 */
    memset(&rx, 0, sizeof rx);
    rx.id = CHG_CAN_CML;
    rx.dlc = 8;
    rx.data[0] = 0x48; rx.data[1] = 0x03; /* v_max = 0x0348 = 840 */
    rx.data[2] = 0xF4; rx.data[3] = 0x01; /* v_min = 0x01F4 = 500 */
    rx.data[4] = 0x30; rx.data[5] = 0x11; /* i_max = 4400 - 4000 = 400 A */
    rx.data[6] = 0xAA; rx.data[7] = 0x0F; /* i_min = 4010 - 4000 = 10 A */
    bms_link_can_rx(&link, &rx);
    expect_eq("can cml v_max", link.cap.v_max_01v, 840);
    expect_eq("can cml v_min", link.cap.v_min_01v, 500);
    expect_eq("can cml i_max", link.cap.i_max_01a, 400);
    expect_eq("can cml i_min", link.cap.i_min_01a, 10);
    expect_eq("can cml kept ready", link.cap.ready, 1);

    /* 短 DLC（< 8）应被丢弃，不动 cap。 */
    rx.dlc = 7;
    bms_link_can_rx(&link, &rx);
    expect_eq("can cml short noop", link.cap.v_max_01v, 840);

    /* dlc=8 + 负电流（i_min_raw < 4000 时被夹到 0）。 */
    memset(&rx, 0, sizeof rx);
    rx.id = CHG_CAN_CML;
    rx.dlc = 8;
    rx.data[0] = 0x90; rx.data[1] = 0x01; /* v_max = 0x0190 = 400 */
    rx.data[2] = 0x20; rx.data[3] = 0x03; /* v_min = 0x0320 = 800 */
    rx.data[4] = 0xFF; rx.data[5] = 0x0F; /* i_max_raw = 0x0FFF = 4095 → 95 A */
    rx.data[6] = 0x00; rx.data[7] = 0x00; /* i_min_raw = 0      → 0 A (can_i_phys 夹) */
    bms_link_can_rx(&link, &rx);
    expect_eq("can cml v_max low", link.cap.v_max_01v, 400);
    expect_eq("can cml v_min high", link.cap.v_min_01v, 800);
    expect_eq("can cml i_max phys", link.cap.i_max_01a, 95);
    expect_eq("can cml i_min clamp", link.cap.i_min_01a, 0);

    /* 端口不匹配：把 link 切到 SIF，再收 CML 应被静默忽略，cap 不变。 */
    (void)tx;
    bms_link_init(&link, CHG_PORT_SIF);
    bms_link_tick(&link, &c, 0, &tx);
    memset(&rx, 0, sizeof rx);
    rx.id = CHG_CAN_CML;
    rx.dlc = 8;
    rx.data[0] = 0xFF; rx.data[1] = 0xFF;
    bms_link_can_rx(&link, &rx);
    expect_eq("can cml wrong port", link.cap.v_max_01v, 0);
}

static void test_485(void)
{
    BmsLink link;
    BmsTx tx;
    ChgCtrl c = energy();
    static const uint8_t cap0[] = {0xA5, 0x5A, 0x01, 0x02, 0x09, 0x00, 0xF4, 0x01, 0xE8, 0x03,
                                   0x0A, 0x00, 0x20, 0x03, 0xE7, 0xEC};
    static const uint8_t cap1[] = {0xA5, 0x5A, 0x01, 0x02, 0x09, 0x01, 0xF4, 0x01, 0xE8, 0x03,
                                   0x0A, 0x00, 0x20, 0x03, 0xEA, 0x7C};
    static const uint8_t hs[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x00, 0x48, 0x03, 0x00, 0x00, 0x00, 0x7C, 0x5D};
    static const uint8_t run[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x01, 0x48, 0x03, 0x90, 0x01, 0x00, 0x7C, 0x31};
    static const uint8_t stat[] = {0xA5, 0x5A, 0x01, 0x03, 0x06, 0x09, 0x44, 0x03, 0x8E, 0x01, 0x00, 0x8C, 0xA7};
    static const uint8_t stop[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x04, 0x48, 0x03, 0x00, 0x00, 0x00, 0x7D, 0xD9};

    bms_link_init(&link, CHG_PORT_485);
    expect_eq("485 down", link.link_ok, 0);
    bms_link_tick(&link, &c, 0, &tx);
    expect_mem("485 hs1", tx.frame, hs, sizeof hs);
    expect_eq("485 hs phase", tx.phase, CHG_PHASE_HS);
    bms_link_tick(&link, &c, 100, &tx);
    expect_eq("485 holds bus", tx.frame_len, 0);

    feed(&link, &c, cap0, 3, &tx);
    bms_link_rx_byte(&link, cap0[3], 60, &c, &tx);
    expect_eq("gap drops", link.seen_ready, 0);
    bms_link_init(&link, CHG_PORT_485);
    bms_link_tick(&link, &c, 0, &tx);
    bms_link_tick(&link, &c, 81, &tx);
    feed(&link, &c, cap0, sizeof cap0, &tx);
    expect_eq("late ignored", link.seen_ready, 0);
    expect_eq("late phase", link.phase, CHG_PHASE_HS);

    bms_link_tick(&link, &c, 250, &tx);
    expect_mem("485 hs again", tx.frame, hs, sizeof hs);
    feed(&link, &c, cap0, sizeof cap0, &tx);
    expect_eq("cap0 silent", tx.frame_len, 0);
    expect_eq("cap0 not ready", link.seen_ready, 0);
    expect_eq("cap0 link", link.link_ok, 1);

    bms_link_tick(&link, &c, 250, &tx);
    expect_eq("485 bat before energy", tx.frame_len, 0);
    expect_eq("485 bat valid clear", tx.bat_frame[5], 0);
    expect_eq("485 still before ready", tx.phase, CHG_PHASE_HS);
    bms_link_tick(&link, &c, 250, &tx);
    expect_mem("485 hs2", tx.frame, hs, sizeof hs);
    feed(&link, &c, cap1, sizeof cap1, &tx);
    expect_eq("485 run", link.phase, CHG_PHASE_RUN);
    expect_eq("cap1 silent", tx.frame_len, 0);

    bms_link_tick(&link, &c, 250, &tx);
    expect_mem("485 run req", tx.frame, run, sizeof run);
    feed(&link, &c, stat, sizeof stat, &tx);
    expect_eq("485 saw output", link.stat.i_out_01a, 398);
    expect_eq("stat silent", tx.frame_len, 0);

    feed(&link, &c, run, sizeof run, &tx);
    expect_eq("485 ignores ctrl", tx.frame_len, 0);
    expect_eq("ctrl keeps run", link.phase, CHG_PHASE_RUN);

    bms_link_tick(&link, &c, 400, &tx);
    expect_eq("485 loss 400", link.loss_ms, 400u);
    feed(&link, &c, cap1, sizeof cap1, &tx);
    expect_eq("485 cap no refresh", link.loss_ms, 400u);
    expect_eq("485 cap still run", link.phase, CHG_PHASE_RUN);
    bms_link_tick(&link, &c, 250, &tx);
    feed(&link, &c, stat, sizeof stat, &tx);
    expect_eq("485 stat clears loss", link.loss_ms, 0u);

    bms_link_tick(&link, &c, 1000, &tx);
    expect_eq("loss not before 1s", tx.phase, CHG_PHASE_RUN);
    bms_link_tick(&link, &c, 200, &tx);
    expect_eq("loss stops", tx.phase, CHG_PHASE_STOP);
    expect_mem("loss stop frame", tx.frame, stop, sizeof stop);
    expect_eq("loss link", tx.link_ok, 0);
}

static ChgBat sample_bat(void)
{
    ChgBat b;
    memset(&b, 0, sizeof b);
    b.valid = 1;
    b.soc_01pct = 500;
    b.v_pack_01v = 760;
    b.v_cell_max_mv = 3900;
    b.v_cell_min_mv = 3800;
    b.t_max_c = 35;
    b.t_min_c = 28;
    b.rated_ah_01ah = 200;
    b.rated_v_01v = 730;
    b.chemistry = CHG_CHEM_NCM;
    b.v_allow_01v = 840;
    b.i_allow_01a = 400;
    b.t_allow_c = 55;
    b.remain_min = 30;
    b.idx_cell_max = 3;
    b.idx_cell_min = 12;
    b.idx_temp_max = 1;
    b.idx_temp_min = 2;
    return b;
}

static void test_bat(void)
{
    static const uint8_t golden[] = {
        0xA5, 0x5A, 0x01, 0x04, 0x1C, 0x01, 0x00, 0xF4, 0x01, 0xF8, 0x02, 0x3C, 0x0F, 0xD8, 0x0E, 0x23,
        0x1C, 0xC8, 0x00, 0xDA, 0x02, 0x01, 0x48, 0x03, 0x90, 0x01, 0x37, 0x1E, 0x00, 0x03, 0x0C, 0x01,
        0x02, 0xC0, 0x92};
    static const uint8_t query[] = {0xA5, 0x5A, 0x01, 0x05, 0x00, 0x28, 0x51};
    static const uint8_t hs[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x00, 0x48, 0x03, 0x00, 0x00, 0x00, 0x7C, 0x5D};
    static const uint8_t cap0[] = {0xA5, 0x5A, 0x01, 0x02, 0x09, 0x00, 0xF4, 0x01, 0xE8, 0x03,
                                   0x0A, 0x00, 0x20, 0x03, 0xE7, 0xEC};
    uint8_t frame[40];
    ChgBat b = sample_bat();
    BmsLink link;
    BmsTx tx;
    ChgCtrl c = energy();
    ChgCan part[4];
    int n;
    int i;

    n = chg_bat_pack(&b, frame, sizeof frame);
    expect_eq("bat pack len", (unsigned)n, sizeof golden);
    expect_mem("bat pack", frame, golden, sizeof golden);
    b.flags = (uint8_t)(CHG_BAT_F_INSUL | CHG_BAT_F_CONN | CHG_BAT_F_CELL_OV |
                        CHG_BAT_F_CELL_UV | CHG_BAT_F_OT | CHG_BAT_F_UT | 0xC0u);
    n = chg_bat_pack(&b, frame, sizeof frame);
    expect_eq("flags packed", frame[6], 0x3F);
    b.flags = 0;
    chg_can_bat(&b, part);
    expect_eq("bat id private", part[0].id, CHG_CAN_BAT);
    expect_eq("bat id not bsd", part[0].id == 0x181C56F4u, 0);
    expect_mem("bat can0", part[0].data, (const uint8_t *)"\x00\x01\x00\xF4\x01\xF8\x02\x3C", 8);
    expect_mem("bat can3", part[3].data, (const uint8_t *)"\x03\x37\x1E\x00\x03\x0C\x01\x02", 8);

    bms_link_init(&link, CHG_PORT_SIF);
    bms_link_tick(&link, &c, 0, &tx);
    expect_eq("unset bat valid", tx.bat_frame[5], 0);
    expect_eq("unset bat len", tx.bat_len, 35);
    expect_mem("unset still hs", tx.frame, hs, sizeof hs);

    bms_link_init(&link, CHG_PORT_SIF);
    bms_link_set_bat(&link, &b);
    bms_link_tick(&link, &c, 0, &tx);
    expect_mem("sif ctrl stays hs", tx.frame, hs, sizeof hs);
    expect_eq("sif still hs", tx.phase, CHG_PHASE_HS);
    expect_mem("sif bat", tx.bat_frame, golden, tx.bat_len);
    bms_link_tick(&link, &c, 250, &tx);
    expect_mem("sif hs2 still", tx.frame, hs, sizeof hs);
    expect_eq("sif bat not every ctrl", tx.bat_len, 0);
    bms_link_tick(&link, &c, 250, &tx);
    expect_eq("sif run after two hs", tx.phase, CHG_PHASE_RUN);
    expect_eq("sif run flag", tx.frame[5], 0x01);

    bms_link_init(&link, CHG_PORT_485);
    bms_link_set_bat(&link, &b);
    bms_link_tick(&link, &c, 0, &tx);
    expect_mem("485 leads hs", tx.frame, hs, sizeof hs);
    expect_eq("485 bat waits hs", tx.bat_len, 0);
    feed(&link, &c, query, sizeof query, &tx);
    expect_eq("485 ignores query", tx.bat_len, 0);
    expect_eq("485 query keeps wait", link.waiting, 1);
    feed(&link, &c, cap0, sizeof cap0, &tx);
    expect_eq("485 still hs", link.phase, CHG_PHASE_HS);
    bms_link_tick(&link, &c, 250, &tx);
    expect_eq("485 bat before current", tx.frame_len, 0);
    expect_eq("485 bat while hs", tx.phase, CHG_PHASE_HS);
    expect_mem("485 pushed bat", tx.bat_frame, golden, sizeof golden);
    {
        static const uint8_t cap1[] = {0xA5, 0x5A, 0x01, 0x02, 0x09, 0x01, 0xF4, 0x01, 0xE8, 0x03,
                                       0x0A, 0x00, 0x20, 0x03, 0xEA, 0x7C};
        static const uint8_t run[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x01, 0x48, 0x03, 0x90, 0x01, 0x00, 0x7C, 0x31};
        static const uint8_t stat[] = {0xA5, 0x5A, 0x01, 0x03, 0x06, 0x09, 0x44, 0x03, 0x8E, 0x01, 0x00, 0x8C, 0xA7};
        static const uint8_t pause[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x03, 0x48, 0x03, 0x90, 0x01, 0x00, 0x7D, 0xD3};
        bms_link_tick(&link, &c, 250, &tx);
        expect_mem("485 hs after bat", tx.frame, hs, sizeof hs);
        feed(&link, &c, cap1, sizeof cap1, &tx);
        expect_eq("485 ready runs", link.phase, CHG_PHASE_RUN);
        bms_link_tick(&link, &c, 250, &tx);
        expect_mem("485 energy", tx.frame, run, sizeof run);
        feed(&link, &c, stat, sizeof stat, &tx);
        c.pause = 1;
        bms_link_tick(&link, &c, 250, &tx);
        expect_mem("485 pause", tx.frame, pause, sizeof pause);
        expect_eq("485 pause stays", tx.phase, CHG_PHASE_RUN);
        c.pause = 0;
        feed(&link, &c, stat, sizeof stat, &tx);
        bms_link_tick(&link, &c, 1000, &tx);
        expect_eq("bat not yet loss", tx.phase, CHG_PHASE_RUN);
        bms_link_tick(&link, &c, 200, &tx);
        expect_eq("bat does not hold", tx.phase, CHG_PHASE_STOP);
    }

    bms_link_init(&link, CHG_PORT_CAN);
    bms_link_set_bat(&link, &b);
    bms_link_tick(&link, &c, 0, &tx);
    expect_eq("can bcl kept", find_can(&tx, CHG_CAN_BCL) >= 0, 1);
    expect_eq("can room", tx.can_n > 8u, 0);
    for (i = 0; i < 4; i++) {
        int ix = -1;
        uint8_t k;
        for (k = 0; k < tx.can_n; k++) {
            if (tx.can[k].id == CHG_CAN_BAT && tx.can[k].data[0] == (uint8_t)i) {
                ix = (int)k;
            }
        }
        expect_eq("can bat seq", ix >= 0, 1);
        if (ix >= 0) {
            expect_mem("can bat bytes", tx.can[ix].data, part[i].data, 8);
        }
    }
}

static void test_edges(void)
{
    BmsLink link;
    BmsTx tx;
    ChgCtrl c = energy();
    ChgBat b = sample_bat();
    uint8_t frame[40];
    static const uint8_t hs[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x00, 0x48, 0x03, 0x00, 0x00, 0x00, 0x7C, 0x5D};
    static const uint8_t cap0[] = {0xA5, 0x5A, 0x01, 0x02, 0x09, 0x00, 0xF4, 0x01, 0xE8, 0x03,
                                   0x0A, 0x00, 0x20, 0x03, 0xE7, 0xEC};
    static const uint8_t bad[] = {0xA5, 0x5A, 0x01, 0x02, 0x09, 0x00, 0xF4, 0x01, 0xE8, 0x03,
                                  0x0A, 0x00, 0x20, 0x03, 0xE7, 0xED};
    static const uint8_t cold[] = {
        0xA5, 0x5A, 0x01, 0x04, 0x1C, 0x01, 0x00, 0xF4, 0x01, 0xF8, 0x02, 0x3C, 0x0F, 0xD8, 0x0E, 0x23,
        0xFB, 0xC8, 0x00, 0xDA, 0x02, 0x01, 0x48, 0x03, 0x90, 0x01, 0x37, 0x1E, 0x00, 0x03, 0x0C, 0x01,
        0x02, 0x3B, 0x55};
    int n;

    b.t_min_c = -5;
    n = chg_bat_pack(&b, frame, sizeof frame);
    expect_eq("cold len", (unsigned)n, sizeof cold);
    expect_mem("cold pack", frame, cold, sizeof cold);

    bms_link_init(&link, CHG_PORT_485);
    bms_link_tick(&link, &c, 0, &tx);
    expect_mem("edge hs", tx.frame, hs, sizeof hs);
    feed(&link, &c, bad, sizeof bad, &tx);
    expect_eq("bad crc waits", link.waiting, 1);
    expect_eq("bad crc not ready", link.seen_ready, 0);
    bms_link_tick(&link, &c, 250, &tx);
    expect_mem("retry still 0x01", tx.frame, hs, sizeof hs);
    feed(&link, &c, cap0, sizeof cap0, &tx);
    expect_eq("good reply", link.answered, 1);
    bms_link_tick(&link, &c, 250, &tx);
    expect_eq("bat not a retry", tx.frame_len, 0);
    expect_eq("bat msg", tx.bat_frame[3], 0x04);

    c.i_req_01a = 0;
    bms_link_init(&link, CHG_PORT_SIF);
    bms_link_tick(&link, &c, 0, &tx);
    expect_eq("zero request stops", tx.phase, CHG_PHASE_STOP);
    expect_eq("zero request flag", tx.frame[5], 0x04);

    bms_link_init(&link, CHG_PORT_CAN);
    c = energy();
    c.cv = 1;
    {
        ChgCan rx;
        ChgCan bcl;
        memset(&rx, 0, sizeof rx);
        rx.id = CHG_CAN_CRO;
        rx.dlc = 1;
        rx.data[0] = 0xAA;
        bms_link_can_rx(&link, &rx);
        bms_link_tick(&link, &c, 0, &tx);
        chg_can_bcl(&c, &bcl);
        expect_eq("cv mode", bcl.data[4], 0x01);
        expect_eq("cv phase", tx.phase, CHG_PHASE_RUN);
    }
}

/* 防御性测试：BMS 收到未识别的 CAN ID、收到自身不应响应的 ID、以及在非 485 口
 * 喂字节流时应被静默忽略。 */
static void enter_can_run(BmsLink *link, BmsTx *tx)
{
    ChgCtrl c = energy();
    ChgCan rx;
    bms_link_init(link, CHG_PORT_CAN);
    bms_link_tick(link, &c, 0, tx);
    memset(&rx, 0, sizeof rx);
    rx.id = CHG_CAN_CRO;
    rx.dlc = 1;
    rx.data[0] = 0xAA;
    bms_link_can_rx(link, &rx);
    bms_link_tick(link, &c, 0, tx);
}

static void test_period_and_address(void)
{
    BmsLink link;
    expect_eq("loss nom not tripped", chg_loss_tripped(CHG_T_LOSS_NOM_MS), 0);
    expect_eq("loss limit tripped", chg_loss_tripped(CHG_T_LOSS_MS), 1);
    BmsTx tx;
    ChgCtrl c = energy();
    ChgCan rx;
    int i;
    uint32_t loss;

    enter_can_run(&link, &tx);
    expect_eq("ccs period run", link.phase, CHG_PHASE_RUN);
    memset(&rx, 0, sizeof rx);
    rx.id = CHG_CAN_CCS;
    rx.dlc = 8;
    rx.data[6] = 0x01;
    bms_link_can_rx(&link, &rx);
    for (i = 0; i < 30; i++) {
        bms_link_tick(&link, &c, 55, &tx);
        bms_link_can_rx(&link, &rx);
    }
    expect_eq("ccs 55 stays", link.phase, CHG_PHASE_RUN);
    expect_eq("ccs 55 not late", link.period_late, 0);

    bms_link_tick(&link, &c, 56, &tx);
    expect_eq("ccs 56 late", link.period_late, 1);
    expect_eq("ccs 56 not stop", link.phase, CHG_PHASE_RUN);

    loss = link.loss_ms;
    rx.id = CHG_CAN_CRO;
    rx.dlc = 1;
    rx.data[0] = 0xAA;
    bms_link_can_rx(&link, &rx);
    memset(&rx, 0, sizeof rx);
    rx.id = CHG_CAN_CML;
    rx.dlc = 8;
    bms_link_can_rx(&link, &rx);
    expect_eq("cro cml no refresh", link.loss_ms, loss);

    rx.id = CHG_CAN_CCS ^ 0x04000000u;
    rx.dlc = 8;
    rx.data[6] = 0x00;
    bms_link_can_rx(&link, &rx);
    rx.id = CHG_CAN_CCS ^ 0x01u;
    bms_link_can_rx(&link, &rx);
    rx.id = CHG_CAN_CCS ^ 0x0100u;
    bms_link_can_rx(&link, &rx);
    expect_eq("ccs bad id drop", link.rx_drop, 3u);
    expect_eq("ccs bad id no refresh", link.loss_ms, loss);
    expect_eq("ccs bad id no fault", link.stat.fault, 0);
    expect_eq("ccs bad id still run", link.phase, CHG_PHASE_RUN);
    bms_link_tick(&link, &c, CHG_T_LOSS_MS - loss, &tx);
    expect_eq("silence then stop", link.phase, CHG_PHASE_STOP);
}

static void test_can_ignore(void)
{
    BmsLink link;
    BmsTx tx;
    ChgCtrl c = energy();
    ChgCan rx;
    uint8_t noise = 0xA5;

    bms_link_init(&link, CHG_PORT_CAN);
    memset(&rx, 0, sizeof rx);
    rx.id = 0x18EE56F4u;
    rx.dlc = 8;
    rx.data[0] = 0xAA;
    bms_link_can_rx(&link, &rx);
    bms_link_can_rx(&link, &rx);
    expect_eq("can noise cap intact", link.cap.ready, 0);
    expect_eq("can noise stat intact", link.stat.fault, 0);
    expect_eq("can noise no force stop", link.force_stop, 0);
    /* 加深 intact：未知 ID 不应推动 phase / waiting / seen_ready / link_ok / bat。 */
    expect_eq("can noise phase intact", link.phase, CHG_PHASE_OFF);
    expect_eq("can noise waiting intact", link.waiting, 0);
    expect_eq("can noise seen ready intact", link.seen_ready, 0);
    expect_eq("can noise link ok intact", link.link_ok, 0);
    expect_eq("can noise bat valid intact", link.bat.valid, 0);

    bms_link_init(&link, CHG_PORT_SIF);
    bms_link_tick(&link, &c, 0, &tx);
    bms_link_rx_byte(&link, noise, 1, &c, &tx);
    bms_link_rx_byte(&link, noise ^ 0xFF, 1, &c, &tx);
    /* SIF/CAN 上 rx_byte 早 return，frame_len 必须 0（不发出错位帧） */
    expect_eq("sif rx byte no send", tx.frame_len, 0);

    bms_link_init(&link, CHG_PORT_CAN);
    bms_link_rx_byte(&link, noise, 1, &c, &tx);
    expect_eq("can rx byte ignored", tx.frame_len, 0);
}

/* T_E2E：BMS 拒绝损坏 CAN 帧（CST dlc=0、BRO=AA dlc=0、CCS dlc 不足）。
 * 与 §5.10.4 "短 DLC ... 整帧忽略" 对齐；与 BMS quirk 场景的桩侧出帧
 * 验证形成闭环：桩出坏帧 → BMS 不动 → e2e 状态不跳。 */
/* 防御性测试：bms_link_* NULL guard + 端口不匹配 guard + accept_485 过滤 +
 * chg_*_pack failure 路径。这些是 silent-return 的早期保护路径，
 * 调用方误传 NULL / 非预期端口 / 错误 msg 都不应崩溃或误改状态。 */
static void test_bms_null_guards(void)
{
    BmsLink link;
    BmsTx tx;
    ChgCtrl c = energy();
    ChgBat b = sample_bat();
    ChgCan rx;
    uint8_t frame[64];

    /* 1. bms_link_set_bat(NULL, &b) — 不崩溃。 */
    bms_link_set_bat(NULL, &b);

    /* 2. bms_link_set_bat(&link, NULL) — 清空 bat。 */
    bms_link_init(&link, CHG_PORT_SIF);
    bms_link_set_bat(&link, &b);
    expect_eq("set_bat first valid", link.bat.valid, 1);
    bms_link_set_bat(&link, NULL);
    expect_eq("set_bat null clears valid", link.bat.valid, 0);

    /* 3. bms_link_can_rx(NULL, &rx) — 不崩溃。 */
    memset(&rx, 0, sizeof rx);
    rx.id = CHG_CAN_CML;
    rx.dlc = 8;
    bms_link_can_rx(NULL, &rx);

    /* 4. bms_link_can_rx 在非 CAN 端口静默忽略（CML/CRO/CCS 都不动 link）。 */
    bms_link_init(&link, CHG_PORT_SIF);
    bms_link_tick(&link, &c, 0, &tx);
    memset(&rx, 0, sizeof rx);
    rx.id = CHG_CAN_CML;
    rx.dlc = 8;
    rx.data[0] = 0xFF; rx.data[1] = 0xFF;
    rx.data[2] = 0xFF; rx.data[3] = 0xFF;
    rx.data[4] = 0xFF; rx.data[5] = 0xFF;
    rx.data[6] = 0xFF; rx.data[7] = 0xFF;
    bms_link_can_rx(&link, &rx);
    expect_eq("can_rx sif port noop", link.cap.v_max_01v, 0);

    /* 5. chg_bat_pack：out=NULL → -1（经 frame_pack 转发）。 */
    expect_eq("bat pack null out", chg_bat_pack(&b, NULL, 100), -1);

    /* 6. chg_bat_pack：cap 太小（< 35）→ -1。 */
    expect_eq("bat pack cap small", chg_bat_pack(&b, frame, 10), -1);

    /* 7. chg_ctrl_pack：c=NULL → -1；cap 太小 → -1。 */
    expect_eq("ctrl pack null", chg_ctrl_pack(NULL, frame, 100), -1);
    expect_eq("ctrl pack cap small", chg_ctrl_pack(&c, frame, 5), -1);

    /* 8. accept_485：非 0x02/0x03 的 msg（如 0x04 BAT）被静默丢弃。
     *    走到这条路径需要 waiting=1，所以先 hs 一下让 link 进入 waiting 态。
     *    然后喂一个合法 CRC 但 msg=0x04 的短帧（len=6, data 全 0）。 */
    {
        BmsLink link2;
        BmsTx tx2;
        static const uint8_t hs[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x00, 0x48, 0x03, 0x00, 0x00, 0x00, 0x7C, 0x5D};
        static const uint8_t bat_msg_wrong[] = {0xA5, 0x5A, 0x01, 0x04, 0x06, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xBC, 0x5B};
        bms_link_init(&link2, CHG_PORT_485);
        bms_link_tick(&link2, &c, 0, &tx2);
        feed(&link2, &c, hs, sizeof hs, &tx2); /* 触发 cap 等待 */
        expect_eq("wait cap", link2.waiting, 1);
        feed(&link2, &c, bat_msg_wrong, sizeof bat_msg_wrong, &tx2);
        /* accept_485 拒绝非 0x02/0x03 的 msg；waiting 仍为 1（未成功应答）。 */
        expect_eq("msg 0x04 dropped", link2.waiting, 1);
        expect_eq("not seen ready", link2.seen_ready, 0);
    }
}

/* 防御性测试：raw 缓冲越界，以及长度大于 0x02/0x03 的帧。
 * raw[40]。声明长度加上 7 超过缓冲时，在第 5 字节丢掉。
 * 长度在 10 到 33 之间时先收满整帧，再整帧丢掉，不写进 9 字节 data。 */
static void test_raw_guards(void)
{
    BmsLink link;
    BmsTx tx;
    ChgCtrl c = energy();
    uint8_t len10[17];
    uint8_t i;

    /* LEN=10 收满 17 字节后整帧丢掉。相位保持握手，raw_n 清零。 */
    memset(len10, 0, sizeof len10);
    len10[0] = 0xA5;
    len10[1] = 0x5A;
    len10[2] = 0x01;
    len10[3] = 0x07;
    len10[4] = 10;
    bms_link_init(&link, CHG_PORT_485);
    bms_link_tick(&link, &c, 0, &tx);
    feed(&link, &c, len10, sizeof len10, &tx);
    expect_eq("raw len10 dropped", tx.frame_len, 0);
    expect_eq("raw len10 reset", link.raw_n, 0);
    expect_eq("raw len10 phase", link.phase, CHG_PHASE_HS);
    expect_eq("raw len10 seen", link.seen_ready, 0);

    /* 声明长度超过缓冲（0xCC + 7 > 40）时，第 5 字节复位。 */
    bms_link_init(&link, CHG_PORT_485);
    bms_link_tick(&link, &c, 0, &tx);
    for (i = 0; i < 5; i++) {
        bms_link_rx_byte(&link, 0xCC, 10, &c, &tx);
    }
    expect_eq("raw len15 reset", link.raw_n, 0);

    /* raw[4] == 9 边界：cap0 是 LEN=9 → 应被尝试解析（CRC 不过不报错）。 */
    {
        static const uint8_t cap0[] = {0xA5, 0x5A, 0x01, 0x02, 0x09, 0x00, 0xF4, 0x01, 0xE8, 0x03,
                                       0x0A, 0x00, 0x20, 0x03, 0xE7, 0xEC};
        bms_link_init(&link, CHG_PORT_485);
        bms_link_tick(&link, &c, 81, &tx);
        feed(&link, &c, cap0, sizeof cap0, &tx);
        expect_eq("raw len9 parsed", link.link_ok, 1); /* CRC 通过 → 链路上线 */
    }

    /* T_GAP_MS > 50 后喂单字节：line 635 的 gap 重置把 raw_n 清 0。 */
    bms_link_init(&link, CHG_PORT_485);
    bms_link_tick(&link, &c, 0, &tx);
    for (i = 0; i < 4; i++) {
        bms_link_rx_byte(&link, 0xA5, 10, &c, &tx);
    }
    expect_eq("raw prep 4", link.raw_n, 4);
    /* gap_ms = 60 > 50 → gap 重置生效，raw_n 变 0，再喂一个新字节 raw_n=1 */
    bms_link_rx_byte(&link, 0x5A, 60, &c, &tx);
    expect_eq("raw gap reset", link.raw_n, 1);
    expect_eq("raw gap new head", link.raw[0], 0x5A);
}

static void test_can_drop_short_dlc(void)
{
    BmsLink link;
    BmsTx tx;
    ChgCtrl c = energy();
    ChgCan rx;

    /* 1. CST dlc=0 → BMS 不应设 fault */
    bms_link_init(&link, CHG_PORT_CAN);
    bms_link_tick(&link, &c, 50, &tx);  /* 进入 RUN */
    /* 用 CRO=AA 强制 promote */
    memset(&rx, 0, sizeof rx);
    rx.id = CHG_CAN_CRO;
    rx.dlc = 1;
    rx.data[0] = 0xAA;
    bms_link_can_rx(&link, &rx);
    bms_link_tick(&link, &c, 50, &tx);
    expect_eq("can drop: bms RUN after CRO", link.phase, CHG_PHASE_RUN);

    memset(&rx, 0, sizeof rx);
    rx.id = CHG_CAN_CST;
    rx.dlc = 0;  /* 短 DLC：被忽略 */
    bms_link_can_rx(&link, &rx);
    expect_eq("can drop: CST dlc=0 no fault", link.stat.fault, 0);
    expect_eq("can drop: CST dlc=0 still RUN", link.phase, CHG_PHASE_RUN);

    /* 2. CCS dlc=4（短 8）→ BMS 不更新 stat */
    memset(&link.stat, 0, sizeof link.stat);
    memset(&rx, 0, sizeof rx);
    rx.id = CHG_CAN_CCS;
    rx.dlc = 4;  /* 短 8：被忽略 */
    bms_link_can_rx(&link, &rx);
    expect_eq("can drop: CCS dlc=4 stat intact v_out", link.stat.v_out_01v, 0);
    expect_eq("can drop: CCS dlc=4 stat intact i_out", link.stat.i_out_01a, 0);

    /* 3. CCS dlc=8 正常 → stat 更新 */
    memset(&rx, 0, sizeof rx);
    rx.id = CHG_CAN_CCS;
    rx.dlc = 8;
    rx.data[0] = 0x48;  /* v_out 0x0348 = 840 = 84.0 V */
    rx.data[1] = 0x03;
    rx.data[2] = 0xA0;  /* i_raw 0x0FA0 = 4000 → i_out = 0 */
    rx.data[3] = 0x0F;
    rx.data[4] = 0x00;
    rx.data[5] = 0x00;
    rx.data[6] = 0x01;  /* no fault */
    rx.data[7] = 0x00;
    bms_link_can_rx(&link, &rx);
    expect_eq("can drop: CCS dlc=8 v_out", link.stat.v_out_01v, 840);
    expect_eq("can drop: CCS dlc=8 i_out", link.stat.i_out_01a, 0);
}

/* §10 验收 14: BMS 见 CST 或 0x03 fault 1 s 内进入 STOP — CST 是事件性的，
 * 应立即 STOP（不等 T_LOSS_MS）。 */
static void test_cst_immediate_stop(void)
{
    BmsLink link;
    BmsTx tx;
    ChgCtrl c = energy();
    ChgCan rx;

    bms_link_init(&link, CHG_PORT_CAN);
    bms_link_tick(&link, &c, 50, &tx);
    memset(&rx, 0, sizeof rx);
    rx.id = CHG_CAN_CRO;
    rx.dlc = 1;
    rx.data[0] = 0xAA;
    bms_link_can_rx(&link, &rx);
    bms_link_tick(&link, &c, 50, &tx);
    expect_eq("cst stops: in RUN", link.phase, CHG_PHASE_RUN);

    /* 有效 CST（4 字节）：立即 STOP，stat.fault=1, fault_code=byte0 & 0x07 */
    memset(&rx, 0, sizeof rx);
    rx.id = CHG_CAN_CST;
    rx.dlc = 4;
    rx.data[0] = 0x05;
    bms_link_can_rx(&link, &rx);
    expect_eq("cst stops: fault set", link.stat.fault, 1);
    expect_eq("cst stops: fault_code", link.stat.fault_code, 0x05);
    rx.data[0] = 0x08;
    bms_link_can_rx(&link, &rx);
    expect_eq("cst 8 is 3", link.stat.fault_code, 0x03u);
    expect_eq("cst stops: phase STOP", link.phase, CHG_PHASE_STOP);
    expect_eq("cst stops: force_stop", link.force_stop, 1);
    expect_eq("cst stops: link down", link.link_ok, 0);
    /* §10 验收 14 + chg_bms.c:444-457：STOP 入口清三字段，避免下次握手假握手。 */
    expect_eq("cst stops: seen_ready cleared", link.seen_ready, 0);
    expect_eq("cst stops: sif_sent cleared", link.sif_sent, 0);
    expect_eq("cst stops: waiting cleared", link.waiting, 0);
}

/* §3.2 阶段 6: RS-485 停止期间不再发送帧。STOP 当周期发停止帧，
 * 之后 slow 到点也不应再发。 */
static void test_485_stop_no_resend(void)
{
    BmsLink link;
    BmsTx tx;
    ChgCtrl c = energy();
    static const uint8_t cap0[] = {0xA5, 0x5A, 0x01, 0x02, 0x09, 0x00, 0xF4, 0x01, 0xE8, 0x03,
                                   0x0A, 0x00, 0x20, 0x03, 0xE7, 0xEC};
    static const uint8_t cap1[] = {0xA5, 0x5A, 0x01, 0x02, 0x09, 0x01, 0xF4, 0x01, 0xE8, 0x03,
                                   0x0A, 0x00, 0x20, 0x03, 0xEA, 0x7C};

    bms_link_init(&link, CHG_PORT_485);
    bms_link_tick(&link, &c, 0, &tx);
    /* HS：发 cap0；第一次喂 cap0 让 seen_ready=1 */
    expect_eq("485 stop: hs phase", tx.phase, CHG_PHASE_HS);
    feed(&link, &c, cap0, sizeof cap0, &tx);
    bms_link_tick(&link, &c, 250, &tx);
    bms_link_tick(&link, &c, 250, &tx);
    /* 再喂一次 cap1 拿 ready；进入 RUN */
    feed(&link, &c, cap1, sizeof cap1, &tx);
    expect_eq("485 stop: in RUN", link.phase, CHG_PHASE_RUN);

    /* 进 STOP（i_req=0）：当周期发 0x01 stop 帧 */
    c.i_req_01a = 0;
    bms_link_tick(&link, &c, 0, &tx);
    expect_eq("485 stop: STOP phase", tx.phase, CHG_PHASE_STOP);
    expect_eq("485 stop: entry frame sent", tx.frame_len, 13);
    expect_eq("485 stop: entry byte0 stop", tx.frame[5], 0x04);

    /* 后续 STOP 期间即使 slow 到点（每 250 ms）也不发任何 RS-485 帧。 */
    bms_link_tick(&link, &c, 250, &tx);
    expect_eq("485 stop: no resend @ 250", tx.frame_len, 0);
    bms_link_tick(&link, &c, 250, &tx);
    expect_eq("485 stop: no resend @ 500", tx.frame_len, 0);
    bms_link_tick(&link, &c, 1000, &tx);
    expect_eq("485 stop: no resend @ 1500", tx.frame_len, 0);
}

/* §6.6 line 720: SIF 持续拉低 ≥ 100 ms → FAULT，force_stop=1。 */
static void test_sif_short_circuit(void)
{
    BmsLink link;
    BmsTx tx;
    ChgCtrl c = energy();
    (void)tx;
    (void)c;

    bms_link_init(&link, CHG_PORT_SIF);
    /* idle 高电平 50 ms：累加器清零 */
    bms_link_set_sif_state(&link, 1, 50);
    expect_eq("sif short: idle phase", link.phase, CHG_PHASE_OFF);
    expect_eq("sif short: idle counter", link.sif_short_ms, 0);

    /* 拉低 50 ms：累加 50 ms，未达阈值 */
    bms_link_set_sif_state(&link, 0, 50);
    expect_eq("sif short: 50ms phase", link.phase, CHG_PHASE_OFF);
    expect_eq("sif short: 50ms counter", link.sif_short_ms, 50);

    /* 再拉低 60 ms：累加 110 ms ≥ 100 → STOP, force_stop=1 */
    bms_link_set_sif_state(&link, 0, 60);
    expect_eq("sif short: STOP phase", link.phase, CHG_PHASE_STOP);
    expect_eq("sif short: force_stop", link.force_stop, 1);
    expect_eq("sif short: link down", link.link_ok, 0);
    /* chg_bms.c:399-404 STOP 入口三字段清零。 */
    expect_eq("sif short: seen_ready cleared", link.seen_ready, 0);
    expect_eq("sif short: sif_sent cleared", link.sif_sent, 0);
    expect_eq("sif short: waiting cleared", link.waiting, 0);

    /* 一旦拉高（修复），累加器归零；不应自动回到 RUN（需要 force_stop 流程）。 */
    bms_link_set_sif_state(&link, 1, 100);
    expect_eq("sif short: idle resets", link.sif_short_ms, 0);
}

static void reach_can_run(BmsLink *link, BmsTx *tx, const ChgCtrl *c)
{
    ChgCan cro;
    bms_link_init(link, CHG_PORT_CAN);
    bms_link_tick(link, c, 0, tx);
    memset(&cro, 0, sizeof cro);
    cro.id = CHG_CAN_CRO;
    cro.dlc = 1;
    cro.data[0] = 0xAA;
    bms_link_can_rx(link, &cro);
    bms_link_tick(link, c, 0, tx);
}

/* §10 验收 15: CAN bus-off ≥ 100 ms 立即 STOP。噪声 CCS 不把链路拉回去。
 * 线恢复后，请求电流先回到 0 才允许新的 HS。 */
static void test_bus_off(void)
{
    BmsLink link;
    BmsTx tx;
    ChgCtrl c = energy();
    ChgCtrl idle;
    ChgCan ccs;
    BmsLink sif;

    reach_can_run(&link, &tx, &c);
    expect_eq("busoff in RUN", link.phase, CHG_PHASE_RUN);

    bms_link_set_bus_off(&link, 1, 50);
    expect_eq("busoff 50ms still RUN", link.phase, CHG_PHASE_RUN);
    expect_eq("busoff 50ms counter", link.line_down_ms, 50);
    expect_eq("busoff 50ms not latched", link.line_fault, 0);

    bms_link_set_bus_off(&link, 1, 50);
    expect_eq("busoff STOP", link.phase, CHG_PHASE_STOP);
    expect_eq("busoff force_stop", link.force_stop, 1);
    expect_eq("busoff link down", link.link_ok, 0);
    expect_eq("busoff latched", link.line_fault, 1);
    expect_eq("busoff seen_ready cleared", link.seen_ready, 0);

    memset(&ccs, 0, sizeof ccs);
    ccs.id = CHG_CAN_CCS;
    ccs.dlc = 8;
    ccs.data[6] = 0x01;
    bms_link_can_rx(&link, &ccs);
    expect_eq("busoff ccs ignored phase", link.phase, CHG_PHASE_STOP);
    expect_eq("busoff ccs ignored link", link.link_ok, 0);

    /* 线还断着，电流回到 0 也不许开新会话。 */
    idle = c;
    idle.allow = 0;
    idle.i_req_01a = 0;
    bms_link_tick(&link, &idle, 10, &tx);
    expect_eq("busoff held while down", link.phase, CHG_PHASE_STOP);
    expect_eq("busoff held force", link.force_stop, 1);

    bms_link_set_bus_off(&link, 0, 10);
    expect_eq("busoff recovered counter", link.line_down_ms, 0);
    expect_eq("busoff recovered latch", link.line_fault, 0);
    bms_link_tick(&link, &c, 10, &tx);
    expect_eq("busoff no resume while current", link.phase, CHG_PHASE_STOP);
    bms_link_tick(&link, &idle, 10, &tx);
    expect_eq("busoff force cleared", link.force_stop, 0);
    expect_eq("busoff stays STOP", link.phase, CHG_PHASE_STOP);
    bms_link_tick(&link, &c, 10, &tx);
    expect_eq("busoff new HS", link.phase, CHG_PHASE_HS);

    /* 错口：SIF 会话不吃 bus-off。 */
    bms_link_init(&sif, CHG_PORT_SIF);
    bms_link_set_bus_off(&sif, 1, 500);
    expect_eq("busoff wrong port", sif.phase, CHG_PHASE_OFF);
    expect_eq("busoff wrong port latch", sif.line_fault, 0);
}

/* §10 验收 15: RS-485 短路 ≥ 100 ms 立即 STOP，应答字节不再推进会话。 */
static void test_485_line_short(void)
{
    BmsLink link;
    BmsTx tx;
    ChgCtrl c = energy();
    static const uint8_t cap0[] = {0xA5, 0x5A, 0x01, 0x02, 0x09, 0x00, 0xF4, 0x01, 0xE8, 0x03,
                                   0x0A, 0x00, 0x20, 0x03, 0xE7, 0xEC};
    static const uint8_t cap1[] = {0xA5, 0x5A, 0x01, 0x02, 0x09, 0x01, 0xF4, 0x01, 0xE8, 0x03,
                                   0x0A, 0x00, 0x20, 0x03, 0xEA, 0x7C};
    /* 规范里的能量传输应答。若物理断开没挡住它，link_ok 会被重新拉高。 */
    static const uint8_t stat_ok[] = {0xA5, 0x5A, 0x01, 0x03, 0x06, 0x09, 0x44, 0x03, 0x8E, 0x01,
                                      0x00, 0x8C, 0xA7};

    bms_link_init(&link, CHG_PORT_485);
    bms_link_tick(&link, &c, 0, &tx);
    feed(&link, &c, cap0, sizeof cap0, &tx);
    bms_link_tick(&link, &c, 250, &tx);
    bms_link_tick(&link, &c, 250, &tx);
    feed(&link, &c, cap1, sizeof cap1, &tx);
    expect_eq("485 short in RUN", link.phase, CHG_PHASE_RUN);

    bms_link_set_485_short(&link, 1, 90);
    expect_eq("485 short 90ms still RUN", link.phase, CHG_PHASE_RUN);
    bms_link_set_485_short(&link, 1, 10);
    expect_eq("485 short STOP", link.phase, CHG_PHASE_STOP);
    expect_eq("485 short force", link.force_stop, 1);
    expect_eq("485 short latch", link.line_fault, 1);
    expect_eq("485 short link down", link.link_ok, 0);

    /* 锁存会清 waiting。这里重新武装，确认应答字节本身被丢掉，而不是因为没在等应答。 */
    link.waiting = 1u;
    feed(&link, &c, stat_ok, sizeof stat_ok, &tx);
    expect_eq("485 short stat ignored", link.phase, CHG_PHASE_STOP);
    expect_eq("485 short stat link", link.link_ok, 0);
}

/* §10 验收 13: aux_undervolt=1 持续 ≥ 100 ms → force_stop=1。 */
static void test_aux_undervoltage(void)
{
    BmsLink link;
    BmsTx tx;
    ChgCtrl c = energy();

    bms_link_init(&link, CHG_PORT_CAN);
    bms_link_set_aux_uv(&link, 0);
    expect_eq("aux uv: idle zero", link.aux_uv_ms, 0);

    /* 拉低 60 ms：未达阈值。tick 推进阶段 OFF→HS，但仍未触发 STOP。 */
    bms_link_set_aux_uv(&link, 1);
    bms_link_tick(&link, &c, 60, &tx);
    expect_eq("aux uv: 60ms phase", link.phase, CHG_PHASE_HS);
    expect_eq("aux uv: 60ms counter", link.aux_uv_ms, 60);
    expect_eq("aux uv: 60ms not stop", link.force_stop, 0);

    /* 再 tick 50 ms：累加到 110 ≥ 100 → STOP */
    bms_link_tick(&link, &c, 50, &tx);
    expect_eq("aux uv: STOP phase", link.phase, CHG_PHASE_STOP);
    expect_eq("aux uv: force_stop", link.force_stop, 1);
    /* chg_bms.c:574-583 STOP 入口三字段清零。 */
    expect_eq("aux uv: seen_ready cleared", link.seen_ready, 0);
    expect_eq("aux uv: sif_sent cleared", link.sif_sent, 0);
    expect_eq("aux uv: waiting cleared", link.waiting, 0);

    /* 恢复正常：累加器归零 */
    bms_link_set_aux_uv(&link, 0);
    expect_eq("aux uv: idle resets", link.aux_uv_ms, 0);
}

/* §10 验收 14: BMS 见 RS-485 0x03 stat.fault=1 或 fault_code≠0 立即进入 STOP，
 * 与 CAN CST (test_cst_immediate_stop) 对称。事件性立即 STOP，不等 T_LOSS_MS。
 * 回归用例：v23 修了 CAN CST，RS-485 0x03 对偶路径之前漏修，靠 T_LOSS_MS=1000 ms
 * 兜底，违反「立即」要求。 */
static void test_485_stop_on_fault_stat(void)
{
    BmsLink link;
    BmsTx tx;
    ChgCtrl c = energy();
    static const uint8_t cap0[] = {0xA5, 0x5A, 0x01, 0x02, 0x09, 0x00, 0xF4, 0x01, 0xE8, 0x03,
                                   0x0A, 0x00, 0x20, 0x03, 0xE7, 0xEC};
    static const uint8_t cap1[] = {0xA5, 0x5A, 0x01, 0x02, 0x09, 0x01, 0xF4, 0x01, 0xE8, 0x03,
                                   0x0A, 0x00, 0x20, 0x03, 0xEA, 0x7C};
    /* 0x03 stat with data[0]=0x02 (bit1=fault=1, bit0=outputting=0),
     * v_out=836 (0x0344), i_out=398 (0x018E), fault_code=0x05。CRC=4D DF。 */
    static const uint8_t stat_fault[] = {0xA5, 0x5A, 0x01, 0x03, 0x06, 0x02, 0x44, 0x03, 0x8E, 0x01,
                                         0x05, 0x4D, 0xDF};

    bms_link_init(&link, CHG_PORT_485);
    bms_link_tick(&link, &c, 0, &tx);
    /* HS：发 cap0；cap0 ready=0，仍 HS。 */
    feed(&link, &c, cap0, sizeof cap0, &tx);
    bms_link_tick(&link, &c, 250, &tx);
    bms_link_tick(&link, &c, 250, &tx);
    /* cap1 ready=1 → RUN。 */
    feed(&link, &c, cap1, sizeof cap1, &tx);
    expect_eq("485 fault: in RUN", link.phase, CHG_PHASE_RUN);

    /* 走一拍 tick 重新让 waiting=1，下一拍喂 stat 才能被 accept_485 接住。 */
    bms_link_tick(&link, &c, 250, &tx);
    /* 收 0x03 fault=1：立即 STOP（不等 T_LOSS_MS）。 */
    feed(&link, &c, stat_fault, sizeof stat_fault, &tx);
    expect_eq("485 fault: STOP phase", link.phase, CHG_PHASE_STOP);
    expect_eq("485 fault: force_stop", link.force_stop, 1);
    expect_eq("485 fault: fault set", link.stat.fault, 1);
    expect_eq("485 fault: fault_code", link.stat.fault_code, 0x05);
    expect_eq("485 fault: link down", link.link_ok, 0);
    expect_eq("485 fault: seen_ready cleared", link.seen_ready, 0);
    expect_eq("485 fault: sif_sent cleared", link.sif_sent, 0);
    expect_eq("485 fault: waiting cleared", link.waiting, 0);
}

/* CCS 字节 6 为 0x00 立即停止，不刷新成一次健康通信。字节 6 为 0x01 保持运行。 */
static void test_ccs_fault_byte(void)
{
    BmsLink link;
    BmsTx tx;
    ChgCtrl c = energy();
    ChgCan rx;
    uint32_t loss;

    enter_can_run(&link, &tx);
    memset(&rx, 0, sizeof rx);
    rx.id = CHG_CAN_CCS;
    rx.dlc = 8;
    rx.data[5] = 0x01;
    rx.data[6] = 0x01;
    bms_link_can_rx(&link, &rx);
    expect_eq("ccs healthy run", link.phase, CHG_PHASE_RUN);
    expect_eq("ccs healthy fault", link.stat.fault, 0);
    expect_eq("ccs byte5 out", link.stat.outputting, 1);
    bms_link_tick(&link, &c, 200, &tx);
    loss = link.loss_ms;
    expect_eq("ccs aged", loss, 200u);
    link.ccs_gap_ms = 40u;
    rx.data[6] = 0x02;
    bms_link_can_rx(&link, &rx);
    expect_eq("ccs other stays", link.phase, CHG_PHASE_RUN);
    expect_eq("ccs other no refresh", link.loss_ms, loss);
    expect_eq("ccs other gap", link.ccs_gap_ms, 40u);
    expect_eq("ccs other drop", link.rx_drop > 0u, 1u);
    rx.data[5] = 0x00;
    rx.data[6] = 0x00;
    bms_link_can_rx(&link, &rx);
    expect_eq("ccs fault stop", link.phase, CHG_PHASE_STOP);
    expect_eq("ccs fault bit", link.stat.fault, 1);
    expect_eq("ccs fault no refresh", link.loss_ms, loss);
    expect_eq("ccs fault gap", link.ccs_gap_ms, 40u);
    bms_link_tick(&link, &c, 0, &tx);
    expect_eq("ccs fault holds", tx.phase, CHG_PHASE_STOP);
}

static uint16_t crc16_test(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    size_t i;
    int b;
    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (b = 0; b < 8; b++) {
            crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0xA001u) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

/* CRO 只认 0x00/0xAA。0x02 保留位、0x03 高 4 位非 0 时不更新能力或状态。 */
static void test_defined_values(void)
{
    BmsLink link;
    BmsTx tx;
    ChgCtrl c = energy();
    ChgCan rx;
    uint8_t cap[16];
    uint8_t stat[13];
    uint16_t crc;
    uint32_t drops;

    bms_link_init(&link, CHG_PORT_CAN);
    memset(&rx, 0, sizeof rx);
    rx.id = CHG_CAN_CRO;
    rx.dlc = 1;
    rx.data[0] = 0xAA;
    bms_link_can_rx(&link, &rx);
    expect_eq("cro aa", link.cap.ready, 1);
    drops = link.rx_drop;
    rx.data[0] = 0x55;
    bms_link_can_rx(&link, &rx);
    expect_eq("cro junk ready", link.cap.ready, 1);
    expect_eq("cro junk seen", link.seen_ready, 1);
    expect_eq("cro junk drop", link.rx_drop, drops + 1u);

    bms_link_init(&link, CHG_PORT_485);
    bms_link_tick(&link, &c, 0, &tx);
    memset(cap, 0, sizeof cap);
    cap[0] = 0xA5; cap[1] = 0x5A; cap[2] = 0x01; cap[3] = 0x02; cap[4] = 0x09;
    cap[5] = 0x03; /* 就绪位 + 保留位 */
    cap[6] = 0xF4; cap[7] = 0x01; cap[8] = 0xE8; cap[9] = 0x03;
    cap[10] = 0x0A; cap[11] = 0x00; cap[12] = 0x20; cap[13] = 0x03;
    crc = crc16_test(cap, 14);
    cap[14] = (uint8_t)(crc & 0xFFu);
    cap[15] = (uint8_t)(crc >> 8);
    feed(&link, &c, cap, 16, &tx);
    expect_eq("cap reserved ignored", link.seen_ready, 0);
    expect_eq("cap reserved phase", link.phase, CHG_PHASE_HS);

    bms_link_init(&link, CHG_PORT_485);
    bms_link_tick(&link, &c, 0, &tx);
    {
        static const uint8_t cap1[] = {0xA5, 0x5A, 0x01, 0x02, 0x09, 0x01, 0xF4, 0x01, 0xE8, 0x03,
                                       0x0A, 0x00, 0x20, 0x03, 0xEA, 0x7C};
        feed(&link, &c, cap1, sizeof cap1, &tx);
    }
    expect_eq("stat setup run", link.phase, CHG_PHASE_RUN);
    bms_link_tick(&link, &c, 250, &tx);
    if (tx.frame_len == 0u) {
        bms_link_tick(&link, &c, 250, &tx);
    }
    expect_eq("stat waiting", tx.frame_len > 0u, 1u);
    memset(stat, 0, sizeof stat);
    stat[0] = 0xA5; stat[1] = 0x5A; stat[2] = 0x01; stat[3] = 0x03; stat[4] = 0x06;
    stat[5] = 0x19; /* 输出中 + 直流 + 保留 bit4 */
    stat[6] = 0x44; stat[7] = 0x03; stat[8] = 0x8E; stat[9] = 0x01; stat[10] = 0x00;
    crc = crc16_test(stat, 11);
    stat[11] = (uint8_t)(crc & 0xFFu);
    stat[12] = (uint8_t)(crc >> 8);
    {
        uint32_t loss = link.loss_ms;
        feed(&link, &c, stat, 13, &tx);
        expect_eq("stat reserved phase", link.phase, CHG_PHASE_RUN);
        expect_eq("stat reserved loss", link.loss_ms, loss);
    }
}

/* 合法 CRC 的 0x07 不改变握手。下一帧 0x02 仍被接受。 */
static void test_skip_auth(void)
{
    BmsLink link;
    BmsTx tx;
    ChgCtrl c = energy();
    uint8_t tok[20];
    uint16_t crc;
    uint8_t phase;
    uint8_t seen;
    static const uint8_t cap1[] = {0xA5, 0x5A, 0x01, 0x02, 0x09, 0x01, 0xF4, 0x01, 0xE8, 0x03,
                                   0x0A, 0x00, 0x20, 0x03, 0xEA, 0x7C};

    memset(tok, 0, sizeof tok);
    tok[0] = 0xA5;
    tok[1] = 0x5A;
    tok[2] = 0x01;
    tok[3] = 0x07;
    tok[4] = 13;
    crc = crc16_test(tok, 18);
    tok[18] = (uint8_t)(crc & 0xFFu);
    tok[19] = (uint8_t)(crc >> 8);

    bms_link_init(&link, CHG_PORT_485);
    bms_link_tick(&link, &c, 0, &tx);
    phase = link.phase;
    seen = link.seen_ready;
    feed(&link, &c, tok, sizeof tok, &tx);
    expect_eq("auth skip phase", link.phase, phase);
    expect_eq("auth skip seen", link.seen_ready, seen);
    expect_eq("auth skip raw", link.raw_n, 0);
    feed(&link, &c, cap1, sizeof cap1, &tx);
    expect_eq("auth then cap", link.phase, CHG_PHASE_RUN);
}

/* 满充停止由策略把停止和请求电流保持住。本链路在此期间只发 BST，不发 BRO。
 * 允许和电流回来后，第一拍仍是握手：BRO 为 00，电流字段为 0。
 * 停止期间先收到 CRO 为 AA，也不跳过这次握手。 */
static uint16_t test_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    size_t i;
    int b;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (b = 0; b < 8; b++) {
            if (crc & 1u) {
                crc = (uint16_t)((crc >> 1) ^ 0xA001u);
            } else {
                crc = (uint16_t)(crc >> 1);
            }
        }
    }
    return crc;
}

static void pack_stat(uint8_t out[13], uint16_t i_01a)
{
    uint16_t crc;

    out[0] = 0xA5;
    out[1] = 0x5A;
    out[2] = 0x01;
    out[3] = 0x03;
    out[4] = 0x06;
    out[5] = 0x09;
    out[6] = 0x44;
    out[7] = 0x03;
    out[8] = (uint8_t)(i_01a & 0xFFu);
    out[9] = (uint8_t)(i_01a >> 8);
    out[10] = 0x00;
    crc = test_crc16(out, 11);
    out[11] = (uint8_t)(crc & 0xFFu);
    out[12] = (uint8_t)(crc >> 8);
}

static void ccs_i(ChgCan *rx, uint16_t i_01a)
{
    uint16_t raw = (uint16_t)(i_01a + 4000u);

    memset(rx, 0, sizeof *rx);
    rx->id = CHG_CAN_CCS;
    rx->dlc = 8;
    rx->data[0] = 0x44;
    rx->data[1] = 0x03;
    rx->data[2] = (uint8_t)(raw & 0xFFu);
    rx->data[3] = (uint8_t)(raw >> 8);
    rx->data[6] = 0x01;
}

static void cml_imax(BmsLink *l, uint16_t imax)
{
    ChgCan rx;
    uint16_t raw = (uint16_t)(imax + 4000u);

    memset(&rx, 0, sizeof rx);
    rx.id = CHG_CAN_CML;
    rx.dlc = 8;
    rx.data[0] = 0x48;
    rx.data[1] = 0x03;
    rx.data[2] = 0xF4;
    rx.data[3] = 0x01;
    rx.data[4] = (uint8_t)(raw & 0xFFu);
    rx.data[5] = (uint8_t)(raw >> 8);
    rx.data[6] = 0xA0;
    rx.data[7] = 0x0F;
    bms_link_can_rx(l, &rx);
}

static void enter_can_run_cap(BmsLink *l, BmsTx *tx, ChgCtrl *c, uint16_t imax)
{
    ChgCan rx;

    bms_link_init(l, CHG_PORT_CAN);
    bms_link_tick(l, c, 0, tx);
    memset(&rx, 0, sizeof rx);
    rx.id = CHG_CAN_CRO;
    rx.dlc = 1;
    rx.data[0] = 0xAA;
    bms_link_can_rx(l, &rx);
    bms_link_tick(l, c, 0, tx);
    if (imax > 0u) {
        cml_imax(l, imax);
    }
}

/* 上报电流：近 0 卡住、零输出后的残余、高出公告最大电流的 5% 带。 */
static void test_i_report(void)
{
    BmsLink link;
    BmsTx tx;
    ChgCtrl c = energy();
    ChgCan rx;
    uint8_t stat[13];
    static const uint8_t known[] = {0xA5, 0x5A, 0x01, 0x03, 0x06, 0x09, 0x44, 0x03,
                                    0x8E, 0x01, 0x00, 0x8C, 0xA7};

    pack_stat(stat, 398);
    expect_mem("stat 398 crc", stat, known, sizeof known);

    enter_can_run_cap(&link, &tx, &c, 500);
    expect_eq("i rep run", link.phase, CHG_PHASE_RUN);
    expect_eq("i rep cap", link.cap.i_max_01a, 500);
    bms_link_tick(&link, &c, 50, &tx);
    expect_eq("stuck sent", link.i_tx_01a, 400);
    ccs_i(&rx, 0);
    bms_link_can_rx(&link, &rx);
    bms_link_tick(&link, &c, 800, &tx);
    expect_eq("stuck 800 holds", link.phase, CHG_PHASE_RUN);
    expect_eq("stuck 800 alarm", link.i_alarm, CHG_I_ALARM_NONE);
    bms_link_tick(&link, &c, 1, &tx);
    expect_eq("stuck 801 stops", link.phase, CHG_PHASE_STOP);
    expect_eq("stuck alarm", link.i_alarm, CHG_I_ALARM_STUCK);
    expect_eq("stuck force", link.force_stop, 1u);
    expect_eq("stuck link", link.link_ok, 0u);
    ccs_i(&rx, 600);
    bms_link_can_rx(&link, &rx);
    expect_eq("stuck keeps alarm", link.i_alarm, CHG_I_ALARM_STUCK);

    c.i_req_01a = 0;
    bms_link_tick(&link, &c, 0, &tx);
    c = energy();
    enter_can_run_cap(&link, &tx, &c, 500);
    c.i_req_01a = 200;
    bms_link_tick(&link, &c, 50, &tx);
    expect_eq("stuck 20a sent", link.i_tx_01a, 200);
    ccs_i(&rx, 0);
    bms_link_can_rx(&link, &rx);
    bms_link_tick(&link, &c, 500, &tx);
    expect_eq("stuck 20a floor", link.phase, CHG_PHASE_RUN);
    bms_link_tick(&link, &c, 1, &tx);
    expect_eq("stuck 20a stops", link.i_alarm, CHG_I_ALARM_STUCK);

    c = energy();
    enter_can_run_cap(&link, &tx, &c, 500);
    ccs_i(&rx, 100);
    bms_link_can_rx(&link, &rx);
    bms_link_tick(&link, &c, 1100, &tx);
    expect_eq("below req holds", link.phase, CHG_PHASE_RUN);
    expect_eq("below req alarm", link.i_alarm, CHG_I_ALARM_NONE);
    ccs_i(&rx, 0);
    bms_link_can_rx(&link, &rx);
    bms_link_tick(&link, &c, 50, &tx);
    ccs_i(&rx, 400);
    bms_link_can_rx(&link, &rx);
    bms_link_tick(&link, &c, 50, &tx);
    expect_eq("slew not fault", link.phase, CHG_PHASE_RUN);
    ccs_i(&rx, 200);
    bms_link_can_rx(&link, &rx);
    expect_eq("resume 20a holds", link.phase, CHG_PHASE_RUN);
    expect_eq("resume 20a alarm", link.i_alarm, CHG_I_ALARM_NONE);

    ccs_i(&rx, 425);
    bms_link_can_rx(&link, &rx);
    expect_eq("high limit holds", link.phase, CHG_PHASE_RUN);
    ccs_i(&rx, 426);
    bms_link_can_rx(&link, &rx);
    expect_eq("high stops", link.phase, CHG_PHASE_STOP);
    expect_eq("high alarm", link.i_alarm, CHG_I_ALARM_HIGH);
    expect_eq("high force", link.force_stop, 1u);

    c.i_req_01a = 0;
    bms_link_tick(&link, &c, 0, &tx);
    c = energy();
    c.i_req_01a = 500;
    enter_can_run_cap(&link, &tx, &c, 500);
    bms_link_tick(&link, &c, 0, &tx);
    expect_eq("over cap unsent", link.i_tx_seen, 0u);
    ccs_i(&rx, 501);
    bms_link_can_rx(&link, &rx);
    expect_eq("over cap before bcl holds", link.phase, CHG_PHASE_RUN);
    bms_link_tick(&link, &c, 50, &tx);
    expect_eq("over cap sent", link.i_tx_01a, 500);
    ccs_i(&rx, 500);
    bms_link_can_rx(&link, &rx);
    expect_eq("at cap holds", link.phase, CHG_PHASE_RUN);
    ccs_i(&rx, 501);
    bms_link_can_rx(&link, &rx);
    expect_eq("over cap stops", link.i_alarm, CHG_I_ALARM_HIGH);

    c = energy();
    enter_can_run_cap(&link, &tx, &c, 0);
    ccs_i(&rx, 0);
    bms_link_can_rx(&link, &rx);
    bms_link_tick(&link, &c, 1100, &tx);
    expect_eq("no cap stuck holds", link.phase, CHG_PHASE_RUN);
    ccs_i(&rx, 0);
    bms_link_can_rx(&link, &rx);
    bms_link_tick(&link, &c, 1100, &tx);
    expect_eq("no cap stuck after send", link.phase, CHG_PHASE_RUN);
    ccs_i(&rx, 5000);
    bms_link_can_rx(&link, &rx);
    expect_eq("no cap high holds", link.phase, CHG_PHASE_RUN);
    expect_eq("no cap alarm", link.i_alarm, CHG_I_ALARM_NONE);

    c = energy();
    enter_can_run_cap(&link, &tx, &c, 300);
    ccs_i(&rx, 0);
    bms_link_can_rx(&link, &rx);
    bms_link_tick(&link, &c, 1100, &tx);
    expect_eq("above cap window holds", link.phase, CHG_PHASE_RUN);
    ccs_i(&rx, 0);
    bms_link_can_rx(&link, &rx);
    bms_link_tick(&link, &c, 1100, &tx);
    expect_eq("above cap window after send", link.phase, CHG_PHASE_RUN);
    ccs_i(&rx, 300);
    bms_link_can_rx(&link, &rx);
    expect_eq("at announced max holds", link.phase, CHG_PHASE_RUN);
    expect_eq("at announced max alarm", link.i_alarm, CHG_I_ALARM_NONE);
    ccs_i(&rx, 400);
    bms_link_can_rx(&link, &rx);
    expect_eq("over announced max stops", link.phase, CHG_PHASE_STOP);
    expect_eq("over announced max alarm", link.i_alarm, CHG_I_ALARM_HIGH);

    c = energy();
    c.i_req_01a = 99;
    enter_can_run_cap(&link, &tx, &c, 500);
    bms_link_tick(&link, &c, 0, &tx);
    ccs_i(&rx, 0);
    bms_link_can_rx(&link, &rx);
    bms_link_tick(&link, &c, 1100, &tx);
    expect_eq("below 20pct holds", link.phase, CHG_PHASE_RUN);
    ccs_i(&rx, 124);
    bms_link_can_rx(&link, &rx);
    expect_eq("below 20pct limit holds", link.phase, CHG_PHASE_RUN);
    ccs_i(&rx, 200);
    bms_link_can_rx(&link, &rx);
    expect_eq("below 20pct high stops", link.phase, CHG_PHASE_STOP);
    expect_eq("below 20pct high alarm", link.i_alarm, CHG_I_ALARM_HIGH);

    c = energy();
    enter_can_run_cap(&link, &tx, &c, 500);
    c.pause = 1;
    /* 250 ms 让 BSM 发出。发出当拍的 39.8 A 仍是上一拍功率级。 */
    bms_link_tick(&link, &c, 250, &tx);
    expect_eq("resid armed", link.zero_armed, 1u);
    expect_eq("resid arm time", link.i_zero_cmd_ms, 0u);
    ccs_i(&rx, 398);
    bms_link_can_rx(&link, &rx);
    expect_eq("resid same beat holds", link.phase, CHG_PHASE_RUN);
    bms_link_tick(&link, &c, 199, &tx);
    ccs_i(&rx, 398);
    bms_link_can_rx(&link, &rx);
    expect_eq("resid 199 holds", link.phase, CHG_PHASE_RUN);
    expect_eq("resid 199 alarm", link.i_alarm, CHG_I_ALARM_NONE);
    bms_link_tick(&link, &c, 1, &tx);
    ccs_i(&rx, 5);
    bms_link_can_rx(&link, &rx);
    expect_eq("resid half amp holds", link.phase, CHG_PHASE_RUN);
    ccs_i(&rx, 6);
    bms_link_can_rx(&link, &rx);
    expect_eq("resid stops", link.phase, CHG_PHASE_STOP);
    expect_eq("resid alarm", link.i_alarm, CHG_I_ALARM_RESID);
    expect_eq("resid force", link.force_stop, 1u);
    c.pause = 0;
    bms_link_tick(&link, &c, 50, &tx);
    expect_eq("resid holds stop", link.phase, CHG_PHASE_STOP);
    c.i_req_01a = 0;
    bms_link_tick(&link, &c, 0, &tx);
    expect_eq("resid clear force", link.force_stop, 0u);
    c = energy();
    bms_link_tick(&link, &c, 0, &tx);
    expect_eq("resid rehs", link.phase, CHG_PHASE_HS);
    expect_eq("resid alarm cleared", link.i_alarm, CHG_I_ALARM_NONE);

    c = energy();
    enter_can_run_cap(&link, &tx, &c, 0);
    c.i_req_01a = 0;
    bms_link_tick(&link, &c, 200, &tx);
    expect_eq("cmd stop phase", link.phase, CHG_PHASE_STOP);
    expect_eq("cmd stop force", link.force_stop, 0u);
    ccs_i(&rx, 398);
    bms_link_can_rx(&link, &rx);
    expect_eq("cmd same beat holds", link.i_alarm, CHG_I_ALARM_NONE);
    bms_link_tick(&link, &c, 200, &tx);
    ccs_i(&rx, 398);
    bms_link_can_rx(&link, &rx);
    expect_eq("cmd resid reason", link.i_alarm, CHG_I_ALARM_RESID);
    expect_eq("cmd resid force stays", link.force_stop, 0u);
    expect_eq("cmd resid phase", link.phase, CHG_PHASE_STOP);

    c = energy();
    bms_link_init(&link, CHG_PORT_SIF);
    bms_link_tick(&link, &c, 0, &tx);
    bms_link_tick(&link, &c, 250, &tx);
    bms_link_tick(&link, &c, 250, &tx);
    expect_eq("sif i run", link.phase, CHG_PHASE_RUN);
    link.cap.i_max_01a = 500;
    link.i_report_seen = 1u;
    link.stat.i_out_01a = 0;
    bms_link_tick(&link, &c, 2000, &tx);
    expect_eq("sif skips i check", link.phase, CHG_PHASE_RUN);
    expect_eq("sif i alarm", link.i_alarm, CHG_I_ALARM_NONE);

    c = energy();
    bms_link_init(&link, CHG_PORT_485);
    link.phase = CHG_PHASE_RUN;
    link.cap.i_max_01a = 500;
    link.cap.ready = 1u;
    link.seen_ready = 1u;
    bms_link_tick(&link, &c, 0, &tx);
    c.pause = 1;
    bms_link_tick(&link, &c, 250, &tx);
    c.pause = 0;
    link.waiting = 1u;
    pack_stat(stat, 398);
    feed(&link, &c, stat, sizeof stat, &tx);
    expect_eq("485 unpause stale holds", link.phase, CHG_PHASE_RUN);
    expect_eq("485 unpause alarm", link.i_alarm, CHG_I_ALARM_NONE);
    expect_eq("485 unpause amps", link.stat.i_out_01a, 398);

    bms_link_tick(&link, &c, 0, &tx);
    link.bat_pending = 0u;
    link.bat_turn = 0u;
    c.pause = 1;
    bms_link_tick(&link, &c, 250, &tx);
    expect_eq("485 rearm", link.zero_armed, 1u);
    expect_eq("485 rearm time", link.i_zero_cmd_ms, 0u);
    link.waiting = 1u;
    pack_stat(stat, 398);
    feed(&link, &c, stat, sizeof stat, &tx);
    expect_eq("485 same beat holds", link.phase, CHG_PHASE_RUN);
    bms_link_tick(&link, &c, 200, &tx);
    link.waiting = 1u;
    link.cap.i_max_01a = 0;
    pack_stat(stat, 398);
    feed(&link, &c, stat, sizeof stat, &tx);
    expect_eq("485 resid stops", link.phase, CHG_PHASE_STOP);
    expect_eq("485 resid alarm", link.i_alarm, CHG_I_ALARM_RESID);
    expect_eq("485 resid force", link.force_stop, 1u);

    /* 现场：请求已从 40.0 A 降到 30.0 A，上报仍停在 40.0 A。最大 50.0 A，带 2.5 A。 */
    c = energy();
    enter_can_run_cap(&link, &tx, &c, 500);
    bms_link_tick(&link, &c, 50, &tx);
    expect_eq("stepdown sent 40", link.i_tx_01a, 400);
    c.i_req_01a = 300;
    bms_link_tick(&link, &c, 0, &tx);
    expect_eq("stepdown req", link.cmd_i_req_01a, 300);
    expect_eq("stepdown still 40", link.i_tx_01a, 400);
    ccs_i(&rx, 400);
    bms_link_can_rx(&link, &rx);
    expect_eq("stepdown unsent holds", link.phase, CHG_PHASE_RUN);
    bms_link_tick(&link, &c, 50, &tx);
    expect_eq("stepdown sent 30", link.i_tx_01a, 300);
    expect_eq("stepdown hold", link.i_high_hold, 1u);
    expect_eq("stepdown prev", link.i_tx_prev_01a, 400);
    ccs_i(&rx, 325);
    bms_link_can_rx(&link, &rx);
    expect_eq("stepdown limit holds", link.phase, CHG_PHASE_RUN);
    expect_eq("stepdown limit alarm", link.i_alarm, CHG_I_ALARM_NONE);
    expect_eq("stepdown hold spent", link.i_high_hold, 0u);
    ccs_i(&rx, 400);
    bms_link_can_rx(&link, &rx);
    expect_eq("stepdown frozen stops", link.phase, CHG_PHASE_STOP);
    expect_eq("stepdown alarm", link.i_alarm, CHG_I_ALARM_HIGH);

    /* 发出 30.0 A 之后，第一帧 CCS 仍是 40.0 A，按 42.5 A。下一帧才按 32.5 A。 */
    c = energy();
    enter_can_run_cap(&link, &tx, &c, 500);
    bms_link_tick(&link, &c, 50, &tx);
    c.i_req_01a = 300;
    bms_link_tick(&link, &c, 50, &tx);
    expect_eq("lag sent 30", link.i_tx_01a, 300);
    expect_eq("lag hold", link.i_high_hold, 1u);
    expect_eq("lag prev", link.i_tx_prev_01a, 400);
    ccs_i(&rx, 400);
    bms_link_can_rx(&link, &rx);
    expect_eq("lag first holds", link.phase, CHG_PHASE_RUN);
    expect_eq("lag first alarm", link.i_alarm, CHG_I_ALARM_NONE);
    expect_eq("lag hold spent", link.i_high_hold, 0u);
    ccs_i(&rx, 400);
    bms_link_can_rx(&link, &rx);
    expect_eq("lag second stops", link.phase, CHG_PHASE_STOP);
    expect_eq("lag second alarm", link.i_alarm, CHG_I_ALARM_HIGH);

    /* 第一帧已经高于改写前的上限 42.5 A，当拍停止。 */
    c = energy();
    enter_can_run_cap(&link, &tx, &c, 500);
    bms_link_tick(&link, &c, 50, &tx);
    c.i_req_01a = 300;
    bms_link_tick(&link, &c, 50, &tx);
    ccs_i(&rx, 426);
    bms_link_can_rx(&link, &rx);
    expect_eq("lag over old stops", link.phase, CHG_PHASE_STOP);
    expect_eq("lag over old alarm", link.i_alarm, CHG_I_ALARM_HIGH);

    /* 40.0 A 再降到 30.0 A、再降到 20.0 A，中间没有上报。第一帧仍按 40.0 A。 */
    c = energy();
    enter_can_run_cap(&link, &tx, &c, 500);
    bms_link_tick(&link, &c, 50, &tx);
    c.i_req_01a = 300;
    bms_link_tick(&link, &c, 50, &tx);
    c.i_req_01a = 200;
    bms_link_tick(&link, &c, 50, &tx);
    expect_eq("lag twice sent", link.i_tx_01a, 200);
    expect_eq("lag twice prev", link.i_tx_prev_01a, 400);
    expect_eq("lag twice hold", link.i_high_hold, 1u);
    ccs_i(&rx, 400);
    bms_link_can_rx(&link, &rx);
    expect_eq("lag twice first holds", link.phase, CHG_PHASE_RUN);
    ccs_i(&rx, 400);
    bms_link_can_rx(&link, &rx);
    expect_eq("lag twice second stops", link.phase, CHG_PHASE_STOP);
    expect_eq("lag twice alarm", link.i_alarm, CHG_I_ALARM_HIGH);

    /* 暂停的那一帧不做高出，也不消耗按改写前请求的这一次。 */
    c = energy();
    enter_can_run_cap(&link, &tx, &c, 500);
    bms_link_tick(&link, &c, 50, &tx);
    c.i_req_01a = 300;
    bms_link_tick(&link, &c, 50, &tx);
    c.pause = 1;
    bms_link_tick(&link, &c, 0, &tx);
    ccs_i(&rx, 400);
    bms_link_can_rx(&link, &rx);
    expect_eq("lag pause holds", link.phase, CHG_PHASE_RUN);
    expect_eq("lag pause keeps", link.i_high_hold, 1u);
    c.pause = 0;
    bms_link_tick(&link, &c, 0, &tx);
    ccs_i(&rx, 400);
    bms_link_can_rx(&link, &rx);
    expect_eq("lag after pause holds", link.phase, CHG_PHASE_RUN);
    expect_eq("lag after pause spent", link.i_high_hold, 0u);
    ccs_i(&rx, 400);
    bms_link_can_rx(&link, &rx);
    expect_eq("lag after pause stops", link.phase, CHG_PHASE_STOP);
    expect_eq("lag after pause alarm", link.i_alarm, CHG_I_ALARM_HIGH);

    /* RS-485：0x01 还没带上 9.9 A 时，上报 20.0 A 不停。发出后第一帧仍按 40.0 A。 */
    c = energy();
    bms_link_init(&link, CHG_PORT_485);
    link.phase = CHG_PHASE_RUN;
    link.cap.i_max_01a = 500;
    link.cap.ready = 1u;
    link.seen_ready = 1u;
    bms_link_tick(&link, &c, 0, &tx);
    expect_eq("485 sent 40", link.i_tx_01a, 400);
    c.i_req_01a = 99;
    pack_stat(stat, 200);
    feed(&link, &c, stat, sizeof stat, &tx);
    expect_eq("485 unsent holds", link.phase, CHG_PHASE_RUN);
    expect_eq("485 unsent alarm", link.i_alarm, CHG_I_ALARM_NONE);
    link.bat_pending = 0u;
    link.bat_turn = 0u;
    bms_link_tick(&link, &c, 250, &tx);
    expect_eq("485 sent 9.9", link.i_tx_01a, 99);
    expect_eq("485 grace hold", link.i_high_hold, 1u);
    expect_eq("485 grace prev", link.i_tx_prev_01a, 400);
    pack_stat(stat, 200);
    feed(&link, &c, stat, sizeof stat, &tx);
    expect_eq("485 sent grace holds", link.phase, CHG_PHASE_RUN);
    expect_eq("485 sent grace alarm", link.i_alarm, CHG_I_ALARM_NONE);
    expect_eq("485 grace spent", link.i_high_hold, 0u);
    link.waiting = 1u;
    pack_stat(stat, 200);
    feed(&link, &c, stat, sizeof stat, &tx);
    expect_eq("485 sent high stops", link.phase, CHG_PHASE_STOP);
    expect_eq("485 sent high alarm", link.i_alarm, CHG_I_ALARM_HIGH);

    /* 请求刚进入窗口、0x01 还没发出时，长拍也不按卡住停止。 */
    c = energy();
    c.i_req_01a = 99;
    bms_link_init(&link, CHG_PORT_485);
    link.phase = CHG_PHASE_RUN;
    link.cap.i_max_01a = 500;
    link.cap.ready = 1u;
    link.seen_ready = 1u;
    bms_link_tick(&link, &c, 0, &tx);
    expect_eq("485 stall sent 9.9", link.i_tx_01a, 99);
    pack_stat(stat, 0);
    feed(&link, &c, stat, sizeof stat, &tx);
    c.i_req_01a = 400;
    link.bat_pending = 0u;
    link.bat_turn = 0u;
    bms_link_tick(&link, &c, 0, &tx);
    expect_eq("485 stall still 9.9", link.i_tx_01a, 99);
    bms_link_tick(&link, &c, 1100, &tx);
    expect_eq("485 stall unsent holds", link.phase, CHG_PHASE_RUN);
    expect_eq("485 stall unsent alarm", link.i_alarm, CHG_I_ALARM_NONE);
    expect_eq("485 stall then 40", link.i_tx_01a, 400);

    /* CAN：已发 9.9 A 时改请求到 40.0 A，BCL 未发，1100 ms 不停。发出后再按 0.8 s。 */
    c = energy();
    c.i_req_01a = 99;
    enter_can_run_cap(&link, &tx, &c, 500);
    bms_link_tick(&link, &c, 50, &tx);
    expect_eq("edge sent 9.9", link.i_tx_01a, 99);
    ccs_i(&rx, 0);
    bms_link_can_rx(&link, &rx);
    c.i_req_01a = 400;
    bms_link_tick(&link, &c, 0, &tx);
    expect_eq("edge still 9.9", link.i_tx_01a, 99);
    bms_link_tick(&link, &c, 1100, &tx);
    expect_eq("edge unsent holds", link.phase, CHG_PHASE_RUN);
    expect_eq("edge unsent alarm", link.i_alarm, CHG_I_ALARM_NONE);
    ccs_i(&rx, 0);
    bms_link_can_rx(&link, &rx);
    bms_link_tick(&link, &c, 800, &tx);
    expect_eq("edge after send holds", link.phase, CHG_PHASE_RUN);
    bms_link_tick(&link, &c, 1, &tx);
    expect_eq("edge after send stops", link.i_alarm, CHG_I_ALARM_STUCK);

    /* 20% 与 100% 两端都计入。允许时间按已发请求。 */
    c = energy();
    c.i_req_01a = 100;
    enter_can_run_cap(&link, &tx, &c, 500);
    bms_link_tick(&link, &c, 50, &tx);
    ccs_i(&rx, 5);
    bms_link_can_rx(&link, &rx);
    bms_link_tick(&link, &c, 500, &tx);
    expect_eq("edge 20pct holds", link.phase, CHG_PHASE_RUN);
    bms_link_tick(&link, &c, 1, &tx);
    expect_eq("edge 20pct stops", link.i_alarm, CHG_I_ALARM_STUCK);

    c = energy();
    c.i_req_01a = 500;
    enter_can_run_cap(&link, &tx, &c, 500);
    bms_link_tick(&link, &c, 50, &tx);
    ccs_i(&rx, 0);
    bms_link_can_rx(&link, &rx);
    bms_link_tick(&link, &c, 1000, &tx);
    expect_eq("edge 100pct holds", link.phase, CHG_PHASE_RUN);
    bms_link_tick(&link, &c, 1, &tx);
    expect_eq("edge 100pct stops", link.i_alarm, CHG_I_ALARM_STUCK);

    /* 0.6 A 清掉累计。回到不超过 0.5 A 后重新计。 */
    c = energy();
    enter_can_run_cap(&link, &tx, &c, 500);
    bms_link_tick(&link, &c, 50, &tx);
    ccs_i(&rx, 0);
    bms_link_can_rx(&link, &rx);
    bms_link_tick(&link, &c, 700, &tx);
    expect_eq("reset low", link.i_low_ms, 700u);
    ccs_i(&rx, 6);
    bms_link_can_rx(&link, &rx);
    bms_link_tick(&link, &c, 800, &tx);
    expect_eq("reset above half", link.phase, CHG_PHASE_RUN);
    expect_eq("reset cleared", link.i_low_ms, 0u);
    ccs_i(&rx, 0);
    bms_link_can_rx(&link, &rx);
    bms_link_tick(&link, &c, 800, &tx);
    expect_eq("reset recount holds", link.phase, CHG_PHASE_RUN);
    bms_link_tick(&link, &c, 1, &tx);
    expect_eq("reset recount stops", link.i_alarm, CHG_I_ALARM_STUCK);

    /* 已发请求离开窗口后清累计。 */
    c = energy();
    enter_can_run_cap(&link, &tx, &c, 500);
    bms_link_tick(&link, &c, 50, &tx);
    ccs_i(&rx, 0);
    bms_link_can_rx(&link, &rx);
    bms_link_tick(&link, &c, 700, &tx);
    c.i_req_01a = 99;
    bms_link_tick(&link, &c, 50, &tx);
    expect_eq("leave still counting", link.i_low_ms, 750u);
    bms_link_tick(&link, &c, 50, &tx);
    expect_eq("leave cleared", link.i_low_ms, 0u);
    expect_eq("leave holds", link.phase, CHG_PHASE_RUN);

    /* 没有新帧时，残余计时本身不停止。 */
    c = energy();
    enter_can_run_cap(&link, &tx, &c, 500);
    c.pause = 1;
    bms_link_tick(&link, &c, 250, &tx);
    expect_eq("resid quiet armed", link.zero_armed, 1u);
    bms_link_tick(&link, &c, 900, &tx);
    expect_eq("resid quiet holds", link.phase, CHG_PHASE_RUN);
    expect_eq("resid quiet alarm", link.i_alarm, CHG_I_ALARM_NONE);
    expect_eq("resid quiet time", link.i_zero_cmd_ms >= 200u, 1u);

    /* 暂停后不做高出。426 高于 40.0 A 的上限 42.5 A。 */
    c = energy();
    enter_can_run_cap(&link, &tx, &c, 500);
    bms_link_tick(&link, &c, 50, &tx);
    c.pause = 1;
    bms_link_tick(&link, &c, 0, &tx);
    ccs_i(&rx, 426);
    bms_link_can_rx(&link, &rx);
    expect_eq("pause skips high", link.phase, CHG_PHASE_RUN);
    expect_eq("pause skips alarm", link.i_alarm, CHG_I_ALARM_NONE);

    /* 公告最大电流不足 2.0 A 时带为 0。等于请求不停，多 0.1 A 停止。 */
    c = energy();
    c.i_req_01a = 10;
    enter_can_run_cap(&link, &tx, &c, 19);
    bms_link_tick(&link, &c, 50, &tx);
    expect_eq("band0 sent", link.i_tx_01a, 10);
    ccs_i(&rx, 10);
    bms_link_can_rx(&link, &rx);
    expect_eq("band0 equal holds", link.phase, CHG_PHASE_RUN);
    ccs_i(&rx, 11);
    bms_link_can_rx(&link, &rx);
    expect_eq("band0 over stops", link.phase, CHG_PHASE_STOP);
    expect_eq("band0 over alarm", link.i_alarm, CHG_I_ALARM_HIGH);

    c.i_req_01a = 0;
    bms_link_tick(&link, &c, 0, &tx);
    expect_eq("band0 clear force", link.force_stop, 0u);
    c = energy();
    bms_link_tick(&link, &c, 0, &tx);
    expect_eq("band0 rehs", link.phase, CHG_PHASE_HS);
    expect_eq("band0 tx cleared", link.i_tx_seen, 0u);
    memset(&rx, 0, sizeof rx);
    rx.id = CHG_CAN_CRO;
    rx.dlc = 1;
    rx.data[0] = 0xAA;
    bms_link_can_rx(&link, &rx);
    bms_link_tick(&link, &c, 0, &tx);
    expect_eq("band0 rerun unsent", link.i_tx_seen, 0u);
    ccs_i(&rx, 600);
    bms_link_can_rx(&link, &rx);
    expect_eq("band0 rerun holds", link.phase, CHG_PHASE_RUN);

    /* RS-485 上限取等号不停，多 0.1 A 停止。最大 50.0 A、请求 9.9 A，上限 12.4 A。 */
    c = energy();
    bms_link_init(&link, CHG_PORT_485);
    link.phase = CHG_PHASE_RUN;
    link.cap.i_max_01a = 500;
    link.cap.ready = 1u;
    link.seen_ready = 1u;
    bms_link_tick(&link, &c, 0, &tx);
    c.i_req_01a = 99;
    link.bat_pending = 0u;
    link.bat_turn = 0u;
    link.waiting = 0u;
    bms_link_tick(&link, &c, 250, &tx);
    expect_eq("485 limit sent", link.i_tx_01a, 99);
    pack_stat(stat, 124);
    feed(&link, &c, stat, sizeof stat, &tx);
    expect_eq("485 limit holds", link.phase, CHG_PHASE_RUN);
    expect_eq("485 limit alarm", link.i_alarm, CHG_I_ALARM_NONE);
    link.waiting = 1u;
    pack_stat(stat, 125);
    feed(&link, &c, stat, sizeof stat, &tx);
    expect_eq("485 limit stops", link.phase, CHG_PHASE_STOP);
    expect_eq("485 limit alarm high", link.i_alarm, CHG_I_ALARM_HIGH);
}

static void test_full_stop_rehandshake(void)
{
    BmsLink link;
    BmsTx tx;
    ChgCtrl c = energy();
    ChgCan rx;
    int i;
    int bcl;
    int bro;

    bms_link_init(&link, CHG_PORT_CAN);
    bms_link_tick(&link, &c, 0, &tx);
    memset(&rx, 0, sizeof rx);
    rx.id = CHG_CAN_CRO;
    rx.dlc = 1;
    rx.data[0] = 0xAA;
    bms_link_can_rx(&link, &rx);
    bms_link_tick(&link, &c, 250, &tx);
    expect_eq("full hold starts in run", tx.phase, CHG_PHASE_RUN);

    c.allow = 0;
    c.stop = 1;
    c.i_req_01a = 0;
    bms_link_tick(&link, &c, 10, &tx);
    expect_eq("full hold stop", tx.phase, CHG_PHASE_STOP);
    expect_eq("full hold bst", find_can(&tx, CHG_CAN_BST) >= 0, 1);
    expect_eq("full hold no bro", find_can(&tx, CHG_CAN_BRO) < 0, 1);
    expect_eq("full hold no bcl", find_can(&tx, CHG_CAN_BCL) < 0, 1);

    bms_link_can_rx(&link, &rx);
    for (i = 0; i < 5; i++) {
        bms_link_tick(&link, &c, 10, &tx);
        expect_eq("full hold stays stop", tx.phase, CHG_PHASE_STOP);
        expect_eq("full hold still no bro", find_can(&tx, CHG_CAN_BRO) < 0, 1);
        expect_eq("full hold still no bcl", find_can(&tx, CHG_CAN_BCL) < 0, 1);
    }

    c = energy();
    bms_link_tick(&link, &c, 250, &tx);
    bcl = find_can(&tx, CHG_CAN_BCL);
    bro = find_can(&tx, CHG_CAN_BRO);
    expect_eq("full release first is hs", tx.phase, CHG_PHASE_HS);
    expect_eq("full release has bcl", bcl >= 0, 1);
    expect_eq("full release has bro", bro >= 0, 1);
    if (bcl >= 0 && bro >= 0) {
        expect_eq("full release current 0", tx.can[bcl].data[2], 0xA0);
        expect_eq("full release bro 00", tx.can[bro].data[0], 0x00);
    }

    bms_link_can_rx(&link, &rx);
    bms_link_tick(&link, &c, 250, &tx);
    bcl = find_can(&tx, CHG_CAN_BCL);
    bro = find_can(&tx, CHG_CAN_BRO);
    expect_eq("full release then run", tx.phase, CHG_PHASE_RUN);
    expect_eq("full release run bcl", bcl >= 0, 1);
    expect_eq("full release run bro", bro >= 0, 1);
    if (bcl >= 0 && bro >= 0) {
        expect_mem("full release run current", tx.can[bcl].data,
                   (const uint8_t *)"\x48\x03\x30\x11\x02", 5);
        expect_eq("full release run bro aa", tx.can[bro].data[0], 0xAA);
    }
}

/* 第 5.5 节还没单列的边界：未收到电流帧、请求改大但未发、允许时间公式、
 * 带从 0 变成 0.1 A、公告最大电流中途缩小、故障帧与非法字节不改判电流、
 * 最后一帧仍有电流时按静默停止、0x02 不是电流样本、请求 0 的残余只记原因。 */
static void test_i_report_more(void)
{
    BmsLink link;
    BmsTx tx;
    ChgCtrl c = energy();
    ChgCan rx;
    uint8_t stat[13];
    static const uint8_t cap1[] = {
        0xA5, 0x5A, 0x01, 0x02, 0x09, 0x01, 0xF4, 0x01, 0xE8, 0x03,
        0x0A, 0x00, 0x20, 0x03, 0xEA, 0x7C};

    enter_can_run_cap(&link, &tx, &c, 500);
    bms_link_tick(&link, &c, 50, &tx);
    bms_link_tick(&link, &c, 1100, &tx);
    expect_eq("no sample holds", link.phase, CHG_PHASE_RUN);
    expect_eq("no sample alarm", link.i_alarm, CHG_I_ALARM_NONE);
    expect_eq("no sample seen", link.i_report_seen, 0u);
    expect_eq("no sample low", link.i_low_ms, 0u);

    c = energy();
    c.i_req_01a = 100;
    enter_can_run_cap(&link, &tx, &c, 500);
    bms_link_tick(&link, &c, 50, &tx);
    expect_eq("raise sent 10", link.i_tx_01a, 100);
    c.i_req_01a = 400;
    bms_link_tick(&link, &c, 0, &tx);
    expect_eq("raise still 10", link.i_tx_01a, 100);
    ccs_i(&rx, 125);
    bms_link_can_rx(&link, &rx);
    expect_eq("raise old limit holds", link.phase, CHG_PHASE_RUN);
    ccs_i(&rx, 126);
    bms_link_can_rx(&link, &rx);
    expect_eq("raise old limit stops", link.phase, CHG_PHASE_STOP);
    expect_eq("raise old limit alarm", link.i_alarm, CHG_I_ALARM_HIGH);
    ccs_i(&rx, 0);
    bms_link_can_rx(&link, &rx);
    expect_eq("high alarm sticks", link.i_alarm, CHG_I_ALARM_HIGH);
    expect_eq("high alarm force", link.force_stop, 1u);

    c = energy();
    c.i_req_01a = 300;
    enter_can_run_cap(&link, &tx, &c, 500);
    bms_link_tick(&link, &c, 50, &tx);
    ccs_i(&rx, 0);
    bms_link_can_rx(&link, &rx);
    bms_link_tick(&link, &c, 600, &tx);
    expect_eq("ramp 30a holds", link.phase, CHG_PHASE_RUN);
    bms_link_tick(&link, &c, 1, &tx);
    expect_eq("ramp 30a stops", link.i_alarm, CHG_I_ALARM_STUCK);
    ccs_i(&rx, 600);
    bms_link_can_rx(&link, &rx);
    expect_eq("stuck alarm sticks", link.i_alarm, CHG_I_ALARM_STUCK);

    c = energy();
    c.i_req_01a = 10;
    enter_can_run_cap(&link, &tx, &c, 20);
    bms_link_tick(&link, &c, 50, &tx);
    ccs_i(&rx, 11);
    bms_link_can_rx(&link, &rx);
    expect_eq("band1 equal holds", link.phase, CHG_PHASE_RUN);
    ccs_i(&rx, 12);
    bms_link_can_rx(&link, &rx);
    expect_eq("band1 over stops", link.i_alarm, CHG_I_ALARM_HIGH);

    c = energy();
    c.i_req_01a = 20;
    enter_can_run_cap(&link, &tx, &c, 20);
    bms_link_tick(&link, &c, 50, &tx);
    ccs_i(&rx, 20);
    bms_link_can_rx(&link, &rx);
    expect_eq("band1 at max holds", link.phase, CHG_PHASE_RUN);
    ccs_i(&rx, 21);
    bms_link_can_rx(&link, &rx);
    expect_eq("band1 at max stops", link.i_alarm, CHG_I_ALARM_HIGH);

    c = energy();
    enter_can_run_cap(&link, &tx, &c, 500);
    bms_link_tick(&link, &c, 50, &tx);
    ccs_i(&rx, 425);
    bms_link_can_rx(&link, &rx);
    expect_eq("cml before holds", link.phase, CHG_PHASE_RUN);
    cml_imax(&link, 300);
    ccs_i(&rx, 300);
    bms_link_can_rx(&link, &rx);
    expect_eq("cml shrink holds", link.phase, CHG_PHASE_RUN);
    ccs_i(&rx, 301);
    bms_link_can_rx(&link, &rx);
    expect_eq("cml shrink stops", link.phase, CHG_PHASE_STOP);
    expect_eq("cml shrink alarm", link.i_alarm, CHG_I_ALARM_HIGH);

    c = energy();
    enter_can_run_cap(&link, &tx, &c, 500);
    bms_link_tick(&link, &c, 50, &tx);
    ccs_i(&rx, 600);
    rx.data[6] = 0x00;
    bms_link_can_rx(&link, &rx);
    expect_eq("fault not high", link.phase, CHG_PHASE_STOP);
    expect_eq("fault not high alarm", link.i_alarm, CHG_I_ALARM_NONE);
    expect_eq("fault not high force", link.force_stop, 1u);

    c = energy();
    enter_can_run_cap(&link, &tx, &c, 500);
    bms_link_tick(&link, &c, 50, &tx);
    ccs_i(&rx, 600);
    rx.data[6] = 0x02;
    bms_link_can_rx(&link, &rx);
    expect_eq("bad byte6 holds", link.phase, CHG_PHASE_RUN);
    expect_eq("bad byte6 seen", link.i_report_seen, 0u);
    bms_link_tick(&link, &c, 1100, &tx);
    expect_eq("bad byte6 still", link.phase, CHG_PHASE_RUN);
    expect_eq("bad byte6 alarm", link.i_alarm, CHG_I_ALARM_NONE);

    c = energy();
    enter_can_run_cap(&link, &tx, &c, 500);
    bms_link_tick(&link, &c, 50, &tx);
    ccs_i(&rx, 400);
    bms_link_can_rx(&link, &rx);
    bms_link_tick(&link, &c, 1000, &tx);
    expect_eq("silence 1000 holds", link.phase, CHG_PHASE_RUN);
    expect_eq("silence 1000 low", link.i_low_ms, 0u);
    expect_eq("silence 1000 alarm", link.i_alarm, CHG_I_ALARM_NONE);
    bms_link_tick(&link, &c, 200, &tx);
    expect_eq("silence 1200 stops", link.phase, CHG_PHASE_STOP);
    expect_eq("silence 1200 alarm", link.i_alarm, CHG_I_ALARM_NONE);
    expect_eq("silence 1200 force", link.force_stop, 1u);

    c = energy();
    c.i_req_01a = 4;
    enter_can_run_cap(&link, &tx, &c, 23);
    bms_link_tick(&link, &c, 50, &tx);
    ccs_i(&rx, 0);
    bms_link_can_rx(&link, &rx);
    bms_link_tick(&link, &c, 500, &tx);
    expect_eq("win inside holds", link.phase, CHG_PHASE_RUN);
    bms_link_tick(&link, &c, 1, &tx);
    expect_eq("win inside stops", link.i_alarm, CHG_I_ALARM_STUCK);

    c = energy();
    c.i_req_01a = 3;
    enter_can_run_cap(&link, &tx, &c, 23);
    bms_link_tick(&link, &c, 50, &tx);
    ccs_i(&rx, 0);
    bms_link_can_rx(&link, &rx);
    bms_link_tick(&link, &c, 1100, &tx);
    expect_eq("win outside holds", link.phase, CHG_PHASE_RUN);
    expect_eq("win outside alarm", link.i_alarm, CHG_I_ALARM_NONE);

    c = energy();
    bms_link_init(&link, CHG_PORT_485);
    link.phase = CHG_PHASE_RUN;
    link.cap.i_max_01a = 500;
    link.cap.ready = 1u;
    link.seen_ready = 1u;
    bms_link_tick(&link, &c, 0, &tx);
    feed(&link, &c, cap1, sizeof cap1, &tx);
    expect_eq("485 cap not sample", link.i_report_seen, 0u);
    expect_eq("485 cap holds", link.phase, CHG_PHASE_RUN);
    bms_link_tick(&link, &c, 1100, &tx);
    expect_eq("485 cap no stall", link.phase, CHG_PHASE_RUN);
    expect_eq("485 cap alarm", link.i_alarm, CHG_I_ALARM_NONE);

    c.i_req_01a = 0;
    link.bat_pending = 0u;
    link.bat_turn = 0u;
    link.waiting = 0u;
    bms_link_tick(&link, &c, 250, &tx);
    expect_eq("485 zero phase", link.phase, CHG_PHASE_STOP);
    expect_eq("485 zero force", link.force_stop, 0u);
    expect_eq("485 zero armed", link.zero_armed, 1u);
    pack_stat(stat, 398);
    feed(&link, &c, stat, sizeof stat, &tx);
    expect_eq("485 zero same beat", link.i_alarm, CHG_I_ALARM_NONE);
    bms_link_tick(&link, &c, 200, &tx);
    link.waiting = 1u;
    pack_stat(stat, 398);
    feed(&link, &c, stat, sizeof stat, &tx);
    expect_eq("485 zero resid", link.i_alarm, CHG_I_ALARM_RESID);
    expect_eq("485 zero force stays", link.force_stop, 0u);
    expect_eq("485 zero still stop", link.phase, CHG_PHASE_STOP);

    /* 离开窗口清掉的累计不得带进下一次进入。恰好 0.5 A 重新计。 */
    c = energy();
    enter_can_run_cap(&link, &tx, &c, 500);
    bms_link_tick(&link, &c, 50, &tx);
    ccs_i(&rx, 0);
    bms_link_can_rx(&link, &rx);
    bms_link_tick(&link, &c, 700, &tx);
    expect_eq("reenter low", link.i_low_ms, 700u);
    c.i_req_01a = 99;
    bms_link_tick(&link, &c, 50, &tx);
    expect_eq("reenter old still", link.i_low_ms, 750u);
    bms_link_tick(&link, &c, 50, &tx);
    expect_eq("reenter cleared", link.i_low_ms, 0u);
    c.i_req_01a = 400;
    bms_link_tick(&link, &c, 50, &tx);
    expect_eq("reenter sent 40", link.i_tx_01a, 400);
    expect_eq("reenter still zero", link.i_low_ms, 0u);
    ccs_i(&rx, 5);
    bms_link_can_rx(&link, &rx);
    bms_link_tick(&link, &c, 800, &tx);
    expect_eq("reenter 800 holds", link.phase, CHG_PHASE_RUN);
    expect_eq("reenter 800 alarm", link.i_alarm, CHG_I_ALARM_NONE);
    expect_eq("reenter 800 low", link.i_low_ms, 800u);
    bms_link_tick(&link, &c, 1, &tx);
    expect_eq("reenter 801 stops", link.phase, CHG_PHASE_STOP);
    expect_eq("reenter 801 alarm", link.i_alarm, CHG_I_ALARM_STUCK);
}

int main(void)
{
    test_sif();
    test_can();
    test_can_cml();
    test_485();
    test_bat();
    test_edges();
    test_can_ignore();
    test_can_drop_short_dlc();
    test_cst_immediate_stop();
    test_485_stop_no_resend();
    test_sif_short_circuit();
    test_bus_off();
    test_485_line_short();
    test_aux_undervoltage();
    test_485_stop_on_fault_stat();
    test_period_and_address();
    test_ccs_fault_byte();
    test_defined_values();
    test_skip_auth();
    test_bsm_flags();
    test_bms_null_guards();
    test_raw_guards();
    test_i_report();
    test_i_report_more();
    test_full_stop_rehandshake();
    if (g_fail) {
        return 1;
    }
    printf("ok\n");
    return 0;
}
