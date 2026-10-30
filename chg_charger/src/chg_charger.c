/* 作者：苏勃(subo)
 * SPDX-License-Identifier: Apache-2.0
 */
#include "chg_charger.h"

#include <string.h>

#define T_SLOW_MS 250u
#define T_FAST_MS 50u
#define T_LINE_DOWN_MS 100u  /* §10 验收 15: SIF 拉低 / CAN bus-off / 485 短路 */
#define T_GAP_MS 50u

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

static void store_cap(const ChgCap *c, uint8_t data[9])
{
    data[0] = c->ready ? 0x01u : 0u;
    put_u16(data + 1, c->v_min_01v);
    put_u16(data + 3, c->v_max_01v);
    put_u16(data + 5, c->i_min_01a);
    put_u16(data + 7, c->i_max_01a);
}

static void store_stat(const ChgStat *s, uint8_t data[6])
{
    data[0] = (uint8_t)((s->outputting ? 0x01u : 0u) | (s->fault ? 0x02u : 0u) |
                        (s->ac_path ? 0x04u : 0u) | (s->dc_path ? 0x08u : 0u));
    put_u16(data + 1, s->v_out_01v);
    put_u16(data + 3, s->i_out_01a);
    data[5] = s->fault_code;
}

int chg_cap_pack(const ChgCap *c, uint8_t *out, size_t cap)
{
    uint8_t data[9];
    if (c == 0) {
        return -1;
    }
    store_cap(c, data);
    return frame_pack(0x02u, data, 9, out, cap);
}

static int load_bat(const uint8_t data[28], ChgBat *b)
{
    /* 有效字节只认 0x00 和 0x01。其他值整帧无效，不覆盖已保存的电池信息。 */
    if (data[0] != 0x00u && data[0] != 0x01u) {
        return -1;
    }
    memset(b, 0, sizeof *b);
    b->valid = data[0];
    b->flags = (uint8_t)(data[1] & 0x3Fu);
    b->soc_01pct = get_u16(data + 2);
    b->v_pack_01v = get_u16(data + 4);
    b->v_cell_max_mv = get_u16(data + 6);
    b->v_cell_min_mv = get_u16(data + 8);
    b->t_max_c = (int8_t)data[10];
    b->t_min_c = (int8_t)data[11];
    b->rated_ah_01ah = get_u16(data + 12);
    b->rated_v_01v = get_u16(data + 14);
    b->chemistry = data[16];
    b->v_allow_01v = get_u16(data + 17);
    b->i_allow_01a = get_u16(data + 19);
    b->t_allow_c = (int8_t)data[21];
    b->remain_min = get_u16(data + 22);
    b->idx_cell_max = data[24];
    b->idx_cell_min = data[25];
    b->idx_temp_max = data[26];
    b->idx_temp_min = data[27];
    return 0;
}

int chg_bat_unpack(const uint8_t *in, size_t n, ChgBat *b)
{
    uint8_t msg = 0;
    uint8_t data[28];
    uint8_t len = 0;
    if (frame_unpack(in, n, &msg, data, &len) <= 0 || msg != 0x04u || len != 28u) {
        return -1;
    }
    if (b != 0 && load_bat(data, b) != 0) {
        return -1;
    }
    if (b == 0) {
        ChgBat tmp;
        if (load_bat(data, &tmp) != 0) {
            return -1;
        }
    }
    return 35;
}

int chg_stat_pack(const ChgStat *s, uint8_t *out, size_t cap)
{
    uint8_t data[6];
    if (s == 0) {
        return -1;
    }
    store_stat(s, data);
    return frame_pack(0x03u, data, 6, out, cap);
}

static int load_ctrl(const uint8_t data[6], ChgCtrl *c)
{
    if (data == 0 || c == 0) {
        return -1;
    }
    /* 字节 0 的高 4 位和字节 5 是保留，必须为 0。非 0 整帧无效。 */
    if ((data[0] & 0xF0u) != 0u || data[5] != 0u) {
        return -1;
    }
    c->allow = (uint8_t)((data[0] & 0x01u) != 0u);
    c->pause = (uint8_t)((data[0] & 0x02u) != 0u);
    c->stop = (uint8_t)((data[0] & 0x04u) != 0u);
    c->cv = (uint8_t)((data[0] & 0x08u) != 0u);
    c->v_req_01v = get_u16(data + 1);
    c->i_req_01a = get_u16(data + 3);
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

void chg_can_cml(const ChgCap *c, ChgCan *f)
{
    can_clear(f);
    f->id = CHG_CAN_CML;
    f->dlc = 8;
    put_u16(f->data, c->v_max_01v);
    put_u16(f->data + 2, c->v_min_01v);
    put_u16(f->data + 4, can_i_raw(c->i_max_01a));
    put_u16(f->data + 6, can_i_raw(c->i_min_01a));
}

void chg_can_cro(uint8_t ready, ChgCan *f)
{
    can_clear(f);
    f->id = CHG_CAN_CRO;
    f->dlc = 1;
    f->data[0] = ready ? 0xAAu : 0x00u;
}

void chg_can_ccs(const ChgStat *s, ChgCan *f)
{
    can_clear(f);
    f->id = CHG_CAN_CCS;
    /* §12.2 CCS 8 字节：byte 0..1 v_out、2..3 i_out(物理+400)、4 累计时间 (0.1 min/bit, 实时帧写 0 见 CEM)、
     * 5 充电机状态 bit0=outputting(功率级已闭合)、6 故障位(1=无故障)、7 累计能量 0.1 kWh/bit(实时帧写 0 见 CEM)。 */
    f->dlc = 8;
    put_u16(f->data, s->v_out_01v);
    put_u16(f->data + 2, can_i_raw(s->i_out_01a));
    f->data[4] = 0x00u;     /* 实时帧不累计，会话级累计由 §12.2 CEM 在 ENDING 一次性发 */
    f->data[5] = s->outputting ? 0x01u : 0x00u;  /* v28: byte 5 outputting 位，与 chg_evcc.c:276 对齐 */
    f->data[6] = (s->fault || s->fault_code) ? 0x00u : 0x01u;
    f->data[7] = 0x00u;     /* §12.2: 累计能量 0.1 kWh/bit，实时帧写 0，见 CEM.energy */
}

void chg_can_cst(ChgCan *f)
{
    can_clear(f);
    f->id = CHG_CAN_CST;
    f->dlc = 4;
    f->data[0] = 0x01;
}

/* 锁压以上 0.5 V 仍执行，电压目标停在锁压。再高的请求不执行。单位 0.1 V。 */
static int v_req_above_lock(uint16_t v_req_01v, uint16_t v_lock_01v)
{
    return (uint32_t)v_req_01v > (uint32_t)v_lock_01v + 5u;
}

void chg_apply(const ChgCtrl *ctrl, const ChgCap *cap, uint8_t cap_valid, uint8_t link_ok,
               uint8_t paths_conflict, uint8_t charger_fault, ChgAct *act)
{
    uint16_t i;
    uint16_t v;
    if (act == 0) {
        return;
    }
    act->i_out_01a = 0;
    act->v_target_01v = 0;
    act->enable = 0;
    if (ctrl == 0 || cap == 0 || !link_ok || paths_conflict || charger_fault) {
        return;
    }
    if (ctrl->pause || ctrl->stop || !ctrl->allow || ctrl->i_req_01a == 0u || ctrl->v_req_01v == 0u) {
        return;
    }
    if (cap_valid && !cap->ready) {
        return;
    }
    i = ctrl->i_req_01a;
    v = ctrl->v_req_01v;
    if (i > cap->i_max_01a) {
        i = cap->i_max_01a;
    }
    /* 低于锁压按请求执行，不得抬回锁压。高出超过 0.5 V 的请求本周期不出流。恰好 0.5 V 钳到锁压。 */
    if (v_req_above_lock(v, cap->v_max_01v)) {
        return;
    }
    if (v > cap->v_max_01v) {
        v = cap->v_max_01v;
    }
    if (i == 0u || v == 0u) {
        return;
    }
    act->i_out_01a = i;
    act->v_target_01v = v;
    act->enable = 1;
}

/* 放得进 uint16 才写出。上限用向下取整，避免结果高过进线功率。 */
static int div_floor_u16(uint64_t n, uint64_t d, uint16_t *out)
{
    if (d == 0u || out == 0) {
        return 0;
    }
    n /= d;
    if (n > 65535u) {
        return 0;
    }
    *out = (uint16_t)n;
    return 1;
}

/* 直流请求向上取整，使桩侧功率盖住电池侧请求。恰好整除时不进位。 */
static int div_ceil_u16(uint64_t n, uint64_t d, uint16_t *out)
{
    uint64_t q;

    if (d == 0u || out == 0) {
        return 0;
    }
    q = (n + d - 1u) / d;
    if (q > 65535u) {
        return 0;
    }
    *out = (uint16_t)q;
    return 1;
}

/* 占空比千分比。533 表示 53.3%。模拟电流为 0 的两档要分开：
 * 3% 至 7% 只在调用方给出数字电流时取电；低于 3%、7%–8% 间隙、高于 97% 即使有数字电流也不取。 */
#define AC_DUTY_ZERO 0
#define AC_DUTY_DIGITAL 1
#define AC_DUTY_ANALOG 2

/* 8% 至不足 10% 为 6.0 A。10% 至 85% 为百分数 × 0.6 A，四舍五入到 0.1 A。
 * 高于 85% 至 96% 为 (百分数 − 64) × 2.5 A，四舍五入到 0.1 A，不得改用 ×0.6。
 * 高于 96% 至 97% 为 80.0 A。单相电缆档再把结果帽到不超过 32.0 A。 */
static int ac_duty_class(uint16_t duty_permille, uint16_t *analog)
{
    uint32_t n;

    if (analog == 0) {
        return AC_DUTY_ZERO;
    }
    *analog = 0u;
    if (duty_permille < 30u || duty_permille > 970u) {
        return AC_DUTY_ZERO;
    }
    if (duty_permille <= 70u) {
        return AC_DUTY_DIGITAL;
    }
    if (duty_permille < 80u) {
        return AC_DUTY_ZERO;
    }
    if (duty_permille < 100u) {
        *analog = 60u;
        return AC_DUTY_ANALOG;
    }
    if (duty_permille <= 850u) {
        n = ((uint32_t)duty_permille * 6u + 5u) / 10u;
    } else if (duty_permille <= 960u) {
        n = (((uint32_t)duty_permille - 640u) * 5u + 1u) / 2u;
    } else {
        n = 800u;
    }
    if (n == 0u || n > 65535u) {
        return AC_DUTY_ZERO;
    }
    *analog = (uint16_t)n;
    return AC_DUTY_ANALOG;
}

/* 国标 CC 与 Type 2 PP 的 1.5 kΩ、680 Ω 电流不同。100 Ω 单相帽在 32.0 A。 */
static uint16_t ac_cable_01a(uint8_t kind, uint16_t ohm)
{
    if (kind == CHG_PILE_GB_AC) {
        if (ohm == 1500u) {
            return 100u;
        }
        if (ohm == 680u) {
            return 160u;
        }
        if (ohm == 220u || ohm == 100u) {
            return 320u;
        }
    } else if (kind == CHG_PILE_EU_AC) {
        if (ohm == 1500u) {
            return 130u;
        }
        if (ohm == 680u) {
            return 200u;
        }
        if (ohm == 220u || ohm == 100u) {
            return 320u;
        }
    }
    return 0u;
}

static uint16_t u16_min(uint16_t a, uint16_t b)
{
    return a < b ? a : b;
}

uint16_t chg_ac_inlet_01a(uint8_t kind, uint16_t cp_nominal_mv, uint16_t duty_permille,
                          uint16_t ohm, uint16_t rated_inlet_01a, uint8_t digital_valid,
                          uint16_t digital_01a)
{
    uint16_t duty_i;
    uint16_t cable_i;
    uint16_t i;
    int cls;

    if (kind != CHG_PILE_GB_AC && kind != CHG_PILE_EU_AC) {
        return 0u;
    }
    if (cp_nominal_mv != CHG_CP_READY_MV || rated_inlet_01a == 0u) {
        return 0u;
    }
    cable_i = ac_cable_01a(kind, ohm);
    if (cable_i == 0u) {
        return 0u;
    }
    cls = ac_duty_class(duty_permille, &duty_i);
    if (cls == AC_DUTY_ZERO) {
        return 0u;
    }
    if (cls == AC_DUTY_DIGITAL) {
        if (digital_valid == 0u || digital_01a == 0u) {
            return 0u;
        }
        return u16_min(digital_01a, u16_min(cable_i, rated_inlet_01a));
    }
    if (duty_i == 0u) {
        return 0u;
    }
    i = u16_min(duty_i, u16_min(cable_i, rated_inlet_01a));
    if (digital_valid != 0u) {
        i = u16_min(i, digital_01a);
    }
    return i;
}

uint16_t chg_ac_batt_01a(uint16_t i_req_01a, uint16_t v_batt_01v, uint16_t v_ac_01v,
                         uint16_t i_inlet_01a, uint16_t eff_permille, uint16_t v_ac_min_01v,
                         uint16_t v_ac_max_01v)
{
    uint16_t i_max;

    if (i_req_01a == 0u || v_batt_01v == 0u || v_ac_01v == 0u || i_inlet_01a == 0u ||
        eff_permille == 0u || eff_permille > 1000u) {
        return 0u;
    }
    if (v_ac_min_01v != 0u || v_ac_max_01v != 0u) {
        if (v_ac_min_01v == 0u || v_ac_max_01v == 0u || v_ac_min_01v > v_ac_max_01v ||
            v_ac_01v < v_ac_min_01v || v_ac_01v > v_ac_max_01v) {
            return 0u;
        }
    }
    /* i_max = 进线电流 × 进线电压 × 效率 / 电池电压。效率是千分比。向下取整。 */
    if (!div_floor_u16((uint64_t)i_inlet_01a * v_ac_01v * eff_permille,
                       (uint64_t)v_batt_01v * 1000u, &i_max)) {
        return 0u;
    }
    return u16_min(i_max, i_req_01a);
}

static int in_window(uint16_t v, uint16_t lo, uint16_t hi)
{
    return v >= lo && v <= hi;
}

static void demand_clear(ChgPileDemand *out)
{
    out->send = 0u;
    out->v_01v = 0u;
    out->i_01a = 0u;
}

/* 本机输入窗口与桩输出范围的交集。没有交集时返回 0。 */
static int intersect_window(uint16_t a_lo, uint16_t a_hi, uint16_t b_lo, uint16_t b_hi,
                            uint16_t *lo, uint16_t *hi)
{
    uint16_t l;
    uint16_t h;

    l = a_lo > b_lo ? a_lo : b_lo;
    h = a_hi < b_hi ? a_hi : b_hi;
    if (l > h || lo == 0 || hi == 0) {
        return 0;
    }
    *lo = l;
    *hi = h;
    return 1;
}

static int pile_limits_ok(uint16_t v_lo, uint16_t v_hi, uint16_t i_lo, uint16_t i_hi)
{
    if (v_lo == 0u || v_hi == 0u || v_lo > v_hi) {
        return 0;
    }
    /* 电流下限 0 是真实的「没有下限」。上限 0 仍表示没收到能力。 */
    if (i_hi == 0u || i_lo > i_hi) {
        return 0;
    }
    return 1;
}

void chg_dc_pile_demand(uint8_t kind, uint16_t v_win_lo_01v, uint16_t v_win_hi_01v,
                        uint16_t v_inlet_01v, uint16_t v_batt_01v, uint16_t i_batt_01a,
                        uint16_t eff_permille, uint16_t v_pile_min_01v, uint16_t v_pile_max_01v,
                        uint16_t i_pile_min_01a, uint16_t i_pile_max_01a, uint8_t pause,
                        uint16_t v_prev_01v, uint16_t i_prev_01a, ChgPileDemand *out)
{
    uint16_t lo;
    uint16_t hi;
    uint16_t v;
    uint16_t i;

    if (out == 0) {
        return;
    }
    demand_clear(out);
    if (kind != CHG_PILE_GB_DC && kind != CHG_PILE_EU_DC) {
        return;
    }
    if (v_win_lo_01v == 0u || v_win_hi_01v == 0u || v_win_lo_01v > v_win_hi_01v) {
        return;
    }
    if (!pile_limits_ok(v_pile_min_01v, v_pile_max_01v, i_pile_min_01a, i_pile_max_01a)) {
        return;
    }
    if (!intersect_window(v_win_lo_01v, v_win_hi_01v, v_pile_min_01v, v_pile_max_01v, &lo, &hi)) {
        return;
    }
    if (pause != 0u) {
        if (i_prev_01a == 0u || !in_window(v_prev_01v, lo, hi)) {
            return;
        }
        i = i_prev_01a;
        if (i > i_pile_max_01a) {
            i = i_pile_max_01a;
        }
        if (i < i_pile_min_01a) {
            return;
        }
        out->send = 1u;
        out->v_01v = v_prev_01v;
        out->i_01a = i;
        return;
    }
    if (eff_permille == 0u || eff_permille > 1000u || v_batt_01v == 0u || i_batt_01a == 0u) {
        return;
    }
    if (v_inlet_01v != 0u && in_window(v_inlet_01v, v_win_lo_01v, v_win_hi_01v)) {
        if (in_window(v_inlet_01v, lo, hi)) {
            v = v_inlet_01v;
        } else if (v_inlet_01v > hi) {
            v = hi;
        } else {
            v = lo;
        }
    } else if (v_inlet_01v == 0u && in_window(v_prev_01v, lo, hi)) {
        v = v_prev_01v;
    } else if (v_inlet_01v == 0u) {
        v = lo;
    } else {
        return;
    }
    if (!div_ceil_u16((uint64_t)v_batt_01v * i_batt_01a * 1000u, (uint64_t)eff_permille * v, &i) ||
        i == 0u) {
        return;
    }
    if (i > i_pile_max_01a) {
        i = i_pile_max_01a;
    }
    if (i < i_pile_min_01a) {
        return;
    }
    out->send = 1u;
    out->v_01v = v;
    out->i_01a = i;
}

uint8_t chg_gb_dc_i_raw(uint16_t i_01a, uint16_t *raw)
{
    if (raw == 0 || i_01a > 4000u) {
        return 0u;
    }
    *raw = (uint16_t)(4000u - i_01a);
    return 1u;
}

uint8_t chg_gb_dc_i_phys(uint16_t raw, uint16_t *i_01a)
{
    if (i_01a == 0 || raw > 4000u) {
        return 0u;
    }
    *i_01a = (uint16_t)(4000u - raw);
    return 1u;
}

uint16_t chg_dc_batt_01a(uint16_t i_req_01a, uint16_t v_batt_01v, uint16_t v_pile_01v,
                         uint16_t i_pile_01a, uint16_t eff_permille)
{
    uint16_t i_max;

    if (i_req_01a == 0u || v_batt_01v == 0u || v_pile_01v == 0u || i_pile_01a == 0u ||
        eff_permille == 0u || eff_permille > 1000u) {
        return 0u;
    }
    /* 传入的桩电流必须已经与桩最大取过小。向下取整，再不得超过请求。 */
    if (!div_floor_u16((uint64_t)i_pile_01a * v_pile_01v * eff_permille,
                       (uint64_t)v_batt_01v * 1000u, &i_max)) {
        return 0u;
    }
    return u16_min(i_max, i_req_01a);
}

uint8_t chg_gb_dc_cml_i(uint16_t raw_max, uint16_t raw_min, uint16_t *i_min, uint16_t *i_max)
{
    uint16_t a;
    uint16_t b;
    uint16_t lo;
    uint16_t hi;

    if (i_min == 0 || i_max == 0) {
        return 0u;
    }
    if (chg_gb_dc_i_phys(raw_max, &a) == 0u || chg_gb_dc_i_phys(raw_min, &b) == 0u) {
        return 0u;
    }
    if (a >= b) {
        hi = a;
        lo = b;
    } else {
        hi = b;
        lo = a;
    }
    /* 最大侧为 0 表示两只都不是充电能力。最小侧为 0 是没有下限，可以写出。 */
    if (hi == 0u) {
        return 0u;
    }
    *i_min = lo;
    *i_max = hi;
    return 1u;
}

static uint8_t pile_kind_ok(uint8_t kind)
{
    return kind == CHG_PILE_GB_AC || kind == CHG_PILE_EU_AC ||
           kind == CHG_PILE_GB_DC || kind == CHG_PILE_EU_DC;
}

void chg_pile_restart(uint8_t kind, uint8_t reason, uint8_t plug,
                      uint8_t retries_left, ChgPileRestart *out)
{
    if (out == 0) {
        return;
    }
    out->cut = 1u;
    out->restart = 0u;
    if (!pile_kind_ok(kind)) {
        return;
    }
    if (plug != CHG_PLUG_HELD && plug != CHG_PLUG_OPEN && plug != CHG_PLUG_RESEATED) {
        return;
    }
    if (reason == CHG_PILE_END_PAUSE) {
        out->cut = 0u;
        return;
    }
    if (reason == CHG_PILE_END_NORMAL) {
        out->restart = (plug == CHG_PLUG_OPEN) ? 0u : 1u;
        return;
    }
    if (reason == CHG_PILE_END_REPLUG) {
        out->restart = (plug == CHG_PLUG_RESEATED) ? 1u : 0u;
        return;
    }
    if (reason == CHG_PILE_END_RETRY) {
        if (kind != CHG_PILE_GB_DC || plug == CHG_PLUG_OPEN) {
            return;
        }
        if (retries_left > 0u || plug == CHG_PLUG_RESEATED) {
            out->restart = 1u;
        }
    }
}

/* 无新鲜合法 0x04 时返回 1，家充仍按 0x01。允许电流不参与。 */
static int bat_compatible(const ChrLink *l)
{
    const ChgBat *b = &l->bat;
    uint8_t cc;
    uint8_t bc;
    if (l->bat_seen == 0u || b->valid == 0u || l->bat_age_ms > CHG_BAT_STALE_MS) {
        return 1;
    }
    if (b->v_allow_01v > 0u && l->cap.v_max_01v > b->v_allow_01v) {
        return 0;
    }
    cc = l->cap.chemistry;
    bc = b->chemistry;
    if ((cc == CHG_CHEM_NCM || cc == CHG_CHEM_LFP) &&
        (bc == CHG_CHEM_NCM || bc == CHG_CHEM_LFP) && cc != bc) {
        return 0;
    }
    return 1;
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

static void push_can(ChrTx *tx, const ChgCan *f)
{
    if (tx->can_n < 3u) {
        tx->can[tx->can_n++] = *f;
    }
}

static void enter_stop(ChrLink *l)
{
    l->phase = CHG_PHASE_STOP;
    l->advertise_ready = 0u;
    l->seen_allow0 = 0u;
    l->sif_got = 0u;
    l->have_ctrl = 0u;
    l->link_ok = 0u;
    memset(&l->ctrl, 0, sizeof l->ctrl);
}

static void latch_line_fault(ChrLink *l)
{
    enter_stop(l);
    l->line_fault = 1u;
    l->raw_n = 0u;
}

/* down=1 累加到 100 ms 后锁存。down=0 解除锁存，会话仍留在 STOP，
 * 等下一次握手，不在本函数里恢复出流。 */
static void observe_line(ChrLink *l, uint8_t down, uint32_t dt_ms)
{
    if (down) {
        if (l->line_down_ms < 0xFFFFFFFFu - dt_ms) {
            l->line_down_ms += dt_ms;
        }
        if (l->line_down_ms >= T_LINE_DOWN_MS) {
            latch_line_fault(l);
        }
    } else {
        l->line_down_ms = 0u;
        l->line_fault = 0u;
    }
}

void chr_link_set_aux_uv(ChrLink *l, uint8_t aux_undervolt, uint32_t dt_ms)
{
    if (l == 0) {
        return;
    }
    if (aux_undervolt) {
        if (l->aux_uv_ms < 0xFFFFFFFFu - dt_ms) {
            l->aux_uv_ms += dt_ms;
        }
        if (l->aux_uv_ms >= 100u) {
            enter_stop(l);
            l->aux_fault = 1u;
        }
    } else {
        l->aux_uv_ms = 0u;
        l->aux_fault = 0u;
    }
}

void chr_link_set_sif_state(ChrLink *l, uint8_t b, uint32_t dt_ms)
{
    if (l == 0 || l->port != CHG_PORT_SIF) {
        return;
    }
    observe_line(l, (uint8_t)(b == 0u), dt_ms);
}

void chr_link_set_bus_off(ChrLink *l, uint8_t bus_off, uint32_t dt_ms)
{
    if (l == 0 || l->port != CHG_PORT_CAN) {
        return;
    }
    observe_line(l, bus_off ? 1u : 0u, dt_ms);
}

void chr_link_set_485_short(ChrLink *l, uint8_t shorted, uint32_t dt_ms)
{
    if (l == 0 || l->port != CHG_PORT_485) {
        return;
    }
    observe_line(l, shorted ? 1u : 0u, dt_ms);
}

static void begin_handshake(ChrLink *l)
{
    l->phase = CHG_PHASE_HS;
    l->advertise_ready = 0u;
    l->seen_allow0 = 0u;
    l->sif_got = 0u;
    l->have_ctrl = 0u;
    l->cap_dirty = 1u;
    l->loss_ms = 0;
    l->link_ok = 0u;
    memset(&l->ctrl, 0, sizeof l->ctrl);
}

static void output_of(ChrLink *l, const ChgStat *meas, ChrTx *tx, int consume_resume)
{
    uint8_t conflict = (uint8_t)(meas != 0 && meas->ac_path && meas->dc_path);
    uint8_t fault = (uint8_t)(meas != 0 && (meas->fault || meas->fault_code != 0u));
    uint8_t link = (uint8_t)(l->link_ok && l->phase == CHG_PHASE_RUN);
    uint8_t cap_valid = (uint8_t)(l->port != CHG_PORT_SIF);
    ChgCap cap = l->cap;
    cap.ready = l->advertise_ready;
    chg_apply(l->have_ctrl ? &l->ctrl : 0, &cap, cap_valid, link, conflict, fault, &tx->act);
    /* 新鲜合法的 0x04 只做电压上限和化学体系。不改请求电流。闸门动作时本周期输出为 0。 */
    if (tx->act.enable && !bat_compatible(l)) {
        tx->act.i_out_01a = 0;
        tx->act.v_target_01v = 0;
        tx->act.enable = 0;
    }
    /* GB/T 18487.1-2023 10.7.2：直流接触器再次闭合时冲击电流峰值不超过 20 A。
     * 暂停恢复的第一拍把电流钳在 20.0 A。电压目标取已经按锁压钳位后的目标，再减 1.0 V。
     * 请求不高于锁压时，这就是请求电压减 1.0 V。 */
    if (l->resume_arm && tx->act.enable) {
        if (tx->act.i_out_01a > 200u) {
            tx->act.i_out_01a = 200u;
        }
        if (tx->act.v_target_01v > 10u) {
            tx->act.v_target_01v = (uint16_t)(tx->act.v_target_01v - 10u);
        }
        /* 置位一直保留到周期 tick。接收函数里的输出也先钳住，避免调用方用到未钳电流。
         * 故障拍 enable 为 0，不会进到这里，置位保留到故障消失后的第一拍。 */
        if (consume_resume) {
            l->resume_arm = 0u;
        }
    }
    tx->phase = l->phase;
    tx->link_ok = l->link_ok;
}

static void note_ctrl(ChrLink *l, const ChgCtrl *c, int from_can_bcl_only)
{
    uint8_t was_pause = l->ctrl.pause;
    l->ctrl = *c;
    l->have_ctrl = 1u;
    l->loss_ms = 0;
    l->link_ok = 1u;
    /* 握手帧就是允许=0、电流=0。只有能量传输阶段的电流 0，或停止位，才结束会话。 */
    if (c->stop || (l->phase == CHG_PHASE_RUN && !from_can_bcl_only && c->i_req_01a == 0u)) {
        enter_stop(l);
        return;
    }
    if (l->phase == CHG_PHASE_STOP || l->phase == CHG_PHASE_OFF) {
        begin_handshake(l);
        l->ctrl = *c;
        l->have_ctrl = 1u;
        l->link_ok = 1u;
    }
    if (l->phase == CHG_PHASE_HS && c->allow == 0u) {
        l->seen_allow0 = 1u;
        if (l->port != CHG_PORT_SIF) {
            l->advertise_ready = 1u;
            l->cap_dirty = 1u;
        }
    }
    if (was_pause && c->pause == 0u && l->phase == CHG_PHASE_RUN) {
        /* 私有链路不因暂停时长结束会话。满 10 min 发 CST 只在桩侧。 */
        l->resume_arm = 1u;
    }
    if (l->port == CHG_PORT_SIF && l->phase == CHG_PHASE_HS) {
        if (l->sif_got < 3u) {
            l->sif_got++;
        }
        if (l->sif_got >= 3u) {
            l->phase = CHG_PHASE_RUN;
            l->advertise_ready = 1u;
        }
    }
    if (l->phase == CHG_PHASE_HS && l->port != CHG_PORT_SIF && l->seen_allow0 && c->allow) {
        l->phase = CHG_PHASE_RUN;
    }
}

static int remember_bat(ChrLink *l, const uint8_t data[28])
{
    ChgBat bat;
    if (load_bat(data, &bat) != 0) {
        l->rx_drop++;
        return -1;
    }
    l->bat = bat;
    l->bat_seen = 1u;
    l->bat_age_ms = 0;
    return 0;
}

int chr_link_bat(const ChrLink *l, ChgBat *out)
{
    if (out != 0) {
        memset(out, 0, sizeof *out);
    }
    if (l == 0 || l->bat_seen == 0u || l->bat.valid == 0u || l->bat_age_ms > CHG_BAT_STALE_MS) {
        return -1;
    }
    if (out != 0) {
        *out = l->bat;
    }
    return 0;
}

void chr_link_init(ChrLink *l, uint8_t port, const ChgCap *cap)
{
    memset(l, 0, sizeof *l);
    l->port = port;
    l->phase = CHG_PHASE_OFF;
    if (cap != 0) {
        l->cap = *cap;
    }
    l->cap.ready = 0u;
    l->cap_dirty = 1u;
}

void chr_link_tick(ChrLink *l, const ChgStat *meas, uint32_t dt_ms, ChrTx *tx)
{
    int slow;
    int fast;
    uint8_t mute_tx = 0u;
    ChgCan f;
    ChgCap advertised;
    ChgStat none;

    if (tx == 0) {
        return;
    }
    memset(tx, 0, sizeof *tx);
    if (l == 0 || (l->port != CHG_PORT_SIF && l->port != CHG_PORT_CAN && l->port != CHG_PORT_485)) {
        return;
    }
    memset(&none, 0, sizeof none);
    if (meas == 0) {
        meas = &none;
    }
    if (l->bat_age_ms > 0xFFFFFFFFu - dt_ms) {
        l->bat_age_ms = 0xFFFFFFFFu;
    } else {
        l->bat_age_ms += dt_ms;
    }
    if (l->line_fault || l->aux_fault) {
        /* 线还断着，或辅助电源仍欠压：停在 STOP，不开上拉，本拍不发就绪。 */
        l->phase = CHG_PHASE_STOP;
        l->link_ok = 0u;
        l->advertise_ready = 0u;
        mute_tx = 1u;
    } else if (l->phase == CHG_PHASE_OFF) {
        begin_handshake(l);
    } else if (l->phase == CHG_PHASE_RUN) {
        l->loss_ms += dt_ms;
        if (l->port == CHG_PORT_CAN && l->bcl_seen) {
            l->bcl_gap_ms += dt_ms;
            if (l->bcl_gap_ms > 50u + 5u) {
                l->period_late = 1u;
            }
        }
        if (chg_loss_tripped(l->loss_ms)) {
            enter_stop(l);
            if (l->port != CHG_PORT_SIF) {
                begin_handshake(l);
                mute_tx = 1u;
            }
        }
    } else if (l->phase == CHG_PHASE_STOP && l->port != CHG_PORT_SIF) {
        begin_handshake(l);
    }
    slow = due(&l->slow_ms, dt_ms, T_SLOW_MS, &l->slow_arm);
    fast = due(&l->fast_ms, dt_ms, T_FAST_MS, &l->fast_arm);
    advertised = l->cap;
    advertised.ready = l->advertise_ready;

    if (l->port == CHG_PORT_CAN && mute_tx == 0u) {
        if (slow || l->cap_dirty) {
            if (l->phase == CHG_PHASE_HS || l->cap_dirty) {
                chg_can_cml(&advertised, &f);
                push_can(tx, &f);
            }
            chg_can_cro(l->advertise_ready, &f);
            push_can(tx, &f);
            l->cap_dirty = 0u;
        }
        if (fast && l->phase == CHG_PHASE_RUN && !(meas->fault || meas->fault_code)) {
            chg_can_ccs(meas, &f);
            push_can(tx, &f);
        }
        if (meas->fault || meas->fault_code) {
            chg_can_cst(&f);
            push_can(tx, &f);
        }
    }
    output_of(l, meas, tx, 1);
}

static void reply_485(ChrLink *l, uint8_t ready_now, const ChgStat *meas, ChrTx *tx)
{
    int n = 0;
    ChgCap cap;
    if (l->port != CHG_PORT_485) {
        return;
    }
    if (l->phase == CHG_PHASE_HS || l->cap_dirty) {
        cap = l->cap;
        cap.ready = ready_now;
        n = chg_cap_pack(&cap, tx->frame, sizeof tx->frame);
        l->cap_dirty = 0u;
    } else {
        n = chg_stat_pack(meas, tx->frame, sizeof tx->frame);
    }
    if (n > 0) {
        tx->frame_len = (uint8_t)n;
    }
}

static void take_ctrl_frame(ChrLink *l, const uint8_t *frame, uint8_t n, const ChgStat *meas, ChrTx *tx)
{
    uint8_t msg = 0;
    uint8_t data[28];
    uint8_t len = 0;
    ChgCtrl c;
    if (frame_unpack(frame, n, &msg, data, &len) <= 0) {
        return;
    }
    if (msg == 0x04u && len == 28u) {
        /* 只保存，不刷新 T_loss，不改写请求电流。新鲜有效时按 5.7 重算本周期输出。 */
        remember_bat(l, data);
        output_of(l, meas, tx, 0);
        return;
    }
    if (msg != 0x01u || len != 6u) {
        return;
    }
    if (load_ctrl(data, &c) != 0) {
        return;
    }
    /* 高于锁压 0.5 V：整帧无效。不刷新 T_loss，不改已保存的请求，也不改锁压。 */
    if (v_req_above_lock(c.v_req_01v, l->cap.v_max_01v)) {
        l->rx_drop++;
        output_of(l, meas, tx, 0);
        return;
    }
    {
        uint8_t ready_now = l->advertise_ready;
        note_ctrl(l, &c, 0);
        reply_485(l, ready_now, meas, tx);
    }
    output_of(l, meas, tx, 0);
}

void chr_link_rx_byte(ChrLink *l, uint8_t b, uint32_t gap_ms, const ChgStat *meas, ChrTx *tx)
{
    uint16_t total;
    ChgStat none;

    if (tx == 0) {
        return;
    }
    memset(tx, 0, sizeof *tx);
    if (l == 0 || (l->port != CHG_PORT_SIF && l->port != CHG_PORT_485)) {
        return;
    }
    memset(&none, 0, sizeof none);
    if (meas == 0) {
        meas = &none;
    }
    if (l->line_fault || l->aux_fault) {
        output_of(l, meas, tx, 0);
        return;
    }
    output_of(l, meas, tx, 0);
    /* OFF：上拉尚未打开。BMS 上电前仪表发来的字节整段丢掉，不计入握手。
     * 第一次 tick 进入 HS 之后才解析 0x01。 */
    if (l->port == CHG_PORT_SIF && l->phase == CHG_PHASE_OFF) {
        l->raw_n = 0;
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
    if (l->raw[4] > 28u) {
        l->raw_n = 0;
        return;
    }
    total = (uint16_t)l->raw[4] + 7u;
    if (total > sizeof l->raw) {
        l->raw_n = 0;
        return;
    }
    if (l->raw_n < total) {
        return;
    }
    take_ctrl_frame(l, l->raw, (uint8_t)total, meas, tx);
    l->raw_n = 0;
}

void chr_link_can_rx(ChrLink *l, const ChgCan *f, const ChgStat *meas, ChrTx *tx)
{
    ChgStat none;
    int bcl_only = 0;

    if (tx == 0) {
        return;
    }
    memset(tx, 0, sizeof *tx);
    if (l == 0 || f == 0 || l->port != CHG_PORT_CAN) {
        return;
    }
    memset(&none, 0, sizeof none);
    if (meas == 0) {
        meas = &none;
    }
    if (l->line_fault || l->aux_fault) {
        output_of(l, meas, tx, 0);
        return;
    }
    if (f->id == CHG_CAN_BAT && f->dlc >= 8u) {
        uint8_t seq = f->data[0];
        if (seq > 3u) {
            l->rx_drop++;
        } else {
            if (seq == 0u) {
                memset(l->bat_part, 0, sizeof l->bat_part);
                l->bat_mask = 0u;
            }
            memcpy(l->bat_part + (size_t)seq * 7u, f->data + 1, 7);
            l->bat_mask = (uint8_t)(l->bat_mask | (uint8_t)(1u << seq));
            if (l->bat_mask == 0x0Fu) {
                remember_bat(l, l->bat_part);
                l->bat_mask = 0u;
            }
        }
        output_of(l, meas, tx, 0);
        return;
    }
    if (f->id == CHG_CAN_BCL && f->dlc >= 5u && chg_id_accept(f->id, CHG_CAN_BCL)) {
        /* 字节 4 只认 0x01 恒压、0x02 恒流。其他值不刷新 T_loss，也不改已保存的请求。 */
        if (f->data[4] != 0x01u && f->data[4] != 0x02u) {
            l->rx_drop++;
            output_of(l, meas, tx, 0);
            return;
        }
        if (v_req_above_lock(get_u16(f->data), l->cap.v_max_01v)) {
            l->rx_drop++;
            output_of(l, meas, tx, 0);
            return;
        }
        if (l->bcl_seen) {
            l->period_late = (uint8_t)!chg_period_ok(50u, l->bcl_gap_ms);
        } else {
            l->period_late = 0u;
        }
        l->bcl_seen = 1u;
        l->bcl_gap_ms = 0u;
        l->ctrl.v_req_01v = get_u16(f->data);
        l->ctrl.i_req_01a = can_i_phys(get_u16(f->data + 2));
        l->ctrl.cv = (uint8_t)(f->data[4] == 0x01u);
        l->have_ctrl = 1u;
        l->loss_ms = 0;
        l->link_ok = 1u;
        bcl_only = 1;
    } else if (f->id == CHG_CAN_BRO && f->dlc >= 1u) {
        /* 只认 0x00 和 0xAA。其他值不改变允许位，也不把充电机置为就绪。 */
        if (f->data[0] != 0x00u && f->data[0] != 0xAAu) {
            l->rx_drop++;
            output_of(l, meas, tx, 0);
            return;
        }
        l->ctrl.allow = (uint8_t)(f->data[0] == 0xAAu);
        l->have_ctrl = 1u;
        /* 就绪帧不携带电流。能量传输的 T_loss 只由 BCL 刷新。 */
        l->link_ok = 1u;
        if (l->ctrl.allow == 0u && (l->phase == CHG_PHASE_HS || l->phase == CHG_PHASE_STOP ||
                                    l->phase == CHG_PHASE_OFF)) {
            if (l->phase != CHG_PHASE_HS) {
                begin_handshake(l);
            }
            l->seen_allow0 = 1u;
            l->advertise_ready = 1u;
            l->cap_dirty = 1u;
            l->have_ctrl = 1u;
            l->link_ok = 1u;
        }
    } else if (f->id == CHG_CAN_BSM && f->dlc >= 7u) {
        {
            uint8_t was_pause = l->ctrl.pause;
            /* 只看 bit4。bit4 = 0 为暂停。bit5–bit7 保留，不参与。 */
            l->ctrl.pause = (uint8_t)((f->data[6] & 0x10u) == 0u);
            if (was_pause && l->ctrl.pause == 0u && l->phase == CHG_PHASE_RUN) {
                l->resume_arm = 1u;
            }
        }
        l->have_ctrl = 1u;
        /* 状态帧可以改暂停位，不携带本拍电流，不刷新 T_loss。 */
        l->link_ok = 1u;
    } else if (f->id == CHG_CAN_BST && f->dlc >= 4u) {
        l->ctrl.stop = 1u;
        l->ctrl.i_req_01a = 0;
        l->have_ctrl = 1u;
        enter_stop(l);
    } else {
        l->rx_drop++;
        output_of(l, meas, tx, 0);
        return;
    }
    if (!bcl_only && l->ctrl.stop) {
        enter_stop(l);
    }
    if (l->phase == CHG_PHASE_HS && l->seen_allow0 && l->ctrl.allow && l->advertise_ready) {
        l->phase = CHG_PHASE_RUN;
    }
    output_of(l, meas, tx, 0);
}
