# 软件在环 e2e

本目录不发给厂商。它把 [`../chg_bms`](../chg_bms/README.md) 和 [`../chg_charger`](../chg_charger/README.md) 放进同一个进程，按 50 ms 一拍互喂字节或 CAN 帧。这是软件在环(SIL)，不是硬件在环(HIL)：没有目标板，不测量导线上的时间。辅助电源和线故障是软件标志。

报文见 [`../doc/protocol.md`](../doc/protocol.md)。

在本目录执行：

```
make test
```

通过时标准输出末行是 `ok`，前面每个口一行。口的编号：1 一线通，2 CAN，3 RS-485。

| 输出 | 核对 |
|------|------|
| `[port=N] ok` | 握手、能量传输、暂停、恢复第一拍不超过 20.0 A 且电压目标减 1.0 V、停止、再次非零请求重新握手。能力上限 30.0 A，请求 40.0 A，输出 30.0 A |
| `[port=N t_loss] ok` | 时间从最后一帧算起。900 ms 和 1000 ms 仍出流。1200 ms 输出为 0 |
| `[port=N line] ok` | 线故障 50 ms 仍有电流。满 100 ms 两侧锁存且电流为 0。线未恢复时请求回到 0 也不再出流 |
| `[port=N fault] ok` | 充电机故障后三口输出为 0。CAN 与 RS-485 的 BMS 进入停止。一线通只核对输出为 0 |
| `[port=N nobat] ok` | 不调用 `bms_link_set_bat` 仍能出流。`chr_link_bat` 返回失败 |
| `[port=N aux] ok` | 欠压标志 50 ms 仍有电流。满 100 ms 输出为 0，BMS 进入停止 |
| `[port=1 meter_off] ok` | 第一次 tick 之前的合法 `0x01` 被丢掉，输出为 0 |
| `[port=1 meter] ok` | 能量传输中写入非帧字节，不改变已有输出 |
| `[port=N bat_full] ok` | 挂上电池模型。荷电上升，温度不下降，输出电压不超过请求 |

任一项失败时进程返回非 0，并在标准错误打印 `FAIL`。这份输出不能填进硬件在环的十步记录。
