/* 作者：苏勃(subo)*/
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
        fprintf(stderr, "FAIL %s\n got", name);
        for (i = 0; i < n; i++) {
            fprintf(stderr, " %02X", got[i]);
        }
        fprintf(stderr, "\nwant");
        for (i = 0; i < n; i++) {
            fprintf(stderr, " %02X", want[i]);
        }
        fprintf(stderr, "\n");
        g_fail = 1;
    }
}

static ChgCap limits(void)
{
    ChgCap c;
    memset(&c, 0, sizeof c);
    c.v_min_01v = 500;
    c.v_max_01v = 1000;
    c.i_min_01a = 10;
    c.i_max_01a = 800;
    return c;
}

static ChgStat meas(void)
{
    ChgStat s;
    memset(&s, 0, sizeof s);
    s.outputting = 1;
    s.dc_path = 1;
    s.v_out_01v = 836;
    s.i_out_01a = 398;
    return s;
}

static void feed(ChrLink *l, const ChgStat *s, const uint8_t *frame, size_t n, ChrTx *tx)
{
    size_t i;
    for (i = 0; i < n; i++) {
        chr_link_rx_byte(l, frame[i], 1, s, tx);
    }
}

static int find_can(const ChrTx *tx, uint32_t id)
{
    uint8_t i;
    for (i = 0; i < tx->can_n; i++) {
        if (tx->can[i].id == id) {
            return (int)i;
        }
    }
    return -1;
}

static void test_sif(void)
{
    ChrLink link;
    ChrTx tx;
    ChgCap cap = limits();
    ChgStat st = meas();
    static const uint8_t run[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x01, 0x48, 0x03, 0x90, 0x01, 0x00, 0x7C, 0x31};
    static const uint8_t pause[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x03, 0x48, 0x03, 0x90, 0x01, 0x00, 0x7D, 0xD3};
    static const uint8_t stop[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x04, 0x48, 0x03, 0x00, 0x00, 0x00, 0x7D, 0xD9};
    static const uint8_t other[] = {0xA5, 0x5A, 0x01, 0x03, 0x06, 0x09, 0x44, 0x03, 0x8E, 0x01, 0x00, 0x8C, 0xA7};

    chr_link_init(&link, CHG_PORT_SIF, &cap);
    expect_eq("sif init off", link.phase, CHG_PHASE_OFF);
    chr_link_tick(&link, &st, 0, &tx);
    expect_eq("sif first tick hs", link.phase, CHG_PHASE_HS);
    expect_eq("sif never tx", tx.frame_len, 0);
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("sif 1", tx.act.enable, 0);
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("sif 2", tx.act.enable, 0);
    feed(&link, &st, other, sizeof other, &tx);
    expect_eq("sif ignores status", link.sif_got, 2);
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("sif 3 enable", tx.act.enable, 1);
    expect_eq("sif 3 amps", tx.act.i_out_01a, 400);
    expect_eq("sif still silent", tx.frame_len, 0);

    feed(&link, &st, pause, sizeof pause, &tx);
    expect_eq("sif pause", tx.act.enable, 0);
    expect_eq("sif pause keeps", tx.phase, CHG_PHASE_RUN);
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("sif resume", tx.act.enable, 1);
    expect_eq("sif resume inrush", tx.act.i_out_01a, 200);
    expect_eq("sif resume precharge v", tx.act.v_target_01v, 830);
    expect_eq("sif resume armed", link.resume_arm, 1u);
    chr_link_tick(&link, &st, 0, &tx);
    expect_eq("sif resume tick", tx.act.i_out_01a, 200);
    expect_eq("sif resume tick v", tx.act.v_target_01v, 830);
    expect_eq("sif resume disarmed", link.resume_arm, 0u);
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("sif resume full", tx.act.i_out_01a, 400);
    expect_eq("sif resume full v", tx.act.v_target_01v, 840);

    feed(&link, &st, stop, sizeof stop, &tx);
    expect_eq("sif stop", tx.act.enable, 0);
    expect_eq("sif stop phase", tx.phase, CHG_PHASE_STOP);
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("sif rehs 1", tx.act.enable, 0);
    feed(&link, &st, run, sizeof run, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("sif rehs 3", tx.act.enable, 1);

    chr_link_tick(&link, &st, 1000, &tx);
    expect_eq("sif loss not yet", tx.phase, CHG_PHASE_RUN);
    chr_link_tick(&link, &st, 200, &tx);
    expect_eq("sif loss", tx.phase, CHG_PHASE_STOP);
    expect_eq("sif loss amps", tx.act.enable, 0);
}

static void test_can(void)
{
    ChrLink link;
    ChrTx tx;
    ChgCap cap = limits();
    ChgStat st = meas();
    ChgCan f;
    int cml;
    int cro;

    chr_link_init(&link, CHG_PORT_CAN, &cap);
    expect_eq("can init off", link.phase, CHG_PHASE_OFF);
    chr_link_tick(&link, &st, 0, &tx);
    expect_eq("can first tick hs", link.phase, CHG_PHASE_HS);
    cml = find_can(&tx, CHG_CAN_CML);
    cro = find_can(&tx, CHG_CAN_CRO);
    expect_eq("cml", cml >= 0, 1);
    expect_mem("cml data", tx.can[cml].data, (const uint8_t *)"\xE8\x03\xF4\x01\xC0\x12\xAA\x0F", 8);
    expect_eq("cro not ready", tx.can[cro].data[0], 0x00);
    expect_eq("can silent out", tx.act.enable, 0);

    memset(&f, 0, sizeof f);
    f.id = CHG_CAN_BRO;
    f.dlc = 1;
    f.data[0] = 0x00;
    chr_link_can_rx(&link, &f, &st, &tx);
    chr_link_tick(&link, &st, 250, &tx);
    cro = find_can(&tx, CHG_CAN_CRO);
    expect_eq("cro ready", tx.can[cro].data[0], 0xAA);

    f.id = CHG_CAN_BCL;
    f.dlc = 5;
    memcpy(f.data, "\x48\x03\x30\x11\x02", 5);
    chr_link_can_rx(&link, &f, &st, &tx);
    f.id = CHG_CAN_BRO;
    f.dlc = 1;
    f.data[0] = 0xAA;
    chr_link_can_rx(&link, &f, &st, &tx);
    f.id = CHG_CAN_BSM;
    f.dlc = 7;
    memset(f.data, 0, sizeof f.data);
    f.data[6] = 0x10;
    chr_link_can_rx(&link, &f, &st, &tx);
    expect_eq("can run", tx.phase, CHG_PHASE_RUN);
    expect_eq("can amps", tx.act.i_out_01a, 400);
    expect_eq("can volts", tx.act.v_target_01v, 840);
    expect_eq("can enable on", tx.act.enable, 1);

    f.id = CHG_CAN_BCL;
    f.dlc = 5;
    memcpy(f.data, "\x48\x03\xA0\x0F\x02", 5);
    chr_link_can_rx(&link, &f, &st, &tx);
    expect_eq("bcl zero not stop", tx.phase, CHG_PHASE_RUN);
    expect_eq("bcl zero amps", tx.act.enable, 0);
    memcpy(f.data, "\x48\x03\x30\x11\x02", 5);
    chr_link_can_rx(&link, &f, &st, &tx);
    expect_eq("bcl restored", tx.act.enable, 1);

    f.id = CHG_CAN_BSM;
    f.dlc = 7;
    memset(f.data, 0, sizeof f.data);
    chr_link_can_rx(&link, &f, &st, &tx);
    expect_eq("bsm pause", tx.act.enable, 0);
    expect_eq("bsm keeps", tx.phase, CHG_PHASE_RUN);
    f.data[6] = 0x20; /* bit5 保留。bit4 仍为 0，继续暂停 */
    chr_link_can_rx(&link, &f, &st, &tx);
    expect_eq("bsm bit5 still pause", tx.act.enable, 0);
    expect_eq("bsm bit5 keeps", tx.phase, CHG_PHASE_RUN);
    expect_eq("bsm bit5 no resume", link.resume_arm, 0u);
    f.data[6] = 0x10;
    chr_link_can_rx(&link, &f, &st, &tx);
    expect_eq("bsm clear", tx.act.enable, 1);
    expect_eq("bsm resume inrush", tx.act.i_out_01a, 200);
    expect_eq("bsm resume v", tx.act.v_target_01v, 830);
    expect_eq("bsm resume armed", link.resume_arm, 1u);
    chr_link_tick(&link, &st, 0, &tx);
    expect_eq("bsm resume tick", tx.act.i_out_01a, 200);
    expect_eq("bsm resume disarmed", link.resume_arm, 0u);
    chr_link_tick(&link, &st, 0, &tx);
    expect_eq("bsm resume full", tx.act.i_out_01a, 400);
    expect_eq("bsm resume full v", tx.act.v_target_01v, 840);

    f.id = CHG_CAN_BST;
    f.dlc = 4;
    f.data[0] = 0x01;
    chr_link_can_rx(&link, &f, &st, &tx);
    expect_eq("bst stop", tx.phase, CHG_PHASE_STOP);
    chr_link_tick(&link, &st, 0, &tx);
    cro = find_can(&tx, CHG_CAN_CRO);
    expect_eq("bst rehs cro", tx.can[cro].data[0], 0x00);
    f.id = CHG_CAN_BCL;
    f.dlc = 5;
    memcpy(f.data, "\x48\x03\x30\x11\x02", 5);
    chr_link_can_rx(&link, &f, &st, &tx);
    f.id = CHG_CAN_BRO;
    f.dlc = 1;
    f.data[0] = 0xAA;
    chr_link_can_rx(&link, &f, &st, &tx);
    expect_eq("no bare resume", tx.act.enable, 0);

    st.fault = 1;
    chr_link_tick(&link, &st, 0, &tx);
    expect_eq("fault cst", find_can(&tx, CHG_CAN_CST) >= 0, 1);
    expect_eq("fault amps", tx.act.enable, 0);

    st.fault = 0;
    st.ac_path = 1;
    chr_link_tick(&link, &st, 250, &tx);
    expect_eq("two paths", tx.act.enable, 0);
}

static void test_period_and_address(void)
{
    ChrLink link;
    expect_eq("loss nom not tripped", chg_loss_tripped(CHG_T_LOSS_NOM_MS), 0);
    expect_eq("loss limit tripped", chg_loss_tripped(CHG_T_LOSS_MS), 1);
    ChrTx tx;
    ChgCap cap = limits();
    ChgStat st = meas();
    ChgCan f;
    int i;
    uint32_t loss;

    chr_link_init(&link, CHG_PORT_CAN, &cap);
    chr_link_tick(&link, &st, 0, &tx);
    memset(&f, 0, sizeof f);
    f.id = CHG_CAN_BRO;
    f.dlc = 1;
    f.data[0] = 0x00;
    chr_link_can_rx(&link, &f, &st, &tx);
    f.id = CHG_CAN_BCL;
    f.dlc = 5;
    memcpy(f.data, "\x48\x03\x30\x11\x02", 5);
    chr_link_can_rx(&link, &f, &st, &tx);
    f.id = CHG_CAN_BRO;
    f.dlc = 1;
    f.data[0] = 0xAA;
    chr_link_can_rx(&link, &f, &st, &tx);
    expect_eq("bcl period run", link.phase, CHG_PHASE_RUN);

    for (i = 0; i < 30; i++) {
        chr_link_tick(&link, &st, 55, &tx);
        f.id = CHG_CAN_BCL;
        f.dlc = 5;
        memcpy(f.data, "\x48\x03\x30\x11\x02", 5);
        chr_link_can_rx(&link, &f, &st, &tx);
    }
    expect_eq("bcl 55 stays", link.phase, CHG_PHASE_RUN);
    expect_eq("bcl 55 not late", link.period_late, 0);
    expect_eq("bcl 55 amps", tx.act.i_out_01a, 400);

    chr_link_tick(&link, &st, 56, &tx);
    expect_eq("bcl 56 late", link.period_late, 1);
    expect_eq("bcl 56 not stop", link.phase, CHG_PHASE_RUN);

    loss = link.loss_ms;
    f.id = CHG_CAN_BCL ^ 0x04000000u;
    chr_link_can_rx(&link, &f, &st, &tx);
    f.id = CHG_CAN_BCL ^ 0x01u;
    chr_link_can_rx(&link, &f, &st, &tx);
    f.id = CHG_CAN_BCL ^ 0x0100u;
    chr_link_can_rx(&link, &f, &st, &tx);
    expect_eq("bcl bad id drop", link.rx_drop, 3u);
    expect_eq("bcl bad id no refresh", link.loss_ms, loss);
    expect_eq("bcl bad id still run", link.phase, CHG_PHASE_RUN);
    chr_link_tick(&link, &st, CHG_T_LOSS_MS - loss, &tx);
    expect_eq("bcl silence rehs", link.phase, CHG_PHASE_HS);
}

/* 能量传输里只有 BCL 刷新 T_loss。BRO / BSM 继续到达也不能保住输出。 */
static void test_loss_source(void)
{
    ChrLink link;
    ChrTx tx;
    ChgCap cap = limits();
    ChgStat st = meas();
    ChgCan f;

    chr_link_init(&link, CHG_PORT_CAN, &cap);
    chr_link_tick(&link, &st, 0, &tx);
    memset(&f, 0, sizeof f);
    f.id = CHG_CAN_BRO;
    f.dlc = 1;
    f.data[0] = 0x00;
    chr_link_can_rx(&link, &f, &st, &tx);
    f.id = CHG_CAN_BCL;
    f.dlc = 5;
    memcpy(f.data, "\x48\x03\x30\x11\x02", 5);
    chr_link_can_rx(&link, &f, &st, &tx);
    f.id = CHG_CAN_BRO;
    f.data[0] = 0xAA;
    chr_link_can_rx(&link, &f, &st, &tx);
    f.id = CHG_CAN_BSM;
    f.dlc = 7;
    memset(f.data, 0, sizeof f.data);
    f.data[6] = 0x10;
    chr_link_can_rx(&link, &f, &st, &tx);
    expect_eq("loss src run", link.phase, CHG_PHASE_RUN);

    chr_link_tick(&link, &st, 400, &tx);
    expect_eq("loss src 400", link.loss_ms, 400u);
    f.id = CHG_CAN_BSM;
    f.data[6] = 0x10;
    chr_link_can_rx(&link, &f, &st, &tx);
    f.id = CHG_CAN_BRO;
    f.dlc = 1;
    f.data[0] = 0xAA;
    chr_link_can_rx(&link, &f, &st, &tx);
    expect_eq("bro bsm keep loss", link.loss_ms, 400u);
    expect_eq("bro bsm still run", link.phase, CHG_PHASE_RUN);
    f.id = CHG_CAN_BCL;
    f.dlc = 5;
    memcpy(f.data, "\x48\x03\x30\x11\x02", 5);
    chr_link_can_rx(&link, &f, &st, &tx);
    expect_eq("bcl clears loss", link.loss_ms, 0u);

    chr_link_tick(&link, &st, 400, &tx);
    f.id = CHG_CAN_BSM;
    f.dlc = 7;
    memset(f.data, 0, sizeof f.data);
    f.data[6] = 0x10;
    chr_link_can_rx(&link, &f, &st, &tx);
    expect_eq("bsm alone keeps loss", link.loss_ms, 400u);
    chr_link_tick(&link, &st, 800, &tx);
    expect_eq("bsm alone rehs", link.phase, CHG_PHASE_HS);
    expect_eq("bsm alone amps", tx.act.enable, 0);
}

static void test_485(void)
{
    ChrLink link;
    ChrTx tx;
    ChgCap cap = limits();
    ChgStat st = meas();
    static const uint8_t cap0[] = {0xA5, 0x5A, 0x01, 0x02, 0x09, 0x00, 0xF4, 0x01, 0xE8, 0x03,
                                   0x0A, 0x00, 0x20, 0x03, 0xE7, 0xEC};
    static const uint8_t cap1[] = {0xA5, 0x5A, 0x01, 0x02, 0x09, 0x01, 0xF4, 0x01, 0xE8, 0x03,
                                   0x0A, 0x00, 0x20, 0x03, 0xEA, 0x7C};
    static const uint8_t hs[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x00, 0x48, 0x03, 0x00, 0x00, 0x00, 0x7C, 0x5D};
    static const uint8_t run[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x01, 0x48, 0x03, 0x90, 0x01, 0x00, 0x7C, 0x31};
    static const uint8_t stat[] = {0xA5, 0x5A, 0x01, 0x03, 0x06, 0x09, 0x44, 0x03, 0x8E, 0x01, 0x00, 0x8C, 0xA7};

    chr_link_init(&link, CHG_PORT_485, &cap);
    chr_link_tick(&link, &st, 0, &tx);
    expect_eq("485 no poll", tx.frame_len, 0);
    feed(&link, &st, hs, sizeof hs, &tx);
    expect_mem("485 cap0", tx.frame, cap0, sizeof cap0);
    expect_eq("485 allow0", tx.act.enable, 0);
    feed(&link, &st, hs, sizeof hs, &tx);
    expect_mem("485 cap1", tx.frame, cap1, sizeof cap1);
    expect_eq("485 still quiet", tx.act.enable, 0);
    feed(&link, &st, run, sizeof run, &tx);
    expect_mem("485 stat", tx.frame, stat, sizeof stat);
    expect_eq("485 run", tx.act.enable, 1);
    expect_eq("485 amps", tx.act.i_out_01a, 400);
    {
        static const uint8_t pause[] = {
            0xA5, 0x5A, 0x01, 0x01, 0x06, 0x03, 0x48, 0x03, 0x90, 0x01, 0x00, 0x7D, 0xD3};
        feed(&link, &st, pause, sizeof pause, &tx);
        expect_eq("485 pause", tx.act.enable, 0);
        expect_eq("485 pause run", link.phase, CHG_PHASE_RUN);
        feed(&link, &st, run, sizeof run, &tx);
        expect_eq("485 resume inrush", tx.act.i_out_01a, 200);
        expect_eq("485 resume v", tx.act.v_target_01v, 830);
        expect_eq("485 resume armed", link.resume_arm, 1u);
        chr_link_tick(&link, &st, 0, &tx);
        expect_eq("485 resume tick", tx.act.i_out_01a, 200);
        expect_eq("485 resume disarmed", link.resume_arm, 0u);
        feed(&link, &st, run, sizeof run, &tx);
        expect_eq("485 resume full", tx.act.i_out_01a, 400);
        expect_eq("485 resume full v", tx.act.v_target_01v, 840);
    }
    {
        ChrLink gap;
        chr_link_init(&gap, CHG_PORT_485, &cap);
        feed(&gap, &st, hs, 4, &tx);
        chr_link_rx_byte(&gap, hs[4], 60, &st, &tx);
        expect_eq("485 gap", tx.frame_len, 0);
        expect_eq("485 gap phase", gap.phase, CHG_PHASE_OFF);
    }
    chr_link_tick(&link, &st, 1000, &tx);
    expect_eq("485 loss not yet", tx.phase, CHG_PHASE_RUN);
    chr_link_tick(&link, &st, 200, &tx);
    expect_eq("485 loss", tx.phase, CHG_PHASE_HS);
    expect_eq("485 loss amps", tx.act.enable, 0);
    expect_eq("485 loss silent", tx.frame_len, 0);
    expect_eq("485 loss ready0", link.advertise_ready, 0);
    /* T_loss 后必须重新握手。第 1 帧 hs 让 note_ctrl 置 seen_allow0=1 且
     * advertise_ready=1（用 ready_now=0 发的 cap，因此仍 cap0）；
     * 第 2 帧 hs 进来时 advertise_ready 已是 1，reply 发 cap1。
     * 再喂 run 才进 RUN，且 reply 切到 stat。直接喂 run 不推进（cap_dirty=1
     * 让 reply 发的是 cap 不是 stat；且 seen_allow0=0 不会切 RUN）。 */
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("no bare resume", tx.act.enable, 0);
    expect_eq("bare resume stays hs", link.phase, CHG_PHASE_HS);
    feed(&link, &st, hs, sizeof hs, &tx);
    expect_eq("rehs 1st hs allow0", link.seen_allow0, 1);
    expect_eq("rehs 1st hs advertises", link.advertise_ready, 1);
    feed(&link, &st, hs, sizeof hs, &tx);
    expect_eq("rehs 2nd hs ready1", link.advertise_ready, 1);
    expect_mem("rehs cap1", tx.frame, cap1, sizeof cap1);
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("rehs ends in run", link.phase, CHG_PHASE_RUN);
    expect_mem("rehs stat", tx.frame, stat, sizeof stat);
    expect_eq("rehs amps", tx.act.i_out_01a, 400);
    expect_eq("rehs enable", tx.act.enable, 1);
}

static const uint8_t BAT_GOLDEN[] = {
    0xA5, 0x5A, 0x01, 0x04, 0x1C, 0x01, 0x00, 0xF4, 0x01, 0xF8, 0x02, 0x3C, 0x0F, 0xD8, 0x0E, 0x23,
    0x1C, 0xC8, 0x00, 0xDA, 0x02, 0x01, 0x48, 0x03, 0x90, 0x01, 0x37, 0x1E, 0x00, 0x03, 0x0C, 0x01,
    0x02, 0xC0, 0x92};
/* 荷电 100.0%，允许电流 1.0 A。出流仍须跟随 0x01 的 40.0 A。 */
static const uint8_t BAT_OTHER[] = {
    0xA5, 0x5A, 0x01, 0x04, 0x1C, 0x01, 0x00, 0xE8, 0x03, 0xF8, 0x02, 0x3C, 0x0F, 0xD8, 0x0E, 0x23,
    0x1C, 0xC8, 0x00, 0xDA, 0x02, 0x01, 0x48, 0x03, 0x0A, 0x00, 0x37, 0x1E, 0x00, 0x03, 0x0C, 0x01,
    0x02, 0xC3, 0xF5};
static const uint8_t BAT_ZERO[] = {
    0xA5, 0x5A, 0x01, 0x04, 0x1C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xB7, 0x57};
/* 字节 1 的 bit0–bit5 全 1。bit6–bit7 在下一帧验证被清掉。 */
static const uint8_t BAT_FLAGS[] = {
    0xA5, 0x5A, 0x01, 0x04, 0x1C, 0x01, 0x3F, 0xF4, 0x01, 0xF8, 0x02, 0x3C, 0x0F, 0xD8, 0x0E, 0x23,
    0x1C, 0xC8, 0x00, 0xDA, 0x02, 0x01, 0x48, 0x03, 0x90, 0x01, 0x37, 0x1E, 0x00, 0x03, 0x0C, 0x01,
    0x02, 0xC1, 0x39};
static const uint8_t BAT_RESBITS[] = {
    0xA5, 0x5A, 0x01, 0x04, 0x1C, 0x01, 0xC0, 0xF4, 0x01, 0xF8, 0x02, 0x3C, 0x0F, 0xD8, 0x0E, 0x23,
    0x1C, 0xC8, 0x00, 0xDA, 0x02, 0x01, 0x48, 0x03, 0x90, 0x01, 0x37, 0x1E, 0x00, 0x03, 0x0C, 0x01,
    0x02, 0xC2, 0xD2};

/* 私有链路暂停满 10 min 仍留在 RUN。10 min 结束只存在于桩侧。 */
/* 恢复置位碰上故障时不消耗。故障消失后的第一拍仍钳在 20 A / 减 1 V。 */
static void test_resume_fault_keeps_arm(void)
{
    ChrLink link;
    ChrTx tx;
    ChgCap cap = limits();
    ChgStat st = meas();
    static const uint8_t run[] = {
        0xA5, 0x5A, 0x01, 0x01, 0x06, 0x01, 0x48, 0x03, 0x90, 0x01, 0x00, 0x7C, 0x31};
    static const uint8_t pause[] = {
        0xA5, 0x5A, 0x01, 0x01, 0x06, 0x03, 0x48, 0x03, 0x90, 0x01, 0x00, 0x7D, 0xD3};

    chr_link_init(&link, CHG_PORT_SIF, &cap);
    chr_link_tick(&link, &st, 0, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    feed(&link, &st, pause, sizeof pause, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("faultarm armed", link.resume_arm, 1u);
    expect_eq("faultarm inrush", tx.act.i_out_01a, 200);
    st.fault = 1;
    chr_link_tick(&link, &st, 0, &tx);
    expect_eq("faultarm quiet", tx.act.enable, 0);
    expect_eq("faultarm kept", link.resume_arm, 1u);
    st.fault = 0;
    chr_link_tick(&link, &st, 0, &tx);
    expect_eq("faultarm first", tx.act.i_out_01a, 200);
    expect_eq("faultarm first v", tx.act.v_target_01v, 830);
    expect_eq("faultarm clear", link.resume_arm, 0u);
    chr_link_tick(&link, &st, 0, &tx);
    expect_eq("faultarm next", tx.act.i_out_01a, 400);
    expect_eq("faultarm next v", tx.act.v_target_01v, 840);
}

static void test_pause_holds_ten_minutes(void)
{
    ChrLink link;
    ChrTx tx;
    ChgCap cap = limits();
    ChgStat st = meas();
    static const uint8_t run[] = {
        0xA5, 0x5A, 0x01, 0x01, 0x06, 0x01, 0x48, 0x03, 0x90, 0x01, 0x00, 0x7C, 0x31};
    static const uint8_t pause[] = {
        0xA5, 0x5A, 0x01, 0x01, 0x06, 0x03, 0x48, 0x03, 0x90, 0x01, 0x00, 0x7D, 0xD3};
    int i;

    chr_link_init(&link, CHG_PORT_SIF, &cap);
    chr_link_tick(&link, &st, 0, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    feed(&link, &st, pause, sizeof pause, &tx);
    expect_eq("pause10 start", tx.act.enable, 0);
    for (i = 0; i < 2400; i++) {
        chr_link_tick(&link, &st, 250, &tx);
        feed(&link, &st, pause, sizeof pause, &tx);
    }
    expect_eq("pause10 phase", link.phase, CHG_PHASE_RUN);
    expect_eq("pause10 amps", tx.act.enable, 0);
    expect_eq("pause10 loss", link.loss_ms < CHG_T_LOSS_MS, 1u);
}

static void test_bat_no_loss_refresh(void)
{
    ChrLink link;
    ChrTx tx;
    ChgCap cap = limits();
    ChgStat st = meas();
    static const uint8_t run[] = {
        0xA5, 0x5A, 0x01, 0x01, 0x06, 0x01, 0x48, 0x03, 0x90, 0x01, 0x00, 0x7C, 0x31};

    /* 允许电压与最大电压同为 84.0 V。本用例只看 0x04 不改电流、不刷新丢失计时。 */
    cap.v_max_01v = 840;
    chr_link_init(&link, CHG_PORT_SIF, &cap);
    chr_link_tick(&link, &st, 0, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("bat refresh run", tx.act.enable, 1);
    chr_link_tick(&link, &st, 400, &tx);
    expect_eq("bat refresh loss", link.loss_ms, 400u);
    feed(&link, &st, BAT_GOLDEN, sizeof BAT_GOLDEN, &tx);
    expect_eq("bat keeps loss", link.loss_ms, 400u);
    expect_eq("bat keeps amps", tx.act.i_out_01a, 400);
    expect_eq("bat readable", chr_link_bat(&link, 0), 0);
}

/* 尚未 tick、阶段仍是 OFF：3 帧合法 0x01 不计入握手。第一次 tick 进入 HS，计数仍是 0。 */
static void test_sif_off_drops_meter(void)
{
    ChrLink link;
    ChrTx tx;
    ChgCap cap = limits();
    ChgStat st = meas();
    static const uint8_t run[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x01, 0x48, 0x03, 0x90, 0x01, 0x00, 0x7C, 0x31};
    int k;

    chr_link_init(&link, CHG_PORT_SIF, &cap);
    memset(&tx, 0, sizeof tx);
    for (k = 0; k < 3; k++) {
        feed(&link, &st, run, sizeof run, &tx);
    }
    expect_eq("off phase", link.phase, CHG_PHASE_OFF);
    expect_eq("off count", link.sif_got, 0);
    expect_eq("off amps", tx.act.enable, 0);
    chr_link_tick(&link, &st, 0, &tx);
    expect_eq("armed phase", link.phase, CHG_PHASE_HS);
    expect_eq("armed count", link.sif_got, 0);
    expect_eq("armed amps", tx.act.enable, 0);
    feed(&link, &st, run, sizeof run, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("after arm count", link.sif_got, 3);
    expect_eq("after arm on", tx.act.enable, 1);
}

static void test_bat(void)
{
    ChrLink link;
    ChrTx tx;
    ChgCap cap = limits();
    ChgStat st = meas();
    ChgBat info;
    static const uint8_t run[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x01, 0x48, 0x03, 0x90, 0x01, 0x00, 0x7C, 0x31};
    static const uint8_t hs[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x00, 0x48, 0x03, 0x00, 0x00, 0x00, 0x7C, 0x5D};
    int i;

    expect_eq("unpack", (unsigned)chg_bat_unpack(BAT_GOLDEN, sizeof BAT_GOLDEN, &info), 35);
    expect_eq("flags", info.flags, 0);
    expect_eq("soc", info.soc_01pct, 500);
    expect_eq("pack", info.v_pack_01v, 760);
    expect_eq("cell", info.v_cell_max_mv, 3900);
    expect_eq("tmin", (unsigned)(uint8_t)info.t_min_c, 28);
    expect_eq("chem", info.chemistry, CHG_CHEM_NCM);
    expect_eq("ah", info.rated_ah_01ah, 200);
    expect_eq("rv", info.rated_v_01v, 730);
    expect_eq("allow i", info.i_allow_01a, 400);
    expect_eq("remain", info.remain_min, 30);
    expect_eq("idx", info.idx_cell_min, 12);
    expect_eq("flags all", (unsigned)chg_bat_unpack(BAT_FLAGS, sizeof BAT_FLAGS, &info), 35);
    expect_eq("flags bits", info.flags, 0x3F);
    expect_eq("flags reserved stripped",
              (unsigned)chg_bat_unpack(BAT_RESBITS, sizeof BAT_RESBITS, &info) == 35 && info.flags == 0, 1);

    /* 与 0x04 的允许电压对齐，避免电压闸门混进「允许电流不改请求电流」。 */
    cap.v_max_01v = 840;
    chr_link_init(&link, CHG_PORT_SIF, &cap);
    chr_link_tick(&link, &st, 0, &tx);
    expect_eq("no bat yet", chr_link_bat(&link, &info) != 0, 1);
    feed(&link, &st, BAT_GOLDEN, sizeof BAT_GOLDEN, &tx);
    expect_eq("sif bat silent", tx.frame_len, 0);
    expect_eq("sif bat not hs", link.sif_got, 0);
    expect_eq("sif bat no amps", tx.act.enable, 0);
    expect_eq("sif bat ok", chr_link_bat(&link, &info), 0);
    expect_eq("sif soc", info.soc_01pct, 500);
    feed(&link, &st, run, sizeof run, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("sif two", link.sif_got, 2);
    feed(&link, &st, BAT_OTHER, sizeof BAT_OTHER, &tx);
    expect_eq("bat skips hs", link.sif_got, 2);
    expect_eq("bat skips amps", tx.act.enable, 0);
    expect_eq("other soc", chr_link_bat(&link, &info) == 0 && info.soc_01pct == 1000, 1);
    expect_eq("other allow", info.i_allow_01a, 10);
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("still 40A", tx.act.i_out_01a, 400);
    expect_eq("still on", tx.act.enable, 1);
    for (i = 0; i < 4; i++) {
        chr_link_tick(&link, &st, 800, &tx);
        expect_eq("age keeps run", tx.phase, CHG_PHASE_RUN);
        if (i < 3) {
            feed(&link, &st, run, sizeof run, &tx);
        }
    }
    expect_eq("stale", chr_link_bat(&link, &info) != 0, 1);
    expect_eq("stale cleared", info.valid, 0);
    expect_eq("stale amps", tx.act.i_out_01a, 400);
    feed(&link, &st, BAT_ZERO, sizeof BAT_ZERO, &tx);
    expect_eq("invalid", chr_link_bat(&link, &info) != 0, 1);
    expect_eq("invalid amps", tx.act.i_out_01a, 400);

    chr_link_init(&link, CHG_PORT_485, &cap);
    chr_link_tick(&link, &st, 0, &tx);
    feed(&link, &st, BAT_GOLDEN, sizeof BAT_GOLDEN, &tx);
    expect_eq("485 unsolicited", chr_link_bat(&link, &info), 0);
    expect_eq("485 still hs", tx.phase, CHG_PHASE_HS);
    feed(&link, &st, hs, sizeof hs, &tx);
    expect_eq("485 ctrl kept", link.advertise_ready, 1);
    expect_eq("485 hs amps", tx.act.enable, 0);

    chr_link_init(&link, CHG_PORT_485, &cap);
    feed(&link, &st, hs, sizeof hs, &tx);
    feed(&link, &st, hs, sizeof hs, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("485 run amps", tx.act.i_out_01a, 400);
    expect_eq("485 answered", tx.frame[3], 0x03);
    feed(&link, &st, BAT_OTHER, sizeof BAT_OTHER, &tx);
    expect_eq("bat no reply", tx.frame_len, 0);
    expect_eq("bat soc", chr_link_bat(&link, &info) == 0 && info.soc_01pct == 1000, 1);
    expect_eq("bat allow ignored", info.i_allow_01a, 10);
    expect_eq("bat amps", tx.act.i_out_01a, 400);
    chr_link_tick(&link, &st, 1200, &tx);
    expect_eq("bat does not hold", tx.phase, CHG_PHASE_HS);
    expect_eq("bat loss amps", tx.act.enable, 0);

    {
        ChgCan f;
        memset(&f, 0, sizeof f);
        f.id = CHG_CAN_BAT;
        f.dlc = 8;
        chr_link_init(&link, CHG_PORT_CAN, &cap);
        chr_link_tick(&link, &st, 0, &tx);
        memcpy(f.data, "\x02\xDA\x02\x01\x48\x03\x90\x01", 8);
        chr_link_can_rx(&link, &f, &st, &tx);
        memcpy(f.data, "\x03\x37\x1E\x00\x03\x0C\x01\x02", 8);
        chr_link_can_rx(&link, &f, &st, &tx);
        memcpy(f.data, "\x01\x0F\xD8\x0E\x23\x1C\xC8\x00", 8);
        chr_link_can_rx(&link, &f, &st, &tx);
        expect_eq("can partial", chr_link_bat(&link, &info) != 0, 1);
        memcpy(f.data, "\x00\x01\x00\xF4\x01\xF8\x02\x3C", 8);
        chr_link_can_rx(&link, &f, &st, &tx);
        expect_eq("seq0 restarts", chr_link_bat(&link, &info) != 0, 1);
        memcpy(f.data, "\x01\x0F\xD8\x0E\x23\x1C\xC8\x00", 8);
        chr_link_can_rx(&link, &f, &st, &tx);
        memcpy(f.data, "\x02\xDA\x02\x01\x48\x03\x90\x01", 8);
        chr_link_can_rx(&link, &f, &st, &tx);
        memcpy(f.data, "\x03\x37\x1E\x00\x03\x0C\x01\x02", 8);
        chr_link_can_rx(&link, &f, &st, &tx);
        expect_eq("can bat", chr_link_bat(&link, &info), 0);
        expect_eq("can soc", info.soc_01pct, 500);
        expect_eq("can chem", info.chemistry, 1);
        expect_eq("can no amps", tx.act.enable, 0);
        f.id = CHG_CAN_BRO;
        f.dlc = 1;
        f.data[0] = 0x00;
        chr_link_can_rx(&link, &f, &st, &tx);
        f.id = CHG_CAN_BCL;
        f.dlc = 5;
        memcpy(f.data, "\x48\x03\x30\x11\x02", 5);
        chr_link_can_rx(&link, &f, &st, &tx);
        f.id = CHG_CAN_BRO;
        f.dlc = 1;
        f.data[0] = 0xAA;
        chr_link_can_rx(&link, &f, &st, &tx);
        expect_eq("can energy", tx.act.i_out_01a, 400);
        f.id = CHG_CAN_BAT;
        f.dlc = 8;
        memcpy(f.data, "\x00\x01\x00\xE8\x03\xF8\x02\x3C", 8);
        chr_link_can_rx(&link, &f, &st, &tx);
        memcpy(f.data, "\x01\x0F\xD8\x0E\x23\x1C\xC8\x00", 8);
        chr_link_can_rx(&link, &f, &st, &tx);
        memcpy(f.data, "\x02\xDA\x02\x01\x48\x03\x0A\x00", 8);
        chr_link_can_rx(&link, &f, &st, &tx);
        memcpy(f.data, "\x03\x37\x1E\x00\x03\x0C\x01\x02", 8);
        chr_link_can_rx(&link, &f, &st, &tx);
        expect_eq("can soc ignored", chr_link_bat(&link, &info) == 0 && info.soc_01pct == 1000, 1);
        expect_eq("can amps hold", tx.act.i_out_01a, 400);
        chr_link_tick(&link, &st, 1200, &tx);
        expect_eq("bat does not hold link", tx.phase, CHG_PHASE_HS);
        expect_eq("bat loss amps", tx.act.enable, 0);
    }
}

/* 最大电压高于允许电压，或两侧化学体系都已声明且不同：本周期输出 0，会话保持。 */
static void test_bat_gate(void)
{
    ChrLink link;
    ChrTx tx;
    ChgCap cap = limits();
    ChgStat st = meas();
    static const uint8_t run[] = {
        0xA5, 0x5A, 0x01, 0x01, 0x06, 0x01, 0x48, 0x03, 0x90, 0x01, 0x00, 0x7C, 0x31};
    int i;

    chr_link_init(&link, CHG_PORT_SIF, &cap);
    chr_link_tick(&link, &st, 0, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("gate before bat", tx.act.i_out_01a, 400);
    feed(&link, &st, BAT_GOLDEN, sizeof BAT_GOLDEN, &tx);
    expect_eq("gate volts", tx.act.i_out_01a, 0);
    expect_eq("gate quiet", tx.act.enable, 0);
    expect_eq("gate stays run", link.phase, CHG_PHASE_RUN);
    for (i = 0; i < 4; i++) {
        chr_link_tick(&link, &st, 800, &tx);
        if (i < 3) {
            feed(&link, &st, run, sizeof run, &tx);
        }
    }
    expect_eq("gate stale back", tx.act.i_out_01a, 400);
    expect_eq("gate stale on", tx.act.enable, 1);

    cap.v_max_01v = 840;
    cap.chemistry = CHG_CHEM_LFP;
    chr_link_init(&link, CHG_PORT_SIF, &cap);
    chr_link_tick(&link, &st, 0, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    feed(&link, &st, BAT_GOLDEN, sizeof BAT_GOLDEN, &tx);
    expect_eq("gate chem", tx.act.enable, 0);
    expect_eq("gate chem run", link.phase, CHG_PHASE_RUN);
    link.cap.chemistry = CHG_CHEM_NCM;
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("gate chem match", tx.act.i_out_01a, 400);

    cap.chemistry = 0;
    chr_link_init(&link, CHG_PORT_SIF, &cap);
    chr_link_tick(&link, &st, 0, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    feed(&link, &st, BAT_OTHER, sizeof BAT_OTHER, &tx);
    expect_eq("gate unknown chem", tx.act.i_out_01a, 400);
    expect_eq("gate allow ignored", link.bat.i_allow_01a, 10);
}

static void test_apply(void)
{
    ChgCtrl c;
    ChgCap cap = limits();
    ChgAct act;
    ChgCan f;
    ChgStat st;
    memset(&c, 0, sizeof c);
    cap.ready = 1;
    c.allow = 1;
    c.v_req_01v = 840;
    c.i_req_01a = 400;
    chg_apply(&c, &cap, 1, 1, 0, 0, &act);
    expect_eq("apply on", act.enable, 1);
    expect_eq("apply amps", act.i_out_01a, 400);
    expect_eq("apply volts", act.v_target_01v, 840);

    /* §12.2: CCS 8 字节（含 byte 7 累计能量 0.1 kWh/bit）。 */
    memset(&st, 0, sizeof st);
    st.v_out_01v = 840;
    st.i_out_01a = 400;
    chg_can_ccs(&st, &f);
    expect_eq("ccs dlc", f.dlc, 8);
    expect_eq("ccs byte 7 energy", f.data[7], 0);
    expect_eq("ccs byte 5 off", f.data[5], 0);
    expect_eq("ccs byte 4 time", f.data[4], 0);
    st.outputting = 1;
    chg_can_ccs(&st, &f);
    expect_eq("ccs byte 5 on", f.data[5], 1);
    st.i_out_01a = 62000;
    chg_can_ccs(&st, &f);
    expect_eq("ccs raw cap lo", f.data[2], 0xFFu);
    expect_eq("ccs raw cap hi", f.data[3], 0xFFu);
    st.i_out_01a = 400;
    st.outputting = 0;

    c.i_req_01a = 900;
    chg_apply(&c, &cap, 1, 1, 0, 0, &act);
    expect_eq("clamp i", act.i_out_01a, 800);
    c.i_req_01a = 400;
    c.v_req_01v = 1200;
    chg_apply(&c, &cap, 1, 1, 0, 0, &act);
    expect_eq("clamp v", act.v_target_01v, 1000);
    c.v_req_01v = 840;

    c.pause = 1;
    chg_apply(&c, &cap, 1, 1, 0, 0, &act);
    expect_eq("apply pause", act.enable, 0);
    c.pause = 0;
    c.stop = 1;
    chg_apply(&c, &cap, 1, 1, 0, 0, &act);
    expect_eq("apply stop", act.enable, 0);
    c.stop = 0;
    c.allow = 0;
    chg_apply(&c, &cap, 1, 1, 0, 0, &act);
    expect_eq("apply deny", act.enable, 0);
    c.allow = 1;
    c.i_req_01a = 0;
    chg_apply(&c, &cap, 1, 1, 0, 0, &act);
    expect_eq("apply zero i", act.enable, 0);
    c.i_req_01a = 400;
    c.v_req_01v = 0;
    chg_apply(&c, &cap, 1, 1, 0, 0, &act);
    expect_eq("apply zero v", act.enable, 0);
    c.v_req_01v = 840;
    chg_apply(&c, &cap, 1, 0, 0, 0, &act);
    expect_eq("apply no link", act.enable, 0);
    chg_apply(&c, &cap, 1, 1, 1, 0, &act);
    expect_eq("apply two paths", act.enable, 0);
    /* paths_conflict half-states: 与其他 guard 同时为 1 时仍是 0（paths_conflict 在门里）。 */
    c.allow = 0;
    chg_apply(&c, &cap, 1, 1, 1, 0, &act);
    expect_eq("conflict beats deny", act.enable, 0);
    c.allow = 1;
    c.pause = 1;
    chg_apply(&c, &cap, 1, 1, 1, 0, &act);
    expect_eq("conflict beats pause", act.enable, 0);
    c.pause = 0;
    cap.ready = 0;
    chg_apply(&c, &cap, 1, 1, 1, 0, &act);
    expect_eq("conflict beats not ready", act.enable, 0);
    cap.ready = 1;
    /* SIF（cap_valid=0）+ conflict=1：SIF 跳过 cap.ready 检查，但 conflict 仍在门内。 */
    chg_apply(&c, &cap, 0, 1, 1, 0, &act);
    expect_eq("conflict sif still off", act.enable, 0);
    chg_apply(&c, &cap, 1, 1, 0, 1, &act);
    expect_eq("apply fault", act.enable, 0);
    chg_apply(0, &cap, 1, 1, 0, 0, &act);
    expect_eq("apply no ctrl", act.enable, 0);
    chg_apply(&c, 0, 1, 1, 0, 0, &act);
    expect_eq("apply no cap", act.enable, 0);
    cap.ready = 0;
    chg_apply(&c, &cap, 1, 1, 0, 0, &act);
    expect_eq("apply not ready", act.enable, 0);
    chg_apply(&c, &cap, 0, 1, 0, 0, &act);
    expect_eq("sif skips ready", act.enable, 1);
    cap.i_max_01a = 0;
    chg_apply(&c, &cap, 0, 1, 0, 0, &act);
    expect_eq("zero imax", act.enable, 0);
    cap.i_max_01a = 800;
    cap.v_max_01v = 0;
    chg_apply(&c, &cap, 0, 1, 0, 0, &act);
    expect_eq("zero vmax", act.enable, 0);
}

static void reach_485(ChrLink *link, ChrTx *tx, const ChgStat *st)
{
    static const uint8_t hs[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x00, 0x48, 0x03, 0x00, 0x00, 0x00, 0x7C, 0x5D};
    static const uint8_t run[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x01, 0x48, 0x03, 0x90, 0x01, 0x00, 0x7C, 0x31};
    ChgCap cap = limits();
    chr_link_init(link, CHG_PORT_485, &cap);
    feed(link, st, hs, sizeof hs, tx);
    feed(link, st, hs, sizeof hs, tx);
    feed(link, st, run, sizeof run, tx);
}

static void test_edges(void)
{
    ChrLink link;
    ChrTx tx;
    ChgCap cap = limits();
    ChgStat st = meas();
    ChgBat info;
    ChgCan f;
    static const uint8_t hs[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x00, 0x48, 0x03, 0x00, 0x00, 0x00, 0x7C, 0x5D};
    static const uint8_t run[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x01, 0x48, 0x03, 0x90, 0x01, 0x00, 0x7C, 0x31};
    static const uint8_t pause[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x03, 0x48, 0x03, 0x90, 0x01, 0x00, 0x7D, 0xD3};
    static const uint8_t stop[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x04, 0x48, 0x03, 0x00, 0x00, 0x00, 0x7D, 0xD9};
    static const uint8_t v0[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x01, 0x00, 0x00, 0x90, 0x01, 0x00, 0x9C, 0x7B};
    static const uint8_t i0[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x01, 0x48, 0x03, 0x00, 0x00, 0x00, 0x7D, 0x8C};
    static const uint8_t cv[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x09, 0x48, 0x03, 0x90, 0x01, 0x00, 0x7D, 0x79};
    static const uint8_t bad[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x01, 0x48, 0x03, 0x90, 0x01, 0x00, 0x7C, 0x30};
    static const uint8_t cold[] = {
        0xA5, 0x5A, 0x01, 0x04, 0x1C, 0x01, 0x00, 0xF4, 0x01, 0xF8, 0x02, 0x3C, 0x0F, 0xD8, 0x0E, 0x23,
        0xFB, 0xC8, 0x00, 0xDA, 0x02, 0x01, 0x48, 0x03, 0x90, 0x01, 0x37, 0x1E, 0x00, 0x03, 0x0C, 0x01,
        0x02, 0x3B, 0x55};

    chr_link_init(&link, CHG_PORT_485, &cap);
    link.cap.v_max_01v = 840; /* 与 cold 的允许电压相同，本段只看恒压位 */
    feed(&link, &st, cold, sizeof cold, &tx);
    expect_eq("cold before request", tx.act.enable, 0);
    expect_eq("cold no reply", tx.frame_len, 0);
    expect_eq("cold ok", chr_link_bat(&link, &info) == 0 && info.t_min_c == -5, 1);
    feed(&link, &st, bad, sizeof bad, &tx);
    expect_eq("bad crc silent", tx.frame_len, 0);
    expect_eq("bad crc stays off", tx.phase, CHG_PHASE_OFF);
    feed(&link, &st, hs, sizeof hs, &tx);
    feed(&link, &st, hs, sizeof hs, &tx);
    feed(&link, &st, cv, sizeof cv, &tx);
    expect_eq("cv on", tx.act.enable, 1);
    expect_eq("cv volts", tx.act.v_target_01v, 840);
    expect_eq("cv flag", link.ctrl.cv, 1);

    reach_485(&link, &tx, &st);
    feed(&link, &st, pause, sizeof pause, &tx);
    expect_eq("485 pause", tx.act.enable, 0);
    expect_eq("485 pause phase", tx.phase, CHG_PHASE_RUN);
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("485 resume", tx.act.enable, 1);
    feed(&link, &st, v0, sizeof v0, &tx);
    expect_eq("485 zero v", tx.act.enable, 0);
    expect_eq("485 zero v keeps", tx.phase, CHG_PHASE_RUN);
    feed(&link, &st, stop, sizeof stop, &tx);
    expect_eq("485 stop", tx.act.enable, 0);
    expect_eq("485 stop phase", tx.phase, CHG_PHASE_STOP);

    reach_485(&link, &tx, &st);
    feed(&link, &st, i0, sizeof i0, &tx);
    expect_eq("485 zero i", tx.act.enable, 0);
    expect_eq("485 zero i phase", tx.phase, CHG_PHASE_STOP);
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("485 no bare run", tx.act.enable, 0);

    reach_485(&link, &tx, &st);
    st.fault = 1;
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("485 fault", tx.act.enable, 0);
    st.fault = 0;
    st.fault_code = 3;
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("485 fault code", tx.act.enable, 0);
    st.fault_code = 0;
    st.ac_path = 1;
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("485 two paths", tx.act.enable, 0);

    chr_link_init(&link, CHG_PORT_CAN, &cap);
    st = meas();
    chr_link_tick(&link, &st, 0, &tx);
    memset(&f, 0, sizeof f);
    f.id = CHG_CAN_BRO;
    f.dlc = 1;
    f.data[0] = 0x00;
    chr_link_can_rx(&link, &f, &st, &tx);
    f.id = CHG_CAN_BCL;
    f.dlc = 5;
    memcpy(f.data, "\x48\x03\x30\x11\x01", 5);
    chr_link_can_rx(&link, &f, &st, &tx);
    f.id = CHG_CAN_BRO;
    f.dlc = 1;
    f.data[0] = 0xAA;
    chr_link_can_rx(&link, &f, &st, &tx);
    expect_eq("can cv", link.ctrl.cv, 1);
    expect_eq("can cv amps", tx.act.i_out_01a, 400);
    f.id = CHG_CAN_BCL;
    f.dlc = 4;
    chr_link_can_rx(&link, &f, &st, &tx);
    expect_eq("short bcl ignored", tx.act.i_out_01a, 400);
    chr_link_tick(&link, &st, 1000, &tx);
    expect_eq("can loss not yet", tx.phase, CHG_PHASE_RUN);
    chr_link_tick(&link, &st, 200, &tx);
    expect_eq("can loss", tx.phase, CHG_PHASE_HS);
    expect_eq("can loss amps", tx.act.enable, 0);
    expect_eq("can loss silent", tx.can_n, 0);
    expect_eq("can loss ready0", link.advertise_ready, 0);

    chr_link_init(&link, CHG_PORT_SIF, &cap);
    chr_link_tick(&link, &st, 0, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    feed(&link, &st, bad, sizeof bad, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("sif bad crc skips", link.sif_got, 2);
    expect_eq("sif bad crc amps", tx.act.enable, 0);
}

/* 防御性测试：chr_link_* NULL guard + 端口不匹配 guard + chg_cap_pack/chg_stat_pack NULL +
 * chr_link_rx_byte NULL guard。 */
static void test_chr_null_guards(void)
{
    ChrLink link;
    ChrTx tx;
    ChgCap cap = limits();
    ChgStat st = meas();
    ChgCan rx;
    uint8_t frame[64];

    /* 1. chg_cap_pack / chg_stat_pack NULL 输入 → -1。 */
    expect_eq("chr cap pack null c", chg_cap_pack(NULL, frame, 100), -1);
    expect_eq("chr cap pack cap small", chg_cap_pack(&cap, frame, 5), -1);
    expect_eq("chr stat pack null s", chg_stat_pack(NULL, frame, 100), -1);
    expect_eq("chr stat pack cap small", chg_stat_pack(&st, frame, 5), -1);

    /* 2. chr_link_tick(NULL, &st, ...) — 不崩溃。 */
    chr_link_tick(NULL, &st, 0, &tx);

    /* 3. chr_link_tick(&link, &st, ..., NULL) — 不崩溃。 */
    chr_link_init(&link, CHG_PORT_SIF, &cap);
    chr_link_tick(&link, &st, 0, NULL);

    /* 4. chr_link_can_rx(NULL, &rx, &st, &tx) — 不崩溃。 */
    memset(&rx, 0, sizeof rx);
    rx.id = CHG_CAN_BCL;
    rx.dlc = 5;
    chr_link_can_rx(NULL, &rx, &st, &tx);

    /* 5. chr_link_can_rx(&link_sif, &rx, &st, &tx) — 静默忽略。 */
    chr_link_init(&link, CHG_PORT_SIF, &cap);
    static const uint8_t ctrl_run[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x01, 0x48, 0x03, 0x90, 0x01, 0x00, 0x7C, 0x31};
    feed(&link, &st, ctrl_run, sizeof ctrl_run, &tx);
    uint8_t prev_frame_len = tx.frame_len;
    memset(&rx, 0, sizeof rx);
    rx.id = CHG_CAN_BCL;
    rx.dlc = 5;
    chr_link_can_rx(&link, &rx, &st, &tx);
    expect_eq("chr can_rx sif noop", tx.frame_len, prev_frame_len);

    /* 6. chr_link_rx_byte(NULL, ...) — 不崩溃。 */
    chr_link_rx_byte(NULL, 0xA5, 0, &st, &tx);
}

/* 防御性测试：take_ctrl_frame 拒绝路径 + chg_bat_unpack 拒绝路径 +
 * chr_link_rx_byte raw 缓冲防护。
 * 注：line 538 的 `raw_n >= sizeof raw` 与 line 550 的 `total > sizeof raw`
 * 在 raw[4]<=28 时不可达（total=7+28=35<=40）— 是防御性分支，留作未来收紧。 */
static void test_chr_rejection(void)
{
    ChrLink link;
    ChrTx tx;
    ChgCap cap = limits();
    ChgStat st = meas();
    static const uint8_t stat[] = {0xA5, 0x5A, 0x01, 0x03, 0x06, 0x09, 0x44, 0x03, 0x8E, 0x01, 0x00, 0x8C, 0xA7};
    static const uint8_t ctrl_run[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x01, 0x48, 0x03, 0x90, 0x01, 0x00, 0x7C, 0x31};
    ChgBat b;

    /* 1. take_ctrl_frame 拒绝 msg=0x03（在 485 端口未测试过）；
     *    valid CRC 但不是 0x01/0x04 → 应被 take_ctrl_frame 直接 return。 */
    chr_link_init(&link, CHG_PORT_485, &cap);
    feed(&link, &st, stat, sizeof stat, &tx);
    expect_eq("chr 485 rejects stat", tx.frame_len, 0);
    expect_eq("chr 485 no run", link.phase, CHG_PHASE_OFF);

    /* 2. chg_bat_unpack：NULL 输入 → -1。 */
    memset(&b, 0, sizeof b);
    expect_eq("chr bat unpack null in", chg_bat_unpack(NULL, 16, &b), -1);
    expect_eq("chr bat unpack null out", chg_bat_unpack(ctrl_run, 13, NULL), -1);

    /* 3. chg_bat_unpack：msg=0x01 ctrl 帧（不是 bat 0x04）→ -1。 */
    expect_eq("chr bat unpack wrong msg", chg_bat_unpack(ctrl_run, sizeof ctrl_run, &b), -1);

    /* 4. chg_bat_unpack：n 不足（< 7）→ frame_unpack 失败 → -1。 */
    {
        static const uint8_t short_frame[] = {0xA5, 0x5A, 0x01, 0x04, 0x1C};
        expect_eq("chr bat unpack short", chg_bat_unpack(short_frame, sizeof short_frame, &b), -1);
    }

    /* 5. chg_bat_unpack：BAT_GOLDEN 完整帧 → 返回 35（28 + 7 包头）。 */
    expect_eq("chr bat unpack golden", chg_bat_unpack(BAT_GOLDEN, sizeof BAT_GOLDEN, &b), 35);
    expect_eq("chr bat unpack soc", b.soc_01pct, 500);
}

static void test_chr_raw_guards(void)
{
    ChrLink link;
    ChrTx tx;
    ChgCap cap = limits();
    ChgStat st = meas();
    uint8_t i;
    uint8_t huge_len[12] = {
        0xA5, 0x5A, 0x01, 0x01,
        0x1D, /* LEN = 29 (declared > 28 → should be dropped immediately) */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };

    /* raw[4] > 28：声明 LEN=29 的帧直接被丢弃，frame_len 不变，raw_n 清零。 */
    chr_link_init(&link, CHG_PORT_485, &cap);
    feed(&link, &st, huge_len, sizeof huge_len, &tx);
    expect_eq("chr raw len29 dropped", tx.frame_len, 0);
    expect_eq("chr raw len29 reset", link.raw_n, 0);

    /* raw[4] > 28u 触发后，喂第 5 字节 LEN>28 → 再次丢弃；累加器重置。 */
    chr_link_init(&link, CHG_PORT_485, &cap);
    for (i = 0; i < 5; i++) {
        chr_link_rx_byte(&link, 0xCC, 10, &st, &tx);
    }
    /* raw_n=5, raw[4]=0xCC>28 → 走 line 545 reset，raw_n=0 */
    expect_eq("chr raw len29 reset2", link.raw_n, 0);

    /* raw[4] == 28 边界：BAT_GOLDEN 是 LEN=28 → 应被尝试解析。 */
    {
        ChrLink l2;
        ChrTx tx2;
        chr_link_init(&l2, CHG_PORT_485, &cap);
        feed(&l2, &st, BAT_GOLDEN, sizeof BAT_GOLDEN, &tx2);
        expect_eq("chr raw len28 parsed", l2.bat.valid, 1);
    }

    /* T_GAP_MS > 50 后喂单字节：line 535 的 gap 重置把 raw_n 清 0。 */
    chr_link_init(&link, CHG_PORT_485, &cap);
    for (i = 0; i < 4; i++) {
        chr_link_rx_byte(&link, 0xA5, 10, &st, &tx);
    }
    expect_eq("chr raw prep 4", link.raw_n, 4);
    chr_link_rx_byte(&link, 0x5A, 60, &st, &tx);
    expect_eq("chr raw gap reset", link.raw_n, 1);
    expect_eq("chr raw gap new head", link.raw[0], 0x5A);
}

/* 端到端会话回归。用最少步骤把 3.会话 与 4.2 三个接口的状态变迁走一遍。 */
static void test_session_state(void)
{
    ChrLink link;
    ChrTx tx;
    ChgCap cap = limits();
    ChgStat st = meas();
    static const uint8_t hs[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x00, 0x48, 0x03, 0x00, 0x00, 0x00, 0x7C, 0x5D};
    static const uint8_t run[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x01, 0x48, 0x03, 0x90, 0x01, 0x00, 0x7C, 0x31};
    static const uint8_t pause[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x03, 0x48, 0x03, 0x90, 0x01, 0x00, 0x7D, 0xD3};
    static const uint8_t stop[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x04, 0x48, 0x03, 0x00, 0x00, 0x00, 0x7D, 0xD9};
    ChgCan f;

    /* 一线通：3 帧握手到 RUN；暂停不重握手；停止后重新 3 帧握手。 */
    chr_link_init(&link, CHG_PORT_SIF, &cap);
    chr_link_tick(&link, &st, 0, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("sess sif run", tx.act.enable, 1);
    feed(&link, &st, pause, sizeof pause, &tx);
    expect_eq("sess sif pause keep", tx.phase, CHG_PHASE_RUN);
    expect_eq("sess sif pause amp", tx.act.enable, 0);
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("sess sif resume", tx.act.enable, 1);
    feed(&link, &st, stop, sizeof stop, &tx);
    expect_eq("sess sif stop", tx.phase, CHG_PHASE_STOP);
    feed(&link, &st, run, sizeof run, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("sess sif rehs 2", tx.act.enable, 0);
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("sess sif rehs done", tx.act.enable, 1);

    /* RS-485：HS 看 cap；运行后看 stat；停止后下一次又从 cap 开始。 */
    chr_link_init(&link, CHG_PORT_485, &cap);
    feed(&link, &st, hs, sizeof hs, &tx);
    expect_eq("sess 485 cap0", tx.frame[3], 0x02);
    feed(&link, &st, hs, sizeof hs, &tx);
    expect_eq("sess 485 cap1", tx.frame[3], 0x02);
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("sess 485 stat", tx.frame[3], 0x03);
    expect_eq("sess 485 amps", tx.act.enable, 1);
    feed(&link, &st, pause, sizeof pause, &tx);
    expect_eq("sess 485 pause keep", tx.phase, CHG_PHASE_RUN);
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("sess 485 resume", tx.act.enable, 1);
    feed(&link, &st, stop, sizeof stop, &tx);
    expect_eq("sess 485 stop", tx.phase, CHG_PHASE_STOP);
    feed(&link, &st, hs, sizeof hs, &tx);
    expect_eq("sess 485 cap again", tx.frame[3], 0x02);

    /* CAN：BSM bit4-5 = 0 表示暂停；BST 直接进 STOP；下一轮握手 CRO = 0。 */
    chr_link_init(&link, CHG_PORT_CAN, &cap);
    memset(&f, 0, sizeof f);
    f.id = CHG_CAN_BRO;
    f.dlc = 1;
    f.data[0] = 0x00;
    chr_link_can_rx(&link, &f, &st, &tx);
    f.id = CHG_CAN_BCL;
    f.dlc = 5;
    memcpy(f.data, "\x48\x03\x30\x11\x02", 5);
    chr_link_can_rx(&link, &f, &st, &tx);
    f.id = CHG_CAN_BRO;
    f.dlc = 1;
    f.data[0] = 0xAA;
    chr_link_can_rx(&link, &f, &st, &tx);
    f.id = CHG_CAN_BSM;
    f.dlc = 7;
    memset(f.data, 0, sizeof f.data);
    f.data[6] = 0x10;
    chr_link_can_rx(&link, &f, &st, &tx);
    expect_eq("sess can run", tx.phase, CHG_PHASE_RUN);
    expect_eq("sess can amps", tx.act.enable, 1);

    /* BCL 电流为 0 不构成停机，但出流确实降到 0；恢复非零继续 RUN。 */
    f.id = CHG_CAN_BCL;
    f.dlc = 5;
    memcpy(f.data, "\x48\x03\xA0\x0F\x02", 5);
    chr_link_can_rx(&link, &f, &st, &tx);
    expect_eq("sess can bcl 0 no stop", tx.phase, CHG_PHASE_RUN);
    expect_eq("sess can bcl 0 amp off", tx.act.enable, 0);
    memcpy(f.data, "\x48\x03\x30\x11\x02", 5);
    chr_link_can_rx(&link, &f, &st, &tx);
    expect_eq("sess can bcl restore", tx.act.enable, 1);

    /* BSM 暂停位：第 7 字节 0 表示暂停，不重新握手。 */
    f.id = CHG_CAN_BSM;
    f.dlc = 7;
    memset(f.data, 0, sizeof f.data);
    chr_link_can_rx(&link, &f, &st, &tx);
    expect_eq("sess can pause amps", tx.act.enable, 0);
    expect_eq("sess can pause phase", tx.phase, CHG_PHASE_RUN);
    f.data[6] = 0x10;
    chr_link_can_rx(&link, &f, &st, &tx);
    expect_eq("sess can resume", tx.act.enable, 1);

    /* BST：直接进 STOP；下一轮 tick 把 CRO 重新置 0。 */
    f.id = CHG_CAN_BST;
    f.dlc = 4;
    f.data[0] = 0x01;
    chr_link_can_rx(&link, &f, &st, &tx);
    expect_eq("sess can bst stop", tx.phase, CHG_PHASE_STOP);
    chr_link_tick(&link, &st, 0, &tx);
    expect_eq("sess can cro 0 after bst", find_can(&tx, CHG_CAN_CRO) >= 0, 1);
    expect_eq("sess can cro 0 byte", tx.can[find_can(&tx, CHG_CAN_CRO)].data[0], 0x00);

    /* BMS 发送冻结：未发 BCL/BRO/BSM、BST 是充电器自己发的；这之后再收 BCL 也不出流，必须重新握手。 */
    f.id = CHG_CAN_BCL;
    f.dlc = 5;
    memcpy(f.data, "\x48\x03\x30\x11\x02", 5);
    chr_link_can_rx(&link, &f, &st, &tx);
    expect_eq("sess can no resume bare", tx.act.enable, 0);

    /* 验证：RUN 期间喂入的坏 CRC 帧不刷新 T_loss。 */
    {
        static const uint8_t bad_run[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x01, 0x48, 0x03, 0x90, 0x01, 0x00, 0x7C, 0x30}; /* 末字节改 0 让 CRC 错 */
        chr_link_init(&link, CHG_PORT_SIF, &cap);
        chr_link_tick(&link, &st, 0, &tx);
        feed(&link, &st, run, sizeof run, &tx);
        feed(&link, &st, run, sizeof run, &tx);
        feed(&link, &st, run, sizeof run, &tx);
        expect_eq("crc sess run", tx.act.enable, 1);
        feed(&link, &st, bad_run, sizeof bad_run, &tx);
        feed(&link, &st, bad_run, sizeof bad_run, &tx);
        feed(&link, &st, bad_run, sizeof bad_run, &tx);
        feed(&link, &st, bad_run, sizeof bad_run, &tx);
        feed(&link, &st, bad_run, sizeof bad_run, &tx);
        expect_eq("crc bad doesn't drop amp", tx.act.enable, 1);
        expect_eq("crc bad stays run", link.phase, CHG_PHASE_RUN);
        feed(&link, &st, run, sizeof run, &tx);
        expect_eq("crc good restores amp", tx.act.enable, 1);
    }
}

/* §10 验收 15: 三口物理断开 100 ms 内输出 0 A。故障期间的请求帧不恢复出流。 */
static void test_line_down(void)
{
    ChrLink link;
    ChrTx tx;
    ChgCap cap = limits();
    ChgStat st = meas();
    ChgCan f;
    static const uint8_t run[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x01, 0x48, 0x03, 0x90, 0x01, 0x00, 0x7C, 0x31};
    static const uint8_t bad[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x01, 0x48, 0x03, 0x90, 0x01, 0x00, 0x7C, 0x30};
    static const uint8_t hs[] = {0xA5, 0x5A, 0x01, 0x01, 0x06, 0x00, 0x48, 0x03, 0x00, 0x00, 0x00, 0x7C, 0x5D};

    chr_link_init(&link, CHG_PORT_SIF, &cap);
    chr_link_tick(&link, &st, 0, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("line sif run", tx.act.enable, 1);

    chr_link_set_sif_state(&link, 0, 40);
    feed(&link, &st, bad, sizeof bad, &tx);
    expect_eq("line sif crc keeps counter", link.line_down_ms, 40);
    chr_link_tick(&link, &st, 0, &tx);
    expect_eq("line sif 40ms still on", tx.act.enable, 1);

    chr_link_set_sif_state(&link, 0, 60);
    chr_link_tick(&link, &st, 0, &tx);
    expect_eq("line sif off", tx.act.enable, 0);
    expect_eq("line sif STOP", link.phase, CHG_PHASE_STOP);
    expect_eq("line sif latched", link.line_fault, 1);
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("line sif frame ignored", tx.act.enable, 0);
    expect_eq("line sif still STOP", link.phase, CHG_PHASE_STOP);
    expect_eq("line sif got stays", link.sif_got, 0);

    chr_link_tick(&link, &st, 500, &tx);
    expect_eq("line sif held", link.phase, CHG_PHASE_STOP);
    expect_eq("line sif held amps", tx.act.enable, 0);

    chr_link_set_sif_state(&link, 1, 10);
    expect_eq("line sif clear latch", link.line_fault, 0);
    expect_eq("line sif clear counter", link.line_down_ms, 0);
    feed(&link, &st, run, sizeof run, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("line sif rehs", tx.act.enable, 1);

    /* CAN：50 ms 仍出流，再 50 ms 停。故障期间 BCL 不恢复。线好了才能重新握手。 */
    chr_link_init(&link, CHG_PORT_CAN, &cap);
    chr_link_tick(&link, &st, 0, &tx);
    memset(&f, 0, sizeof f);
    f.id = CHG_CAN_BRO;
    f.dlc = 1;
    f.data[0] = 0x00;
    chr_link_can_rx(&link, &f, &st, &tx);
    f.id = CHG_CAN_BCL;
    f.dlc = 5;
    memcpy(f.data, "\x48\x03\x30\x11\x02", 5);
    chr_link_can_rx(&link, &f, &st, &tx);
    f.id = CHG_CAN_BRO;
    f.dlc = 1;
    f.data[0] = 0xAA;
    chr_link_can_rx(&link, &f, &st, &tx);
    expect_eq("line can run", tx.act.enable, 1);

    chr_link_set_bus_off(&link, 1, 50);
    chr_link_tick(&link, &st, 0, &tx);
    expect_eq("line can 50ms on", tx.act.enable, 1);
    chr_link_set_bus_off(&link, 1, 50);
    chr_link_tick(&link, &st, 0, &tx);
    expect_eq("line can off", tx.act.enable, 0);
    expect_eq("line can STOP", tx.phase, CHG_PHASE_STOP);
    expect_eq("line can silent", tx.can_n, 0);
    f.id = CHG_CAN_BCL;
    f.dlc = 5;
    memcpy(f.data, "\x48\x03\x30\x11\x02", 5);
    chr_link_can_rx(&link, &f, &st, &tx);
    expect_eq("line can bcl ignored", tx.act.enable, 0);
    expect_eq("line can bcl phase", link.phase, CHG_PHASE_STOP);

    chr_link_set_bus_off(&link, 0, 10);
    chr_link_tick(&link, &st, 0, &tx);
    expect_eq("line can back to HS", link.phase, CHG_PHASE_HS);
    expect_eq("line can hs amps", tx.act.enable, 0);

    /* RS-485。 */
    reach_485(&link, &tx, &st);
    expect_eq("line 485 run", tx.act.enable, 1);
    chr_link_set_485_short(&link, 1, 99);
    chr_link_tick(&link, &st, 0, &tx);
    expect_eq("line 485 99ms on", tx.act.enable, 1);
    chr_link_set_485_short(&link, 1, 1);
    chr_link_tick(&link, &st, 0, &tx);
    expect_eq("line 485 off", tx.act.enable, 0);
    expect_eq("line 485 STOP", link.phase, CHG_PHASE_STOP);
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("line 485 frame ignored", tx.act.enable, 0);
    expect_eq("line 485 still STOP", link.phase, CHG_PHASE_STOP);
    chr_link_set_485_short(&link, 0, 10);
    chr_link_tick(&link, &st, 0, &tx);
    expect_eq("line 485 recovered HS", link.phase, CHG_PHASE_HS);
    feed(&link, &st, hs, sizeof hs, &tx);
    feed(&link, &st, run, sizeof run, &tx);
    expect_eq("line 485 rehs", tx.act.enable, 1);

    /* 错口不累加。 */
    chr_link_init(&link, CHG_PORT_SIF, &cap);
    chr_link_set_bus_off(&link, 1, 500);
    chr_link_set_485_short(&link, 1, 500);
    expect_eq("line wrong port", link.line_fault, 0);
    expect_eq("line wrong port counter", link.line_down_ms, 0);
}

/* v30 新增：SIF 端口字节间隔 > 50 ms 半帧重置单测（A-3）。
 * SIF 半帧判定实际在 charger 侧 `chr_link_rx_byte`（line 540 左右），
 * 因为 SIF 是从 BMS 出到充电机的串口，充电机侧做 raw_n 累加；
 * BMS 侧 `bms_link_rx_byte` 只接受 CHG_PORT_485。 */
static void test_chr_sif_gap(void)
{
    ChrLink link;
    ChrTx   tx;
    ChgStat st = meas();
    ChgCap  cap = limits();

    /* 4 字节 0xA5 (gap=10 ms)，raw_n 累到 4；下一字节 0x5A 间隔 60 ms（> T_GAP_MS=50）
     * 应重置 raw_n=1，把 0x5A 当新帧头。 */
    chr_link_init(&link, CHG_PORT_SIF, &cap);
    chr_link_tick(&link, &st, 0, &tx);
    chr_link_rx_byte(&link, 0xA5, 10, &st, &tx);
    chr_link_rx_byte(&link, 0xA5, 10, &st, &tx);
    chr_link_rx_byte(&link, 0xA5, 10, &st, &tx);
    chr_link_rx_byte(&link, 0xA5, 10, &st, &tx);
    expect_eq("sif 4*0xA5 raw_n", link.raw_n, 4u);
    chr_link_rx_byte(&link, 0x5A, 60, &st, &tx); /* gap=60 > 50 */
    expect_eq("sif gap>50 reset raw_n", link.raw_n, 1u);
    expect_eq("sif new head 0x5A", link.raw[0], 0x5Au);

    /* 边界：恰好 50 ms（不重置，> T_GAP_MS 才重置），raw_n 累到 2。 */
    chr_link_init(&link, CHG_PORT_SIF, &cap);
    chr_link_tick(&link, &st, 0, &tx);
    chr_link_rx_byte(&link, 0xA5, 10, &st, &tx);
    chr_link_rx_byte(&link, 0x5A, 50, &st, &tx); /* gap=50 == T_GAP_MS, 不重置 */
    expect_eq("sif gap=50 no reset", link.raw_n, 2u);

    /* 51 ms（> T_GAP_MS, 重置）→ raw_n=1，新帧头 0x5A。 */
    chr_link_init(&link, CHG_PORT_SIF, &cap);
    chr_link_tick(&link, &st, 0, &tx);
    chr_link_rx_byte(&link, 0xA5, 10, &st, &tx);
    chr_link_rx_byte(&link, 0x5A, 51, &st, &tx);
    expect_eq("sif gap=51 reset", link.raw_n, 1u);
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

static void pack_01(uint8_t flags, uint8_t reserved, uint8_t out[13])
{
    uint16_t crc;
    out[0] = 0xA5; out[1] = 0x5A; out[2] = 0x01; out[3] = 0x01; out[4] = 0x06;
    out[5] = flags;
    out[6] = 0x48; out[7] = 0x03; out[8] = 0x90; out[9] = 0x01;
    out[10] = reserved;
    crc = crc16_test(out, 11);
    out[11] = (uint8_t)(crc & 0xFFu);
    out[12] = (uint8_t)(crc >> 8);
}

/* BCL 模式只认 0x01/0x02。BRO 只认 0x00/0xAA。0x01 保留位非 0 整帧无效。 */
static void test_defined_values(void)
{
    ChrLink link;
    ChrTx tx;
    ChgCap cap = limits();
    ChgStat st = meas();
    ChgCan f;
    uint8_t frame[13];
    uint32_t loss;
    uint32_t drops;

    chr_link_init(&link, CHG_PORT_CAN, &cap);
    chr_link_tick(&link, &st, 0, &tx);
    memset(&f, 0, sizeof f);
    f.id = CHG_CAN_BRO;
    f.dlc = 1;
    f.data[0] = 0x11;
    drops = link.rx_drop;
    chr_link_can_rx(&link, &f, &st, &tx);
    expect_eq("bro junk not ready", link.advertise_ready, 0);
    expect_eq("bro junk no allow0", link.seen_allow0, 0);
    expect_eq("bro junk dropped", link.rx_drop, drops + 1u);

    f.data[0] = 0x00;
    chr_link_can_rx(&link, &f, &st, &tx);
    f.id = CHG_CAN_BCL;
    f.dlc = 5;
    memcpy(f.data, "\x48\x03\x30\x11\x02", 5);
    chr_link_can_rx(&link, &f, &st, &tx);
    f.id = CHG_CAN_BRO;
    f.dlc = 1;
    f.data[0] = 0xAA;
    chr_link_can_rx(&link, &f, &st, &tx);
    expect_eq("defined run", tx.phase, CHG_PHASE_RUN);
    expect_eq("defined amps", tx.act.i_out_01a, 400);

    chr_link_tick(&link, &st, 100, &tx);
    loss = link.loss_ms;
    link.bcl_gap_ms = 40u;
    drops = link.rx_drop;
    memset(&f, 0, sizeof f);
    f.id = CHG_CAN_BCL;
    f.dlc = 5;
    memcpy(f.data, "\x48\x03\x00\x00\x00", 5);
    chr_link_can_rx(&link, &f, &st, &tx);
    expect_eq("bcl mode stays", tx.phase, CHG_PHASE_RUN);
    expect_eq("bcl mode amps", tx.act.i_out_01a, 400);
    expect_eq("bcl mode cv", link.ctrl.cv, 0);
    expect_eq("bcl mode loss", link.loss_ms, loss);
    expect_eq("bcl mode gap", link.bcl_gap_ms, 40u);
    expect_eq("bcl mode drop", link.rx_drop, drops + 1u);

    f.id = CHG_CAN_BRO;
    f.dlc = 1;
    f.data[0] = 0x11;
    chr_link_can_rx(&link, &f, &st, &tx);
    expect_eq("bro junk keeps allow", link.ctrl.allow, 1);
    expect_eq("bro junk keeps run", tx.phase, CHG_PHASE_RUN);

    chr_link_init(&link, CHG_PORT_SIF, &cap);
    chr_link_tick(&link, &st, 0, &tx);
    pack_01(0x11, 0x00, frame);
    feed(&link, &st, frame, sizeof frame, &tx);
    expect_eq("sif reserved flags", link.sif_got, 0);
    expect_eq("sif reserved amps", tx.act.enable, 0);
    pack_01(0x01, 0x01, frame);
    feed(&link, &st, frame, sizeof frame, &tx);
    expect_eq("sif reserved tail", link.sif_got, 0);
    pack_01(0x01, 0x00, frame);
    feed(&link, &st, frame, sizeof frame, &tx);
    expect_eq("sif legal counts", link.sif_got, 1);

    chr_link_init(&link, CHG_PORT_SIF, &cap);
    chr_link_tick(&link, &st, 0, &tx);
    feed(&link, &st, BAT_GOLDEN, sizeof BAT_GOLDEN, &tx);
    expect_eq("bat golden", chr_link_bat(&link, 0), 0);
    chr_link_tick(&link, &st, 100, &tx);
    {
        uint8_t bad[35];
        uint16_t crc;
        memcpy(bad, BAT_GOLDEN, sizeof bad);
        bad[5] = 0x02;
        crc = crc16_test(bad, 33);
        bad[33] = (uint8_t)(crc & 0xFFu);
        bad[34] = (uint8_t)(crc >> 8);
        expect_eq("bat valid 2 unpack", (unsigned)chg_bat_unpack(bad, sizeof bad, 0) == 35u, 0u);
        feed(&link, &st, bad, sizeof bad, &tx);
        expect_eq("bat valid 2 age", link.bat_age_ms, 100u);
        expect_eq("bat valid 2 kept", chr_link_bat(&link, 0), 0);
    }

    chr_link_init(&link, CHG_PORT_CAN, &cap);
    chr_link_tick(&link, &st, 0, &tx);
    memset(&f, 0, sizeof f);
    f.id = CHG_CAN_BAT;
    f.dlc = 8;
    f.data[0] = 0x00;
    chr_link_can_rx(&link, &f, &st, &tx);
    expect_eq("seq0 mask", link.bat_mask, 0x01u);
    drops = link.rx_drop;
    f.data[0] = 0x04;
    chr_link_can_rx(&link, &f, &st, &tx);
    expect_eq("seq4 mask", link.bat_mask, 0x01u);
    expect_eq("seq4 drop", link.rx_drop, drops + 1u);
}

int main(void)
{
    test_apply();
    test_sif();
    test_can();
    test_485();
    test_sif_off_drops_meter();
    test_bat();
    test_bat_gate();
    test_bat_no_loss_refresh();
    test_pause_holds_ten_minutes();
    test_resume_fault_keeps_arm();
    test_edges();
    test_chr_rejection();
    test_chr_raw_guards();
    test_chr_null_guards();
    test_session_state();
    test_chr_sif_gap();
    test_line_down();
    test_period_and_address();
    test_defined_values();
    test_loss_source();
    if (g_fail) {
        return 1;
    }
    printf("ok\n");
    return 0;
}
