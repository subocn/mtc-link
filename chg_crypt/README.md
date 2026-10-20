# 令牌

作者：苏勃(subo)

公开套件 `0x00`（明文）和 `0x01`（HMAC-SHA256，取前 8 字节）的参考实现。厂商套件 `0x80`–`0xEF` 只在这里组外壳，算法在 `mtc-crypt` 的钩子里。

报文、八步、绑定和封条的正文在 `doc/protocol.md` 的 5.6.1。本文件只写怎么调用。

不改 `0x01`–`0x04` 的字节。调用方不启用本模块时，已有会话仍是明文。

## 调用顺序

两块存储由调用方提供，放在静态区：`ChgCrypt` 一份，`ChgMacWork` 一份。同一块工作区同时只算一次。模块内部不分配内存。

上电：

1. 映像里带有 HMAC 时，先 `chg_crypt_install_hmac`。会话壳不按符号调用哈希；没安装时 1 档也只写出 `0x00`。`CHG_CRYPT_OMIT_HMAC=1` 不链接 `chg_sha256.c`，`chg_crypt_set_mac` 为空操作。然后 `chg_crypt_level_suites` 按算力档填写套件表，再 `chg_crypt_init`。0 档只写 `0x00`。1 档在 MAC 已安装时只写 `0x01`，并带明文许可。充电机用 1 档。放不下令牌的 BMS 用 0 档。标志还可或上 bit1（要求双向盐）。不走这两档时，直接把最多 4 个套件交给 `chg_crypt_init`。
2. 存储里的封条已经是断开，就调用 `chg_crypt_set_seal(c, 1)`。传入 0 不会清除。
3. 出厂或检修时，在工装口调用 `chg_crypt_factory_activate(c, key, peer_id)`。BMS 的 `peer_id` 是这只充电机的编号，充电机的 `peer_id` 是这只电池的编号。两台写入同一把密钥。这一次清掉 RAM 里的封条。应用也只在这里清掉存储里的封条。调用后会话回到空闲，要重新握手。`0x05`–`0x08` 都不会调用这个函数。
4. 需要再收窄时，调用 `chg_crypt_set_allow`。名单只能再要求对端编号落在名单里。已经绑定的那一只，不会因为名单里还有别的编号而放行。
5. 厂商套件调用 `chg_crypt_set_vendor`。钩子成功时交回标准 `0x01`（6 字节）或 `0x04`（28 字节）。失败则输出 0。

每次充电：

1. BMS 调用 `chg_crypt_set_salt_bms`，再 `chg_crypt_pack_hello_bms`。
2. 充电机用 `chg_crypt_rx_frame` 收下 `0x05`。选定套件要盐时，先 `chg_crypt_set_salt_chr`，再 `chg_crypt_pack_hello_chr`。80 ms 内只发这一帧，不发 `0x02`。
3. BMS 用 `chg_crypt_rx_frame` 收下 `0x06`。
4. 每一拍先 `chg_crypt_set_ctrl`，写入本拍 `0x01` 的 6 字节。已有 `0x04` 时调用 `chg_crypt_set_bat`，没有则传入空指针。这两次写入都会清掉上一枚已通过的 BMS 令牌。
5. 套件 `0x01` 用 `chg_crypt_pack_token` 组出本方向的 `0x07`。接收方用 `chg_crypt_rx_frame` 核对。先写入第 4 步，再算标签。令牌密钥由模块在第一次算标签时从长期密钥、两只盐和两只编号派生，调用方不填会话密钥。BMS 盐全 0 时 `chg_crypt_set_salt_bms` 不收下。非一线通上，套件需要盐而充电机盐全 0 时，`0x06` 组不出来。
6. 充电机在 `chg_apply` 算出电流之后调用 `chg_crypt_bind_output`。它内部看 `chg_crypt_output_allowed`。不允许时，电流和使能都写成 0。电压上限和化学体系在 `chg_apply` 之后另判，见 5.7，不放宽这里的结果。
7. BMS 在把允许位置 1 之前调用 `chg_crypt_bms_may_allow`。返回 0 就保持允许 = 0。已绑定，或要求双向盐时，要先收到充电机的令牌。这枚令牌超过 1000 ms 未再通过，返回值回到 0。
8. 周期调用 `chg_crypt_tick(c, dt_ms)`。锁定期把剩余的 10 s 减下去，减到 0 才回到空闲。运行中，充电机令牌的 1000 ms 也在这里减。`tick(999)` 还留 1 ms，再 `tick(1)` 或一次 `tick(1000)` 则过期。

CAN 用 `chg_crypt_can_split` 拆帧，用 `chg_crypt_can_rx` 拼回。标识符是 `0x181656F4` 和 `0x1816F456`。

组帧成功返回帧长，失败返回 -1。`chg_crypt_rx_frame` 返回 1 表示已处理，0 表示不是 `0x05`–`0x08`，-1 表示是认证帧但要丢掉。认证帧不刷新充电控制的超时。

## 接到已有明文

`chg_bms` 与 `chg_charger` 不调用本模块，`0x01`–`0x04` 的字节和相位机保持原样。启用令牌时，在这两侧旁边做三件事：

1. BMS 在组 `0x01` 之前调用 `chg_crypt_cover_ctrl`。本拍不能置允许位时，它清掉这 6 字节的 bit0，再记入覆盖。套件是 `0x01` 时另组 `0x07` 并返回帧长。明文套件返回 0，不另组帧。
2. 接收方先 `chg_crypt_observe`。只把 CRC 正确的 `0x01`（6 字节）或 `0x04`（28 字节）写入会话。其它帧返回 0，不改会话。
3. 然后 `chg_crypt_rx_frame` 收 `0x07`。顺序不能反：写入 `0x01` 会清掉上一枚已通过的令牌。充电机再 `chg_crypt_bind_output`。

认证帧另发，不改明文帧。RS-485 上 BMS 把放得进缓冲、但不是 `0x02` / `0x03` 的帧整帧丢掉。充电机本来就整帧忽略不是 `0x01` / `0x04` 的报文。CAN 上认证用另一组标识符。

## 内存

无堆。`ChgCrypt` 与 `ChgMacWork` 由调用方提供，放在静态存储即可。同一块工作区同时只算一次。

本机 `make size`（`-Os`）测得：

| 对象 | 占用 |
|------|------|
| `ChgSha256` | 108 字节 |
| `ChgMacWork` | 236 字节 |
| `ChgCrypt` | 208 字节（64 位指针；32 位 MCU 上三只指针各少 4 字节） |
| SHA-256 目标文件 text | 1988 字节 |
| 会话与组帧 text | 6631 字节 |
| 去掉 HMAC 的会话 text | 4652 字节 |

HMAC 不另放 64 字节垫块。密钥块直接装进调度字。派生报文、57 字节覆盖和内层摘要共用 `blk`。主机 `-Os` 上，从 `chg_crypt_cover_ctrl` 到一次 HMAC 的调用链大约是 cover_ctrl 64、pack_token 96、mac_cover 80、安装指针 16、hmac 48、hmac_blocks 112、sha_update 80、sha_compress 16 字节，相加 512 字节。派生和打标签两次 HMAC 先后执行。大缓冲在 `ChgMacWork` 里。

两块状态合计不超过 `CHG_CRYPT_STATE_MAX`（1024 字节），放在外部 RAM 或通用 RAM。32 位档另留栈，调用方剩余 RAM 不少于 2 KB，剩余程序不少于 8 KB。8051 档剩余程序不少于 24 KB，外部 RAM 不少于 1 KB，内部 256 字节只留给栈。

比特计数是两个 32 位，源码里没有 64 位整数。编译器定义了 `__C51__` 时，256 字节轮常数放在 `code` 区。令牌在主循环里算。`CHG_CRYPT_OMIT_HMAC=1` 的映像不链接 SHA-256，hello 里写不出 `0x01`，也不接厂商钩子。这幅映像的 `ChgMacWork` 只有 1 字节，`ChgCrypt` 在 64 位主机上是 192 字节，会话 text 就是上表最后一行。本仓库没有用 Keil 编译，上表的 text 不是 C51 的程序体积。

AES-128-CMAC、AES-GCM、ChaCha20-Poly1305 不进这幅映像。双方要这些算法时，用厂商套件号走钩子。

长期密钥 16 字节只由调用方写入 `ChgCrypt`，组帧函数不从帧里读密钥。会话密钥由模块写入，调用方不填。

## 测试

```bash
make -C chg_crypt test
make -C chg_crypt size
```

主机测试核对 SHA-256 与 HMAC-SHA256 的公开答案、48 字节派生后再算 57 字节标签、盐或 `0x04` 一变标签就变、全 0 盐被拒绝、明文可出流、充电机令牌 1000 ms 后允许位回到 0、标签失败或钩子失败时电流为 0、一线通要求双向盐时组不出握手、编号不一致或封条锁存时电流为 0、只有本地激活能清封条。另有两档协商：0 档对 1 档在未绑定时走明文，已绑定则输出为 0；1 档对 1 档走令牌；去掉 HMAC 的映像不链入 SHA-256。`test_ext` 在安装 HMAC 之后，用 `cover_ctrl` 盖住明文 `0x01`，并确认 `0x05` / `0x07` 同线时明文相位不变。`nm` 确认会话目标文件不引用 HMAC，去掉 HMAC 的映像也没有 `chg_hmac`。未在实板上量过。
