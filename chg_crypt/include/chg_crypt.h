/* 作者：苏勃(subo)
 * 通信令牌。套件 0x00 与 0x01 在本文件。厂商套件 0x80–0xEF 只留钩子。
 *
 * 无堆。ChgCrypt 与 ChgMacWork 由调用方提供，可放在静态存储。
 * 同一块 ChgMacWork 同时只做一次运算。
 * 不调用本模块时，0x01–0x04 的会话与原来相同。
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
    uint64_t bits;
    uint8_t buf[64];
    uint8_t n;
} ChgSha256;

/* 调用方持有的工作区。SHA-256 的块状态、HMAC 的垫块和报文暂存都在这里。 */
typedef struct {
    ChgSha256 sha;
    uint8_t pad[64];
    uint32_t w[16];
    uint8_t dig[32];
    uint8_t msg[64];
} ChgMacWork;

/* 钩子把厂商正文还原成标准 0x01（6 字节）或 0x04（28 字节）。失败返回非 0。 */
typedef int (*ChgCryptVendorFn)(uint8_t suite,
                                const uint8_t *body, uint8_t body_len,
                                uint8_t *msg, uint8_t *data, uint8_t *data_len,
                                void *user);

typedef struct {
    const uint8_t *allow;
    ChgCryptVendorFn vendor;
    void *vendor_user;
    uint32_t rx_counter;
    uint32_t tx_counter;
    uint32_t lock_ms;
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
    uint8_t asm_mask;
    uint8_t key[CHG_CRYPT_KEY];
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

/* key 为 16 字节长期密钥。suites 最多 4 个。flags 只用 bit0 与 bit1。 */
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

#endif
