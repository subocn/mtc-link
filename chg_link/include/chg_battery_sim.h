/* 作者：苏勃(subo)
 * 共享简化电池模型 — 给 chg_evcc 桩侧模拟器与 e2e harness 同时用。
 *
 * 设计目标：在主机进程里把 "充电器接到电池上" 的物理行为近似出来，
 * 让 BMS-link / EVCC-link / charger-link 看到的是真接电池时应该看到
 * 的电压 / SOC / 温度演化，而不是裸请求值。
 *
 * 不模拟：滞回、SEI、电解液分解、二阶 RC、热扩散。只够做软件在环里的
 * 被控对象（低 SOC 时 OCV 低、端电压随充电电流上升、温度随 I²R 涨）。
 * 跑在主机上，不是硬件在环。
 */
#ifndef CHG_BATTERY_SIM_H
#define CHG_BATTERY_SIM_H

#include <stdint.h>

/* 简化电池模型：OCV 查表 + 串联内阻 + 一阶 RC pair + 简化热模型。
 *
 * 配置给一次后，运行期只读不写：
 *   cap_01ah      — 容量（0.1 Ah）
 *   cells_s/p     — 串/并联节数
 *   r_int_mohm    — 单体内阻（mΩ）
 *   rc_r_mohm     — RC pair 电阻（mΩ）
 *   rc_tau_s      — RC 时间常数（秒）
 *   t_ambient_c   — 环境温度（°C）
 *   thermal_mass  — 热容（J/°C），默认 96：40.0 A、30 mΩ、环境 25°C，1 s 后为 74°C
 *   ocv_mv / ocv_n— 单体 OCV 表（mV），默认 7 点 NCM
 *
 * 状态：
 *   soc_01pct     — 0-10000（0.01% 步进）
 *   v_rc_mv       — RC 两端残压（mV），可正可负
 *   t_cell_c      — 单体平均温度（°C）
 *   t_cell_max_c / t_cell_min_c — 极值跟踪，给 BSM 用 */
typedef struct SimBattery_s {
    uint16_t cap_01ah;
    uint16_t cells_s;
    uint16_t cells_p;
    uint16_t r_int_mohm;
    uint16_t rc_r_mohm;
    uint16_t rc_tau_s;
    int8_t   t_ambient_c;
    uint8_t  thermal_mass;
    uint16_t soc_01pct;
    int16_t  v_rc_mv;
    int8_t   t_cell_c;
    int8_t   t_cell_max_c;
    int8_t   t_cell_min_c;
    const uint16_t *ocv_mv;
    uint8_t  ocv_n;
    uint32_t ms_acc; /* 未满 1 s 的余数。50 ms 的一拍不再当成 1 s。 */
} SimBattery;

void sim_battery_init(SimBattery *b, uint16_t cap_01ah, uint16_t cells_s,
                      uint16_t cells_p, int8_t t_ambient_c);
/* 推一拍的电池状态。dt_ms 是上次 tick 到现在的毫秒数；i_in_01a 是充电电流
 * （电池看进去的电流，>0 充电）。电池不验 sign，由调用方负责。 */
void sim_battery_tick(SimBattery *b, uint32_t dt_ms, uint16_t i_in_01a);

/* 只读输出 — 这些就是 CCS / BSM / BSD 应当看到的实测值。 */
uint16_t sim_battery_v_term_01v(const SimBattery *b);
uint16_t sim_battery_v_cell_max_mv(const SimBattery *b);
uint16_t sim_battery_v_cell_min_mv(const SimBattery *b);
uint16_t sim_battery_soc_01pct(const SimBattery *b);
uint16_t sim_battery_remain_min(const SimBattery *b);
int8_t   sim_battery_t_max_c(const SimBattery *b);
int8_t   sim_battery_t_min_c(const SimBattery *b);

#endif