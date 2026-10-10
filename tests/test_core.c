/* Native test for rflcore: builds a synthetic RFL_DB.dat, imports Miis,
 * exercises eviction/dedupe/CRC, writes result files for check_db.py. */
#include "../source/rflcore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }

static uint8_t *make_db(int scheme_start, int le)
{
    uint8_t *db = calloc(1, RFL_FILE_SIZE);
    memcpy(db, "RNOD", 4);
    /* official Miis with ids 0x80000001.. */
    for (int i = 0; i < 3; i++) {
        uint8_t *m = db + RFL_OFFICIAL_OFF + i * RFL_MII_SIZE;
        m[2] = 0; m[3] = 'A' + i;
        m[0x18] = 0x80; m[0x1B] = 1 + i; m[0x1F] = 7;
    }
    memcpy(db + RFL_HDB_OFF, "RNHD", 4);
    put16(db + RFL_HDB_OFF + 4, 0xFFFF);
    put16(db + RFL_HDB_OFF + 6, 0xFFFF);
    for (int i = 0; i < RFL_HIDDEN_MAX; i++) {
        uint8_t *n = db + RFL_NODES_OFF + i * RFL_NODE_SIZE;
        put16(n + 8, 0x7FFF); put16(n + 10, 0x7FFF);
    }
    uint16_t c = rfl_crc16(db + scheme_start, RFL_CRC_OFF - scheme_start);
    if (le) { db[RFL_CRC_OFF] = (uint8_t)c; db[RFL_CRC_OFF + 1] = c >> 8; } else put16(db + RFL_CRC_OFF, c);
    return db;
}

static void make_mii(uint8_t *m, uint32_t n)
{
    memset(m, 0, RFL_MII_SIZE);
    put16(m, 0x1FFF);                 /* birthday + favourite set, will be stripped */
    m[2] = 0; m[3] = 'M';
    m[0x16] = 64; m[0x17] = 64;
    m[0x18] = 0x80 | ((n >> 24) & 0x0F); m[0x19] = n >> 16; m[0x1A] = n >> 8; m[0x1B] = n;
    m[0x1C] = 1; m[0x1D] = 2; m[0x1E] = 3; m[0x1F] = 4;
    put16(m + 0x20, 0x0400);
    m[0x36] = 0; m[0x37] = 'C';       /* creator name must be dropped */
}

int main(int argc, char **argv)
{
    const char *outdir = argc > 1 ? argv[1] : ".";
    char path[512];

    /* 1. every CRC scheme / endianness must be detected */
    int starts[] = { 0, RFL_HDB_OFF };
    for (int s = 0; s < 2; s++) for (int le = 0; le < 2; le++) {
        uint8_t *db = make_db(starts[s], le);
        RflDb h; CHECK(rfl_open(&h, db, RFL_FILE_SIZE) == RFL_OK);
        CHECK(rfl_verify(&h) == RFL_OK);
        free(db);
    }
    /* corrupt CRC must be refused */
    { uint8_t *db = make_db(0, 0); db[RFL_CRC_OFF] ^= 0x55;
      RflDb h; CHECK(rfl_open(&h, db, RFL_FILE_SIZE) == RFL_ERR_CRC); free(db); }
    { uint8_t *db = make_db(0, 0); RflDb h; CHECK(rfl_open(&h, db, RFL_FILE_SIZE - 1) == RFL_ERR_SIZE); free(db); }
    { uint8_t *db = make_db(0, 0); db[0] = 'X'; RflDb h; CHECK(rfl_open(&h, db, RFL_FILE_SIZE) == RFL_ERR_MAGIC); free(db); }

    /* 2. import 12000 unique Miis, no eviction -> exactly 10000 stored */
    uint8_t *db = make_db(0, 0);
    uint8_t *orig = malloc(RFL_FILE_SIZE); memcpy(orig, db, RFL_FILE_SIZE);
    snprintf(path, sizeof path, "%s/before.dat", outdir);
    FILE *f = fopen(path, "wb"); fwrite(db, 1, RFL_FILE_SIZE, f); fclose(f);

    RflDb h; CHECK(rfl_open(&h, db, RFL_FILE_SIZE) == RFL_OK);
    IdSet ids; CHECK(idset_init(&ids, 1 << 18) == 0);
    CHECK(rfl_official_ids(&h, &ids) == 3);
    CHECK(rfl_hidden_ids(&h, &ids) == 0);

    int added = 0, full = 0, dup = 0;
    for (uint32_t n = 1; n <= 12000; n++) {
        uint8_t m[RFL_MII_SIZE], rec[RFL_HREC_SIZE];
        make_mii(m, n);
        CHECK(rfl_check_mii(m, 0) == -1);
        if (idset_add(&ids, rfl_id_of(m)) != 1) { dup++; continue; }
        rfl_to_hidden(m, rec, 0);
        int r = rfl_hidden_add(&h, rec, 0);
        if (r == 1) added++; else if (r == 0) full++; else CHECK(0);
    }
    CHECK(added == 10000 && full == 2000 && dup == 0 && h.count == 10000);
    rfl_seal(&h);
    CHECK(rfl_verify(&h) == RFL_OK);

    /* duplicates of an official Mii and of an imported one are rejected */
    { uint8_t m[RFL_MII_SIZE]; make_mii(m, 5); CHECK(idset_add(&ids, rfl_id_of(m)) == 0);
      memcpy(m + RFL_ID_OFF, db + RFL_OFFICIAL_OFF + RFL_ID_OFF, 8); CHECK(idset_add(&ids, rfl_id_of(m)) == 0); }

    /* stripped fields */
    { const uint8_t *r = rfl_hidden_rec(&h, 0);
      CHECK((((r[0] << 8) | r[1]) & 0x3FE1) == 0);
      CHECK(r[0x36] == 0 && r[0x37] == 0); }

    snprintf(path, sizeof path, "%s/full.dat", outdir);
    f = fopen(path, "wb"); fwrite(db, 1, RFL_FILE_SIZE, f); fclose(f);

    /* official region untouched */
    CHECK(memcmp(db, orig, RFL_HDB_OFF) == 0);

    /* 3. re-open the saved image (CRC + list survive a round trip) */
    { uint8_t *copy = malloc(RFL_FILE_SIZE); memcpy(copy, db, RFL_FILE_SIZE);
      RflDb h2; CHECK(rfl_open(&h2, copy, RFL_FILE_SIZE) == RFL_OK); CHECK(h2.count == 10000);
      free(copy); }

    /* 4. eviction: add 500 more, oldest 500 go away, still consistent */
    for (uint32_t n = 20001; n <= 20500; n++) {
        uint8_t m[RFL_MII_SIZE], rec[RFL_HREC_SIZE];
        make_mii(m, n); rfl_to_hidden(m, rec, 0);
        CHECK(rfl_hidden_add(&h, rec, 1) == 1);
    }
    CHECK(h.count == 10000);
    rfl_seal(&h);
    CHECK(rfl_verify(&h) == RFL_OK);
    { static int16_t order[RFL_HIDDEN_MAX]; CHECK(rfl_hidden_order(&h, order) == 10000);
      CHECK(((rfl_id_of(rfl_hidden_rec(&h, order[0])) >> 32) & 0x0FFFFFFF) == 501);
      CHECK(((rfl_id_of(rfl_hidden_rec(&h, order[9999])) >> 32) & 0x0FFFFFFF) == 20500); }
    snprintf(path, sizeof path, "%s/evicted.dat", outdir);
    f = fopen(path, "wb"); fwrite(db, 1, RFL_FILE_SIZE, f); fclose(f);

    /* 5. candidate filters */
    { uint8_t m[RFL_MII_SIZE]; make_mii(m, 77);
      m[0x18] = 0x20; CHECK(rfl_check_mii(m, 0) == REJ_SPECIAL);
      make_mii(m, 77); m[0x18] |= 0x20; CHECK(rfl_check_mii(m, 0) == REJ_DELETE);
      make_mii(m, 77); m[2] = m[3] = 0; CHECK(rfl_check_mii(m, 0) == REJ_INVALID);
      make_mii(m, 77); put16(m + 0x20, 0x0004); CHECK(rfl_check_mii(m, 0) == REJ_PRIVATE);
      CHECK(rfl_check_mii(m, 1) == -1);
      uint8_t rec[RFL_HREC_SIZE]; rfl_to_hidden(m, rec, 1); CHECK((((rec[0x20] << 8) | rec[0x21]) & 0x24) == 0);
      make_mii(m, 77); put16(m + 0x22, 0x7F << 9); CHECK(rfl_check_mii(m, 0) == REJ_RANGE);
      make_mii(m, 77); memset(m + 0x18, 0, 8); CHECK(rfl_check_mii(m, 0) == REJ_ZERO); }

    /* 6. record <-> mii round trip */
    { uint8_t m[RFL_MII_SIZE], rec[RFL_HREC_SIZE], back[RFL_MII_SIZE], rec2[RFL_HREC_SIZE];
      make_mii(m, 4242); rfl_to_hidden(m, rec, 0); rfl_to_mii(rec, back); rfl_to_hidden(back, rec2, 0);
      CHECK(memcmp(rec, rec2, RFL_HREC_SIZE) == 0); }

    idset_free(&ids); free(db); free(orig);
    printf("ALL CORE TESTS PASSED\n");
    return 0;
}
