/* 作者：苏勃(subo)
 * 套件选择、0x05–0x08 组帧和出流闸门。HMAC 见 chg_sha256.c。
 * 套件 0x01 的标签用会话密钥。长期密钥只按 SP 800-108 派生这一把。
 */
#include "chg_crypt.h"

#include <string.h>

static const uint8_t ID_ZERO[CHG_CRYPT_ID];

static uint16_t crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    size_t i;
    int b;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (b = 0; b < 8; b++) {
            if (crc & 1u) {
                crc = (uint16_t)((crc >> 1) ^ 0xA001u);
            } else {
                crc = (uint16_t)(crc >> 1);
            }
        }
    }
    return crc;
}

static int frame_pack(uint8_t msg, const uint8_t *data, uint8_t len, uint8_t *out, size_t cap)
{
    uint16_t crc;
    if (out == 0 || (len > 0u && data == 0) || cap < (size_t)len + 7u) {
        return -1;
    }
    out[0] = 0xA5u;
    out[1] = 0x5Au;
    out[2] = 0x01u;
    out[3] = msg;
    out[4] = len;
    if (len > 0u) {
        memcpy(out + 5, data, len);
    }
    crc = crc16(out, (size_t)len + 5u);
    out[5u + len] = (uint8_t)(crc & 0xFFu);
    out[6u + len] = (uint8_t)(crc >> 8);
    return (int)len + 7;
}

static void asm_reset(ChgCrypt *c)
{
    c->asm_mask = 0;
    memset(c->asm_n, 0, sizeof c->asm_n);
    memset(c->asm_buf, 0, sizeof c->asm_buf);
}

static void drop_sk(ChgCrypt *c)
{
    c->sk_ready = 0;
    c->peer_ms = 0;
    memset(c->sk, 0, sizeof c->sk);
}

static int salt_zero(const uint8_t *s)
{
    uint8_t i;
    uint8_t d = 0;
    for (i = 0; i < CHG_CRYPT_SALT; i++) {
        d = (uint8_t)(d | s[i]);
    }
    return d == 0;
}

static int suite_needs_salt(uint8_t suite)
{
    if (suite == CHG_SUITE_HMAC) {
        return 1;
    }
    if (suite >= 0x80u && suite <= 0xEFu) {
        return 1;
    }
    return 0;
}

static void enter_lock(ChgCrypt *c)
{
    c->phase = CHG_CRYPT_PH_LOCK;
    c->lock_ms = CHG_CRYPT_LOCK_MS;
    c->token_ok = 0;
    c->peer_ok = 0;
    c->suite = CHG_SUITE_NONE;
    c->salt_bms_set = 0;
    c->salt_chr_set = 0;
    memset(c->salt_bms, 0, sizeof c->salt_bms);
    memset(c->salt_chr, 0, sizeof c->salt_chr);
    c->rx_counter = 0;
    c->tx_counter = 0;
    drop_sk(c);
    asm_reset(c);
}

static void note_fail(ChgCrypt *c)
{
    c->token_ok = 0;
    if (c->fails < 255u) {
        c->fails++;
    }
    if (c->fails >= CHG_CRYPT_FAIL_MAX) {
        enter_lock(c);
    }
}

static int listed(const uint8_t *s, uint8_t n, uint8_t v)
{
    uint8_t i;
    for (i = 0; i < n; i++) {
        if (s[i] == v) {
            return 1;
        }
    }
    return 0;
}

static int runnable(const ChgCrypt *c, uint8_t s)
{
    if (s == CHG_SUITE_HMAC) {
        return 1;
    }
    if (s >= 0x80u && s <= 0xEFu && c->vendor != 0) {
        return 1;
    }
    return 0;
}

static uint8_t pick_suite(const ChgCrypt *c, const uint8_t *remote, uint8_t nremote,
                          uint8_t remote_flags)
{
    uint8_t best = 0;
    uint8_t i;
    for (i = 0; i < c->suite_n; i++) {
        uint8_t s = c->suites[i];
        if (!runnable(c, s) || !listed(remote, nremote, s)) {
            continue;
        }
        if (s > best) {
            best = s;
        }
    }
    if (best != 0u) {
        return best;
    }
    if ((c->flags & CHG_CRYPT_F_PLAIN) && (remote_flags & CHG_CRYPT_F_PLAIN)) {
        return CHG_SUITE_PLAIN;
    }
    return CHG_SUITE_NONE;
}

static int bidi_on(const ChgCrypt *c)
{
    return ((c->flags | c->peer_flags) & CHG_CRYPT_F_BIDI) != 0;
}

static int sif_bidi_blocked(const ChgCrypt *c)
{
    return c->port == CHG_CRYPT_PORT_SIF && bidi_on(c);
}

static int sealed(const ChgCrypt *c)
{
    return c->seal || c->peer_seal;
}

/* 一对一绑定：对端编号必须与出厂写入的那一只相同，且不能走明文或一线通。 */
static int bound_ok(const ChgCrypt *c)
{
    if (!c->pair_on) {
        return 1;
    }
    if (c->port == CHG_CRYPT_PORT_SIF || c->suite == CHG_SUITE_PLAIN) {
        return 0;
    }
    return memcmp(c->id_peer, c->pair_id, CHG_CRYPT_ID) == 0;
}

static int id_listed(const ChgCrypt *c)
{
    uint8_t i;
    if (c->allow == 0 || c->allow_n == 0u) {
        return 0;
    }
    for (i = 0; i < c->allow_n; i++) {
        if (memcmp(c->allow + (size_t)i * CHG_CRYPT_ID, c->id_peer, CHG_CRYPT_ID) == 0) {
            return 1;
        }
    }
    return 0;
}

static int session_open(const ChgCrypt *c)
{
    if (c == 0 || c->phase != CHG_CRYPT_PH_RUN) {
        return 0;
    }
    if (sif_bidi_blocked(c)) {
        return 0;
    }
    if (c->suite == CHG_SUITE_PLAIN || c->suite == CHG_SUITE_HMAC) {
        return 1;
    }
    if (c->suite >= 0x80u && c->suite <= 0xEFu) {
        return 1;
    }
    return 0;
}

static void enter_run(ChgCrypt *c, uint8_t suite)
{
    c->suite = suite;
    c->phase = CHG_CRYPT_PH_RUN;
    c->fails = 0;
    c->token_ok = 0;
    c->peer_ok = 0;
    c->rx_counter = 0;
    c->tx_counter = 0;
    drop_sk(c);
    asm_reset(c);
}

static int tag_eq(const uint8_t *a, const uint8_t *b)
{
    uint8_t d = 0;
    uint8_t i;
    for (i = 0; i < CHG_CRYPT_TAG; i++) {
        d = (uint8_t)(d | (a[i] ^ b[i]));
    }
    return d == 0;
}

/* 已绑定用出厂对端编号。未绑定用 hello 宣布的编号。一线通的充电机编号为 0。 */
static void kdf_ids(const ChgCrypt *c, const uint8_t **id_bms, const uint8_t **id_chr)
{
    if (c->port == CHG_CRYPT_PORT_SIF) {
        *id_chr = ID_ZERO;
        *id_bms = (c->role == CHG_CRYPT_ROLE_BMS) ? c->id_self : c->id_peer;
        return;
    }
    if (c->role == CHG_CRYPT_ROLE_BMS) {
        *id_bms = c->id_self;
        *id_chr = c->pair_on ? c->pair_id : c->id_peer;
        return;
    }
    *id_chr = c->id_self;
    *id_bms = c->pair_on ? c->pair_id : c->id_peer;
}

/* SP 800-108 计数器模式，i = 1，L = 128。结果最左 16 字节写入 sk。与 57 字节共用 w->msg。 */
static int derive_sk(ChgCrypt *c, ChgMacWork *w)
{
    const uint8_t *id_bms;
    const uint8_t *id_chr;
    if (!c->salt_bms_set || !c->salt_chr_set) {
        return 0;
    }
    kdf_ids(c, &id_bms, &id_chr);
    memset(w->msg, 0, 48u);
    w->msg[3] = 0x01u;
    memcpy(w->msg + 4, "mtc-sk", 6u);
    memcpy(w->msg + 11, c->salt_bms, CHG_CRYPT_SALT);
    memcpy(w->msg + 19, c->salt_chr, CHG_CRYPT_SALT);
    memcpy(w->msg + 27, id_bms, CHG_CRYPT_ID);
    memcpy(w->msg + 35, id_chr, CHG_CRYPT_ID);
    w->msg[43] = c->suite;
    w->msg[47] = 0x80u;
    chg_hmac_sha256(w, c->key, CHG_CRYPT_KEY, w->msg, 48u, w->dig);
    memcpy(c->sk, w->dig, CHG_CRYPT_KEY);
    c->sk_ready = 1u;
    return 1;
}

static int mac_cover(ChgCrypt *c, ChgMacWork *w, uint32_t counter, uint8_t dir, uint8_t tag[8])
{
    if (!c->sk_ready && !derive_sk(c, w)) {
        return 0;
    }
    memset(w->msg, 0, 57u);
    w->msg[0] = c->suite;
    memcpy(w->msg + 1, c->salt_bms, CHG_CRYPT_SALT);
    memcpy(w->msg + 9, c->salt_chr, CHG_CRYPT_SALT);
    w->msg[17] = (uint8_t)(counter & 0xFFu);
    w->msg[18] = (uint8_t)((counter >> 8) & 0xFFu);
    w->msg[19] = (uint8_t)((counter >> 16) & 0xFFu);
    w->msg[20] = (uint8_t)((counter >> 24) & 0xFFu);
    w->msg[21] = dir;
    memcpy(w->msg + 22, c->ctrl, CHG_CRYPT_CTRL);
    w->msg[28] = c->have_bat ? 1u : 0u;
    if (c->have_bat) {
        memcpy(w->msg + 29, c->bat, CHG_CRYPT_BAT);
    }
    chg_hmac_sha256(w, c->sk, CHG_CRYPT_KEY, w->msg, 57u, w->dig);
    memcpy(tag, w->dig, CHG_CRYPT_TAG);
    return 1;
}

void chg_crypt_init(ChgCrypt *c, uint8_t role, uint8_t port,
                    const uint8_t key[CHG_CRYPT_KEY], const uint8_t id_self[CHG_CRYPT_ID],
                    const uint8_t *suites, uint8_t suite_n, uint8_t flags)
{
    uint8_t n;
    if (c == 0) {
        return;
    }
    memset(c, 0, sizeof *c);
    c->role = role;
    c->port = port;
    c->flags = (uint8_t)(flags & (CHG_CRYPT_F_PLAIN | CHG_CRYPT_F_BIDI));
    c->suite = CHG_SUITE_NONE;
    c->phase = CHG_CRYPT_PH_IDLE;
    if (key != 0) {
        memcpy(c->key, key, CHG_CRYPT_KEY);
    }
    if (id_self != 0) {
        memcpy(c->id_self, id_self, CHG_CRYPT_ID);
    }
    n = suite_n;
    if (n > 4u) {
        n = 4u;
    }
    c->suite_n = n;
    if (n > 0u && suites != 0) {
        memcpy(c->suites, suites, n);
    }
}

void chg_crypt_set_allow(ChgCrypt *c, const uint8_t *ids, uint8_t n, uint8_t restrict_id)
{
    if (c == 0) {
        return;
    }
    c->allow = (n > 0u) ? ids : 0;
    c->allow_n = (ids == 0) ? 0u : n;
    c->restrict_id = restrict_id ? 1u : 0u;
}

void chg_crypt_factory_activate(ChgCrypt *c, const uint8_t key[CHG_CRYPT_KEY],
                                const uint8_t peer_id[CHG_CRYPT_ID])
{
    if (c == 0 || key == 0 || peer_id == 0) {
        return;
    }
    memcpy(c->key, key, CHG_CRYPT_KEY);
    memcpy(c->pair_id, peer_id, CHG_CRYPT_ID);
    c->pair_on = 1u;
    c->seal = 0u;
    c->peer_seal = 0u;
    c->phase = CHG_CRYPT_PH_IDLE;
    c->suite = CHG_SUITE_NONE;
    c->token_ok = 0u;
    c->peer_ok = 0u;
    c->fails = 0u;
    c->lock_ms = 0u;
    c->rx_counter = 0u;
    c->tx_counter = 0u;
    drop_sk(c);
    asm_reset(c);
}

void chg_crypt_set_seal(ChgCrypt *c, uint8_t opened)
{
    if (c == 0 || opened == 0u) {
        return;
    }
    c->seal = 1u;
    c->token_ok = 0u;
    c->peer_ok = 0u;
}

void chg_crypt_set_vendor(ChgCrypt *c, ChgCryptVendorFn fn, void *user)
{
    if (c == 0) {
        return;
    }
    c->vendor = fn;
    c->vendor_user = user;
}

void chg_crypt_set_salt_bms(ChgCrypt *c, const uint8_t salt[CHG_CRYPT_SALT])
{
    if (c == 0 || salt == 0) {
        return;
    }
    c->sk_ready = 0u;
    if (salt_zero(salt)) {
        memset(c->salt_bms, 0, CHG_CRYPT_SALT);
        c->salt_bms_set = 0u;
        return;
    }
    memcpy(c->salt_bms, salt, CHG_CRYPT_SALT);
    c->salt_bms_set = 1u;
}

void chg_crypt_set_salt_chr(ChgCrypt *c, const uint8_t salt[CHG_CRYPT_SALT])
{
    if (c == 0 || salt == 0) {
        return;
    }
    memcpy(c->salt_chr, salt, CHG_CRYPT_SALT);
    c->salt_chr_set = 1u;
    c->sk_ready = 0u;
}

void chg_crypt_set_ctrl(ChgCrypt *c, const uint8_t data[CHG_CRYPT_CTRL])
{
    if (c == 0 || data == 0) {
        return;
    }
    memcpy(c->ctrl, data, CHG_CRYPT_CTRL);
    c->have_ctrl = 1u;
    c->token_ok = 0u;
}

void chg_crypt_set_bat(ChgCrypt *c, const uint8_t data[CHG_CRYPT_BAT])
{
    if (c == 0) {
        return;
    }
    if (data == 0) {
        memset(c->bat, 0, CHG_CRYPT_BAT);
        c->have_bat = 0u;
    } else {
        memcpy(c->bat, data, CHG_CRYPT_BAT);
        c->have_bat = 1u;
    }
    c->token_ok = 0u;
}

void chg_crypt_tick(ChgCrypt *c, uint32_t dt_ms)
{
    if (c == 0) {
        return;
    }
    if (c->phase == CHG_CRYPT_PH_LOCK) {
        if (dt_ms >= c->lock_ms) {
            c->lock_ms = 0;
            c->phase = CHG_CRYPT_PH_IDLE;
            c->fails = 0;
            c->token_ok = 0;
            c->peer_ok = 0;
            c->suite = CHG_SUITE_NONE;
            c->rx_counter = 0;
            c->tx_counter = 0;
            drop_sk(c);
        } else {
            c->lock_ms -= dt_ms;
        }
        return;
    }
    if (c->phase == CHG_CRYPT_PH_RUN && c->peer_ok && dt_ms > 0u) {
        if (dt_ms >= c->peer_ms) {
            c->peer_ms = 0;
            c->peer_ok = 0;
        } else {
            c->peer_ms -= dt_ms;
        }
    }
}

int chg_crypt_pack_hello_bms(ChgCrypt *c, uint8_t *out, size_t cap)
{
    uint8_t data[22];
    int n;
    if (c == 0 || c->role != CHG_CRYPT_ROLE_BMS || c->phase == CHG_CRYPT_PH_LOCK) {
        return -1;
    }
    if (!c->salt_bms_set || salt_zero(c->salt_bms)) {
        return -1;
    }
    if (c->port == CHG_CRYPT_PORT_SIF && (c->flags & CHG_CRYPT_F_BIDI)) {
        return -1;
    }
    memset(data, 0, sizeof data);
    data[0] = (uint8_t)(c->flags | (c->seal ? CHG_CRYPT_F_SEAL : 0u));
    data[1] = c->suite_n == 0u ? 1u : c->suite_n;
    if (c->suite_n == 0u) {
        data[2] = CHG_SUITE_PLAIN;
    } else {
        memcpy(data + 2, c->suites, c->suite_n);
    }
    memcpy(data + 6, c->salt_bms, CHG_CRYPT_SALT);
    memcpy(data + 14, c->id_self, CHG_CRYPT_ID);
    n = frame_pack(CHG_MSG_HELLO_BMS, data, 22, out, cap);
    if (n < 0) {
        return -1;
    }
    if (c->port == CHG_CRYPT_PORT_SIF) {
        uint8_t local_n = data[1];
        uint8_t chosen = pick_suite(c, data + 2, local_n, c->flags);
        c->peer_flags = c->flags;
        memset(c->salt_chr, 0, sizeof c->salt_chr);
        c->salt_chr_set = 1u;
        if (chosen == CHG_SUITE_NONE || sif_bidi_blocked(c)) {
            c->phase = CHG_CRYPT_PH_IDLE;
            c->suite = CHG_SUITE_NONE;
            drop_sk(c);
        } else {
            enter_run(c, chosen);
        }
    } else {
        c->phase = CHG_CRYPT_PH_WAIT;
        c->suite = CHG_SUITE_NONE;
        c->token_ok = 0;
        c->peer_ok = 0;
        drop_sk(c);
    }
    return n;
}

int chg_crypt_pack_hello_chr(ChgCrypt *c, uint8_t *out, size_t cap)
{
    uint8_t data[18];
    if (c == 0 || c->role != CHG_CRYPT_ROLE_CHR || c->port == CHG_CRYPT_PORT_SIF) {
        return -1;
    }
    if (c->phase != CHG_CRYPT_PH_WAIT) {
        return -1;
    }
    if (suite_needs_salt(c->suite) && (!c->salt_chr_set || salt_zero(c->salt_chr))) {
        return -1;
    }
    memset(data, 0, sizeof data);
    data[0] = (uint8_t)(c->flags | (c->seal ? CHG_CRYPT_F_SEAL : 0u));
    data[1] = c->suite;
    if (c->salt_chr_set) {
        memcpy(data + 2, c->salt_chr, CHG_CRYPT_SALT);
    }
    memcpy(data + 10, c->id_self, CHG_CRYPT_ID);
    {
        int n = frame_pack(CHG_MSG_HELLO_CHR, data, 18, out, cap);
        uint8_t chosen = c->suite;
        if (n < 0) {
            return -1;
        }
        if (chosen == CHG_SUITE_NONE) {
            c->phase = CHG_CRYPT_PH_IDLE;
            c->token_ok = 0;
        } else {
            enter_run(c, chosen);
        }
        return n;
    }
}

int chg_crypt_pack_token(ChgCrypt *c, ChgMacWork *w, uint8_t *out, size_t cap)
{
    uint8_t data[13];
    uint8_t dir;
    uint32_t counter;
    if (c == 0 || w == 0 || c->phase != CHG_CRYPT_PH_RUN || c->suite != CHG_SUITE_HMAC) {
        return -1;
    }
    if (!c->have_ctrl || c->tx_counter == 0xFFFFFFFFu) {
        return -1;
    }
    dir = (c->role == CHG_CRYPT_ROLE_BMS) ? CHG_DIR_BMS : CHG_DIR_CHR;
    counter = c->tx_counter + 1u;
    if (!mac_cover(c, w, counter, dir, data + 5)) {
        return -1;
    }
    data[0] = dir;
    data[1] = (uint8_t)(counter & 0xFFu);
    data[2] = (uint8_t)((counter >> 8) & 0xFFu);
    data[3] = (uint8_t)((counter >> 16) & 0xFFu);
    data[4] = (uint8_t)((counter >> 24) & 0xFFu);
    {
        int n = frame_pack(CHG_MSG_TOKEN, data, 13, out, cap);
        if (n < 0) {
            return -1;
        }
        c->tx_counter = counter;
        return n;
    }
}

int chg_crypt_pack_opaque(ChgCrypt *c, const uint8_t *body, uint8_t body_len,
                          uint8_t *out, size_t cap)
{
    uint8_t data[2u + CHG_CRYPT_BODY_MAX];
    if (c == 0 || c->phase != CHG_CRYPT_PH_RUN || c->suite < 0x80u || c->suite > 0xEFu) {
        return -1;
    }
    if (body_len > CHG_CRYPT_BODY_MAX || (body_len > 0u && body == 0)) {
        return -1;
    }
    data[0] = c->suite;
    data[1] = body_len;
    if (body_len > 0u) {
        memcpy(data + 2, body, body_len);
    }
    return frame_pack(CHG_MSG_OPAQUE, data, (uint8_t)(2u + body_len), out, cap);
}

static int on_hello_bms(ChgCrypt *c, const uint8_t *data, uint8_t len)
{
    uint8_t n;
    uint8_t i;
    uint8_t chosen;
    if (c->role != CHG_CRYPT_ROLE_CHR) {
        return 1;
    }
    if (len != 22u) {
        return -1;
    }
    if ((data[0] & ~(CHG_CRYPT_F_PLAIN | CHG_CRYPT_F_BIDI | CHG_CRYPT_F_SEAL)) != 0u) {
        return -1;
    }
    n = data[1];
    if (n < 1u || n > 4u) {
        return -1;
    }
    for (i = n; i < 4u; i++) {
        if (data[2u + i] != 0u) {
            return -1;
        }
    }
    {
        uint8_t hinted = (uint8_t)(data[0] & (CHG_CRYPT_F_PLAIN | CHG_CRYPT_F_BIDI));
        if (pick_suite(c, data + 2, n, hinted) == CHG_SUITE_HMAC && salt_zero(data + 6)) {
            return -1;
        }
    }
    drop_sk(c);
    c->peer_flags = (uint8_t)(data[0] & (CHG_CRYPT_F_PLAIN | CHG_CRYPT_F_BIDI));
    c->peer_seal = (data[0] & CHG_CRYPT_F_SEAL) ? 1u : 0u;
    memcpy(c->salt_bms, data + 6, CHG_CRYPT_SALT);
    c->salt_bms_set = 1u;
    memcpy(c->id_peer, data + 14, CHG_CRYPT_ID);
    chosen = pick_suite(c, data + 2, n, c->peer_flags);
    if (c->port == CHG_CRYPT_PORT_SIF) {
        memset(c->salt_chr, 0, sizeof c->salt_chr);
        c->salt_chr_set = 1u;
        if (chosen == CHG_SUITE_NONE || sif_bidi_blocked(c)) {
            c->phase = CHG_CRYPT_PH_IDLE;
            c->suite = CHG_SUITE_NONE;
            c->token_ok = 0;
            drop_sk(c);
            return 1;
        }
        enter_run(c, chosen);
        return 1;
    }
    c->suite = chosen;
    c->phase = CHG_CRYPT_PH_WAIT;
    c->token_ok = 0;
    c->peer_ok = 0;
    drop_sk(c);
    return 1;
}

static int on_hello_chr(ChgCrypt *c, const uint8_t *data, uint8_t len)
{
    uint8_t chosen;
    uint8_t peer_flags;
    if (c->role != CHG_CRYPT_ROLE_BMS) {
        return 1;
    }
    if (c->phase != CHG_CRYPT_PH_WAIT || len != 18u) {
        return -1;
    }
    if ((data[0] & ~(CHG_CRYPT_F_PLAIN | CHG_CRYPT_F_BIDI | CHG_CRYPT_F_SEAL)) != 0u) {
        return -1;
    }
    peer_flags = (uint8_t)(data[0] & (CHG_CRYPT_F_PLAIN | CHG_CRYPT_F_BIDI));
    chosen = data[1];
    if (c->port != CHG_CRYPT_PORT_SIF && suite_needs_salt(chosen) && salt_zero(data + 2)) {
        return -1;
    }
    drop_sk(c);
    c->peer_flags = peer_flags;
    c->peer_seal = (data[0] & CHG_CRYPT_F_SEAL) ? 1u : 0u;
    memcpy(c->salt_chr, data + 2, CHG_CRYPT_SALT);
    c->salt_chr_set = 1u;
    memcpy(c->id_peer, data + 10, CHG_CRYPT_ID);
    if (sif_bidi_blocked(c)) {
        c->phase = CHG_CRYPT_PH_IDLE;
        c->suite = CHG_SUITE_NONE;
        return 1;
    }
    if (chosen == CHG_SUITE_NONE) {
        c->phase = CHG_CRYPT_PH_IDLE;
        c->suite = CHG_SUITE_NONE;
        c->token_ok = 0;
        return 1;
    }
    if (chosen == CHG_SUITE_PLAIN) {
        if ((c->flags & CHG_CRYPT_F_PLAIN) && (peer_flags & CHG_CRYPT_F_PLAIN)) {
            enter_run(c, CHG_SUITE_PLAIN);
            return 1;
        }
        c->phase = CHG_CRYPT_PH_IDLE;
        c->suite = CHG_SUITE_NONE;
        return 1;
    }
    if (!runnable(c, chosen) || !listed(c->suites, c->suite_n, chosen)) {
        c->phase = CHG_CRYPT_PH_IDLE;
        c->suite = CHG_SUITE_NONE;
        c->token_ok = 0;
        return 1;
    }
    enter_run(c, chosen);
    return 1;
}

static int on_token(ChgCrypt *c, ChgMacWork *w, const uint8_t *data, uint8_t len)
{
    uint8_t dir;
    uint32_t counter;
    uint8_t expect[8];
    uint8_t peer_dir;
    if (len != 13u || w == 0) {
        return -1;
    }
    if (c->phase != CHG_CRYPT_PH_RUN || c->suite != CHG_SUITE_HMAC) {
        if (c->phase == CHG_CRYPT_PH_RUN) {
            note_fail(c);
        }
        return 1;
    }
    if (!c->have_ctrl) {
        note_fail(c);
        return 1;
    }
    dir = data[0];
    peer_dir = (c->role == CHG_CRYPT_ROLE_BMS) ? CHG_DIR_CHR : CHG_DIR_BMS;
    counter = (uint32_t)data[1] | ((uint32_t)data[2] << 8) | ((uint32_t)data[3] << 16) |
              ((uint32_t)data[4] << 24);
    if (dir != peer_dir || counter == 0u || counter != c->rx_counter + 1u) {
        note_fail(c);
        return 1;
    }
    if (!mac_cover(c, w, counter, dir, expect) || !tag_eq(expect, data + 5)) {
        note_fail(c);
        return 1;
    }
    c->rx_counter = counter;
    c->fails = 0;
    if (dir == CHG_DIR_BMS) {
        c->token_ok = 1u;
    } else {
        c->peer_ok = 1u;
        c->peer_ms = CHG_CRYPT_PEER_MS;
    }
    return 1;
}

static int on_opaque(ChgCrypt *c, const uint8_t *data, uint8_t len)
{
    uint8_t suite;
    uint8_t blen;
    uint8_t msg = 0;
    uint8_t out[CHG_CRYPT_BAT];
    uint8_t out_len = 0;
    int rc;
    if (len < 2u || c->phase != CHG_CRYPT_PH_RUN) {
        return -1;
    }
    suite = data[0];
    blen = data[1];
    if ((uint8_t)(2u + blen) != len || blen > CHG_CRYPT_BODY_MAX) {
        return -1;
    }
    if (suite != c->suite || suite < 0x80u || suite > 0xEFu || c->vendor == 0) {
        note_fail(c);
        return 1;
    }
    memset(out, 0, sizeof out);
    rc = c->vendor(suite, data + 2, blen, &msg, out, &out_len, c->vendor_user);
    if (rc != 0) {
        note_fail(c);
        return 1;
    }
    if (msg == 0x01u && out_len == CHG_CRYPT_CTRL) {
        memcpy(c->ctrl, out, CHG_CRYPT_CTRL);
        c->have_ctrl = 1u;
        c->fails = 0;
        if (c->role == CHG_CRYPT_ROLE_CHR) {
            c->token_ok = 1u;
        } else {
            c->peer_ok = 1u;
            c->peer_ms = CHG_CRYPT_PEER_MS;
        }
        return 1;
    }
    if (msg == 0x04u && out_len == CHG_CRYPT_BAT) {
        memcpy(c->bat, out, CHG_CRYPT_BAT);
        c->have_bat = 1u;
        c->token_ok = 0u;
        c->fails = 0;
        return 1;
    }
    note_fail(c);
    return 1;
}

static int on_msg(ChgCrypt *c, ChgMacWork *w, uint8_t msg, const uint8_t *data, uint8_t len)
{
    if (c->phase == CHG_CRYPT_PH_LOCK && msg != 0u) {
        return 1;
    }
    if (msg == CHG_MSG_HELLO_BMS) {
        return on_hello_bms(c, data, len);
    }
    if (msg == CHG_MSG_HELLO_CHR) {
        return on_hello_chr(c, data, len);
    }
    if (msg == CHG_MSG_TOKEN) {
        return on_token(c, w, data, len);
    }
    if (msg == CHG_MSG_OPAQUE) {
        return on_opaque(c, data, len);
    }
    return 0;
}

int chg_crypt_rx_frame(ChgCrypt *c, ChgMacWork *w, const uint8_t *frame, size_t n)
{
    uint8_t msg;
    uint8_t len;
    uint16_t crc;
    uint16_t got;
    if (c == 0 || frame == 0 || n < 7u) {
        return 0;
    }
    if (frame[0] != 0xA5u || frame[1] != 0x5Au || frame[2] != 0x01u) {
        return 0;
    }
    msg = frame[3];
    if (msg < CHG_MSG_HELLO_BMS || msg > CHG_MSG_OPAQUE) {
        return 0;
    }
    len = frame[4];
    if (n < (size_t)len + 7u) {
        return -1;
    }
    crc = crc16(frame, (size_t)len + 5u);
    got = (uint16_t)(frame[5u + len] | ((uint16_t)frame[6u + len] << 8));
    if (crc != got) {
        return -1;
    }
    return on_msg(c, w, msg, frame + 5, len);
}

int chg_crypt_can_split(uint32_t id, uint8_t msg, const uint8_t *data, uint8_t len,
                        ChgCan *out, uint8_t cap)
{
    uint8_t stream[28];
    uint8_t need;
    uint8_t frames;
    uint8_t seq;
    uint8_t off;
    if (out == 0 || len > 26u || (len > 0u && data == 0)) {
        return -1;
    }
    stream[0] = msg;
    stream[1] = len;
    if (len > 0u) {
        memcpy(stream + 2, data, len);
    }
    need = (uint8_t)(2u + len);
    frames = (uint8_t)((need + 6u) / 7u);
    if (cap < frames) {
        return -1;
    }
    off = 0;
    for (seq = 0; seq < frames; seq++) {
        uint8_t n = (uint8_t)(need - off);
        if (n > 7u) {
            n = 7u;
        }
        memset(&out[seq], 0, sizeof out[seq]);
        out[seq].id = id;
        out[seq].dlc = (uint8_t)(1u + n);
        out[seq].data[0] = seq;
        memcpy(out[seq].data + 1, stream + off, n);
        off = (uint8_t)(off + n);
    }
    return (int)frames;
}

int chg_crypt_can_rx(ChgCrypt *c, ChgMacWork *w, const ChgCan *f)
{
    uint8_t seq;
    uint8_t n;
    uint8_t dlen;
    uint8_t need;
    uint8_t frames;
    uint8_t have;
    uint8_t i;
    uint32_t expect;
    if (c == 0 || f == 0) {
        return 0;
    }
    expect = (c->role == CHG_CRYPT_ROLE_BMS) ? CHG_CAN_TOK_CHR : CHG_CAN_TOK_BMS;
    if (f->id != expect) {
        return 0;
    }
    if (f->dlc < 2u || f->dlc > 8u) {
        return -1;
    }
    seq = f->data[0];
    if (seq > 3u) {
        asm_reset(c);
        return -1;
    }
    n = (uint8_t)(f->dlc - 1u);
    if ((c->asm_mask & (uint8_t)(1u << seq)) != 0u) {
        if (c->asm_n[seq] != n || memcmp(c->asm_buf + (size_t)seq * 7u, f->data + 1, n) != 0) {
            asm_reset(c);
            return -1;
        }
    } else {
        if ((size_t)seq * 7u + n > sizeof c->asm_buf) {
            asm_reset(c);
            return -1;
        }
        memcpy(c->asm_buf + (size_t)seq * 7u, f->data + 1, n);
        c->asm_n[seq] = n;
        c->asm_mask = (uint8_t)(c->asm_mask | (uint8_t)(1u << seq));
    }
    if ((c->asm_mask & 1u) == 0u || c->asm_n[0] < 2u) {
        return 1;
    }
    dlen = c->asm_buf[1];
    if (dlen > 26u) {
        asm_reset(c);
        return -1;
    }
    need = (uint8_t)(2u + dlen);
    frames = (uint8_t)((need + 6u) / 7u);
    have = 0;
    for (i = 0; i < frames; i++) {
        if ((c->asm_mask & (uint8_t)(1u << i)) == 0u) {
            return 1;
        }
        have = (uint8_t)(have + c->asm_n[i]);
    }
    if (have != need) {
        asm_reset(c);
        return -1;
    }
    {
        uint8_t msg = c->asm_buf[0];
        uint8_t body[26];
        if (dlen > 0u) {
            memcpy(body, c->asm_buf + 2, dlen);
        }
        asm_reset(c);
        return on_msg(c, w, msg, body, dlen);
    }
}

int chg_crypt_output_allowed(const ChgCrypt *c)
{
    if (!session_open(c) || sealed(c) || !bound_ok(c)) {
        return 0;
    }
    if (c->suite == CHG_SUITE_PLAIN) {
        return c->restrict_id ? 0 : 1;
    }
    if (!c->token_ok) {
        return 0;
    }
    if (c->restrict_id && !id_listed(c)) {
        return 0;
    }
    return 1;
}

int chg_crypt_bms_may_allow(const ChgCrypt *c)
{
    if (!session_open(c) || c->role != CHG_CRYPT_ROLE_BMS || sealed(c) || !bound_ok(c)) {
        return 0;
    }
    if (c->pair_on && !c->peer_ok) {
        return 0;
    }
    if (c->suite == CHG_SUITE_PLAIN) {
        return c->restrict_id ? 0 : 1;
    }
    if (c->restrict_id) {
        if (!c->peer_ok || !id_listed(c)) {
            return 0;
        }
    } else if (bidi_on(c) && !c->peer_ok) {
        return 0;
    }
    return 1;
}

void chg_crypt_bind_output(const ChgCrypt *c, uint16_t *i_out_01a, uint8_t *enable)
{
    if (i_out_01a == 0 || enable == 0) {
        return;
    }
    if (!chg_crypt_output_allowed(c)) {
        *i_out_01a = 0;
        *enable = 0;
    }
}
