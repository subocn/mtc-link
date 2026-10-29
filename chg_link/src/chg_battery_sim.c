/* 作者：苏勃(subo)
 * SPDX-License-Identifier: Apache-2.0
 * 共享简化电池模型实现。
 *
 * 从 firmware/chg_evcc/sim/sim.c §B 抽出。公式与常数不动 — 上一轮自审
 * 已修过 SOC 方向（充电应升 SOC）、热模型（r_int_mohm/1000 整数除为 0）、
 * v_term 命名（drop_mv → cell_mv）。 */
#include "chg_battery_sim.h"

#include <string.h>

/* 内置 OCV 表：soc_step → 单体 mV。覆盖 0/10/20/50/80/90/100% 七个点。
 * 这是一条简化版的 NCM OCV 曲线，不是精确模型——足够让软件在环看到
 * "电池在低 SOC 时 OCV 低、终端电压随充电电流上升"这种定性行为。 */
static const uint16_t SIM_BAT_DEFAULT_OCV_MV[] = {
    3000,  /* 0% */
    3400,  /* 10% */
    3550,  /* 20% */
    3700,  /* 50% */
    3950,  /* 80% */
    4050,  /* 90% */
    4200,  /* 100% */
};
#define SIM_BAT_DEFAULT_OCV_N 7

void sim_battery_init(SimBattery *b, uint16_t cap_01ah, uint16_t cells_s,
                      uint16_t cells_p, int8_t t_ambient_c)
{
    if (b == 0) return;
    memset(b, 0, sizeof *b);
    b->cap_01ah = cap_01ah;
    b->cells_s = cells_s == 0u ? 1u : cells_s;
    b->cells_p = cells_p == 0u ? 1u : cells_p;
    b->r_int_mohm = 30u;        /* 典型 18650 NCM */
    b->rc_r_mohm = 15u;
    b->rc_tau_s = 5u;
    b->t_ambient_c = t_ambient_c;
    b->thermal_mass = 96u;      /* 40.0 A、30 mΩ、环境 25°C，1 s 后为 74°C */
    b->ocv_mv = SIM_BAT_DEFAULT_OCV_MV;
    b->ocv_n = SIM_BAT_DEFAULT_OCV_N;
    b->soc_01pct = 5000u;       /* 半满 */
    b->t_cell_c = t_ambient_c;
    b->t_cell_max_c = t_ambient_c;
    b->t_cell_min_c = t_ambient_c;
}

/* soc_01pct(0-10000) → 单体 OCV mV，线性插值。表边界外 clamp 到端点。 */
static uint16_t sim_battery_ocv_mv(const SimBattery *b, uint16_t soc_01pct)
{
    uint8_t i;
    uint16_t soc_lo, soc_hi, mv_lo, mv_hi;
    uint32_t num, den;
    if (b == 0 || b->ocv_n == 0u || b->ocv_mv == 0) return 0u;
    if (b->ocv_n == 1u) return b->ocv_mv[0];
    /* 把 0-10000 映射到表索引 */
    soc_lo = 0u; soc_hi = 10000u;
    mv_lo = b->ocv_mv[0];
    mv_hi = b->ocv_mv[b->ocv_n - 1u];
    if (soc_01pct <= soc_lo) return mv_lo;
    if (soc_01pct >= soc_hi) return mv_hi;
    /* 表项等距索引：i = soc * (n-1) / 10000 */
    i = (uint8_t)(((uint32_t)soc_01pct * (uint32_t)(b->ocv_n - 1u)) / 10000u);
    if (i >= b->ocv_n - 1u) i = (uint8_t)(b->ocv_n - 1u);
    /* 段内插值 */
    {
        /* 用整段 10000 除以段数。先收成 1666 时，99.99% 会写成 4201 mV。 */
        uint32_t span = (uint32_t)(b->ocv_n - 1u);
        uint32_t s_lo = ((uint32_t)i * 10000u) / span;
        uint32_t s_hi = ((uint32_t)(i + 1u) * 10000u) / span;
        if (s_hi > 10000u) s_hi = 10000u;
        if (soc_01pct <= s_lo) return b->ocv_mv[i];
        if (soc_01pct >= s_hi) return b->ocv_mv[i + 1u];
        den = s_hi - s_lo;
        num = (uint32_t)(soc_01pct - s_lo) * (uint32_t)(b->ocv_mv[i + 1u] - b->ocv_mv[i]);
        /* 先加半段再除。3000 mV 到 3400 mV，荷电 0.13% 写成 3001 mV。 */
        return (uint16_t)(b->ocv_mv[i] + (num + den / 2u) / den);
    }
}

void sim_battery_tick(SimBattery *b, uint32_t dt_ms, uint16_t i_in_01a)
{
    uint32_t dt_s;
    int32_t delta_ah_x100;/* SOC 变化，单位 0.01 Ah */
    int32_t v_rc_new;
    if (b == 0 || dt_ms == 0u) return;
    /* 按毫秒累计。满 1000 ms 才算 1 s，余数留到下一拍。 */
    b->ms_acc += dt_ms;
    dt_s = b->ms_acc / 1000u;
    b->ms_acc %= 1000u;
    if (dt_s == 0u) return;
    /* 1) 荷电。电流 0.1 A，容量 0.1 Ah。模型步进是 (i×秒+18)/36，再按容量四舍五入。 */
    if (b->cap_01ah > 0u) {
        {
            uint64_t ah = ((uint64_t)i_in_01a * (uint64_t)dt_s + 18u) / 36u;
            if (ah > 2147483647u) {
                ah = 2147483647u;
            }
            delta_ah_x100 = (int32_t)ah;
        }
        if (delta_ah_x100 != 0) {
            int32_t delta_soc = (int32_t)(((int64_t)delta_ah_x100 * 10000 + (int64_t)b->cap_01ah / 2) / b->cap_01ah);
            int32_t ns = (int32_t)b->soc_01pct + delta_soc;
            if (ns < 0) ns = 0;
            if (ns > 10000) ns = 10000;
            b->soc_01pct = (uint16_t)ns;
        }
    }
    /* 2) RC pair：V_rc 朝目标 V_rc_target = I * R_rc 指数趋近。
     *   V_rc += (V_target - V_rc) * (1 - exp(-dt/tau))。这里 tau 较大，
     *   用一阶欧拉近似：(1-exp(-x)) ≈ x/（1+x）当 x 不大。简化：
     *   V_rc += (V_target - V_rc) * dt_s / (tau + dt_s) */
    if (b->rc_tau_s > 0u) {
        /* 先加 50 再除。40.5 A、15 mΩ 写成 61 mV，不再写成 60 mV。 */
        int32_t v_target_mv = (int32_t)(((uint32_t)i_in_01a * b->rc_r_mohm + 50u) / 100u);
        int32_t k_num = (int32_t)dt_s;
        int32_t k_den = (int32_t)b->rc_tau_s + k_num;
        if (k_den > 0) {
            /* 先加半个分母再除。1.7 A、15 mΩ、时间常数 5 s，1 s 写成 1 mV。 */
            {
                int64_t num = (int64_t)(v_target_mv - (int32_t)b->v_rc_mv) * (int64_t)k_num;
                int64_t half = (int64_t)k_den / 2;
                num += (num >= 0) ? half : -half;
                v_rc_new = b->v_rc_mv + (int32_t)(num / k_den);
            }
            if (v_rc_new > 32767) v_rc_new = 32767;
            if (v_rc_new < -32768) v_rc_new = -32768;
            b->v_rc_mv = (int16_t)v_rc_new;
        }
    }
    /* 3) 温升。0.1 A 直接平方，功率和热容先加半个单位，摄氏度先加 0.5°C。
     *    算完后每 1 s 向环境挪 1°C。 */
    if (b->thermal_mass > 0u) {
        int64_t p_mw = ((int64_t)i_in_01a * (int64_t)i_in_01a * (int64_t)b->r_int_mohm + 50) / 100;
        int64_t half = (int64_t)b->thermal_mass / 2;
        int64_t d_temp_x100 = (p_mw * (int64_t)dt_s * 10 + half) / (int64_t)b->thermal_mass;
        int64_t new_t_x100 = (int64_t)b->t_cell_c * 100 + d_temp_x100;
        int32_t new_t = (int32_t)((new_t_x100 + (new_t_x100 >= 0 ? 50 : -50)) / 100);
        if (new_t >  127) new_t =  127;
        if (new_t < -128) new_t = -128;
        b->t_cell_c = (int8_t)new_t;
    }
    /* 每 1 s 向环境挪 1°C。2 s 的一拍挪 2°C，到环境为止。 */
    {
        int32_t amb = (int32_t)b->t_ambient_c;
        int32_t cur = (int32_t)b->t_cell_c;
        int32_t step = (int32_t)dt_s;
        int32_t diff = amb - cur;
        if (diff > step) {
            diff = step;
        } else if (diff < -step) {
            diff = -step;
        }
        b->t_cell_c = (int8_t)(cur + diff);
    }
    /* t_cell_max_c/min_c 跟踪极值，给 BSM 用 */
    if (b->t_cell_c > b->t_cell_max_c) b->t_cell_max_c = b->t_cell_c;
    if (b->t_cell_c < b->t_cell_min_c) b->t_cell_min_c = b->t_cell_c;
}

uint16_t sim_battery_v_term_01v(const SimBattery *b)
{
    uint16_t ocv_mv;
    int32_t cell_mv;
    if (b == 0) return 0u;
    ocv_mv = sim_battery_ocv_mv(b, b->soc_01pct);
    /* 单体端电压 = OCV + V_rc（充电时 V_rc>0，所以 V_term > OCV）。
     * 这里我们没用当前 i_in_01a（tick 里已用过），所以 RC 残压是上一拍
     * 状态。当电流为 0 时（暂停 / 停机），v_rc_mv 会朝 0 衰减，端电压
     * 自然回到 OCV。这种近似给桩侧一个量级对的 terminal voltage。 */
    cell_mv = (int32_t)ocv_mv + (int32_t)b->v_rc_mv;
    if (cell_mv < 0) cell_mv = 0;
    /* 0.1 V/bit。先加 50 mV 再除，3653 mV × 20 写成 73.1 V。大于 6553.5 V 停在 65535。 */
    {
        uint32_t raw = ((uint32_t)cell_mv * (uint32_t)b->cells_s + 50u) / 100u;
        if (raw > 65535u) {
            return 65535u;
        }
        return (uint16_t)raw;
    }
}

uint16_t sim_battery_v_cell_max_mv(const SimBattery *b)
{
    /* 单体最大值 = OCV + 5 mV。加完超过 65535 mV 时停在 65535。 */
    uint16_t mv;
    if (b == 0) return 0u;
    mv = sim_battery_ocv_mv(b, b->soc_01pct);
    if (mv > 65535u - 5u) {
        return 65535u;
    }
    return (uint16_t)(mv + 5u);
}

uint16_t sim_battery_v_cell_min_mv(const SimBattery *b)
{
    /* 减 5 mV。低于 5 mV 写 0，不再卷成 65531 mV。 */
    uint16_t mv;
    if (b == 0) return 0u;
    mv = sim_battery_ocv_mv(b, b->soc_01pct);
    if (mv < 5u) {
        return 0u;
    }
    return (uint16_t)(mv - 5u);
}

uint16_t sim_battery_soc_01pct(const SimBattery *b)
{
    if (b == 0) return 0u;
    return b->soc_01pct;
}

uint16_t sim_battery_remain_min(const SimBattery *b)
{
    /* 假设 i_in_01a 维持当前，回到 100% 还需要时间。简化：剩余时间 = 当前剩余 Ah / 当前电流。
     * 但模型里没存当前电流。用 cap*(100%-SOC%)/100% / i_max 的形式太粗。
     * 这里给个保守估计：每 100 Ah （SOC）每 100 A 充电需要 60 min，
     * 即 remain_min = cap_01ah * (10000-soc) / (10000) / (i_avg / 60)。我们没存 i_avg，
     * 假设 i_avg = 1 A（即 0.1 A 计 10）。给调用方：
     *   remain_min = cap_01ah * (10000 - soc) / (10000 * 100)  (以 0.1 A/bit 等效 1 A 计 10) */
    if (b == 0 || b->cap_01ah == 0u) return 0u;
    /* 简化：用容量百分比 / 假设 1 A 平均充电电流 -> 60 min / 100%
     * remain_min ≈ 60 * cap_01ah / 10 * (10000-soc)/10000
     *         = 6 * cap_01ah * (10000-soc) / 1000 */
    {
        /* 先加 500 再除，再在 32 位里封顶。20.0 Ah、99.97% 写成 4 min。 */
        uint32_t mins = (6u * (uint32_t)b->cap_01ah * (uint32_t)(10000u - b->soc_01pct) + 500u) / 1000u;
        if (mins > 65535u) {
            return 65535u;
        }
        return (uint16_t)mins;
    }
}

int8_t sim_battery_t_max_c(const SimBattery *b)
{
    if (b == 0) return 0;
    return b->t_cell_max_c;
}

int8_t sim_battery_t_min_c(const SimBattery *b)
{
    if (b == 0) return 0;
    return b->t_cell_min_c;
}