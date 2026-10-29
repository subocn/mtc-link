/* 作者：苏勃(subo)
 * SPDX-License-Identifier: Apache-2.0
 * e2e：背靠背 harness。把 chg_bms 与 chg_charger 链到一个进程里跑，
 * 按规范 §3 的 10 拍序列（HS→RUN→PAUSE→RESUME→STOP→重握）走一遍。
 *
 * 字节走「喂入对端 rx_byte」；CAN 走「对端 can_rx」。两端都被动等
 * 对方送来，再推进 tick。两端状态机必须对称（HS/RUN/STOP 同步）。
 *
 * 三个端口各跑一遍：CHG_PORT_SIF、CHG_PORT_CAN、CHG_PORT_485。
 * 任一端口失败返回非零，CI 才能定位。
 */
#include "chg_bms.h"
#include "chg_charger.h"
#include "chg_battery_sim.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DT_MS       50u
#define HS_TICKS    20u   /* 1 s 进入 HS */
#define RUN_TICKS   40u   /* 2 s RUN */
#define PAUSE_TICKS 20u   /* 1 s PAUSE */
#define STOP_TICKS  20u   /* 1 s STOP */
/* 长 RUN：50 ms 一拍，满 1000 ms 才算电池模型的 1 s。50 拍约 2 s。 */
#define LONG_RUN_TICKS 50u

/* 全局状态；step() 直接读写。 */
static BmsLink g_bms;
static ChrLink g_chr;
static ChgCtrl g_ctrl;
static ChgStat g_stat;
static ChgBat  g_bat;
static ChgCap  g_cap;
/* 电池仿真（共享自 chg_link）：软件在环里的被控对象。g_stat.v_out_01v
 * 在每拍末尾由电池端电压驱动。不是硬件在环。 */
static SimBattery g_bat_sim;
static uint16_t g_tick_v;

/* CAN 中继：BMS 出的 ChgCan → charger 的 can_rx；过滤 charger 不收的 ID。 */
static void mirror_can_bms_to_charger(const ChgCan *can, uint8_t n)
{
    ChrTx tx;
    uint8_t i;
    for (i = 0; i < n; i++) {
        if (can[i].id == CHG_CAN_BCL || can[i].id == CHG_CAN_BRO ||
            can[i].id == CHG_CAN_BSM || can[i].id == CHG_CAN_BST ||
            can[i].id == CHG_CAN_BAT) {
            chr_link_can_rx(&g_chr, &can[i], &g_stat, &tx);
        }
    }
}

static void mirror_can_charger_to_bms(const ChgCan *can, uint8_t n)
{
    uint8_t i;
    for (i = 0; i < n; i++) {
        if (can[i].id == CHG_CAN_CML || can[i].id == CHG_CAN_CRO ||
            can[i].id == CHG_CAN_CCS || can[i].id == CHG_CAN_CST) {
            bms_link_can_rx(&g_bms, &can[i]);
        }
    }
}

/* 字节中继（SIF/RS-485）：把 frame[] 按 byte 喂给对端 rx_byte。
 * 首字节 gap=200 ms 表示空闲，随后每字节 DT_MS。返回对端的 reply（BmsTx/ChrTx）。 */
static void mirror_byte_bms_to_charger(const uint8_t *frame, uint8_t n,
                                        uint8_t bat_frame, const uint8_t *bat, uint8_t bat_n,
                                        ChrTx *reply)
{
    uint8_t i;
    memset(reply, 0, sizeof *reply);
    for (i = 0; i < n; i++) {
        chr_link_rx_byte(&g_chr, frame[i], i == 0 ? 200u : DT_MS, &g_stat, reply);
    }
    if (bat_frame) {
        for (i = 0; i < bat_n; i++) {
            chr_link_rx_byte(&g_chr, bat[i], i == 0 ? 200u : DT_MS, &g_stat, reply);
        }
    }
}

static void mirror_byte_charger_to_bms(const uint8_t *frame, uint8_t n, BmsTx *reply)
{
    uint8_t i;
    memset(reply, 0, sizeof *reply);
    for (i = 0; i < n; i++) {
        bms_link_rx_byte(&g_bms, frame[i], i == 0 ? 200u : DT_MS, &g_ctrl, reply);
    }
}

/* 推进一拍：根据端口选择 byte 或 CAN 中继。返回 charger 的 act。
 *
 * 电池仿真：本拍 chr_link_tick 决定 act.i_out_01a 后，按该电流推一拍
 * 电池，端电压作为下一拍 g_stat.v_out_01v（即下一拍 charger 在 CCS/
 * 0x03 stat 里看到的输出电压）。这一拍延迟是单 tick，对测试无影响
 * ——跑几十拍后 CCS 自然收敛到 OCV 量级。 */
static void step(uint8_t port, uint16_t *act_i_out)
{
    BmsTx bms_tx;
    ChrTx chr_tx;
    bms_link_tick(&g_bms, &g_ctrl, DT_MS, &bms_tx);
    chr_link_tick(&g_chr, &g_stat, DT_MS, &chr_tx);
    if (port == CHG_PORT_CAN) {
        mirror_can_bms_to_charger(bms_tx.can, bms_tx.can_n);
        mirror_can_charger_to_bms(chr_tx.can, chr_tx.can_n);
    } else {
        /* SIF/RS-485：把 BMS 字节喂给 charger，charger 的 reply 字节再喂给 BMS。 */
        ChrTx chr_reply;
        BmsTx bms_reply;
        uint8_t send_bat = (port == CHG_PORT_SIF && bms_tx.bat_len > 0u);
        mirror_byte_bms_to_charger(bms_tx.frame, bms_tx.frame_len,
                                   send_bat, bms_tx.bat_frame, bms_tx.bat_len,
                                   &chr_reply);
        mirror_byte_charger_to_bms(chr_reply.frame, chr_reply.frame_len, &bms_reply);
        (void)bms_reply;
    }
    {
        uint16_t v_term;
        uint16_t i_in = (chr_tx.act.enable && chr_tx.act.i_out_01a > 0u
                         ? chr_tx.act.i_out_01a : 0u);
        sim_battery_tick(&g_bat_sim, DT_MS, i_in);
        /* 充电器是 CC/CV 源：实测输出电压 = min(电池端电压, 请求电压)。
         * 电池 OCV 在充电过程上升，10 s 长 RUN 可能涨到超过 v_req——这是
         * CV 模式的触发点；harness 在这里把 v_out 钳到 v_req。 */
        v_term = sim_battery_v_term_01v(&g_bat_sim);
        if (v_term > g_ctrl.v_req_01v) v_term = g_ctrl.v_req_01v;
        g_stat.v_out_01v = v_term;
        /* 状态帧带的是已经闭合的功率级。本拍给定要到下一拍的 CCS / 0x03 才写出去。 */
        g_stat.outputting = chr_tx.act.enable;
        g_stat.i_out_01a = chr_tx.act.enable ? chr_tx.act.i_out_01a : 0u;
    }
    *act_i_out = chr_tx.act.i_out_01a;
    g_tick_v = chr_tx.act.v_target_01v;
}

/* 跑一个端口的完整序列。返回 0=ok，1=fail。 */
/* T_loss 测试：进 RUN 后抑制充电机→BMS 的 CCS/CML/CRO，BMS 应该在
 * 到 CHG_T_LOSS_MS（1200 ms）自动 STOP。不满 CHG_T_LOSS_NOM_MS 不判超时。
 *
 * 「抑制」= 在 step() 里跳过 mirror_can_charger_to_bms（CAN）或
 * mirror_byte_charger_to_bms（其它口）。 */
static int run_timeout_port(uint8_t port)
{
    ChgCtrl ctrl;
    uint32_t t;
    uint16_t act_i;

    memset(&ctrl, 0, sizeof ctrl);
    ctrl.allow     = 1;
    ctrl.v_req_01v = 840;
    ctrl.i_req_01a = 400;

    bms_link_init(&g_bms, port);
    chr_link_init(&g_chr, port, &g_cap);
    bms_link_set_bat(&g_bms, &g_bat);
    g_ctrl = ctrl;

    /* 先把两边带到 RUN */
    for (t = 0; t < 200u; t++) {
        step(port, &act_i);
        if (g_bms.phase == CHG_PHASE_RUN && g_chr.phase == CHG_PHASE_RUN) break;
        if (port != CHG_PORT_SIF && g_bms.phase == CHG_PHASE_RUN && !g_ctrl.allow) {
            g_ctrl = ctrl;
        }
    }
    if (g_bms.phase != CHG_PHASE_RUN || g_chr.phase != CHG_PHASE_RUN) {
        fprintf(stderr, "FAIL timeout[%u]: handshake bms=%u chr=%u\n",
                port, g_bms.phase, g_chr.phase);
        return 1;
    }

    /* 阻断中继。时间零点是最后一帧已经交换完的时刻，两边的累计清零后再计。
     * 900 ms 和 1000 ms 仍出流；1200 ms 输出为 0。1000 ms 不得已经停。 */
    g_ctrl = ctrl;
    g_chr.loss_ms = 0;
    g_bms.loss_ms = 0;
    {
        uint32_t elapsed = 0;
        uint8_t saw_900 = 0;
        uint8_t saw_1000 = 0;
        uint8_t saw_1200 = 0;
        for (t = 0; t < 24u; t++) {  /* 24 × 50 ms = 1200 ms */
            BmsTx bms_tx;
            ChrTx chr_tx;
            bms_link_tick(&g_bms, &g_ctrl, DT_MS, &bms_tx);
            chr_link_tick(&g_chr, &g_stat, DT_MS, &chr_tx);
            (void)bms_tx;
            elapsed += DT_MS;
            act_i = chr_tx.act.i_out_01a;
            if (elapsed == 900u || elapsed == 1000u) {
                if (chr_tx.act.enable == 0u || act_i == 0u || g_chr.phase != CHG_PHASE_RUN) {
                    fprintf(stderr, "FAIL timeout[%u]: %u ms already off en=%u i=%u chr=%u\n",
                            port, elapsed, chr_tx.act.enable, act_i, g_chr.phase);
                    return 1;
                }
                if (port != CHG_PORT_SIF && g_bms.phase != CHG_PHASE_RUN) {
                    fprintf(stderr, "FAIL timeout[%u]: %u ms BMS phase=%u\n",
                            port, elapsed, g_bms.phase);
                    return 1;
                }
                if (elapsed == 900u) {
                    saw_900 = 1u;
                } else {
                    saw_1000 = 1u;
                }
            }
            if (elapsed == 1200u) {
                if (chr_tx.act.enable != 0u || act_i != 0u) {
                    fprintf(stderr, "FAIL timeout[%u]: 1200 ms still out en=%u i=%u\n",
                            port, chr_tx.act.enable, act_i);
                    return 1;
                }
                if (port != CHG_PORT_SIF && g_bms.phase != CHG_PHASE_STOP) {
                    fprintf(stderr, "FAIL timeout[%u]: 1200 ms BMS phase=%u\n",
                            port, g_bms.phase);
                    return 1;
                }
                if (port == CHG_PORT_SIF && g_chr.phase != CHG_PHASE_STOP) {
                    fprintf(stderr, "FAIL timeout[%u]: 1200 ms SIF phase=%u\n",
                            port, g_chr.phase);
                    return 1;
                }
                saw_1200 = 1u;
            }
        }
        if (!saw_900 || !saw_1000 || !saw_1200) {
            fprintf(stderr, "FAIL timeout[%u]: missed sample\n", port);
            return 1;
        }
    }

    printf("[port=%u t_loss] ok\n", (unsigned)port);
    return 0;
}

/* 两侧同时采样物理线。SIF：down=1 表示拉低。 */
static void note_line(uint8_t port, uint8_t down, uint32_t dt_ms)
{
    if (port == CHG_PORT_SIF) {
        bms_link_set_sif_state(&g_bms, down ? 0u : 1u, dt_ms);
        chr_link_set_sif_state(&g_chr, down ? 0u : 1u, dt_ms);
    } else if (port == CHG_PORT_CAN) {
        bms_link_set_bus_off(&g_bms, down, dt_ms);
        chr_link_set_bus_off(&g_chr, down, dt_ms);
    } else {
        bms_link_set_485_short(&g_bms, down, dt_ms);
        chr_link_set_485_short(&g_chr, down, dt_ms);
    }
}

static int handshake_to_run_bat(uint8_t port, const ChgCtrl *run, int use_bat)
{
    uint32_t t;
    uint16_t act_i;
    ChgCtrl hs;

    memset(&hs, 0, sizeof hs);
    hs.v_req_01v = run->v_req_01v;
    hs.i_req_01a = run->i_req_01a;
    hs.allow = (uint8_t)(port == CHG_PORT_SIF);
    bms_link_init(&g_bms, port);
    chr_link_init(&g_chr, port, &g_cap);
    if (use_bat) {
        bms_link_set_bat(&g_bms, &g_bat);
    }
    g_ctrl = hs;
    for (t = 0; t < 200u; t++) {
        step(port, &act_i);
        if (g_bms.phase == CHG_PHASE_RUN && g_chr.phase == CHG_PHASE_RUN) {
            break;
        }
        if (port != CHG_PORT_SIF && g_bms.phase == CHG_PHASE_RUN && !g_ctrl.allow) {
            g_ctrl = *run;
        }
    }
    if (g_bms.phase != CHG_PHASE_RUN || g_chr.phase != CHG_PHASE_RUN) {
        return 1;
    }
    g_ctrl = *run;
    for (t = 0; t < 8u; t++) {
        step(port, &act_i);
    }
    if (act_i == 0u) {
        return 1;
    }
    /* 上一拍已经出流，这一拍的状态应能被 BMS 读到。一线通没有状态帧。 */
    if (port != CHG_PORT_SIF &&
        (g_bms.stat.outputting != 1u || g_bms.stat.i_out_01a == 0u)) {
        return 1;
    }
    return 0;
}

static int handshake_to_run(uint8_t port, const ChgCtrl *run)
{
    return handshake_to_run_bat(port, run, 1);
}

/* §10 验收 15 的软件在环：两侧同时看到线故障。50 ms 不出停，
 * 再 50 ms 必须两侧都停并且电流为 0。此后中继仍在跑，帧不得把电流拉回去。
 * 线恢复后按原会话规则重新握手，电流回来。 */
static int run_line_fault(uint8_t port)
{
    ChgCtrl run;
    ChgCtrl idle;
    uint16_t act_i = 0;
    uint32_t t;

    memset(&g_cap, 0, sizeof g_cap);
    g_cap.v_max_01v = 840;
    g_cap.v_min_01v = 700;
    g_cap.i_max_01a = 500;
    g_cap.i_min_01a = 5;
    memset(&run, 0, sizeof run);
    run.allow = 1;
    run.v_req_01v = 840;
    run.i_req_01a = 400;
    idle = run;
    idle.allow = 0;
    idle.i_req_01a = 0;

    if (handshake_to_run(port, &run) != 0) {
        fprintf(stderr, "FAIL line[%u]: handshake bms=%u chr=%u\n",
                port, g_bms.phase, g_chr.phase);
        return 1;
    }

    note_line(port, 1, 50);
    step(port, &act_i);
    if (g_bms.phase != CHG_PHASE_RUN || g_chr.phase != CHG_PHASE_RUN || act_i == 0u) {
        fprintf(stderr, "FAIL line[%u]: 50 ms tripped bms=%u chr=%u i=%u\n",
                port, g_bms.phase, g_chr.phase, act_i);
        return 1;
    }

    note_line(port, 1, 50);
    step(port, &act_i);
    if (g_bms.line_fault != 1u || g_chr.line_fault != 1u ||
        g_bms.phase != CHG_PHASE_STOP || g_chr.phase != CHG_PHASE_STOP ||
        act_i != 0u) {
        fprintf(stderr, "FAIL line[%u]: 100 ms bms=%u/%u chr=%u/%u i=%u\n",
                port, g_bms.phase, g_bms.line_fault,
                g_chr.phase, g_chr.line_fault, act_i);
        return 1;
    }

    /* 线还断着，中继继续。请求仍是非零电流。 */
    g_ctrl = run;
    note_line(port, 1, 50);
    for (t = 0; t < 4u; t++) {
        step(port, &act_i);
        if (act_i != 0u || g_bms.phase != CHG_PHASE_STOP ||
            g_chr.phase != CHG_PHASE_STOP) {
            fprintf(stderr, "FAIL line[%u]: resumed while down i=%u bms=%u chr=%u\n",
                    port, act_i, g_bms.phase, g_chr.phase);
            return 1;
        }
    }

    note_line(port, 0, 10);
    g_ctrl = idle;
    for (t = 0; t < 4u; t++) {
        step(port, &act_i);
    }
    if (g_bms.force_stop != 0u || act_i != 0u) {
        fprintf(stderr, "FAIL line[%u]: after recover force=%u i=%u\n",
                port, g_bms.force_stop, act_i);
        return 1;
    }

    /* 请求电流为 0 只会停在 STOP。重新握手必须给出非零电流。
     * CAN/485 先允许=0，等 BMS 进 RUN 再允许=1。 */
    g_ctrl = run;
    if (port != CHG_PORT_SIF) {
        g_ctrl.allow = 0;
    }
    for (t = 0; t < 200u; t++) {
        step(port, &act_i);
        if (g_bms.phase == CHG_PHASE_RUN && g_chr.phase == CHG_PHASE_RUN) {
            break;
        }
        if (port != CHG_PORT_SIF && g_bms.phase == CHG_PHASE_RUN && !g_ctrl.allow) {
            g_ctrl = run;
        }
    }
    if (g_bms.phase != CHG_PHASE_RUN || g_chr.phase != CHG_PHASE_RUN) {
        fprintf(stderr, "FAIL line[%u]: re-HS bms=%u chr=%u\n",
                port, g_bms.phase, g_chr.phase);
        return 1;
    }
    for (t = 0; t < 8u; t++) {
        step(port, &act_i);
    }
    if (act_i == 0u) {
        fprintf(stderr, "FAIL line[%u]: no current after re-HS\n", port);
        return 1;
    }
    printf("[port=%u line] ok\n", (unsigned)port);
    return 0;
}

/* §10 验收 14：充电机 fault=1。三口输出立刻为 0。
 * CAN/485 把 CST 或 0x03 故障位送到 BMS，BMS 进入 STOP。一线通没有状态帧。 */
static int run_charger_fault(uint8_t port)
{
    ChgCtrl run;
    uint16_t act_i = 0;
    uint32_t t;

    memset(&g_cap, 0, sizeof g_cap);
    g_cap.v_max_01v = 840;
    g_cap.v_min_01v = 700;
    g_cap.i_max_01a = 500;
    g_cap.i_min_01a = 5;
    memset(&run, 0, sizeof run);
    run.allow = 1;
    run.v_req_01v = 840;
    run.i_req_01a = 400;
    if (handshake_to_run(port, &run) != 0) {
        fprintf(stderr, "FAIL fault[%u]: handshake\n", port);
        return 1;
    }
    g_stat.fault = 1;
    {
        int seen = 0;
        /* 16×50 ms = 800 ms，短于 T_loss，避免用通信超时冒充故障帧。 */
        for (t = 0; t < 16u; t++) {
            step(port, &act_i);
            if (act_i != 0u) {
                continue;
            }
            if (port == CHG_PORT_SIF ||
                (g_bms.phase == CHG_PHASE_STOP && g_bms.stat.fault == 1u)) {
                seen = 1;
                break;
            }
        }
        g_stat.fault = 0;
        if (!seen) {
            fprintf(stderr, "FAIL fault[%u]: t=%u bms=%u stat.fault=%u i=%u\n",
                    port, (unsigned)t, g_bms.phase, g_bms.stat.fault, act_i);
            return 1;
        }
    }
    printf("[port=%u fault] ok\n", (unsigned)port);
    return 0;
}

static int run_port(uint8_t port)
{
    ChgCtrl hs_ctrl = {0};
    ChgCtrl run_ctrl = {0};
    ChgCtrl stop_ctrl = {0};
    uint32_t t;
    uint16_t act_i;

    /* 握手期 BMS 已发非零电流请求（v_req/i_req 不为 0），允许位分端口：
     *   SIF    → 握手期即 allow=1（与现有 test_sif 一致）
     *   CAN/RS → 握手期 allow=0 → CRO/0x02 翻转 → promote → 再翻 allow=1 */
    if (port == CHG_PORT_SIF) {
        hs_ctrl.allow     = 1;
        hs_ctrl.v_req_01v = 840;
        hs_ctrl.i_req_01a = 100;
    } else {
        hs_ctrl.v_req_01v = 840;
        hs_ctrl.i_req_01a = 100;
    }

    run_ctrl.allow     = 1;
    run_ctrl.v_req_01v = 840;
    run_ctrl.i_req_01a = 400;

    stop_ctrl.stop = 1;

    bms_link_init(&g_bms, port);
    chr_link_init(&g_chr, port, &g_cap);
    bms_link_set_bat(&g_bms, &g_bat);
    g_ctrl = hs_ctrl;

    /* 1. 握手直到双方 RUN。SIF 用 3 帧 0x01；CAN/RS 在 BMS 进 RUN 后必须
     *    翻 allow=1，否则充电机因 BRO=0x00 不进 RUN。 */
    for (t = 0; t < 200u; t++) {
        step(port, &act_i);
        if (g_bms.phase == CHG_PHASE_RUN && g_chr.phase == CHG_PHASE_RUN) break;
        if (port != CHG_PORT_SIF && g_bms.phase == CHG_PHASE_RUN && !g_ctrl.allow) {
            g_ctrl = run_ctrl;
        }
    }
    if (g_bms.phase != CHG_PHASE_RUN || g_chr.phase != CHG_PHASE_RUN) {
        fprintf(stderr, "FAIL [port=%u] handshake RUN bms=%u chr=%u t=%u\n",
                port, g_bms.phase, g_chr.phase, (unsigned)t);
        return 1;
    }
    /* settle：act_i 来自上一次 step 内的 chr_link_tick，但字节/CAN 中继
     * 在本 step 末尾完成。再过几拍让两边状态机反映新输入。 */
    for (t = 0; t < 8u; t++) step(port, &act_i);

    /* SIF 握手期用了 hs_ctrl（i=100），现在抬到满请求。 */
    if (port == CHG_PORT_SIF) {
        g_ctrl = run_ctrl;
        for (t = 0; t < 8u; t++) step(port, &act_i);
    }

    /* cap 钳位：want_i = min(req, cap.i_max)。cap 设到 300 < 400 让钳位真的生效 */
    {
        uint16_t want_i = (uint16_t)(run_ctrl.i_req_01a > g_cap.i_max_01a
                                     ? g_cap.i_max_01a : run_ctrl.i_req_01a);
        if (act_i != want_i) {
            fprintf(stderr, "FAIL [port=%u] act.i_out %u want %u\n",
                    port, act_i, want_i);
            return 1;
        }
    }

    /* 3. 维持 RUN 2 s */
    g_ctrl = run_ctrl;
    for (t = 0; t < RUN_TICKS; t++) {
        step(port, &act_i);
    }
    if (g_bms.phase != CHG_PHASE_RUN || g_chr.phase != CHG_PHASE_RUN) {
        fprintf(stderr, "FAIL [port=%u] stable RUN lost bms=%u chr=%u act=%u\n",
                port, g_bms.phase, g_chr.phase, act_i);
        return 1;
    }

    /* 4. 暂停：pause=1，链路透传 BSM/0x01 字节 pause 位 */
    g_ctrl = run_ctrl;
    g_ctrl.pause = 1;
    for (t = 0; t < PAUSE_TICKS; t++) {
        step(port, &act_i);
    }
    if (act_i != 0u) {
        fprintf(stderr, "FAIL [port=%u] pause act_i=%u want 0\n", port, act_i);
        return 1;
    }

    /* 5. 恢复。第一拍非零电流不超过 20.0 A，电压目标为请求减 1.0 V。 */
    g_ctrl = run_ctrl;
    {
        uint16_t first_i = 0u;
        uint16_t first_v = 0u;
        for (t = 0; t < PAUSE_TICKS; t++) {
            step(port, &act_i);
            if (first_i == 0u && act_i > 0u) {
                first_i = act_i;
                first_v = g_tick_v;
            }
        }
        if (first_i == 0u || first_i > 200u) {
            fprintf(stderr, "FAIL [port=%u] resume first i=%u\n", port, first_i);
            return 1;
        }
        if (run_ctrl.v_req_01v > 10u && first_v != (uint16_t)(run_ctrl.v_req_01v - 10u)) {
            fprintf(stderr, "FAIL [port=%u] resume first v=%u want %u\n",
                    port, first_v, (unsigned)(run_ctrl.v_req_01v - 10u));
            return 1;
        }
    }

    /* 6. 停止：stop=1 → 双方 STOP */
    g_ctrl = stop_ctrl;
    for (t = 0; t < STOP_TICKS; t++) {
        step(port, &act_i);
        if (g_bms.phase == CHG_PHASE_STOP && g_chr.phase == CHG_PHASE_STOP) break;
    }
    if (g_bms.phase != CHG_PHASE_STOP || g_chr.phase != CHG_PHASE_STOP) {
        fprintf(stderr, "FAIL [port=%u] STOP bms=%u chr=%u\n",
                port, g_bms.phase, g_chr.phase);
        return 1;
    }

    /* 7. 重握：清 stop，给非零请求。SIF 直接 allow=1；CAN/RS 先 allow=0 走到
     *    BMS 进 RUN，再翻 allow=1 让充电机进 RUN。 */
    if (port == CHG_PORT_SIF) {
        g_ctrl = run_ctrl;
    } else {
        g_ctrl = hs_ctrl;
    }
    for (t = 0; t < 200u; t++) {
        step(port, &act_i);
        if (g_bms.phase == CHG_PHASE_RUN && g_chr.phase == CHG_PHASE_RUN) break;
        if (port != CHG_PORT_SIF && g_bms.phase == CHG_PHASE_RUN && !g_ctrl.allow) {
            g_ctrl = run_ctrl;
        }
    }
    if (g_bms.phase != CHG_PHASE_RUN || g_chr.phase != CHG_PHASE_RUN) {
        fprintf(stderr, "FAIL [port=%u] re-HS RUN bms=%u chr=%u\n",
                port, g_bms.phase, g_chr.phase);
        return 1;
    }
    for (t = 0; t < 8u; t++) step(port, &act_i);
    if (act_i == 0u) {
        fprintf(stderr, "FAIL [port=%u] re-HS act_i=0 (no output after re-handshake)\n",
                port);
        return 1;
    }

    printf("[port=%u] ok\n", (unsigned)port);
    return 0;
}

/* 全流程电池仿真测试：握手 → 长时间 RUN（电池持续充电）→ 暂停 →
 * 恢复 → 停止。三端口各跑一遍。验证：
 *   - SOC 单调上升（充电 10 s @ 30 A 至少涨一点）
 *   - t_cell 不下降（充电会升温，热模型正确）
 *   - g_stat.v_out_01v ≤ v_req_01v（充电器是恒压源）
 *   - g_stat.v_out_01v < v_req_01v 至少一帧（OCV 钳住输出）
 *
 * 与 run_port() 不同：RUN 阶段 200 tick（10 s）让 SOC / 温度显著变化；
 * 然后 PAUSE 期间断言 SOC 不再上升（电流=0，电池静置）；RESUME 验证
 * 恢复后又有电流。 */
static int run_full_charging_with_battery(uint8_t port)
{
    ChgCtrl ctrl = {0};
    uint16_t soc0, t0;
    uint16_t soc_pause0, soc_resume1;
    uint16_t act_i;
    uint32_t t;
    int saw_ocv_clamp = 0;

    /* cap 设到 500（>i_req=400），不被 cap 钳位，纯看电池钳位 */
    memset(&g_cap, 0, sizeof g_cap);
    g_cap.v_max_01v = 840;
    g_cap.v_min_01v = 700;
    g_cap.i_max_01a = 500;
    g_cap.i_min_01a = 5;

    ctrl.allow     = 1;
    ctrl.v_req_01v = 840;
    ctrl.i_req_01a = 400;

    sim_battery_init(&g_bat_sim, /*cap_01ah*/2000, /*cells_s*/20,
                     /*cells_p*/1, /*t_ambient_c*/25);
    soc0 = sim_battery_soc_01pct(&g_bat_sim);
    t0   = sim_battery_t_max_c(&g_bat_sim);

    bms_link_init(&g_bms, port);
    chr_link_init(&g_chr, port, &g_cap);
    bms_link_set_bat(&g_bms, &g_bat);
    g_ctrl = ctrl;

    /* 1. 握手直到 RUN */
    for (t = 0; t < 400u; t++) {
        step(port, &act_i);
        if (g_bms.phase == CHG_PHASE_RUN && g_chr.phase == CHG_PHASE_RUN) break;
    }
    if (g_bms.phase != CHG_PHASE_RUN || g_chr.phase != CHG_PHASE_RUN) {
        fprintf(stderr, "FAIL [port=%u bat_full] HS bms=%u chr=%u\n",
                port, g_bms.phase, g_chr.phase);
        return 1;
    }
    /* settle 几拍让 g_stat.v_out_01v 从 0 走到 OCV 量级 */
    for (t = 0; t < 8u; t++) step(port, &act_i);

    /* 2. 持续 RUN 10 s。期间扫 OCV 钳位与 SOC 增量。 */
    g_ctrl = ctrl;
    for (t = 0; t < LONG_RUN_TICKS; t++) {
        step(port, &act_i);
        if (g_stat.v_out_01v > 0u && g_stat.v_out_01v < ctrl.v_req_01v) {
            saw_ocv_clamp = 1;
        }
    }
    {
        uint16_t soc_run_end = sim_battery_soc_01pct(&g_bat_sim);
        if (soc_run_end <= soc0) {
            fprintf(stderr, "FAIL [port=%u bat_full] SOC did not rise %u -> %u\n",
                    port, soc0, soc_run_end);
            return 1;
        }
        if (sim_battery_t_max_c(&g_bat_sim) < t0) {
            fprintf(stderr, "FAIL [port=%u bat_full] t_cell decreased %d -> %d\n",
                    port, t0, sim_battery_t_max_c(&g_bat_sim));
            return 1;
        }
        if (g_stat.v_out_01v > ctrl.v_req_01v) {
            fprintf(stderr, "FAIL [port=%u bat_full] v_out %u > v_req %u\n",
                    port, g_stat.v_out_01v, ctrl.v_req_01v);
            return 1;
        }
        if (!saw_ocv_clamp) {
            fprintf(stderr, "FAIL [port=%u bat_full] OCV never clamped v_out (v_out=%u v_req=%u)\n",
                    port, g_stat.v_out_01v, ctrl.v_req_01v);
            return 1;
        }
    }

    /* 3. 暂停：pause=1 期间 SOC 不应再涨 */
    g_ctrl = ctrl;
    g_ctrl.pause = 1;
    for (t = 0; t < PAUSE_TICKS; t++) step(port, &act_i);
    soc_pause0 = sim_battery_soc_01pct(&g_bat_sim);
    if (act_i != 0u) {
        fprintf(stderr, "FAIL [port=%u bat_full] pause act_i=%u want 0\n", port, act_i);
        return 1;
    }

    /* 4. 恢复：act_i 应恢复，SOC 应继续涨 */
    g_ctrl = ctrl;
    for (t = 0; t < PAUSE_TICKS; t++) step(port, &act_i);
    soc_resume1 = sim_battery_soc_01pct(&g_bat_sim);
    if (act_i == 0u) {
        fprintf(stderr, "FAIL [port=%u bat_full] resume act_i=0\n", port);
        return 1;
    }
    if (soc_resume1 <= soc_pause0) {
        fprintf(stderr, "FAIL [port=%u bat_full] resume SOC %u <= pause SOC %u\n",
                port, soc_resume1, soc_pause0);
        return 1;
    }

    /* 5. 停止 */
    {
        ChgCtrl stop_ctrl = {0};
        stop_ctrl.stop = 1;
        g_ctrl = stop_ctrl;
    }
    for (t = 0; t < STOP_TICKS; t++) {
        step(port, &act_i);
        if (g_bms.phase == CHG_PHASE_STOP && g_chr.phase == CHG_PHASE_STOP) break;
    }
    if (g_bms.phase != CHG_PHASE_STOP || g_chr.phase != CHG_PHASE_STOP) {
        fprintf(stderr, "FAIL [port=%u bat_full] STOP bms=%u chr=%u\n",
                port, g_bms.phase, g_chr.phase);
        return 1;
    }

    printf("[port=%u bat_full] ok\n", (unsigned)port);
    return 0;
}

/* 不调用 set_bat。三口仍按 0x01 出流。chr_link_bat 成功时返回 0，这里必须是失败。 */
static int run_without_bat(uint8_t port)
{
    ChgCtrl run;
    uint16_t act_i = 0;
    ChgBat out;
    int bat_rc;

    memset(&g_cap, 0, sizeof g_cap);
    g_cap.v_max_01v = 840;
    g_cap.v_min_01v = 700;
    g_cap.i_max_01a = 500;
    g_cap.i_min_01a = 5;
    memset(&run, 0, sizeof run);
    run.allow = 1;
    run.v_req_01v = 840;
    run.i_req_01a = 400;
    if (handshake_to_run_bat(port, &run, 0) != 0) {
        fprintf(stderr, "FAIL nobat[%u]: handshake\n", port);
        return 1;
    }
    step(port, &act_i);
    bat_rc = chr_link_bat(&g_chr, &out);
    if (act_i == 0u || bat_rc != -1) {
        fprintf(stderr, "FAIL nobat[%u]: i=%u bat_rc=%d\n", port, act_i, bat_rc);
        return 1;
    }
    printf("[port=%u nobat] ok\n", (unsigned)port);
    return 0;
}

/* 辅助电源欠压 50 ms 仍出流，再 50 ms 充电机输出为 0。 */
static int run_aux_uv(uint8_t port)
{
    ChgCtrl run;
    uint16_t act_i = 0;

    memset(&g_cap, 0, sizeof g_cap);
    g_cap.v_max_01v = 840;
    g_cap.v_min_01v = 700;
    g_cap.i_max_01a = 500;
    g_cap.i_min_01a = 5;
    memset(&run, 0, sizeof run);
    run.allow = 1;
    run.v_req_01v = 840;
    run.i_req_01a = 400;
    if (handshake_to_run(port, &run) != 0) {
        fprintf(stderr, "FAIL aux[%u]: handshake\n", port);
        return 1;
    }
    bms_link_set_aux_uv(&g_bms, 1);
    chr_link_set_aux_uv(&g_chr, 1, 50);
    step(port, &act_i);
    if (act_i == 0u) {
        fprintf(stderr, "FAIL aux[%u]: 50 ms already off\n", port);
        return 1;
    }
    bms_link_set_aux_uv(&g_bms, 1);
    chr_link_set_aux_uv(&g_chr, 1, 50);
    step(port, &act_i);
    if (act_i != 0u || g_chr.aux_fault != 1u || g_bms.phase != CHG_PHASE_STOP) {
        fprintf(stderr, "FAIL aux[%u]: i=%u aux=%u bms=%u\n",
                port, act_i, g_chr.aux_fault, g_bms.phase);
        return 1;
    }
    bms_link_set_aux_uv(&g_bms, 0);
    chr_link_set_aux_uv(&g_chr, 0, 10);
    printf("[port=%u aux] ok\n", (unsigned)port);
    return 0;
}

/* 充电机仍在 OFF、尚未 tick 时，连续 3 帧合法 0x01 不计入握手，输出保持 0。 */
static int run_meter_before_arm(void)
{
    ChrTx tx;
    ChgStat st;
    uint8_t n;
    uint8_t k;
    static const uint8_t run[] = {
        0xA5, 0x5A, 0x01, 0x01, 0x06, 0x01, 0x48, 0x03, 0x90, 0x01, 0x00, 0x7C, 0x31
    };

    memset(&g_cap, 0, sizeof g_cap);
    g_cap.v_max_01v = 840;
    g_cap.v_min_01v = 700;
    g_cap.i_max_01a = 500;
    g_cap.i_min_01a = 5;
    memset(&st, 0, sizeof st);
    chr_link_init(&g_chr, CHG_PORT_SIF, &g_cap);
    memset(&tx, 0, sizeof tx);
    for (k = 0; k < 3u; k++) {
        for (n = 0; n < (uint8_t)sizeof run; n++) {
            chr_link_rx_byte(&g_chr, run[n], n == 0u ? 200u : 1u, &st, &tx);
        }
    }
    if (g_chr.phase != CHG_PHASE_OFF || g_chr.sif_got != 0u || tx.act.enable != 0u) {
        fprintf(stderr, "FAIL meter_off: phase=%u got=%u en=%u\n",
                g_chr.phase, g_chr.sif_got, tx.act.enable);
        return 1;
    }
    chr_link_tick(&g_chr, &st, 0, &tx);
    if (g_chr.phase != CHG_PHASE_HS || g_chr.sif_got != 0u || tx.act.enable != 0u) {
        fprintf(stderr, "FAIL meter_off tick: phase=%u got=%u en=%u\n",
                g_chr.phase, g_chr.sif_got, tx.act.enable);
        return 1;
    }
    printf("[port=1 meter_off] ok\n");
    return 0;
}

/* 一线通上的非充电帧不改变输出。 */
static int run_meter_noise(void)
{
    ChgCtrl run;
    uint16_t act_i = 0;
    uint16_t before;
    ChrTx tx;
    uint8_t i;

    memset(&g_cap, 0, sizeof g_cap);
    g_cap.v_max_01v = 840;
    g_cap.v_min_01v = 700;
    g_cap.i_max_01a = 500;
    g_cap.i_min_01a = 5;
    memset(&run, 0, sizeof run);
    run.allow = 1;
    run.v_req_01v = 840;
    run.i_req_01a = 400;
    if (handshake_to_run(CHG_PORT_SIF, &run) != 0) {
        fprintf(stderr, "FAIL meter: handshake\n");
        return 1;
    }
    step(CHG_PORT_SIF, &act_i);
    before = act_i;
    for (i = 0; i < 8u; i++) {
        chr_link_rx_byte(&g_chr, 0x11u, 1, &g_stat, &tx);
    }
    step(CHG_PORT_SIF, &act_i);
    if (before == 0u || act_i != before || g_chr.phase != CHG_PHASE_RUN) {
        fprintf(stderr, "FAIL meter: before=%u after=%u phase=%u\n",
                before, act_i, g_chr.phase);
        return 1;
    }
    printf("[port=1 meter] ok\n");
    return 0;
}

int main(void)
{
    memset(&g_cap, 0, sizeof g_cap);
    g_cap.v_max_01v = 840;
    g_cap.v_min_01v = 700;
    /* cap 故意设到 300（小于 run_ctrl.i_req_01a=400）让钳位真的生效 */
    g_cap.i_max_01a = 300;
    g_cap.i_min_01a = 5;

    memset(&g_bat, 0, sizeof g_bat);
    g_bat.valid         = 1;
    g_bat.chemistry     = CHG_CHEM_NCM;
    g_bat.rated_ah_01ah = 200;
    g_bat.rated_v_01v   = 730;
    g_bat.v_allow_01v   = 840;
    g_bat.i_allow_01a   = 400;
    g_bat.t_allow_c     = 55;
    g_bat.remain_min    = 30;

    memset(&g_stat, 0, sizeof g_stat);

    if (run_port(CHG_PORT_SIF) != 0) return 1;
    if (run_port(CHG_PORT_CAN) != 0) return 1;
    if (run_port(CHG_PORT_485) != 0) return 1;
    /* §4 T_loss：阻断中继 1.25 s。三口充电机输出为 0。
     * CAN/485 的 BMS 进入 STOP。一线通 BMS 不累计 loss_ms，停流在充电机。 */
    if (run_timeout_port(CHG_PORT_SIF) != 0) return 1;
    if (run_timeout_port(CHG_PORT_CAN) != 0) return 1;
    if (run_timeout_port(CHG_PORT_485) != 0) return 1;
    /* §10 验收 15 / 14：软件在环，不是台架。线故障和充电机故障要两侧同时成立。 */
    if (run_line_fault(CHG_PORT_SIF) != 0) return 1;
    if (run_line_fault(CHG_PORT_CAN) != 0) return 1;
    if (run_line_fault(CHG_PORT_485) != 0) return 1;
    if (run_charger_fault(CHG_PORT_SIF) != 0) return 1;
    if (run_charger_fault(CHG_PORT_CAN) != 0) return 1;
    if (run_charger_fault(CHG_PORT_485) != 0) return 1;
    if (run_without_bat(CHG_PORT_SIF) != 0) return 1;
    if (run_without_bat(CHG_PORT_CAN) != 0) return 1;
    if (run_without_bat(CHG_PORT_485) != 0) return 1;
    if (run_aux_uv(CHG_PORT_SIF) != 0) return 1;
    if (run_aux_uv(CHG_PORT_CAN) != 0) return 1;
    if (run_aux_uv(CHG_PORT_485) != 0) return 1;
    if (run_meter_before_arm() != 0) return 1;
    if (run_meter_noise() != 0) return 1;
    /* 软件在环：握手 + 长 RUN（电池模型充电） + 暂停 + 恢复 + 停止。
     * 三端口各跑一遍，验证 SOC / 温度 / OCV 钳位。 */
    if (run_full_charging_with_battery(CHG_PORT_SIF) != 0) return 1;
    if (run_full_charging_with_battery(CHG_PORT_CAN) != 0) return 1;
    if (run_full_charging_with_battery(CHG_PORT_485) != 0) return 1;

    printf("ok\n");
    return 0;
}