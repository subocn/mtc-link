# 令牌

作者：苏勃(subo)

公开套件 `0x00`（明文）和 `0x01`（HMAC-SHA256，取前 8 字节）的参考实现。厂商套件 `0x80`–`0xEF` 只在这里组外壳，算法在 `mtc-crypt` 的钩子里。

报文、八步、绑定和封条的正文在 `doc/protocol.md` 的 5.6.1。本文件只写怎么调用。

不改 `0x01`–`0x04` 的字节。调用方不启用本模块时，已有会话仍是明文。

## 调用顺序

两块存储由调用方提供，放在静态区：`ChgCrypt` 一份，`ChgMacWork` 一份。同一块工作区同时只算一次。模块内部不分配内存。

上电：

1. `chg_crypt_init`。参数是角色、口、16 字节密钥、本机 8 字节编号、最多 4 个套件，以及标志。标志只用 bit0（没有共同套件时允许明文）和 bit1（要求双向盐）。
2. 存储里的封条已经是断开，就调用 `chg_crypt_set_seal(c, 1)`。传入 0 不会清除。
3. 出厂或检修时，在工装口调用 `chg_crypt_factory_activate(c, key, peer_id)`。BMS 的 `peer_id` 是这只充电机的编号，充电机的 `peer_id` 是这只电池的编号。两台写入同一把密钥。这一次清掉 RAM 里的封条。应用也只在这里清掉存储里的封条。调用后会话回到空闲，要重新握手。`0x05`–`0x08` 都不会调用这个函数。
4. 需要再收窄时，调用 `chg_crypt_set_allow`。名单只能再要求对端编号落在名单里。已经绑定的那一只，不会因为名单里还有别的编号而放行。
5. 厂商套件调用 `chg_crypt_set_vendor`。钩子成功时交回标准 `0x01`（6 字节）或 `0x04`（28 字节）。失败则输出 0。

每次充电：

1. BMS 调用 `chg_crypt_set_salt_bms`，再 `chg_crypt_pack_hello_bms`。
2. 充电机用 `chg_crypt_rx_frame` 收下 `0x05`。选定套件要盐时，先 `chg_crypt_set_salt_chr`，再 `chg_crypt_pack_hello_chr`。80 ms 内只发这一帧，不发 `0x02`。
3. BMS 用 `chg_crypt_rx_frame` 收下 `0x06`。
4. 每一拍先 `chg_crypt_set_ctrl`，写入本拍 `0x01` 的 6 字节。已有 `0x04` 时调用 `chg_crypt_set_bat`，没有则传入空指针。这两次写入都会清掉上一枚已通过的 BMS 令牌。
5. 套件 `0x01` 用 `chg_crypt_pack_token` 组出本方向的 `0x07`。接收方用 `chg_crypt_rx_frame` 核对。先写入第 4 步，再算标签。
6. 充电机在 `chg_apply` 算出电流之后调用 `chg_crypt_bind_output`。它内部看 `chg_crypt_output_allowed`。不允许时，电流和使能都写成 0。
7. BMS 在把允许位置 1 之前调用 `chg_crypt_bms_may_allow`。返回 0 就保持允许 = 0。已绑定，或要求双向盐时，要先收到充电机的令牌。
8. 周期调用 `chg_crypt_tick(c, dt_ms)`。只有锁定期会把剩余的 10 s 减下去，减到 0 才回到空闲。

CAN 用 `chg_crypt_can_split` 拆帧，用 `chg_crypt_can_rx` 拼回。标识符是 `0x181656F4` 和 `0x1816F456`。

组帧成功返回帧长，失败返回 -1。`chg_crypt_rx_frame` 返回 1 表示已处理，0 表示不是 `0x05`–`0x08`，-1 表示是认证帧但要丢掉。认证帧不刷新充电控制的超时。

## 内存

无堆。`ChgCrypt` 与 `ChgMacWork` 由调用方提供，放在静态存储即可。同一块工作区同时只算一次。

本机 `make size`（`-Os`）测得：

| 对象 | 占用 |
|------|------|
| `ChgSha256` | 112 字节 |
| `ChgMacWork` | 336 字节 |
| `ChgCrypt` | 184 字节（64 位指针；32 位 MCU 上三只指针各少 4 字节） |
| SHA-256 目标文件 text | 1760 字节 |
| 会话与组帧 text | 4920 字节 |

HMAC 这一条调用链上，各函数自己的栈大约是 112 + 64 + 16 字节。算上组帧，一条令牌的调用链大约 300 字节。大缓冲在 `ChgMacWork` 里，不在栈上。

AES-128-CMAC、AES-GCM、ChaCha20-Poly1305 不进这幅映像。双方要这些算法时，用厂商套件号走钩子。

长期密钥 16 字节只由调用方写入 `ChgCrypt`，组帧函数不从帧里读密钥。

## 测试

```bash
make -C chg_crypt test
make -C chg_crypt size
```

主机测试核对 SHA-256 与 HMAC-SHA256 的公开答案、57 字节标签、盐或 `0x04` 一变标签就变、明文可出流、标签失败或钩子失败时电流为 0、一线通要求双向盐时组不出握手、编号不一致或封条锁存时电流为 0、只有本地激活能清封条。未在实板上量过。
