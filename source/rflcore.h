/*
 * rflcore - platform independent logic for editing the Wii's RFL_DB.dat
 * (/shared2/menu/FaceLib/RFL_DB.dat), specifically the "hidden database"
 * that feeds the Mii Parade.
 *
 * Layout (from the community decompilation of Nintendo's RFL library):
 *   0x00000  RNOD official database (100 x 74-byte Miis)  - never touched
 *   0x01D00  RNHD hidden (parade) database header:
 *              +0  u32 'RNHD'
 *              +4  s16 head, +6 s16 tail   (-1 = empty)
 *              +8  10000 x 12-byte list nodes:
 *                    8 bytes createID, u16 [sex:1|next:15], u16 [pad:1|prev:15]
 *              ... padding, u16 CRC at 0x1F1DE
 *   0x1F1E0  10000 x 64-byte hidden Mii records (index == list node index)
 *   total    0xBB5E0 bytes
 */
#ifndef RFLCORE_H
#define RFLCORE_H

#include <stdint.h>
#include <stddef.h>

#define RFL_FILE_SIZE      0xBB5E0u
#define RFL_OFFICIAL_MAX   100
#define RFL_MII_SIZE       74
#define RFL_OFFICIAL_OFF   4u
#define RFL_HDB_OFF        0x1D00u
#define RFL_NODES_OFF      (RFL_HDB_OFF + 8u)
#define RFL_NODE_SIZE      12u
#define RFL_HIDDEN_MAX     10000
#define RFL_CRC_OFF        0x1F1DEu
#define RFL_HREC_OFF       0x1F1E0u
#define RFL_HREC_SIZE      64u
#define RFL_ID_OFF         0x18u   /* createID inside a Mii record */

enum {
    RFL_OK = 0,
    RFL_ERR_SIZE = -1,
    RFL_ERR_MAGIC = -2,
    RFL_ERR_CRC = -3,      /* stored CRC matches none of the known schemes */
    RFL_ERR_LIST = -4,     /* hidden list is inconsistent */
    RFL_ERR_MEM = -5
};

/* reasons a candidate Mii is rejected */
enum {
    REJ_ZERO, REJ_INVALID, REJ_SPECIAL, REJ_DELETE, REJ_PRIVATE, REJ_RANGE,
    REJ_OFFICIAL, REJ_DUP, REJ_COUNT
};

typedef struct {
    uint64_t *keys;
    uint32_t cap;   /* power of two */
    uint32_t n;
} IdSet;

typedef struct {
    int scheme;     /* index into the CRC range table */
    int little;     /* stored little endian? */
} RflCrc;

typedef struct {
    uint8_t *db;            /* RFL_FILE_SIZE bytes, owned by caller */
    RflCrc crc;
    int16_t head, tail;
    int count;
    uint16_t scan;          /* free slot search hint */
    uint8_t used[RFL_HIDDEN_MAX];
} RflDb;

uint16_t rfl_crc16(const uint8_t *p, size_t n);

int  idset_init(IdSet *s, uint32_t cap_pow2);
void idset_free(IdSet *s);
int  idset_add(IdSet *s, uint64_t k);   /* 1 inserted, 0 existed, -1 full */
int  idset_has(const IdSet *s, uint64_t k);
uint64_t rfl_id_of(const uint8_t *mii_or_rec);  /* reads bytes at +0x18 */

/* Validate and index a loaded RFL_DB.dat image. */
int  rfl_open(RflDb *h, uint8_t *db, size_t size);
const char *rfl_strerror(int code);

/* Check list integrity + CRC without modifying anything. */
int  rfl_verify(RflDb *h);

/* Official (plaza) Miis: copies their IDs into the set; returns how many. */
int  rfl_official_ids(const RflDb *h, IdSet *s);
/* Existing parade Miis: copies IDs into the set; returns how many. */
int  rfl_hidden_ids(const RflDb *h, IdSet *s);

/* Walk the parade in list order. out must hold RFL_HIDDEN_MAX entries. */
int  rfl_hidden_order(const RflDb *h, int16_t *out);
const uint8_t *rfl_hidden_rec(const RflDb *h, int idx);

/* Candidate handling. m is a 74 byte Mii. Returns -1 if ok, else REJ_*. */
int  rfl_check_mii(const uint8_t *m, int allow_private);
/* 74-byte Mii -> 64-byte parade record. */
void rfl_to_hidden(const uint8_t *m, uint8_t *out, int allow_private);
/* 64-byte parade record -> 74-byte Mii (birthday/creator blank). */
void rfl_to_mii(const uint8_t *rec, uint8_t *out);

/* Append a record. Returns 1 added, 0 parade full and evict==0, -1 error. */
int  rfl_hidden_add(RflDb *h, const uint8_t *rec, int evict);

/* Recompute and store the CRC. Call after all edits. */
void rfl_seal(RflDb *h);

#endif
