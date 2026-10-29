/* 作者：苏勃(subo)
 * SPDX-License-Identifier: Apache-2.0
 * 共享的 BMS↔充电机 CAN 标识符。
 *
 * 两边工程目录独立（chg_bms / chg_charger），但 CAN 标识符必须完全一致。
 * 集中到这里，避免两边 #define 数值漂移。e2e harness 也包含此头以校验。
 *
 * 桩侧（chg_evcc）的标识符另见 chg_evcc.h：GB/T 27930 桩 CAN 是另一组 ID。
 */
#ifndef CHG_LINK_CAN_IDS_H
#define CHG_LINK_CAN_IDS_H

#include <stdint.h>

/* BMS → 充电机 */
#define CHG_CAN_BCL 0x181056F4u  /* BMS 充电需求（电压/电流） */
#define CHG_CAN_BRO 0x180956F4u  /* BMS 就绪 */
#define CHG_CAN_BSM 0x181356F4u  /* BMS 状态（单体电压/温度/SOC） */
#define CHG_CAN_BST 0x181956F4u  /* BMS 终止 */

/* 充电机 → BMS */
#define CHG_CAN_CML 0x1808F456u  /* 充电机能力（最大/最小电压/电流） */
#define CHG_CAN_CRO 0x180AF456u  /* 充电机就绪 */
#define CHG_CAN_CCS 0x1812F456u  /* 充电机状态（实测输出） */
#define CHG_CAN_CST 0x181AF456u  /* 充电机终止 */

/* J1939 专用 A 类 0x04，PF=0xEF。仅在 BMS↔充电机之间使用，禁止放到桩 CAN。 */
#define CHG_CAN_BAT 0x18EF56F4u

/* 通信超时窗口。标称 1000 ms 内不得判超时，1200 ms 必须停。
 * BMS 与充电机共用这一对常数，避免两侧各写一个数。 */
#define CHG_T_LOSS_NOM_MS 1000u
#define CHG_T_LOSS_MS 1200u

static inline int chg_loss_tripped(uint32_t loss_ms)
{
    return loss_ms >= CHG_T_LOSS_MS;
}

/* 周期不小于 50 ms 时，间隔落在标称值 ±10% 内不算超时。
 * gap 为 0 表示同一拍里的连发，还没有形成周期。标称小于 50 ms 时本条款不套用。 */
static inline int chg_period_ok(uint32_t nominal_ms, uint32_t gap_ms)
{
    uint32_t tol;
    if (nominal_ms < 50u) {
        return 1;
    }
    if (gap_ms == 0u) {
        return 1;
    }
    tol = nominal_ms / 10u;
    if (gap_ms + tol < nominal_ms) {
        return 0;
    }
    if (gap_ms > nominal_ms + tol) {
        return 0;
    }
    return 1;
}

/* 优先级（bit 26–28）、源地址、以及 PDU1 的目的地址必须与期望标识符一致。
 * 任一不符就丢掉，调用方不得据此停机。 */
static inline int chg_id_accept(uint32_t got, uint32_t expect)
{
    uint32_t pf = (expect >> 16) & 0xFFu;
    uint32_t mask = 0x1FFF00FFu; /* 优先级、保留位、数据页、PF、源地址 */
    if (pf < 240u) {
        mask |= 0x0000FF00u; /* 目的地址 */
    }
    return ((got ^ expect) & mask) == 0u;
}

#endif