/* 作者：苏勃(subo)
 * BMS↔充电机 之间共享的数据结构。
 *
 * 两边工程（chg_bms、chg_charger）独立分发，但这些 struct 字段必须一致：
 * 字节序、宽度、长度都决定帧能不能相互解析。集中到这里，避免漂移。
 *
 * 桩侧（chg_evcc）的同名字段在 chg_evcc.h 里独立定义（GB/T 27930 桩格式
 * 与 BMS↔充电机 不同），不要混入。
 */
#ifndef CHG_LINK_COMMON_H
#define CHG_LINK_COMMON_H

#include <stdint.h>

typedef struct {
    uint8_t  allow;
    uint8_t  pause;
    uint8_t  stop;
    uint8_t  cv;
    uint16_t v_req_01v;
    uint16_t i_req_01a;
    uint8_t  flags;     /* BMS 0x04 byte 1 status flags (绝缘/连接器/过欠压/过低温)；
                         * 经 `flags & 0x0F` 写到 CAN BSM byte 6 低 4 位。
                         * 留 0 表示无附加状态字。 */
} ChgCtrl;

typedef struct {
    uint8_t  ready;
    uint16_t v_min_01v;
    uint16_t v_max_01v;
    uint16_t i_min_01a;
    uint16_t i_max_01a;
} ChgCap;

typedef struct {
    uint8_t  outputting;
    uint8_t  fault;
    uint8_t  ac_path;
    uint8_t  dc_path;
    uint16_t v_out_01v;
    uint16_t i_out_01a;
    uint8_t  fault_code;
} ChgStat;

typedef struct {
    uint8_t  valid;
    uint8_t  flags;
    uint16_t soc_01pct;
    uint16_t v_pack_01v;
    uint16_t v_cell_max_mv;
    uint16_t v_cell_min_mv;
    int8_t   t_max_c;
    int8_t   t_min_c;
    uint16_t rated_ah_01ah;
    uint16_t rated_v_01v;
    uint8_t  chemistry;
    uint16_t v_allow_01v;
    uint16_t i_allow_01a;
    int8_t   t_allow_c;
    uint16_t remain_min;
    uint8_t  idx_cell_max;
    uint8_t  idx_cell_min;
    uint8_t  idx_temp_max;
    uint8_t  idx_temp_min;
} ChgBat;

typedef struct {
    uint32_t id;
    uint8_t  dlc;
    uint8_t  data[8];
} ChgCan;

#define CHG_BAT_F_INSUL 0x01u
#define CHG_BAT_F_CONN  0x02u
#define CHG_BAT_F_CELL_OV 0x04u
#define CHG_BAT_F_CELL_UV 0x08u
#define CHG_BAT_F_OT     0x10u
#define CHG_BAT_F_UT     0x20u
#define CHG_CHEM_NCM 1u
#define CHG_CHEM_LFP 2u
#define CHG_BAT_REMAIN_UNKNOWN 0xFFFFu

#endif