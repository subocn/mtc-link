/* 作者：苏勃(subo)
 * SPDX-License-Identifier: Apache-2.0
 */
#include "chg_bms.h"

#include <string.h>

#define T_SLOW_MS 250u
#define T_FAST_MS 50u
#define T_BST_MS 10u
#define T_GAP_MS 50u
#define T_BAT_MS 1000u
#define T_REPLY_MS 80u
#define T_LINE_DOWN_MS 100u  /* §10 验收 15: SIF 拉低 / CAN bus-off / 485 短路 */
#define T_AUX_UV_MS 100u     /* §10 验收 13: 辅助电源欠压 100 ms 内停输出 */
#define I_NEAR_01A 5u        /* 0.5 A。不超过此值算近 0；大于此值才算残余 */
#define I_ZERO_HOLD_MS 200u  /* 零输出命令连续满此时长后，新样本才判残余 */
#define I_STUCK_FLOOR_MS 500u
#define I_SLEW_01A_PER_S 500u /* 50 A/s，只用于卡住的爬升留量，不另设斜率故障 */

static uint16_t crc16(const uint8_t *data, size_t len)
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

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static int frame_pack(uint8_t msg, const uint8_t *data, uint8_t len, uint8_t *out, size_t cap)
{
    uint16_t crc;
    if (out == 0 || (len > 0u && data == 0) || cap < (size_t)len + 7u) {
        return -1;
    }
    out[0] = 0xA5;
    out[1] = 0x5A;
    out[2] = 0x01;
    out[3] = msg;
    out[4] = len;
    if (len > 0u) {
        memcpy(out + 5, data, len);
    }
    crc = crc16(out, (size_t)len + 5u);
    out[5 + len] = (uint8_t)(crc & 0xFFu);
    out[6 + len] = (uint8_t)(crc >> 8);
    return (int)len + 7;
}

static int frame_unpack(const uint8_t *in, size_t n, uint8_t *msg, uint8_t *data, uint8_t *len)
{
    uint8_t l;
    uint16_t crc;
    uint16_t got;
    if (in == 0 || msg == 0 || len == 0 || n < 7u) {
        return -1;
    }
    if (in[0] != 0xA5u || in[1] != 0x5Au || in[2] != 0x01u) {
        return -1;
    }
    l = in[4];
    if (n < (size_t)l + 7u) {
        return -1;
    }
    crc = crc16(in, (size_t)l + 5u);
    got = (uint16_t)(in[5 + l] | ((uint16_t)in[6 + l] << 8));
    if (crc != got) {
        return -1;
    }
    *msg = in[3];
    *len = l;
    if (l > 0u) {
        if (data == 0) {
            return -1;
        }
        memcpy(data, in + 5, l);
    }
    return (int)l + 7;
}

static void store_bat(const ChgBat *b, uint8_t data[28])
{
    memset(data, 0, 28);
    if (b == 0) {
        return;
    }
    data[0] = b->valid ? 0x01u : 0u;
    data[1] = (uint8_t)(b->flags & 0x3Fu);
    put_u16(data + 2, b->soc_01pct);
    put_u16(data + 4, b->v_pack_01v);
    put_u16(data + 6, b->v_cell_max_mv);
    put_u16(data + 8, b->v_cell_min_mv);
    data[10] = (uint8_t)b->t_max_c;
    data[11] = (uint8_t)b->t_min_c;
    put_u16(data + 12, b->rated_ah_01ah);
    put_u16(data + 14, b->rated_v_01v);
    data[16] = b->chemistry;
    put_u16(data + 17, b->v_allow_01v);
    put_u16(data + 19, b->i_allow_01a);
    data[21] = (uint8_t)b->t_allow_c;
    put_u16(data + 22, b->remain_min);
    data[24] = b->idx_cell_max;
    data[25] = b->idx_cell_min;
    data[26] = b->idx_temp_max;
    data[27] = b->idx_temp_min;
}

static void store_ctrl(const ChgCtrl *c, uint8_t data[6])
{
    data[0] = (uint8_t)((c->allow ? 0x01u : 0u) | (c->pause ? 0x02u : 0u) |
                        (c->stop ? 0x04u : 0u) | (c->cv ? 0x08u : 0u));
    put_u16(data + 1, c->v_req_01v);
    put_u16(data + 3, c->i_req_01a);
    data[5] = 0;
}

int chg_ctrl_pack(const ChgCtrl *c, uint8_t *out, size_t cap)
{
    uint8_t data[6];
    if (c == 0) {
        return -1;
    }
    store_ctrl(c, data);
    return frame_pack(0x01u, data, 6, out, cap);
}

static int load_cap(const uint8_t data[9], ChgCap *c)
{
    if (data == 0 || c == 0) {
        return -1;
    }
    if ((data[0] & 0xFEu) != 0u) {
        return -1;
    }
    c->ready = (uint8_t)((data[0] & 0x01u) != 0u);
    c->v_min_01v = get_u16(data + 1);
    c->v_max_01v = get_u16(data + 3);
    c->i_min_01a = get_u16(data + 5);
    c->i_max_01a = get_u16(data + 7);
    return 0;
}

static int load_stat(const uint8_t data[6], ChgStat *s)
{
    if (data == 0 || s == 0) {
        return -1;
    }
    if ((data[0] & 0xF0u) != 0u) {
        return -1;
    }
    s->outputting = (uint8_t)((data[0] & 0x01u) != 0u);
    s->fault = (uint8_t)((data[0] & 0x02u) != 0u);
    s->ac_path = (uint8_t)((data[0] & 0x04u) != 0u);
    s->dc_path = (uint8_t)((data[0] & 0x08u) != 0u);
    s->v_out_01v = get_u16(data + 1);
    s->i_out_01a = get_u16(data + 3);
    s->fault_code = data[5];
    return 0;
}

static uint16_t can_i_raw(uint16_t i_01a)
{
    /* 大于 6153.5 A 时停在 65535，避免加 4000 后回绕成小电流。 */
    if (i_01a > 65535u - 4000u) {
        return 65535u;
    }
    return (uint16_t)(i_01a + 4000u);
}

static uint16_t can_i_phys(uint16_t raw)
{
    if (raw < 4000u) {
        return 0;
    }
    return (uint16_t)(raw - 4000u);
}

static void can_clear(ChgCan *f)
{
    memset(f, 0, sizeof *f);
}

void chg_can_bcl(const ChgCtrl *c, ChgCan *f)
{
    can_clear(f);
    f->id = CHG_CAN_BCL;
    f->dlc = 5;
    put_u16(f->data, c->v_req_01v);
    put_u16(f->data + 2, can_i_raw(c->i_req_01a));
    f->data[4] = c->cv ? 0x01u : 0x02u;
}

void chg_can_bro(uint8_t allow, ChgCan *f)
{
    can_clear(f);
    f->id = CHG_CAN_BRO;
    f->dlc = 1;
    f->data[0] = allow ? 0xAAu : 0x00u;
}

void chg_can_bsm(uint8_t pause, uint8_t flags, ChgCan *f)
{
    can_clear(f);
    f->id = CHG_CAN_BSM;
    f->dlc = 7;
    /* GB/T 27930-2023 §6 表 17：bit4=1 表示未暂停；低 4 位为 BMS 上报的
     * flags（绝缘/连接器/单体过压/单体欠压）。flags 高 4 位 mask 掉。 */
    f->data[6] = (uint8_t)((flags & 0x0Fu) | (pause ? 0u : 0x10u));
}

void chg_can_bst(ChgCan *f)
{
    can_clear(f);
    f->id = CHG_CAN_BST;
    f->dlc = 4;
    f->data[0] = 0x01;
}

int chg_bat_pack(const ChgBat *b, uint8_t *out, size_t cap)
{
    uint8_t data[28];
    store_bat(b, data);
    return frame_pack(0x04u, data, 28, out, cap);
}

void chg_can_bat(const ChgBat *b, ChgCan f[4])
{
    uint8_t data[28];
    int i;
    store_bat(b, data);
    for (i = 0; i < 4; i++) {
        can_clear(&f[i]);
        f[i].id = CHG_CAN_BAT;
        f[i].dlc = 8;
        f[i].data[0] = (uint8_t)i;
        memcpy(f[i].data + 1, data + (i * 7), 7);
    }
}

static void tx_clear(BmsTx *tx, const BmsLink *l)
{
    memset(tx, 0, sizeof *tx);
    tx->phase = l->phase;
    tx->link_ok = l->link_ok;
}

static void push_can(BmsTx *tx, const ChgCan *f)
{
    if (tx->can_n < 8u) {
        tx->can[tx->can_n++] = *f;
    }
}

static int due(uint32_t *acc, uint32_t dt, uint32_t period, uint8_t *arm)
{
    if (*arm == 0u) {
        *arm = 1u;
        *acc = 0;
        return 1;
    }
    *acc += dt;
    if (*acc >= period) {
        *acc %= period;
        return 1;
    }
    return 0;
}

static ChgCtrl as_stop(const ChgCtrl *c)
{
    ChgCtrl s = *c;
    s.allow = 0;
    s.pause = 0;
    s.stop = 1;
    s.cv = 0;
    s.i_req_01a = 0;
    return s;
}

static ChgCtrl as_hs(const ChgCtrl *c)
{
    ChgCtrl s = *c;
    s.allow = 0;
    s.pause = 0;
    s.stop = 0;
    s.cv = 0;
    s.i_req_01a = 0;
    return s;
}

static void open_phase(BmsLink *l, const ChgCtrl *c)
{
    /* 线还断着时，请求电流回到 0 也不许开新会话。 */
    if (l->line_fault) {
        l->phase = CHG_PHASE_STOP;
        l->force_stop = 1u;
        return;
    }
    if (c->stop || c->i_req_01a == 0u) {
        l->force_stop = 0u;
        if (l->phase != CHG_PHASE_STOP) {
            l->phase = CHG_PHASE_STOP;
            l->sif_sent = 0;
            l->seen_ready = 0;
        }
        return;
    }
    if (l->force_stop) {
        l->phase = CHG_PHASE_STOP;
        return;
    }
    if (l->phase == CHG_PHASE_OFF || l->phase == CHG_PHASE_STOP) {
        uint8_t from_stop = (uint8_t)(l->phase == CHG_PHASE_STOP);
        l->phase = CHG_PHASE_HS;
        l->sif_sent = 0;
        if (from_stop) {
            l->seen_ready = 0u;
            l->i_alarm = CHG_I_ALARM_NONE;
            l->i_report_seen = 0u;
            l->i_low_ms = 0u;
            l->i_tx_seen = 0u;
            l->i_high_hold = 0u;
            l->i_tx_01a = 0u;
            l->i_tx_prev_01a = 0u;
        }
        l->loss_ms = 0;
    }
}

static void promote(BmsLink *l)
{
    if (l->phase != CHG_PHASE_HS || l->force_stop) {
        return;
    }
    if (l->port == CHG_PORT_SIF && l->sif_sent >= 2u) {
        l->phase = CHG_PHASE_RUN;
    } else if (l->port != CHG_PORT_SIF && l->seen_ready) {
        l->phase = CHG_PHASE_RUN;
    }
}

static const ChgCtrl *payload(BmsLink *l, const ChgCtrl *c, ChgCtrl *tmp)
{
    if (l->phase == CHG_PHASE_STOP) {
        *tmp = as_stop(c);
        return tmp;
    }
    if (l->phase == CHG_PHASE_HS) {
        *tmp = as_hs(c);
        return tmp;
    }
    return c;
}

static void remember_cap(BmsLink *l, const ChgCap *c)
{
    l->cap = *c;
    l->seen_ready = c->ready ? 1u : 0u;
}

static void remember_stat(BmsLink *l, const ChgStat *s)
{
    l->stat = *s;
    l->loss_ms = 0;
    l->link_ok = 1u;
}

static void alarm_stop(BmsLink *l, uint8_t code)
{
    if (l->i_alarm == CHG_I_ALARM_NONE) {
        l->i_alarm = code;
    }
    l->phase = CHG_PHASE_STOP;
    l->force_stop = 1u;
    l->link_ok = 0u;
    l->seen_ready = 0u;
    l->sif_sent = 0u;
    l->waiting = 0u;
}

static int in_track_window(uint16_t imax, uint16_t req)
{
    /* 近 0 卡住只看 20%–100%，两端都含。高出不走这个窗口。 */
    if (imax == 0u) {
        return 0;
    }
    if (req < imax / 5u || req > imax) {
        return 0;
    }
    return 1;
}

static uint32_t stuck_allow_ms(uint16_t req_01a)
{
    uint32_t ramp = ((uint32_t)req_01a * 1000u) / I_SLEW_01A_PER_S;

    if (ramp < I_STUCK_FLOOR_MS) {
        return I_STUCK_FLOOR_MS;
    }
    return ramp;
}

/* pause/stop/i_req 是这一样本对应的命令。CAN 用上一拍快照，RS-485 用本帧携带的 ctrl。 */
static void judge_i_sample(BmsLink *l, uint16_t i_out, uint8_t pause, uint8_t stop, uint16_t i_req)
{
    uint16_t imax;
    uint32_t sum;
    uint16_t limit;
    int zero_now;

    l->i_report_seen = 1u;
    if (l->port == CHG_PORT_SIF || l->force_stop) {
        return;
    }
    zero_now = pause || stop || i_req == 0u;
    if (zero_now && l->i_zero_cmd_ms >= I_ZERO_HOLD_MS && i_out > I_NEAR_01A) {
        /* 命令已经把会话停住时只记原因，避免把 force_stop 的清除语义反过来。 */
        if (l->phase == CHG_PHASE_STOP && l->force_stop == 0u) {
            if (l->i_alarm == CHG_I_ALARM_NONE) {
                l->i_alarm = CHG_I_ALARM_RESID;
            }
            return;
        }
        alarm_stop(l, CHG_I_ALARM_RESID);
        return;
    }
    if (l->phase != CHG_PHASE_RUN || zero_now) {
        return;
    }
    /* 还没发出过请求时不做高出。没有公告最大电流时也不做，且不消耗待判的上一帧。 */
    if (l->i_tx_seen == 0u) {
        return;
    }
    imax = l->cap.i_max_01a;
    if (imax == 0u) {
        return;
    }
    /* 新请求已经发出后，第一帧仍是改写前的功率级，按改写前的已发请求。
     * 尚未发出时 i_high_hold 为 0，仍用最近一帧已发请求。 */
    if (l->i_high_hold) {
        i_req = l->i_tx_prev_01a;
        l->i_high_hold = 0u;
    } else {
        i_req = l->i_tx_01a;
    }
    /* 请求高于公告最大电流时，上限就是该最大电流，不按超出的请求加带。 */
    if (i_req > imax) {
        sum = (uint32_t)imax + (uint32_t)(imax / 20u);
    } else {
        sum = (uint32_t)i_req + (uint32_t)(imax / 20u);
    }
    if (sum > imax || sum > 65535u) {
        limit = imax;
    } else {
        limit = (uint16_t)sum;
    }
    if (i_out > limit) {
        alarm_stop(l, CHG_I_ALARM_HIGH);
    }
}

static void observe_i_cmd(BmsLink *l, const ChgCtrl *ctrl, uint32_t dt_ms)
{
    int zero_cmd;

    l->cmd_pause = ctrl->pause ? 1u : 0u;
    l->cmd_stop = ctrl->stop ? 1u : 0u;
    l->cmd_i_req_01a = ctrl->i_req_01a;
    zero_cmd = l->cmd_pause || l->cmd_stop || l->cmd_i_req_01a == 0u;
    /* 计时从命令发出之后的下一拍开始。还没发出时，总线上仍是上一段电流。 */
    if (!zero_cmd) {
        l->i_zero_cmd_ms = 0u;
        l->zero_armed = 0u;
    } else if (l->zero_armed) {
        if (l->i_zero_cmd_ms <= 0xFFFFFFFFu - dt_ms) {
            l->i_zero_cmd_ms += dt_ms;
        } else {
            l->i_zero_cmd_ms = 0xFFFFFFFFu;
        }
    } else {
        l->i_zero_cmd_ms = 0u;
    }
    /* 一线通没有上报电流。未见到样本时，初值 0 不计卡住。
     * 窗口和允许时间跟已经发出的请求。还没发出，或离开窗口，就清累计。 */
    if (l->port == CHG_PORT_SIF || l->force_stop || l->phase != CHG_PHASE_RUN || zero_cmd ||
        l->i_report_seen == 0u || l->i_tx_seen == 0u ||
        !in_track_window(l->cap.i_max_01a, l->i_tx_01a) ||
        l->stat.i_out_01a > I_NEAR_01A) {
        l->i_low_ms = 0u;
        return;
    }
    if (l->i_low_ms <= 0xFFFFFFFFu - dt_ms) {
        l->i_low_ms += dt_ms;
    } else {
        l->i_low_ms = 0xFFFFFFFFu;
    }
    if (l->i_low_ms > stuck_allow_ms(l->i_tx_01a)) {
        alarm_stop(l, CHG_I_ALARM_STUCK);
    }
}

/* 能量传输中的请求电流刚写进 BCL 或 0x01。握手帧里的 0 不记。
 * 第一次发出只记下当前请求。之后改写时留下改写前的值，且不覆盖更早的一帧。 */
static void note_i_tx(BmsLink *l, const ChgCtrl *use)
{
    if (l->phase != CHG_PHASE_RUN) {
        return;
    }
    if (l->i_tx_seen && use->i_req_01a != l->i_tx_01a && l->i_high_hold == 0u) {
        l->i_tx_prev_01a = l->i_tx_01a;
        l->i_high_hold = 1u;
    }
    l->i_tx_01a = use->i_req_01a;
    l->i_tx_seen = 1u;
}

/* 零输出命令刚交给发送。同拍返回的 CCS / 0x03 仍是上一拍功率级，计时从下一拍起。 */
static void note_zero_tx(BmsLink *l)
{
    if ((l->cmd_pause || l->cmd_stop || l->cmd_i_req_01a == 0u) && l->zero_armed == 0u) {
        l->zero_armed = 1u;
        l->i_zero_cmd_ms = 0u;
    }
}

void bms_link_init(BmsLink *l, uint8_t port)
{
    memset(l, 0, sizeof *l);
    l->port = port;
    l->phase = CHG_PHASE_OFF;
    l->link_ok = (uint8_t)(port == CHG_PORT_SIF);
}

void bms_link_set_bat(BmsLink *l, const ChgBat *b)
{
    if (l == 0) {
        return;
    }
    if (b == 0) {
        memset(&l->bat, 0, sizeof l->bat);
        return;
    }
    l->bat = *b;
}

void bms_link_set_aux_uv(BmsLink *l, uint8_t aux_undervolt)
{
    if (l == 0) return;
    l->aux_undervolt = aux_undervolt ? 1u : 0u;
    if (l->aux_undervolt == 0u) {
        l->aux_uv_ms = 0u;
    }
}

static void latch_line(BmsLink *l)
{
    l->line_fault = 1u;
    l->phase = CHG_PHASE_STOP;
    l->force_stop = 1u;
    l->link_ok = 0u;
    l->seen_ready = 0u;
    l->sif_sent = 0;
    l->waiting = 0u;
    l->raw_n = 0u;
}

/* down=1 累加；达到 100 ms 锁存物理断开。down=0 只清计时和锁存，
 * force_stop 留到请求电流回到 0 再由 open_phase 放开。 */
static void observe_line(BmsLink *l, uint8_t down, uint32_t dt_ms, uint32_t *acc)
{
    if (down) {
        if (*acc < 0xFFFFFFFFu - dt_ms) {
            *acc += dt_ms;
        }
        if (*acc >= T_LINE_DOWN_MS) {
            latch_line(l);
        }
    } else {
        *acc = 0u;
        l->line_fault = 0u;
    }
}

/* SIF idle = 1；b == 0 是拉低。调用方每拍传入当前电平。 */
void bms_link_set_sif_state(BmsLink *l, uint8_t b, uint32_t dt_ms)
{
    if (l == 0 || l->port != CHG_PORT_SIF) {
        return;
    }
    observe_line(l, (uint8_t)(b == 0u), dt_ms, &l->sif_short_ms);
}

void bms_link_set_bus_off(BmsLink *l, uint8_t bus_off, uint32_t dt_ms)
{
    if (l == 0 || l->port != CHG_PORT_CAN) {
        return;
    }
    observe_line(l, bus_off ? 1u : 0u, dt_ms, &l->line_down_ms);
}

void bms_link_set_485_short(BmsLink *l, uint8_t shorted, uint32_t dt_ms)
{
    if (l == 0 || l->port != CHG_PORT_485) {
        return;
    }
    observe_line(l, shorted ? 1u : 0u, dt_ms, &l->line_down_ms);
}

void bms_link_can_rx(BmsLink *l, const ChgCan *f)
{
    if (l == 0 || f == 0 || l->port != CHG_PORT_CAN) {
        return;
    }
    /* 物理断开期间，噪声帧不得刷新 T_loss，也不得把 link_ok 拉回去。 */
    if (l->line_fault) {
        return;
    }
    if (f->id == CHG_CAN_CML && f->dlc >= 8u) {
        ChgCap c;
        memset(&c, 0, sizeof c);
        c.v_max_01v = get_u16(f->data);
        c.v_min_01v = get_u16(f->data + 2);
        c.i_max_01a = can_i_phys(get_u16(f->data + 4));
        c.i_min_01a = can_i_phys(get_u16(f->data + 6));
        c.ready = l->cap.ready;
        l->cap = c;
        return;
    }
    if (f->id == CHG_CAN_CRO && f->dlc >= 1u) {
        /* 只认 0x00 和 0xAA。其他值不改变就绪位。 */
        if (f->data[0] != 0x00u && f->data[0] != 0xAAu) {
            l->rx_drop++;
            return;
        }
        l->cap.ready = (uint8_t)(f->data[0] == 0xAAu);
        l->seen_ready = l->cap.ready;
        return;
    }
    if (f->id == CHG_CAN_CCS && f->dlc >= 8u && chg_id_accept(f->id, CHG_CAN_CCS)) {
        ChgStat s;
        memset(&s, 0, sizeof s);
        s.v_out_01v = get_u16(f->data);
        s.i_out_01a = can_i_phys(get_u16(f->data + 2));
        s.outputting = (uint8_t)(f->data[5] == 0x01u);
        /* 只认两个值。0x00 立即停止，不刷新 T_loss，也不清周期间隔。
         * 0x01 才是无故障。其余值丢掉。 */
        if (f->data[6] == 0x00u) {
            s.fault = 1u;
            l->stat = s;
            l->phase = CHG_PHASE_STOP;
            l->force_stop = 1u;
            l->link_ok = 0u;
            l->seen_ready = 0u;
            l->sif_sent = 0;
            l->waiting = 0u;
            return;
        }
        if (f->data[6] != 0x01u) {
            l->rx_drop++;
            return;
        }
        if (l->ccs_seen) {
            l->period_late = (uint8_t)!chg_period_ok(50u, l->ccs_gap_ms);
        } else {
            l->period_late = 0u;
        }
        l->ccs_seen = 1u;
        l->ccs_gap_ms = 0u;
        remember_stat(l, &s);
        judge_i_sample(l, s.i_out_01a, l->cmd_pause, l->cmd_stop, l->cmd_i_req_01a);
        return;
    }
    if (f->id == CHG_CAN_CST && f->dlc >= 4u) {
        l->stat.fault = 1u;
        /* 1–7 原样。大于 7 记为 0x03，不取低 3 位，避免 0x08 变成 0。 */
        l->stat.fault_code = f->data[0] > 0x07u ? 0x03u : f->data[0];
        l->stat.outputting = 0u;
        l->loss_ms = 0;
        /* §10 验收 14: BMS 见 CST 或 0x03 fault 1 s 内进入 STOP。CST 是事件性的，
         * 直接立即 STOP（不等 T_LOSS_MS）。与 T_loss 路径（line 620-626）一致，
         * 清 seen_ready / sif_sent / waiting，下一会话从 HS 重新握手。 */
        l->phase = CHG_PHASE_STOP;
        l->force_stop = 1u;
        l->link_ok = 0u;
        l->seen_ready = 0u;
        l->sif_sent = 0;
        l->waiting = 0u;
        return;
    }
    l->rx_drop++;
}

static void fill_tx(BmsLink *l, const ChgCtrl *use, int send_sif, int slow, int fast, int bst, BmsTx *tx)
{
    ChgCan f;
    if (l->port == CHG_PORT_SIF && send_sif) {
        int n = chg_ctrl_pack(use, tx->frame, sizeof tx->frame);
        if (n > 0) {
            tx->frame_len = (uint8_t)n;
            note_zero_tx(l);
            if (l->phase == CHG_PHASE_HS) {
                l->sif_sent++;
            }
        }
    }
    if (l->port != CHG_PORT_CAN) {
        tx->phase = l->phase;
        tx->link_ok = l->link_ok;
        return;
    }
    if (l->phase == CHG_PHASE_STOP) {
        if (bst) {
            chg_can_bst(&f);
            push_can(tx, &f);
            note_zero_tx(l);
        }
    } else {
        if (fast) {
            chg_can_bcl(use, &f);
            if (tx->can_n < 8u) {
                note_i_tx(l, use);
            }
            push_can(tx, &f);
        }
        if (slow) {
            /* 暂停只在 BSM。BCL 仍带请求电流，不能从这一帧起算残余。 */
            chg_can_bro(use->allow, &f);
            push_can(tx, &f);
            chg_can_bsm(use->pause, use->flags, &f);
            push_can(tx, &f);
            note_zero_tx(l);
        }
    }
    tx->phase = l->phase;
    tx->link_ok = l->link_ok;
}

static void fill_bat(BmsLink *l, BmsTx *tx)
{
    if (l->port == CHG_PORT_SIF) {
        int n = chg_bat_pack(&l->bat, tx->bat_frame, sizeof tx->bat_frame);
        if (n > 0) {
            tx->bat_len = (uint8_t)n;
        }
        return;
    }
    if (l->port == CHG_PORT_CAN) {
        ChgCan part[4];
        int i;
        chg_can_bat(&l->bat, part);
        for (i = 0; i < 4; i++) {
            push_can(tx, &part[i]);
        }
    }
}

static void send_485(BmsLink *l, const ChgCtrl *use, int slow, int bst, BmsTx *tx)
{
    int n;
    int want;
    if (bst) {
        l->stop_due = 1u;
    }
    if (l->waiting) {
        return;
    }
    /* §3.2 阶段 6: RS-485 停止期间不再发送帧。stop_due 仅在进入 STOP 那一拍
     * 是 1（让出停止帧），之后清零；后续 STOP 期间即使 slow 到点也不发。 */
    if (l->phase == CHG_PHASE_STOP && l->stop_due == 0u) {
        return;
    }
    want = slow || l->stop_due;
    if (!want) {
        return;
    }
    /* 上一帧 0x01 已有应答时，才单独占用一个发送周期送电池信息。没有应答就重发 0x01。 */
    if (l->stop_due == 0u && l->answered && l->bat_pending && l->bat_turn &&
        (l->phase == CHG_PHASE_HS || l->phase == CHG_PHASE_RUN)) {
        n = chg_bat_pack(&l->bat, tx->bat_frame, sizeof tx->bat_frame);
        if (n > 0) {
            tx->bat_len = (uint8_t)n;
        }
        l->bat_pending = 0u;
        l->bat_turn = 0u;
        return;
    }
    n = chg_ctrl_pack(use, tx->frame, sizeof tx->frame);
    if (n > 0) {
        tx->frame_len = (uint8_t)n;
        note_i_tx(l, use);
        note_zero_tx(l);
        l->waiting = 1u;
        l->wait_ms = 0;
        l->answered = 0u;
        if (l->phase == CHG_PHASE_STOP) {
            l->stop_due = 0u;
        }
        if ((l->phase == CHG_PHASE_HS || l->phase == CHG_PHASE_RUN) && l->bat_pending) {
            l->bat_turn = 1u;
        }
    }
}

/* §10 验收 13: aux_undervolt=1 持续 ≥ 100 ms → STOP。每拍调用。 */
static void aux_uv_observe(BmsLink *l, uint32_t dt_ms)
{
    if (l == 0) return;
    if (l->aux_undervolt) {
        if (l->aux_uv_ms < 0xFFFFFFFFu - dt_ms) {
            l->aux_uv_ms += dt_ms;
        }
        if (l->aux_uv_ms >= T_AUX_UV_MS) {
            l->phase = CHG_PHASE_STOP;
            l->force_stop = 1u;
            l->link_ok = 0u;
            l->seen_ready = 0u;
            l->sif_sent = 0;
            l->waiting = 0u;
        }
    } else {
        l->aux_uv_ms = 0u;
    }
}

void bms_link_tick(BmsLink *l, const ChgCtrl *ctrl, uint32_t dt_ms, BmsTx *tx)
{
    ChgCtrl held;
    const ChgCtrl *use;
    int slow;
    int fast;
    int bat;
    int bst;
    uint8_t was;
    ChgCtrl none;

    if (tx == 0) {
        return;
    }
    memset(&none, 0, sizeof none);
    none.stop = 1;
    if (l == 0 || (l->port != CHG_PORT_SIF && l->port != CHG_PORT_CAN && l->port != CHG_PORT_485)) {
        memset(tx, 0, sizeof *tx);
        return;
    }
    if (ctrl == 0) {
        ctrl = &none;
    }
    was = l->phase;
    aux_uv_observe(l, dt_ms);
    if (l->port == CHG_PORT_485 && l->waiting) {
        l->wait_ms += dt_ms;
        if (l->wait_ms >= T_REPLY_MS) {
            l->waiting = 0u;
        }
    }
    if ((l->port == CHG_PORT_CAN || l->port == CHG_PORT_485) && l->phase == CHG_PHASE_RUN) {
        l->loss_ms += dt_ms;
        if (l->port == CHG_PORT_CAN && l->ccs_seen) {
            l->ccs_gap_ms += dt_ms;
            if (l->ccs_gap_ms > 50u + 5u) {
                l->period_late = 1u;
            }
        }
        if (chg_loss_tripped(l->loss_ms)) {
            l->phase = CHG_PHASE_STOP;
            l->force_stop = 1u;
            l->link_ok = 0u;
            l->seen_ready = 0u;
            l->sif_sent = 0;
            l->waiting = 0u;
        }
    }
    open_phase(l, ctrl);
    promote(l);
    observe_i_cmd(l, ctrl, dt_ms);
    use = payload(l, ctrl, &held);
    tx_clear(tx, l);
    slow = due(&l->slow_ms, dt_ms, T_SLOW_MS, &l->slow_arm);
    bat = due(&l->bat_ms, dt_ms, T_BAT_MS, &l->bat_arm);
    fast = 0;
    if (l->port == CHG_PORT_CAN) {
        fast = due(&l->fast_ms, dt_ms, l->phase == CHG_PHASE_STOP ? T_BST_MS : T_FAST_MS, &l->fast_arm);
    }
    bst = (was != CHG_PHASE_STOP && l->phase == CHG_PHASE_STOP);
    if (l->port == CHG_PORT_SIF) {
        fill_tx(l, use, slow || bst, 0, 0, 0, tx);
        if (bat) {
            fill_bat(l, tx);
        }
    } else if (l->port == CHG_PORT_CAN) {
        fill_tx(l, use, 0, slow, fast, fast || bst, tx);
        if (bat) {
            fill_bat(l, tx);
        }
    } else {
        if (bat) {
            l->bat_pending = 1u;
        }
        send_485(l, use, slow, bst, tx);
    }
    tx->phase = l->phase;
    tx->link_ok = l->link_ok;
}

static void accept_485(BmsLink *l, uint8_t msg, const uint8_t *data, uint8_t len, const ChgCtrl *ctrl)
{
    ChgStat s;
    int have_stat = 0;

    if (l->waiting == 0u) {
        return;
    }
    if (!((msg == 0x02u && len == 9u) || (msg == 0x03u && len == 6u))) {
        return;
    }
    open_phase(l, ctrl);
    if (msg == 0x02u) {
        ChgCap c;
        if (load_cap(data, &c) != 0) {
            return;
        }
        remember_cap(l, &c);
    } else {
        if (load_stat(data, &s) != 0) {
            return;
        }
        remember_stat(l, &s);
        /* §10 验收 14: BMS 见 0x03 stat.fault=1 或 fault_code≠0 立即进入 STOP，
         * 与 CAN CST 对称。事件性立即 STOP，不等 T_LOSS_MS。故障帧不再改判电流。 */
        if (s.fault || s.fault_code) {
            l->phase = CHG_PHASE_STOP;
            l->force_stop = 1u;
            l->link_ok = 0u;
            l->seen_ready = 0u;
            l->sif_sent = 0;
            l->waiting = 0u;
            l->answered = 1u;
            l->loss_ms = 0;
            return;
        }
        have_stat = 1;
    }
    l->waiting = 0u;
    l->answered = 1u;
    /* 能量传输中只有 0x03 表示输出仍在。0x02 只更新能力，不刷新 T_loss。 */
    if (l->phase != CHG_PHASE_RUN || msg == 0x03u) {
        l->loss_ms = 0;
    }
    l->link_ok = 1u;
    promote(l);
    if (have_stat) {
        judge_i_sample(l, s.i_out_01a, ctrl->pause ? 1u : 0u, ctrl->stop ? 1u : 0u, ctrl->i_req_01a);
    }
}

void bms_link_rx_byte(BmsLink *l, uint8_t b, uint32_t gap_ms, const ChgCtrl *ctrl, BmsTx *tx)
{
    uint8_t msg = 0;
    uint8_t data[9];
    uint8_t len = 0;
    uint16_t total;
    ChgCtrl none;

    if (tx == 0) {
        return;
    }
    memset(tx, 0, sizeof *tx);
    if (l == 0 || l->port != CHG_PORT_485) {
        return;
    }
    memset(&none, 0, sizeof none);
    none.stop = 1;
    if (ctrl == 0) {
        ctrl = &none;
    }
    tx->phase = l->phase;
    tx->link_ok = l->link_ok;
    if (l->line_fault) {
        return;
    }
    if (gap_ms > T_GAP_MS && l->raw_n > 0u) {
        l->raw_n = 0;
    }
    if (l->raw_n >= sizeof l->raw) {
        l->raw_n = 0;
    }
    l->raw[l->raw_n++] = b;
    if (l->raw_n < 5u) {
        return;
    }
    /* 放得进 raw 的长帧先收满再整帧丢掉。0x05–0x08 与 0x02/0x03 同线时不会错同步。
     * data 只有 9 字节，长度大于 9 的帧不得解进这块缓冲。 */
    if ((size_t)l->raw[4] + 7u > sizeof l->raw) {
        l->raw_n = 0;
        return;
    }
    total = (uint16_t)l->raw[4] + 7u;
    if (l->raw_n < total) {
        return;
    }
    if (l->raw[4] > 9u) {
        l->raw_n = 0;
        return;
    }
    if (frame_unpack(l->raw, total, &msg, data, &len) > 0) {
        accept_485(l, msg, data, len, ctrl);
    }
    tx->phase = l->phase;
    tx->link_ok = l->link_ok;
    l->raw_n = 0;
}
