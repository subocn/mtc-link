/* 作者：苏勃(subo)
 * SPDX-License-Identifier: Apache-2.0
 * 充电机侧充电通信。充电机侧实现。
 * 语义见 doc/protocol.md。本工程按充电控制决定输出电流。
 * 电池数据只从 BMS 的 0x04 取得。本工程不估算荷电状态，也不据此改请求电流。
 * 新鲜有效的 0x04：最大电压高于允许电压，或两侧化学体系都已声明且不同，本周期输出为 0。
 * 一次会话只用一个口：一线通、CAN 或 RS-485。
 */
#ifndef CHG_CHARGER_H
#define CHG_CHARGER_H

#include <stddef.h>
#include <stdint.h>

#include "chg_link_can_ids.h"
#include "chg_link_common.h"

#define CHG_PORT_SIF 1u
#define CHG_PORT_CAN 2u
#define CHG_PORT_485 3u

#define CHG_PHASE_OFF 0u
#define CHG_PHASE_HS 1u
#define CHG_PHASE_RUN 2u
#define CHG_PHASE_STOP 3u

/* 充电机侧附加：电池信息 0x04 失效判定阈值。其他 CAN-ID 见 chg_link_can_ids.h。 */
#define CHG_BAT_STALE_MS 3000u

typedef struct {
    uint16_t i_out_01a;
    uint16_t v_target_01v;
    uint8_t  enable;
} ChgAct;

/* ChgCtrl/ChgCap/ChgStat/ChgBat/ChgCan 由 chg_link_common.h 提供。 */

typedef struct {
    uint8_t port;
    uint8_t phase;
    uint8_t sif_got;
    uint8_t have_ctrl;
    uint8_t link_ok;
    uint8_t advertise_ready;
    uint8_t seen_allow0;
    uint8_t cap_dirty;
    uint8_t bat_seen;
    uint8_t bat_mask;
    uint32_t slow_ms;
    uint32_t fast_ms;
    uint32_t loss_ms;
    uint32_t bcl_gap_ms;      /* 距上一帧合法 BCL。±10% 只记 period_late */
    uint32_t rx_drop;
    uint8_t bcl_seen;
    uint8_t period_late;
    uint32_t bat_age_ms;
    uint8_t slow_arm;
    uint8_t fast_arm;
    uint8_t line_fault;       /* §10 验收 15: 物理断开已锁存。期间输出 0 A，不开上拉 */
    uint8_t aux_fault;        /* 辅助电源欠压满 100 ms。输出保持 0，直到电压恢复 */
    uint8_t resume_arm;       /* 暂停刚撤销。第一拍电流不超过 20 A，电压目标为钳位后的目标再减 1.0 V */
    uint32_t line_down_ms;    /* SIF 拉低 / CAN bus-off / RS-485 短路累计，阈值 100 ms */
    uint32_t aux_uv_ms;
    ChgCap cap;
    ChgCtrl ctrl;
    ChgBat bat;
    uint8_t bat_part[28];
    uint8_t raw[40];
    uint8_t raw_n;
} ChrLink;

typedef struct {
    ChgAct act;
    uint8_t frame[24];
    uint8_t frame_len;
    ChgCan can[3];
    uint8_t can_n;
    uint8_t phase;
    uint8_t link_ok;
} ChrTx;

/* 组充电机要发出的能力、状态，以及 CAN 上的 CML / CRO / CCS / CST。 */
int chg_cap_pack(const ChgCap *c, uint8_t *out, size_t cap);
int chg_stat_pack(const ChgStat *s, uint8_t *out, size_t cap);
void chg_can_cml(const ChgCap *c, ChgCan *f);
void chg_can_cro(uint8_t ready, ChgCan *f);
void chg_can_ccs(const ChgStat *s, ChgCan *f);
void chg_can_cst(ChgCan *f);

/* 解析 BMS 发来的 0x04。成功返回 35。不根据电压补荷电状态。 */
int chg_bat_unpack(const uint8_t *in, size_t n, ChgBat *b);

/* 有效位为 1 且年龄不超过 3 s 时返回 0，并写出最近一帧。
 * 否则返回 -1，并把 out 清零。失败时不得把 out 填到桩侧。
 * 年龄由 chr_link_tick 累计，0x04 不刷新充电控制的 1 s 超时。 */
int chr_link_bat(const ChrLink *l, ChgBat *out);

/* cap_valid=0：一线通，不要求就绪，仍用书面最大电压和最大电流。
 * 故障、请求电压为 0、最大电压或最大电流为 0、双路径同时有效：输出 0。 */
void chg_apply(const ChgCtrl *ctrl, const ChgCap *cap, uint8_t cap_valid, uint8_t link_ok,
               uint8_t paths_conflict, uint8_t charger_fault, ChgAct *act);

/* 桩侧种类。交流函数不接受直流，直流函数不接受交流。 */
#define CHG_PILE_GB_AC 1u
#define CHG_PILE_EU_AC 2u
#define CHG_PILE_GB_DC 3u
#define CHG_PILE_EU_DC 4u

/* 控制导引已分类的标称电压，单位 mV。只认这五档，不在这里另造容差带。 */
#define CHG_CP_FAULT_MV 0u
#define CHG_CP_VENT_MV 3000u
#define CHG_CP_READY_MV 6000u
#define CHG_CP_PRESENT_MV 9000u
#define CHG_CP_OPEN_MV 12000u

typedef struct {
    uint8_t send; /* 0：不发桩侧电压电流请求。调用方发停止，不发电流为 0 的需求帧 */
    uint16_t v_01v;
    uint16_t i_01a;
} ChgPileDemand;

/* 交流进线允许电流，0.1 A。kind 只接受国标交流或欧标交流，否则返回 0。
 * cp_nominal_mv 不是 6000 时返回 0。duty_permille 里 533 表示 53.3%。
 * ohm 只认 1500、680、220、100，其他返回 0。100 Ω 单相返回 32.0 A，不取 63 A。
 * 高于 85% 至 96% 按 (百分数 − 64) × 2.5 A，四舍五入到 0.1 A，不得改用 ×0.6。
 * 高于 96% 至 97% 为 80.0 A。这两档再与电缆档和额定取较小者，单相不得超过 32.0 A。
 * rated_inlet_01a 为调用方给出的进线额定，0 表示未知，返回 0。
 * 低于 3%、高于 7% 且低于 8%、高于 97% 为 0，数字电流也不放开。
 * 3% 至 7% 没有模拟电流。digital_valid 为 0，或 digital_01a 为 0，则结果为 0。
 * digital_valid 为 1 且数字电流非 0 时，取数字电流、电缆档与额定的较小者。
 * 8% 至 97% 已有模拟电流时，digital_valid 为 1 再取较小者；数字电流为 0 则结果为 0。
 * digital_valid 为 0 时不另造数字限流。不改写已保存的 0x01。 */
uint16_t chg_ac_inlet_01a(uint8_t kind, uint16_t cp_nominal_mv, uint16_t duty_permille,
                          uint16_t ohm, uint16_t rated_inlet_01a, uint8_t digital_valid,
                          uint16_t digital_01a);

/* 电池侧电流只下调，0.1 A。请求、电压或进线电流为 0 则返回 0。结果不超过 i_req_01a。
 * eff_permille 由符合表传入，1000 表示 100%。大于 1000 或为 0 时返回 0。本函数不另定系数。
 * 上限向下取整，避免四舍五入后超过进线功率。
 * v_ac_min_01v 与 v_ac_max_01v 都为 0 时不检查交流电压窗口，也不另定上下限。
 * 只传入一端，或下限高于上限，或实测落在窗口外，返回 0。
 * 不得把进线安培数原样当成电池侧电流。 */
uint16_t chg_ac_batt_01a(uint16_t i_req_01a, uint16_t v_batt_01v, uint16_t v_ac_01v,
                         uint16_t i_inlet_01a, uint16_t eff_permille, uint16_t v_ac_min_01v,
                         uint16_t v_ac_max_01v);

/* 直流桩目标。国标直流与欧标直流量值用同一函数。out 为空则直接返回。
 * 本机输入窗口或桩输出电压范围任一端为 0，或下限高于上限，则 send 为 0。
 * 桩电流上限为 0 表示没收到能力，send 为 0。电流下限为 0 表示桩没有下限。
 * 电流下限高于上限则 send 为 0。eff_permille 为 0 或大于 1000 则 send 为 0。
 * 两个窗口没有交集则 send 为 0。
 * 进线落在交集内用进线。进线在本机窗口内但高于交集上限时，向下取交集上限。
 * 进线在本机窗口内但低于交集下限时，取交集下限。进线在本机窗口外则 send 为 0，
 * 不得把窗口外的电压抬进窗口。没有实测时，上一次目标仍在交集内则用上一次，否则用交集下限。
 * 电流按电池侧功率除以目标电压向上取整，再与桩最大电流取较小者。
 * 小于桩最小电流时 send 为 0，不发一帧会让桩按最小能力续出的需求。
 * 下限为 0 时，这一条不拒绝正电流。
 * 暂停时电压保持上一次仍在交集内的值，不改成电池口电压，也不改成 0。
 * 暂停电流高于桩最大电流时只向下钳；低于桩最小电流，或上一次电压不在交集内，则 send 为 0。
 * 目标电压不取电池口请求电压。电池口是否按本结果下调，由 chg_dc_batt_01a 另算。 */
void chg_dc_pile_demand(uint8_t kind, uint16_t v_win_lo_01v, uint16_t v_win_hi_01v,
                        uint16_t v_inlet_01v, uint16_t v_batt_01v, uint16_t i_batt_01a,
                        uint16_t eff_permille, uint16_t v_pile_min_01v, uint16_t v_pile_max_01v,
                        uint16_t i_pile_min_01a, uint16_t i_pile_max_01a, uint8_t pause,
                        uint16_t v_prev_01v, uint16_t i_prev_01a, ChgPileDemand *out);

/* 国标直流公共桩 CAN 的充电电流原始值。物理值为负，原始值 = 4000 − i_01a。
 * 40.0 A 充电得到 3600，不是本链路 BCL 的 4400。恰好 400.0 A 得到 0。
 * 大于 400.0 A，或 raw 为空，返回 0，且不写出原始值。
 * 0 A 得到 4000；调用方仍不得发送电流为 0 的需求帧。欧标 EXI 不用这个原始值。 */
uint8_t chg_gb_dc_i_raw(uint16_t i_01a, uint16_t *raw);

/* 国标直流公共桩 CAN 电流的反向换算，得到正的 0.1 A 充电计数。
 * 原始值大于 4000 时失败，且不写出。不得把本链路 BCL 的 4400 当成 40.0 A 充电。
 * 恰好 4000 得到 0 A。恰好 0 得到 400.0 A。i_01a 为空时返回 0。欧标 EXI 不用。 */
uint8_t chg_gb_dc_i_phys(uint16_t raw, uint16_t *i_01a);

/* 直流电池侧只下调，0.1 A。i_pile_01a 必须是 chg_dc_pile_demand 已经与桩最大取过小的电流。
 * 传入钳位前的计算值会让电池口按桩给不出的功率出流。
 * 上限向下取整，结果不超过 i_req_01a，不得抬高。
 * 请求、电池电压、桩电压、桩电流为 0，或效率为 0、大于 1000，返回 0。
 * 结果为 0 时电池口不出流，桩侧也不要发该需求，改发停止。暂停时请求已是 0，本函数返回 0。 */
uint16_t chg_dc_batt_01a(uint16_t i_req_01a, uint16_t v_batt_01v, uint16_t v_pile_01v,
                         uint16_t i_pile_01a, uint16_t eff_permille);

/* 国标直流 CML 的最大、最小充电电流。两只原始值都按 chg_gb_dc_i_phys 解码，再按幅值排序。
 * 较大的写入 i_max，较小的写入 i_min。字段标签写反仍得到同一区间。
 * 任一原始值大于 4000，或指针为空，失败且不写出。排序后的最大为 0 时失败。
 * 最小解出 0 表示没有下限。欧标 EXI 不用。 */
uint8_t chg_gb_dc_cml_i(uint16_t raw_max, uint16_t raw_min, uint16_t *i_min, uint16_t *i_max);

/* 充电结束后能否再开一回桩侧握手。暂停不是结束。本函数不读 CC2、CP，不组帧。 */
#define CHG_PILE_END_PAUSE  0u /* 同会话保持。不断流结束，也不新开握手 */
#define CHG_PILE_END_NORMAL 1u /* 已经断流并离开能量传输。仍连接时可以重新握手 */
#define CHG_PILE_END_REPLUG 2u /* 必须先断开再连接。含 BSM 超时，以及绝缘、连接器、单体过压、单体欠压 */
#define CHG_PILE_END_RETRY  3u /* 仅国标直流处理方式 c。次数用尽后与必须拔枪相同 */

/* 调用方已经分类的插头。 */
#define CHG_PLUG_HELD     0u /* 结束后仍连接，没有见过断开 */
#define CHG_PLUG_OPEN     1u /* 当前已经断开，还没有再次连接 */
#define CHG_PLUG_RESEATED 2u /* 结束后见过断开，并且已经再次连接 */

typedef struct {
    uint8_t cut;     /* 1：断流并离开能量传输。0：留在原会话 */
    uint8_t restart; /* 1：允许开始下一次桩侧握手 */
} ChgPileRestart;

/* kind 用 CHG_PILE_*。reason 用 CHG_PILE_END_*。plug 用 CHG_PLUG_*。
 * retries_left 只在处理方式 c 时表示还剩的重握次数，其他原因不看。
 * out 为空则直接返回。种类、原因或插头无法识别时 cut 为 1、restart 为 0。
 * 暂停时 cut 为 0、restart 为 0：不得发结束报文，也不得要求拔枪。
 * 正常结束时 cut 为 1。插头仍连接或已经重新连接则 restart 为 1；当前断开为 0。
 * 交流正常结束不要求拔枪。国标直流正常结束可以在仍连接时重新握手。
 * 欧标直流未把 ISO 15118-2 写成 Pause 时，停止用正常结束。Pause 用暂停。
 * 必须拔枪时，只有已经重新连接才 restart 为 1。
 * 处理方式 c 只接受国标直流。次数还在、且插头不是当前断开时 restart 为 1。
 * 次数为 0 时与必须拔枪相同。交流或欧标直流传入该原因则 restart 为 0。 */
void chg_pile_restart(uint8_t kind, uint8_t reason, uint8_t plug,
                      uint8_t retries_left, ChgPileRestart *out);

/* cap 是本机输出能力。一线通把书面限值放在这里。就绪位由会话改写。
 * 初始化后为 OFF。第一次 tick 进入握手。 */
void chr_link_init(ChrLink *l, uint8_t port, const ChgCap *cap);

/* 周期调用。一线通和 RS-485 不在本函数里发帧。CAN 到点填写 tx。
 * 实测状态用于 CCS，以及故障和交直流两路同时有效时的停流。
 * RS-485 的应答在 chr_link_rx_byte 里，收到 0x01 后 80 ms 内发出。
 * 能量传输中 1 秒没有合法 0x01 时，CAN 与 RS-485 回到握手且本周期不发送。 */
void chr_link_tick(ChrLink *l, const ChgStat *meas, uint32_t dt_ms, ChrTx *tx);

/* 一线通或 RS-485 收 BMS 字节。gap_ms 超过 50 ms 丢弃半帧。
 * 0x01 是充电请求。0x04 是电池信息，不计入握手，也不应答应。
 * 一线通发送长度始终为 0。RS-485 只在收到 0x01 时把应答放进 tx->frame。 */
void chr_link_rx_byte(ChrLink *l, uint8_t b, uint32_t gap_ms, const ChgStat *meas, ChrTx *tx);

/* CAN 收到 BMS 的 BCL / BRO / BSM / BST，或四帧 CHG_CAN_BAT。
 * 仅 BCL 电流为 0 不结束会话。电池帧不刷新 1 s 超时。 */
void chr_link_can_rx(ChrLink *l, const ChgCan *f, const ChgStat *meas, ChrTx *tx);

/* §6.6 / §10 验收 15: 物理断开。电平或故障持续 ≥ 100 ms 后输出 0 A，
 * 会话停在 STOP，不再打开上拉，也不把随后的字节当成新的充电请求。
 * 每个函数只作用于自己的口；线恢复（参数为健康电平）后才允许下一次握手。
 * SIF：b=1 为 idle，b=0 为拉低。bus_off / shorted：1 为故障。 */
void chr_link_set_aux_uv(ChrLink *l, uint8_t aux_undervolt, uint32_t dt_ms);
void chr_link_set_sif_state(ChrLink *l, uint8_t b, uint32_t dt_ms);
void chr_link_set_bus_off(ChrLink *l, uint8_t bus_off, uint32_t dt_ms);
void chr_link_set_485_short(ChrLink *l, uint8_t shorted, uint32_t dt_ms);

#endif
