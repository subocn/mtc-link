/* 作者：苏勃(subo)
 * 通信令牌。套件 0x00 与 0x01 在本文件。厂商套件 0x80–0xEF 只留钩子。
 *
 * 无堆。ChgCrypt 与 ChgMacWork 由调用方提供，可放在静态存储。
 * 同一块 ChgMacWork 同时只做一次运算。
 * 不调用本模块时，0x01–0x04 的会话与原来相同。
 * 哈希不编进会话壳。映像里有算法时，用 chg_crypt_set_mac 装上。
 */
#ifndef CHG_CRYPT_H
#define CHG_CRYPT_H

#include <stddef.h>
#include <stdint.h>

#include "chg_link_common.h"

/* 与 chg_bms.h / chg_charger.h 的 CHG_PORT_* 数值相同。此处不重复定义那些宏。 */
#define CHG_CRYPT_PORT_SIF 1u
#define CHG_CRYPT_PORT_CAN 2u
#define CHG_CRYPT_PORT_485 3u

#define CHG_CRYPT_ROLE_BMS 0u
#define CHG_CRYPT_ROLE_CHR 1u

#define CHG_SUITE_PLAIN 0x00u
#define CHG_SUITE_HMAC  0x01u
#define CHG_SUITE_NONE  0xFFu

/* 0 档只宣布明文。1 档宣布 0x01，并允许对端退回明文。 */
#define CHG_CRYPT_LEVEL_PLAIN 0u
#define CHG_CRYPT_LEVEL_HMAC  1u

/* 两块状态的上限。8051 的外部 RAM 至少要有这么多空闲。 */
#define CHG_CRYPT_STATE_MAX 1024u
/* 32 位档：剩余程序、剩余 RAM。栈预算已算在 RAM 里。 */
#define CHG_CRYPT_CODE_32 8192u
#define CHG_CRYPT_RAM_32 2048u
#define CHG_CRYPT_STACK_BUDGET 544u
/* 8051 档：剩余程序。外部 RAM 的下限就是 CHG_CRYPT_STATE_MAX。 */
#define CHG_CRYPT_CODE_51 24576u

/* 置 1 时本映像不调用 HMAC，也不能把 0x01 当成可执行套件。默认映像保持 0。 */
#ifndef CHG_CRYPT_OMIT_HMAC
#define CHG_CRYPT_OMIT_HMAC 0
#endif

#define CHG_CRYPT_F_PLAIN 0x01u /* 无共同套件时允许明文 */
#define CHG_CRYPT_F_BIDI  0x02u /* 要求充电机也出盐。一线通完不成 */
#define CHG_CRYPT_F_SEAL  0x04u /* 本机封条已断开。只报告，充电帧不能把它清掉 */

#define CHG_CRYPT_KEY 16u
#define CHG_CRYPT_SALT 8u
#define CHG_CRYPT_ID 8u
#define CHG_CRYPT_TAG 8u
#define CHG_CRYPT_CTRL 6u
#define CHG_CRYPT_BAT 28u
#define CHG_CRYPT_BODY_MAX 24u
#define CHG_CRYPT_FAIL_MAX 3u
#define CHG_CRYPT_LOCK_MS 10000u
#define CHG_CRYPT_PEER_MS 1000u /* 充电机令牌在 BMS 侧的保持时间 */

#define CHG_MSG_HELLO_BMS 0x05u
#define CHG_MSG_HELLO_CHR 0x06u
#define CHG_MSG_TOKEN     0x07u
#define CHG_MSG_OPAQUE    0x08u

#define CHG_DIR_BMS 0u
#define CHG_DIR_CHR 1u

/* BMS → 充电机、充电机 → BMS。序号在数据第 1 字节，其后最多 7 字节。 */
#define CHG_CAN_TOK_BMS 0x181656F4u
#define CHG_CAN_TOK_CHR 0x1816F456u

#define CHG_CRYPT_PH_IDLE 0u
#define CHG_CRYPT_PH_WAIT 1u
#define CHG_CRYPT_PH_RUN  2u
#define CHG_CRYPT_PH_LOCK 3u

typedef struct {
    uint32_t s[8];
    uint32_t bits_hi; /* 已处理比特数的高 32 位。C51 没有 64 位整数 */
    uint32_t bits_lo;
    uint8_t buf[64];
    uint8_t n;
} ChgSha256;

/* 调用方持有的工作区。去掉 HMAC 的映像没有哈希状态。
 * 默认映像里，调度字和 64 字节暂存各一份。派生报文、57 字节覆盖和内层摘要
 * 共用这块暂存。长期密钥和会话密钥不要指向它。 */
#if CHG_CRYPT_OMIT_HMAC
typedef struct {
    uint8_t unused;
} ChgMacWork;
#else
typedef struct {
    ChgSha256 sha;
    uint32_t w[16];
    uint8_t blk[64];
} ChgMacWork;
#endif

/* 钩子把厂商正文还原成标准 0x01（6 字节）或 0x04（28 字节）。失败返回非 0。 */
typedef int (*ChgCryptVendorFn)(uint8_t suite,
                                const uint8_t *body, uint8_t body_len,
                                uint8_t *msg, uint8_t *data, uint8_t *data_len,
                                void *user);

typedef struct {
    const uint8_t *allow;
#if !CHG_CRYPT_OMIT_HMAC
    ChgCryptVendorFn vendor;
    void *vendor_user;
#endif
    uint32_t rx_counter;
    uint32_t tx_counter;
    uint32_t lock_ms;
    uint32_t peer_ms;    /* 方向 1 令牌的剩余毫秒。只在 BMS 侧递减 */
    uint8_t role;
    uint8_t port;
    uint8_t phase;
    uint8_t flags;
    uint8_t peer_flags;
    uint8_t suite;
    uint8_t fails;
    uint8_t token_ok;
    uint8_t peer_ok;
    uint8_t restrict_id;
    uint8_t suite_n;
    uint8_t allow_n;
    uint8_t salt_bms_set;
    uint8_t salt_chr_set;
    uint8_t have_ctrl;
    uint8_t have_bat;
    uint8_t pair_on;     /* 1：只接受 pair_id 这一只对端 */
    uint8_t seal;        /* 1：本机封条已断开，掉电也要由产品记住 */
    uint8_t peer_seal;   /* 对端 hello 带来的封条 */
    uint8_t sk_ready;    /* 1：sk 已按本会话的盐和编号派生 */
    uint8_t asm_mask;
    uint8_t key[CHG_CRYPT_KEY]; /* 长期密钥。只用于派生，不直接算标签 */
    uint8_t sk[CHG_CRYPT_KEY];  /* 会话密钥。调用方不填写 */
    uint8_t salt_bms[CHG_CRYPT_SALT];
    uint8_t salt_chr[CHG_CRYPT_SALT];
    uint8_t id_self[CHG_CRYPT_ID];
    uint8_t id_peer[CHG_CRYPT_ID];
    uint8_t pair_id[CHG_CRYPT_ID];
    uint8_t suites[4];
    uint8_t ctrl[CHG_CRYPT_CTRL];
    uint8_t bat[CHG_CRYPT_BAT];
    uint8_t asm_buf[28];
    uint8_t asm_n[4];
} ChgCrypt;

void chg_sha256(ChgMacWork *w, const uint8_t *msg, size_t n, uint8_t out[32]);
void chg_hmac_sha256(ChgMacWork *w, const uint8_t *key, size_t key_len,
                     const uint8_t *msg, size_t msg_len, uint8_t out[32]);

/* 套件 0x01 的 MAC。参数非法返回 0，成功返回 1，并写出 32 字节摘要。
 * 会话壳只经这只指针调用。CHG_CRYPT_OMIT_HMAC=1 时 chg_crypt_set_mac 为空操作。 */
typedef int (*ChgCryptMacFn)(ChgMacWork *w, const uint8_t *key, size_t key_len,
                             const uint8_t *msg, size_t msg_len, uint8_t out[32]);
void chg_crypt_set_mac(ChgCryptMacFn fn);

/* 把本文件里的 HMAC-SHA256 装进会话。实现在 chg_sha256.c。
 * 去掉 HMAC 的映像不链接该文件，也不要调用本函数。 */
void chg_crypt_install_hmac(void);

/* 按算力档填写 hello 的套件表。成功返回写入的个数，参数无效返回 0。
 * 0 档、去掉 HMAC 的映像、以及 MAC 尚未安装：只写 0x00。
 * 1 档且 MAC 已安装：只写 0x01。
 * flags 写出明文许可，调用方可再或上双向盐。未知档按 0 档。 */
uint8_t chg_crypt_level_suites(uint8_t level, uint8_t *suites, uint8_t cap, uint8_t *flags);

/* key 为 16 字节长期密钥，只用来派生会话密钥。suites 最多 4 个。flags 只用 bit0 与 bit1。 */
void chg_crypt_init(ChgCrypt *c, uint8_t role, uint8_t port,
                    const uint8_t key[CHG_CRYPT_KEY], const uint8_t id_self[CHG_CRYPT_ID],
                    const uint8_t *suites, uint8_t suite_n, uint8_t flags);

/* restrict_id 为 1 时，名单为空视为拒绝。明文会话不把编号当作身份证明。 */
void chg_crypt_set_allow(ChgCrypt *c, const uint8_t *ids, uint8_t n, uint8_t restrict_id);

/* 出厂或检修的本地写入。充电口上的任何帧都不会调用它。
 * 写入长期密钥和唯一对端编号，并清除本机封条锁存。调用后会话回到空闲，要重新握手。
 * 产品把封条锁存放在掉电仍在的存储里：上电时若已断开则调用 chg_crypt_set_seal(c, 1)。
 * 只有这条写入可以清掉锁存。 */
void chg_crypt_factory_activate(ChgCrypt *c, const uint8_t key[CHG_CRYPT_KEY],
                                const uint8_t peer_id[CHG_CRYPT_ID]);

/* opened 非 0：锁存封条断开，允许位和出流都保持关闭。opened 为 0 时不清除。 */
void chg_crypt_set_seal(ChgCrypt *c, uint8_t opened);
void chg_crypt_set_vendor(ChgCrypt *c, ChgCryptVendorFn fn, void *user);
void chg_crypt_set_salt_bms(ChgCrypt *c, const uint8_t salt[CHG_CRYPT_SALT]);
void chg_crypt_set_salt_chr(ChgCrypt *c, const uint8_t salt[CHG_CRYPT_SALT]);

/* 更换本拍 0x01 或最近 0x04 后，上一枚令牌不再覆盖，出流条件重新关掉。 */
void chg_crypt_set_ctrl(ChgCrypt *c, const uint8_t data[CHG_CRYPT_CTRL]);
void chg_crypt_set_bat(ChgCrypt *c, const uint8_t data[CHG_CRYPT_BAT]); /* 传入 0 表示尚无 0x04 */

void chg_crypt_tick(ChgCrypt *c, uint32_t dt_ms);

/* 成功返回帧长，失败返回 -1。帧壳仍是 A5 5A / VER 01 / CRC-16。 */
int chg_crypt_pack_hello_bms(ChgCrypt *c, uint8_t *out, size_t cap);
int chg_crypt_pack_hello_chr(ChgCrypt *c, uint8_t *out, size_t cap);
int chg_crypt_pack_token(ChgCrypt *c, ChgMacWork *w, uint8_t *out, size_t cap);
/* 厂商套件的外壳。正文不透明，最长 24 字节。算法在钩子里。 */
int chg_crypt_pack_opaque(ChgCrypt *c, const uint8_t *body, uint8_t body_len,
                          uint8_t *out, size_t cap);

/* 1 已处理，0 不是 0x05–0x08，-1 是认证帧但须丢弃。不刷新充电控制的超时。 */
int chg_crypt_rx_frame(ChgCrypt *c, ChgMacWork *w, const uint8_t *frame, size_t n);

int chg_crypt_can_split(uint32_t id, uint8_t msg, const uint8_t *data, uint8_t len,
                        ChgCan *out, uint8_t cap);
int chg_crypt_can_rx(ChgCrypt *c, ChgMacWork *w, const ChgCan *f);

/* 充电机：1 表示本拍可以保留已经算出的电流。 */
int chg_crypt_output_allowed(const ChgCrypt *c);
/* BMS：1 表示本拍可以把允许位置 1。 */
int chg_crypt_bms_may_allow(const ChgCrypt *c);
void chg_crypt_bind_output(const ChgCrypt *c, uint16_t *i_out_01a, uint8_t *enable);

/* 从已经组好的明文帧取出覆盖内容。只接受 CRC 正确的 0x01（6 字节）和 0x04（28 字节）。
 * 写入成功返回 1。其它帧、CRC 不对或长度不够时返回 0，并且不改会话。
 * 认证帧仍只走 chg_crypt_rx_frame。先调用本函数，再核对 0x07。 */
int chg_crypt_observe(ChgCrypt *c, const uint8_t *frame, size_t n);

/* 在组 0x01 之前调用。就地修改 ctrl 的 6 字节。
 * BMS 且本拍不能置允许位时，清掉 ctrl[0] 的 bit0。然后记入本拍覆盖。
 * bat 为 0 表示尚无 0x04。套件为 0x01 时把 0x07 写入 auth，返回帧长。
 * 明文套件返回 0，不另组帧。参数无效或令牌组不出来返回 -1。 */
int chg_crypt_cover_ctrl(ChgCrypt *c, ChgMacWork *w, uint8_t ctrl[CHG_CRYPT_CTRL],
                         const uint8_t *bat, uint8_t *auth, size_t auth_cap);

#endif
