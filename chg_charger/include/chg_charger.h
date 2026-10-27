/* 作者：苏勃(subo)
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
    uint8_t resume_arm;       /* 暂停刚撤销。chr_link_tick 的第一拍电流不超过 20 A，电压先降 1 V */
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
