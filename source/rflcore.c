#include "rflcore.h"
#include <stdlib.h>
#include <string.h>

/* ---------- helpers ---------- */

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

uint16_t rfl_crc16(const uint8_t *p, size_t n)
{
    /* CRC-16/XMODEM: poly 0x1021, init 0, MSB first (equivalent to the
       "16 extra zero shifts" description on WiiBrew). */
    uint16_t crc = 0;
    while (n--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

uint64_t rfl_id_of(const uint8_t *m)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | m[RFL_ID_OFF + i];
    return v;
}

/* ---------- id set ---------- */

int idset_init(IdSet *s, uint32_t cap)
{
    s->keys = (uint64_t *)calloc(cap, sizeof(uint64_t));
    s->cap = cap;
    s->n = 0;
    return s->keys ? 0 : -1;
}

void idset_free(IdSet *s) { free(s->keys); s->keys = NULL; s->n = s->cap = 0; }

static uint32_t hslot(uint64_t k, uint32_t cap)
{
    k *= 0x9E3779B97F4A7C15ull;
    return (uint32_t)(k >> 32) & (cap - 1);
}

int idset_add(IdSet *s, uint64_t k)
{
    if (k == 0) return -1;
    if (s->n >= (s->cap / 4) * 3) return -1;
    uint32_t i = hslot(k, s->cap);
    while (s->keys[i]) {
        if (s->keys[i] == k) return 0;
        i = (i + 1) & (s->cap - 1);
    }
    s->keys[i] = k;
    s->n++;
    return 1;
}

int idset_has(const IdSet *s, uint64_t k)
{
    if (k == 0) return 0;
    uint32_t i = hslot(k, s->cap);
    while (s->keys[i]) {
        if (s->keys[i] == k) return 1;
        i = (i + 1) & (s->cap - 1);
    }
    return 0;
}

/* ---------- CRC schemes ----------
 * The stored CRC is at 0x1F1DE. The exact range it covers is not documented
 * with certainty, so we test the plausible ranges against the user's own
 * untouched file. If none match we refuse to write anything. */

typedef struct { uint32_t start, end; } Range;
static const Range schemes[] = {
    { 0x00000, 0x1F1DE },                       /* whole file prefix (WiiBrew) */
    { RFL_HDB_OFF, 0x1F1DE },                   /* whole hidden header block   */
    { RFL_HDB_OFF, RFL_HDB_OFF + 8 },           /* header words only           */
    { RFL_HDB_OFF, RFL_HDB_OFF + 8 + 120000 },  /* header + node table         */
    { 0x00000, RFL_HDB_OFF + 8 + 120000 },
};
#define NSCHEMES (int)(sizeof(schemes) / sizeof(schemes[0]))

static uint16_t calc_crc(const uint8_t *db, int scheme)
{
    return rfl_crc16(db + schemes[scheme].start, schemes[scheme].end - schemes[scheme].start);
}

static int detect_crc(const uint8_t *db, RflCrc *out)
{
    uint16_t stored_be = be16(db + RFL_CRC_OFF);
    uint16_t stored_le = (uint16_t)(db[RFL_CRC_OFF] | (db[RFL_CRC_OFF + 1] << 8));
    for (int s = 0; s < NSCHEMES; s++) {
        uint16_t c = calc_crc(db, s);
        if (c == stored_be) { out->scheme = s; out->little = 0; return 0; }
        if (c == stored_le) { out->scheme = s; out->little = 1; return 0; }
    }
    return -1;
}

/* ---------- node access ---------- */

static uint8_t *node(const RflDb *h, int i) { return h->db + RFL_NODES_OFF + (size_t)i * RFL_NODE_SIZE; }
static int node_next(const uint8_t *n) { int v = be16(n + 8) & 0x7FFF; return v == 0x7FFF ? -1 : v; }
static int node_prev(const uint8_t *n) { int v = be16(n + 10) & 0x7FFF; return v == 0x7FFF ? -1 : v; }
static void node_set(uint8_t *n, const uint8_t *id, int sex, int next, int prev)
{
    memcpy(n, id, 8);
    put16(n + 8, (uint16_t)(((sex & 1) << 15) | (next < 0 ? 0x7FFF : next)));
    put16(n + 10, (uint16_t)(prev < 0 ? 0x7FFF : prev));
}
static void set_next(uint8_t *n, int next)
{
    put16(n + 8, (uint16_t)((be16(n + 8) & 0x8000) | (next < 0 ? 0x7FFF : next)));
}
static void set_prev(uint8_t *n, int prev)
{
    put16(n + 10, (uint16_t)(prev < 0 ? 0x7FFF : prev));
}
static void write_hdr(RflDb *h)
{
    put16(h->db + RFL_HDB_OFF + 4, (uint16_t)h->head);
    put16(h->db + RFL_HDB_OFF + 6, (uint16_t)h->tail);
}

/* Walk list, fill used[], check links. */
static int index_list(RflDb *h)
{
    memset(h->used, 0, sizeof(h->used));
    h->count = 0;
    h->head = (int16_t)be16(h->db + RFL_HDB_OFF + 4);
    h->tail = (int16_t)be16(h->db + RFL_HDB_OFF + 6);
    if (h->head == -1 || h->tail == -1) {
        return (h->head == -1 && h->tail == -1) ? RFL_OK : RFL_ERR_LIST;
    }
    int prev = -1, cur = h->head;
    while (cur != -1) {
        if (cur < 0 || cur >= RFL_HIDDEN_MAX || h->used[cur]) return RFL_ERR_LIST;
        const uint8_t *n = node(h, cur);
        if (node_prev(n) != prev) return RFL_ERR_LIST;
        h->used[cur] = 1;
        h->count++;
        prev = cur;
        cur = node_next(n);
    }
    return prev == h->tail ? RFL_OK : RFL_ERR_LIST;
}

int rfl_open(RflDb *h, uint8_t *db, size_t size)
{
    memset(h, 0, sizeof(*h));
    h->db = db;
    if (size != RFL_FILE_SIZE) return RFL_ERR_SIZE;
    if (memcmp(db, "RNOD", 4) != 0 || memcmp(db + RFL_HDB_OFF, "RNHD", 4) != 0) return RFL_ERR_MAGIC;
    if (detect_crc(db, &h->crc) != 0) return RFL_ERR_CRC;
    return index_list(h);
}

const char *rfl_strerror(int code)
{
    switch (code) {
    case RFL_OK: return "ok";
    case RFL_ERR_SIZE: return "RFL_DB.dat is not the expected size (not a stock Mii database)";
    case RFL_ERR_MAGIC: return "RFL_DB.dat headers (RNOD/RNHD) not found";
    case RFL_ERR_CRC: return "stored checksum does not match any known scheme - refusing to edit";
    case RFL_ERR_LIST: return "parade list is inconsistent";
    case RFL_ERR_MEM: return "out of memory";
    }
    return "unknown error";
}

int rfl_verify(RflDb *h)
{
    int r = index_list(h);
    if (r != RFL_OK) return r;
    uint16_t c = calc_crc(h->db, h->crc.scheme);
    uint16_t stored = h->crc.little ? (uint16_t)(h->db[RFL_CRC_OFF] | (h->db[RFL_CRC_OFF + 1] << 8))
                                    : be16(h->db + RFL_CRC_OFF);
    return c == stored ? RFL_OK : RFL_ERR_CRC;
}

void rfl_seal(RflDb *h)
{
    write_hdr(h);
    uint16_t c = calc_crc(h->db, h->crc.scheme);
    if (h->crc.little) { h->db[RFL_CRC_OFF] = (uint8_t)c; h->db[RFL_CRC_OFF + 1] = (uint8_t)(c >> 8); }
    else put16(h->db + RFL_CRC_OFF, c);
}

/* ---------- reading ---------- */

int rfl_official_ids(const RflDb *h, IdSet *s)
{
    int n = 0;
    for (int i = 0; i < RFL_OFFICIAL_MAX; i++) {
        const uint8_t *m = h->db + RFL_OFFICIAL_OFF + (size_t)i * RFL_MII_SIZE;
        uint64_t id = rfl_id_of(m);
        if (id && idset_add(s, id) == 1) n++;
    }
    return n;
}

const uint8_t *rfl_hidden_rec(const RflDb *h, int idx)
{
    return h->db + RFL_HREC_OFF + (size_t)idx * RFL_HREC_SIZE;
}

int rfl_hidden_order(const RflDb *h, int16_t *out)
{
    int n = 0, cur = h->head;
    while (cur != -1 && n < RFL_HIDDEN_MAX) {
        out[n++] = (int16_t)cur;
        cur = node_next(node(h, cur));
    }
    return n;
}

int rfl_hidden_ids(const RflDb *h, IdSet *s)
{
    int n = 0, cur = h->head, guard = 0;
    while (cur != -1 && guard++ < RFL_HIDDEN_MAX) {
        if (idset_add(s, rfl_id_of(rfl_hidden_rec(h, cur))) == 1) n++;
        cur = node_next(node(h, cur));
    }
    return n;
}

/* ---------- candidate Miis ---------- */

int rfl_check_mii(const uint8_t *m, int allow_private)
{
    int zero = 1;
    for (int i = 0; i < 8; i++) if (m[RFL_ID_OFF + i]) zero = 0;
    if (zero) return REJ_ZERO;
    uint16_t w0 = be16(m);
    if (w0 & 0x8000) return REJ_INVALID;                 /* invalid flag */
    if (m[2] == 0 && m[3] == 0) return REJ_INVALID;      /* no name */
    const uint8_t b = m[RFL_ID_OFF];
    if (!(b & 0x80)) return REJ_SPECIAL;                 /* gold-pants ids are system reserved */
    if (b & 0x20) return REJ_DELETE;                     /* "delete me" id flag */
    uint16_t w20 = be16(m + 0x20);
    if (!allow_private && (w20 & ((1u << 5) | (1u << 2)))) return REJ_PRIVATE; /* mingle off */
    /* range checks - the Mii Channel hides Miis with out-of-range parts */
    if (((w20 >> 10) & 7) >= 6) return REJ_RANGE;                 /* skin colour */
    if (((be16(m + 0x22) >> 9) & 0x7F) >= 72) return REJ_RANGE;   /* hair type */
    if (((be16(m + 0x24) >> 11) & 0x1F) >= 24) return REJ_RANGE;  /* eyebrow type */
    if (((be16(m + 0x28) >> 10) & 0x3F) >= 48) return REJ_RANGE;  /* eye type */
    if (((be16(m + 0x2A) >> 13) & 7) >= 6) return REJ_RANGE;      /* eye colour */
    if (((be16(m + 0x2C) >> 12) & 0xF) >= 12) return REJ_RANGE;   /* nose type */
    if (((be16(m + 0x2E) >> 11) & 0x1F) >= 24) return REJ_RANGE;  /* mouth type */
    if (((be16(m + 0x2E) >> 9) & 3) >= 3) return REJ_RANGE;       /* mouth colour */
    return -1;
}

void rfl_to_hidden(const uint8_t *m, uint8_t *out, int allow_private)
{
    memcpy(out, m, 0x36);
    memset(out + 0x36, 0, RFL_HREC_SIZE - 0x36);
    uint16_t w0 = be16(out);
    w0 &= (uint16_t)~(0x3FE0u | 0x0001u);    /* drop birthday (9 bits) and favourite */
    put16(out, w0);
    if (allow_private) {
        uint16_t w20 = be16(out + 0x20);
        w20 &= (uint16_t)~((1u << 5) | (1u << 2));
        put16(out + 0x20, w20);
    }
}

void rfl_to_mii(const uint8_t *rec, uint8_t *out)
{
    memset(out, 0, RFL_MII_SIZE);
    memcpy(out, rec, RFL_HREC_SIZE);
}

/* ---------- adding ---------- */

int rfl_hidden_add(RflDb *h, const uint8_t *rec, int evict)
{
    int idx = -1;
    if (h->count >= RFL_HIDDEN_MAX) {
        if (!evict) return 0;
        /* drop the oldest entry and reuse its slot */
        int old = h->head;
        if (old < 0) return -1;
        int nx = node_next(node(h, old));
        h->head = (int16_t)nx;
        if (nx == -1) h->tail = -1; else set_prev(node(h, nx), -1);
        h->used[old] = 0;
        h->count--;
        idx = old;
    } else {
        for (int k = 0; k < RFL_HIDDEN_MAX; k++) {
            int c = (h->scan + k) % RFL_HIDDEN_MAX;
            if (!h->used[c]) { idx = c; h->scan = (uint16_t)((c + 1) % RFL_HIDDEN_MAX); break; }
        }
        if (idx < 0) return -1;
    }
    int sex = (be16(rec) >> 14) & 1;
    memcpy(h->db + RFL_HREC_OFF + (size_t)idx * RFL_HREC_SIZE, rec, RFL_HREC_SIZE);
    node_set(node(h, idx), rec + RFL_ID_OFF, sex, -1, h->tail);
    if (h->tail != -1) set_next(node(h, h->tail), idx); else h->head = (int16_t)idx;
    h->tail = (int16_t)idx;
    h->used[idx] = 1;
    h->count++;
    return 1;
}
