# BMS 充电通信

只发给 BMS 厂商。充电机工程在 [`../chg_charger`](../chg_charger/README.md)，不要把两个目录合成一份发出去。

报文见 [`../doc/protocol.md`](../doc/protocol.md)。本目录实现 BMS 在一线通、CAN、RS-485 上的发送和接收。充电电流由调用方传入，本目录不算出流。

一次会话只选一个口。

```c
#include "chg_bms.h"

BmsLink link;
BmsTx tx;
ChgCtrl ctrl = {.allow = 1, .v_req_01v = 840, .i_req_01a = 400};
ChgBat bat = {.valid = 1, .soc_01pct = 500, .chemistry = 1,
              .rated_ah_01ah = 200, .rated_v_01v = 730};

bms_link_init(&link, CHG_PORT_SIF);          /* 或 CHG_PORT_CAN / CHG_PORT_485 */
bms_link_set_bat(&link, &bat);               /* 电池数据只从这里来。未调用则 0x04 有效位为 0 */
bms_link_set_sif_state(&link, level, dt_ms); /* 本口的物理线。CAN 用 set_bus_off，485 用 set_485_short */
bms_link_tick(&link, &ctrl, dt_ms, &tx);     /* 三个口到点填写 tx。485 先发 0x01，0x04 单独占用一个发送周期 */
bms_link_can_rx(&link, &rx);                 /* CAN 收 CML / CRO / CCS / CST */
bms_link_rx_byte(&link, byte, gap_ms, &ctrl, &tx); /* 485 收 0x02 / 0x03。超过 80 ms 的应答丢弃 */
```

一线通发 `0x01` 和约每 1 秒一帧 `0x04`。`0x04` 不计入握手。前两帧 `0x01` 是握手（允许=0，电流=0），第三帧起才按策略发送。CAN 在看到 CRO=`0xAA` 之前同样只发握手；电池信息走 `0x18EF56F4` 四帧，不占用桩侧 BSD。请求电流为 0 或停止时发 BST，不用 BCL 电流 0 表示停止。到 1.2 s 没有 CCS 则发 BST，并保持停止，直到策略先把请求电流降到 0 再重新允许。不满 1 s 不发。SIF 拉低、CAN bus-off、RS-485 短路各自累计 100 ms 即进入同一停止，不等这 1.2 s；线还断着时，请求电流回到 0 也不开始新的握手。RS-485 由 BMS 先发 `0x01`，充电机在 80 ms 内应答 `0x02` 或 `0x03`。没有应答就重发 `0x01`。第一次应答之后、非零电流之前先发一帧 `0x04`，充电机不应答。能量传输中到 1.2 s 没有 0x03 则发停止，不满 1 s 不发。字节间隔超过 50 ms 的半帧丢弃。

`make test` 锁定规范里的帧字节，以及三个口的握手、暂停、停止和超时。
