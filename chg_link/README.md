# 已拆成两份

通信参考不再放在这一个目录里。

| 目录 | 发给谁 |
|------|--------|
| [`../chg_bms`](../chg_bms/README.md) | 只发给 BMS 厂商。一线通、CAN、RS-485 的 BMS 实现 |
| [`../chg_charger`](../chg_charger/README.md) | 只发给充电机厂商。一线通、CAN、RS-485 的充电机实现 |

两边都按 [`../doc/protocol.md`](../doc/protocol.md) 实现，帧字节相同。电池信息是 `0x04`，只能由 BMS 填写。充放电策略不在本仓。
