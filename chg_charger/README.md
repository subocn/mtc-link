# 充电机充电通信

只发给充电机厂商。BMS 工程在 [`../chg_bms`](../chg_bms/README.md)，不要把两个目录合成一份发出去。

报文见 [`../doc/protocol.md`](../doc/protocol.md)。本目录实现充电机在一线通、CAN、RS-485 上的接收、发送和出流。`chg_apply` 决定本周期输出。`chr_link_init` 之后为 OFF，第一次 `chr_link_tick` 进入握手。

一次会话只选一个口。`ChgCap` 填写本机最大电压和最大电流；一线通没有能力帧，把书面限值放在这里。

```c
#include "chg_charger.h"

ChrLink link;
ChrTx tx;
ChgCap cap = {.v_min_01v = 500, .v_max_01v = 1000, .i_min_01a = 10, .i_max_01a = 800};
ChgStat meas = {.outputting = 1, .dc_path = 1, .v_out_01v = 836, .i_out_01a = 398};

chr_link_init(&link, CHG_PORT_485, &cap);    /* 或 CHG_PORT_SIF / CHG_PORT_CAN */
chr_link_set_485_short(&link, shorted, dt_ms); /* 本口的物理线。SIF 用 set_sif_state，CAN 用 set_bus_off */
chr_link_tick(&link, &meas, dt_ms, &tx);     /* CAN、485 到点填写要发送的帧；tx.act 是输出 */
chr_link_rx_byte(&link, byte, gap_ms, &meas, &tx); /* 一线通、485 收 BMS。0x04 不计入握手 */
chr_link_can_rx(&link, &rx, &meas, &tx);     /* CAN 收 BCL / BRO / BSM / BST，以及 18EF56F4 */
ChgBat info;
if (chr_link_bat(&link, &info) == 0) {
    /* 返回 0 才可以把 info 译到桩侧。输出电流仍只看 tx.act */
}
/* tx.act.enable == 0 时输出 0 A */
```

一线通收 `0x01` 和 `0x04`，发送长度始终为 0。连续 3 帧合法充电控制后才允许非零电流，`0x04` 不计入这 3 帧。CAN 先发就绪=0 的能力，收到允许=0 之后才改发就绪=1。RS-485 不主动发帧：收到 `0x01` 后 80 ms 内，握手应答 `0x02`，能量传输应答 `0x03`。第一次应答就绪=0，见到允许=0 之后，下一次应答就绪=1。`0x04` 只保存，不应答。仅收到 BCL 电流 0、而没有 BST，不算停止，输出回到 0，也不按最小电流续出。停止，或一线通、RS-485 上能量传输中的请求电流变为 0，会话结束。只收到 BCL 电流 0 而没有 BST，会话不结束。无合法充电控制时，不满 1 s 不停，到 1.2 s 输出必须为 0。下一次非零输出要重新握手。SIF 拉低、CAN bus-off、RS-485 短路各自累计 100 ms 也结束会话，输出立刻为 0，并且不再打开上拉；这段时间里到来的充电请求丢掉。线恢复之后才允许下一次握手。暂停只把输出打到 0，会话保留。`0x04` 超过 3 秒未更新时 `chr_link_bat` 失败；这不改变 `tx.act`。面向国标直流桩时，失败期间不进入桩侧能量传输。字节间隔超过 50 ms 的半帧丢弃。电池数据只来自 BMS，本工程不估算荷电状态。

`make test` 锁定规范里的帧字节，以及三个口的握手、暂停、停止、超时和出流。
