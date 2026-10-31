/* 作者：苏勃(subo)
 * SPDX-License-Identifier: Apache-2.0
 * BMS 侧充电通信。BMS 侧实现。
 * 语义见 doc/protocol.md。本工程不计算输出电流。
 * CAN 与 RS-485 对上报电流做近 0、残余和高出三项检查。一线通不做。
 * 电池信息只在 0x04 里发给充电机，不参与充电控制。
 * 一次会话只用一个口：一线通、CAN 或 RS-485。
 */
#ifndef CHG_BMS_H
#define CHG_BMS_H

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

/* 上报电流检查的原因。不进 0x01–0x08。0 表示这三项尚未触发。 */
#define CHG_I_ALARM_NONE 0u
#define CHG_I_ALARM_STUCK 1u
#define CHG_I_ALARM_RESID 2u
#define CHG_I_ALARM_HIGH 3u

/* ChgCtrl/ChgCap/ChgStat/ChgBat/ChgCan 由 chg_link_common.h 提供。 */

typedef struct {
    uint8_t port;
    uint8_t phase;
    uint8_t sif_sent;
    uint8_t seen_ready;
    uint8_t force_stop;
    uint8_t link_ok;
    uint8_t waiting;
    uint8_t answered;
    uint8_t stop_due;
    uint8_t bat_pending;
    uint8_t bat_turn;
    uint8_t aux_undervolt;   /* §10 验收 13: 辅助电源欠压状态（0=正常，1=欠压） */
    uint8_t line_fault;       /* §10 验收 15: 物理断开已锁存。线恢复前不接受对端帧 */
    uint32_t aux_uv_ms;       /* 欠压累计时间；累计 ≥ 100 ms 触发 force_stop */
    uint32_t sif_short_ms;    /* SIF 拉低累计 ≥ 100 ms 触发物理断开 */
    uint32_t line_down_ms;    /* CAN bus-off / RS-485 短路累计，阈值同为 100 ms */
    uint32_t slow_ms;
    uint32_t fast_ms;
    uint32_t loss_ms;
    uint32_t ccs_gap_ms;      /* 距上一帧合法 CCS。±10% 只记 period_late */
    uint32_t rx_drop;         /* 优先级 / 源地址 / 目的地址不符 */
    uint8_t ccs_seen;
    uint8_t period_late;
    uint8_t i_alarm;          /* CHG_I_ALARM_*。已经因故障停过就不再改写 */
    uint8_t i_report_seen;    /* 已收过一帧无故障的 CCS 或 0x03 */
    uint8_t cmd_pause;        /* 最近一拍调用方命令。CAN 收帧没有 ctrl，用这组快照 */
    uint8_t cmd_stop;
    uint16_t cmd_i_req_01a;
    uint32_t i_low_ms;        /* 跟踪窗内上报仍 ≤0.5 A 的累计 */
    uint32_t i_zero_cmd_ms;   /* 零输出命令已经发出之后的连续时间 */
    uint8_t zero_armed;       /* 本段零输出命令已经交给发送。发出当拍不累计 */
    uint32_t wait_ms;
    uint8_t slow_arm;
    uint8_t fast_arm;
    uint32_t bat_ms;
    uint8_t bat_arm;
    ChgCap cap;
    ChgStat stat;
    ChgBat bat;
    uint8_t raw[40];
    uint8_t raw_n;
} BmsLink;

typedef struct {
    uint8_t frame[40];
    uint8_t frame_len;
    uint8_t bat_frame[40];
    uint8_t bat_len;
    ChgCan can[8];
    uint8_t can_n;
    uint8_t phase;
    uint8_t link_ok;
} BmsTx;

/* 组 BMS 要发出的充电控制，以及 CAN 上的 BCL / BRO / BSM / BST。 */
int chg_ctrl_pack(const ChgCtrl *c, uint8_t *out, size_t cap);
void chg_can_bcl(const ChgCtrl *c, ChgCan *f);
void chg_can_bro(uint8_t allow, ChgCan *f);
void chg_can_bsm(uint8_t pause, uint8_t flags, ChgCan *f);
void chg_can_bst(ChgCan *f);

/* 0x04，35 字节。未调用 bms_link_set_bat 时，发出的有效位为 0。 */
int chg_bat_pack(const ChgBat *b, uint8_t *out, size_t cap);
/* 四帧，序号 0 到 3。data[0] 为序号，data[1..7] 为载荷的 7 字节。 */
void chg_can_bat(const ChgBat *b, ChgCan f[4]);

void bms_link_init(BmsLink *l, uint8_t port);

/* 电池数据只从这里来。传入 0 则之后的 0x04 有效位为 0。不改变充电控制。 */
void bms_link_set_bat(BmsLink *l, const ChgBat *b);

/* 一线通、CAN、RS-485 的周期发送。dt_ms 为距上次调用的毫秒。
 * 调用间隔：CAN 不大于 50 ms，一线通和 RS-485 不大于 250 ms。
 * 跨过多个周期时，每种帧只保留最新的一帧。
 * 电池信息由 BMS 按约 1 s 发送，充电机不查询。
 * 一线通放进 bat_frame，不计入握手帧数。CAN 为四帧 CHG_CAN_BAT。
 * RS-485 由 BMS 先发 0x01。上一帧没有应答时重发 0x01，不改发 0x04。
 * 第一次应答之后、非零电流之前会先发一帧 0x04。0x04 单独占用一个发送周期，对方不应答。 */
void bms_link_tick(BmsLink *l, const ChgCtrl *ctrl, uint32_t dt_ms, BmsTx *tx);

/* CAN 收到充电机的 CML / CRO / CCS / CST。其他标识符忽略。 */
void bms_link_can_rx(BmsLink *l, const ChgCan *f);

/* §10 验收 13: 辅助电源欠压状态由调用方更新。aux_undervolt=1 时累计
 * aux_uv_ms；累计 ≥ 100 ms 触发 force_stop。 */
void bms_link_set_aux_uv(BmsLink *l, uint8_t aux_undervolt);

/* §6.6 / §10 验收 15: SIF 拉低 ≥ 100 ms 进入物理断开。b=1 是 idle（高），b=0 是拉低。
 * 只在 CHG_PORT_SIF 上累加。调用方每个周期传入当前电平。 */
void bms_link_set_sif_state(BmsLink *l, uint8_t b, uint32_t dt_ms);

/* §10 验收 15: CAN bus-off 持续 ≥ 100 ms 进入物理断开。bus_off=1 为故障。
 * 只在 CHG_PORT_CAN 上累加。线未恢复前丢弃 CAN 帧，请求电流回到 0 也不清除停止。 */
void bms_link_set_bus_off(BmsLink *l, uint8_t bus_off, uint32_t dt_ms);

/* §10 验收 15: RS-485 短路持续 ≥ 100 ms 进入物理断开。shorted=1 为短路。
 * 只在 CHG_PORT_485 上累加。线未恢复前丢弃应答字节。 */
void bms_link_set_485_short(BmsLink *l, uint8_t shorted, uint32_t dt_ms);

/* RS-485 收充电机的应答。gap_ms 超过 50 ms 丢弃半帧。
 * 只接受等待中的 0x02 或 0x03。超过 80 ms 的应答丢弃。
 * 不是这两条、但长度仍放得进缓冲的帧整帧收下后丢掉，0x05–0x08 可以同线。
 * 本函数不产生发送帧。一线通调用此函数不会改变会话。 */
void bms_link_rx_byte(BmsLink *l, uint8_t b, uint32_t gap_ms, const ChgCtrl *ctrl, BmsTx *tx);

#endif
