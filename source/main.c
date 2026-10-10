/*
 * MiiFlood - bulk-import Miis into the Wii Mii Parade (RFL_DB.dat hidden DB)
 *
 * Reads Miis from SD/USB, merges them into the parade (up to 10,000), keeps
 * an unlimited archive of every parade Mii on the card, and always backs up
 * RFL_DB.dat before touching NAND.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <fat.h>
#include "rflcore.h"

#define DB_PATH      "/shared2/menu/FaceLib/RFL_DB.dat"
#define MAX_NEW      100000
#define MAX_FILE     (16u * 1024u * 1024u)
#define IDSET_CAP    (1u << 19)

static void *xfb = NULL;
static GXRModeObj *rmode = NULL;
static char root[32];            /* "sd:/apps/MiiFlood" or "usb:/apps/MiiFlood" */

/* ------------------------------------------------------------ video/input */

static void video_init(void)
{
    VIDEO_Init();
    WPAD_Init();
    rmode = VIDEO_GetPreferredMode(NULL);
    xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
}

static u32 wait_buttons(void)
{
    for (;;) {
        WPAD_ScanPads();
        u32 d = WPAD_ButtonsDown(0);
        if (d) return d;
        VIDEO_WaitVSync();
    }
}

static void quit(int code)
{
    printf("\nExiting...\n");
    sleep(2);
    exit(code);
}

static void pause_exit(const char *msg)
{
    printf("\n%s\nPress HOME to exit.\n", msg);
    for (;;) { if (wait_buttons() & WPAD_BUTTON_HOME) quit(1); }
}

/* ------------------------------------------------------------------ NAND */

static fstats st_buf ATTRIBUTE_ALIGN(32);

static int nand_read_db(uint8_t *buf, size_t want, size_t *got)
{
    s32 fd = ISFS_Open(DB_PATH, ISFS_OPEN_READ);
    if (fd < 0) return fd;
    s32 r = ISFS_GetFileStats(fd, &st_buf);
    if (r < 0) { ISFS_Close(fd); return r; }
    size_t len = st_buf.file_length;
    if (len > want) len = want;
    size_t pos = 0;
    while (pos < len) {
        size_t n = len - pos > 16384 ? 16384 : len - pos;
        r = ISFS_Read(fd, buf + pos, n);
        if (r <= 0) { ISFS_Close(fd); return r < 0 ? r : -1; }
        pos += (size_t)r;
    }
    ISFS_Close(fd);
    *got = pos;
    return 0;
}

static int nand_write_db(const uint8_t *buf, size_t len)
{
    s32 fd = ISFS_Open(DB_PATH, ISFS_OPEN_RW);
    if (fd < 0) return fd;
    size_t pos = 0;
    while (pos < len) {
        size_t n = len - pos > 16384 ? 16384 : len - pos;
        s32 r = ISFS_Write(fd, buf + pos, n);
        if (r != (s32)n) { ISFS_Close(fd); return r < 0 ? r : -1; }
        pos += n;
    }
    ISFS_Close(fd);
    return 0;
}

/* ------------------------------------------------------------- SD helpers */

static void ensure_dir(const char *p) { mkdir(p, 0777); }

static int mount_storage(void)
{
    if (!fatInitDefault()) return -1;
    DIR *d;
    if ((d = opendir("sd:/")) != NULL) { closedir(d); snprintf(root, sizeof root, "sd:/apps/MiiFlood"); }
    else if ((d = opendir("usb:/")) != NULL) { closedir(d); snprintf(root, sizeof root, "usb:/apps/MiiFlood"); }
    else return -1;
    ensure_dir(strncmp(root, "sd", 2) == 0 ? "sd:/apps" : "usb:/apps");
    char p[80];
    ensure_dir(root);
    snprintf(p, sizeof p, "%s/import", root);  ensure_dir(p);
    snprintf(p, sizeof p, "%s/backup", root);  ensure_dir(p);
    snprintf(p, sizeof p, "%s/archive", root); ensure_dir(p);
    return 0;
}

static int write_file(const char *path, const uint8_t *buf, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(buf, 1, len, f);
    fclose(f);
    return w == len ? 0 : -1;
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (size_t)sz > MAX_FILE) { fclose(f); return NULL; }
    uint8_t *b = (uint8_t *)malloc((size_t)sz);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)sz;
    return b;
}

static int file_exists(const char *p) { struct stat s; return stat(p, &s) == 0; }

/* newest backup index, or -1 */
static int latest_backup(void)
{
    char p[96];
    int last = -1;
    for (int i = 0; i < 999; i++) {
        snprintf(p, sizeof p, "%s/backup/RFL_DB_%03d.dat", root, i);
        if (file_exists(p)) last = i; else if (last >= 0) break;
    }
    return last;
}

static int make_backup(const uint8_t *db, size_t len)
{
    char p[96];
    int i = latest_backup() + 1;
    snprintf(p, sizeof p, "%s/backup/RFL_DB_%03d.dat", root, i);
    if (write_file(p, db, len) != 0) return -1;
    size_t l2 = 0;
    uint8_t *chk = read_file(p, &l2);
    int ok = chk && l2 == len && memcmp(chk, db, len) == 0;
    free(chk);
    if (ok) printf("Backup saved: backup/RFL_DB_%03d.dat\n", i);
    return ok ? 0 : -1;
}

/* ---------------------------------------------------------------- state */

typedef struct {
    RflDb *h;
    IdSet *seen;            /* ids already in plaza/parade/queue */
    uint8_t *recs;          /* queued 64-byte parade records */
    int nrecs;
    int allow_private;
    int rej[REJ_COUNT];
    int files;
    int stopped;
} Scan;

static void queue_mii(Scan *s, const uint8_t *m)
{
    if (s->nrecs >= MAX_NEW) { s->stopped = 1; return; }
    int r = rfl_check_mii(m, s->allow_private);
    if (r >= 0) { s->rej[r]++; return; }
    uint64_t id = rfl_id_of(m);
    int a = idset_add(s->seen, id);
    if (a == 0) { s->rej[REJ_DUP]++; return; }
    if (a < 0) { s->stopped = 1; return; }
    rfl_to_hidden(m, s->recs + (size_t)s->nrecs * RFL_HREC_SIZE, s->allow_private);
    s->nrecs++;
}

static int ext_is(const char *name, const char *ext)
{
    const char *d = strrchr(name, '.');
    return d && strcasecmp(d, ext) == 0;
}

static void scan_file(Scan *s, const char *path, const char *name, size_t size)
{
    size_t len = 0;
    if (size < RFL_MII_SIZE || size > MAX_FILE) return;
    uint8_t *b = read_file(path, &len);
    if (!b) return;
    s->files++;
    if (len == RFL_FILE_SIZE || (len == RFL_HREC_OFF && !ext_is(name, ".mii"))) {
        /* a whole RFL_DB.dat from another console */
        if (!memcmp(b, "RNOD", 4)) {
            for (int i = 0; i < RFL_OFFICIAL_MAX; i++) queue_mii(s, b + RFL_OFFICIAL_OFF + (size_t)i * RFL_MII_SIZE);
            if (len == RFL_FILE_SIZE && !memcmp(b + RFL_HDB_OFF, "RNHD", 4)) {
                RflDb other;
                /* read-only walk: tolerate CRC mismatch on foreign dumps */
                other.db = b;
                int16_t head = (int16_t)((b[RFL_HDB_OFF + 4] << 8) | b[RFL_HDB_OFF + 5]);
                int cur = head, guard = 0;
                while (cur >= 0 && cur < RFL_HIDDEN_MAX && guard++ < RFL_HIDDEN_MAX && !s->stopped) {
                    uint8_t m[RFL_MII_SIZE];
                    rfl_to_mii(rfl_hidden_rec(&other, cur), m);
                    queue_mii(s, m);
                    const uint8_t *n = b + RFL_NODES_OFF + (size_t)cur * RFL_NODE_SIZE;
                    int nx = ((n[8] << 8) | n[9]) & 0x7FFF;
                    cur = (nx == 0x7FFF) ? -1 : nx;
                }
            }
        }
    } else if (ext_is(name, ".mii") || (size >= RFL_MII_SIZE && size <= 76 && !ext_is(name, ".miis"))) {
        queue_mii(s, b);                                    /* one Mii (74 or 76 bytes) */
    } else {
        for (size_t o = 0; o + RFL_MII_SIZE <= len && !s->stopped; o += RFL_MII_SIZE)
            queue_mii(s, b + o);                            /* bundle of 74-byte Miis */
    }
    free(b);
}

static void scan_dir(Scan *s, const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && !s->stopped) {
        if (e->d_name[0] == '.') continue;
        char p[256];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 8) scan_dir(s, p, depth + 1); }
        else scan_file(s, p, e->d_name, (size_t)st.st_size);
        if ((s->files & 31) == 0) { printf("\rScanning... %d files, %d new Miis   ", s->files, s->nrecs); }
    }
    closedir(d);
}

/* ------------------------------------------------------------- archiving */

static void archive_parade(RflDb *h)
{
    char p[96];
    snprintf(p, sizeof p, "%s/archive/parade_archive.miis", root);
    IdSet arch;
    if (idset_init(&arch, IDSET_CAP) != 0) return;
    size_t len = 0;
    uint8_t *old = file_exists(p) ? read_file(p, &len) : NULL;
    for (size_t o = 0; old && o + RFL_MII_SIZE <= len; o += RFL_MII_SIZE) idset_add(&arch, rfl_id_of(old + o));
    free(old);
    FILE *f = fopen(p, "ab");
    if (!f) { idset_free(&arch); printf("Could not open archive file.\n"); return; }
    static int16_t order[RFL_HIDDEN_MAX];
    int n = rfl_hidden_order(h, order), added = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t *rec = rfl_hidden_rec(h, order[i]);
        if (idset_add(&arch, rfl_id_of(rec)) == 1) {
            uint8_t m[RFL_MII_SIZE];
            rfl_to_mii(rec, m);
            fwrite(m, 1, RFL_MII_SIZE, f);
            added++;
        }
    }
    fclose(f);
    printf("Archive: +%d Miis (archive now holds %u)\n", added, (unsigned)arch.n);
    idset_free(&arch);
}

/* ------------------------------------------------------------------ main */

static void show_rejects(const Scan *s)
{
    printf("Skipped: %d duplicate/already-have, %d private, %d invalid, %d special, %d bad parts\n",
           s->rej[REJ_DUP], s->rej[REJ_PRIVATE], s->rej[REJ_INVALID] + s->rej[REJ_ZERO],
           s->rej[REJ_SPECIAL] + s->rej[REJ_DELETE], s->rej[REJ_RANGE]);
}

static int commit(RflDb *h, const uint8_t *orig_copy, size_t len)
{
    rfl_seal(h);
    if (rfl_verify(h) != RFL_OK) { printf("Internal check failed; NAND not touched.\n"); return -1; }
    printf("Writing to NAND...\n");
    int r = nand_write_db(h->db, len);
    uint8_t *chk = (uint8_t *)memalign(32, RFL_FILE_SIZE);
    size_t got = 0;
    int ok = (r == 0) && chk && nand_read_db(chk, RFL_FILE_SIZE, &got) == 0 && got == len && memcmp(chk, h->db, len) == 0;
    free(chk);
    if (ok) { printf("Verified OK.\n"); return 0; }
    printf("Write/verify FAILED (%d). Restoring original...\n", r);
    nand_write_db(orig_copy, len);
    return -1;
}

int main(void)
{
    video_init();
    printf("\x1b[2;0H");
    printf("MiiFlood - Mii Parade bulk importer\n");
    printf("-----------------------------------\n");

    if (mount_storage() != 0) pause_exit("No SD/USB storage found. Insert an SD card (FAT32).");
    if (ISFS_Initialize() < 0) pause_exit("NAND access failed (ISFS_Initialize).");

    uint8_t *db = (uint8_t *)memalign(32, RFL_FILE_SIZE);
    uint8_t *orig = (uint8_t *)malloc(RFL_FILE_SIZE);
    if (!db || !orig) pause_exit("Out of memory.");
    size_t got = 0;
    int r = nand_read_db(db, RFL_FILE_SIZE, &got);
    if (r < 0) {
        printf("Cannot read RFL_DB.dat (error %d).\n", r);
        printf("Launch from the Homebrew Channel on an IOS with NAND access.\n");
        pause_exit("");
    }
    RflDb h;
    r = rfl_open(&h, db, got);
    if (r != RFL_OK) {
        printf("RFL_DB.dat check failed: %s\n", rfl_strerror(r));
        printf("(size read: %u bytes, expected %u)\n", (unsigned)got, (unsigned)RFL_FILE_SIZE);
        pause_exit("Nothing was changed.");
    }
    memcpy(orig, db, RFL_FILE_SIZE);

    int evict = 0, allow_private = 0;
    for (;;) {
        printf("\x1b[5;0H\x1b[J");
        printf("Parade: %d / %d Miis   (%d free)\n\n", h.count, RFL_HIDDEN_MAX, RFL_HIDDEN_MAX - h.count);
        printf("Put Mii files in  %s/import\n", root);
        printf("  .mii (single), .miis (bundle of 74-byte Miis),\n");
        printf("  or whole RFL_DB.dat files from other Wiis.\n\n");
        printf(" A  : Import everything now (backs up first)\n");
        printf(" 1  : Archive current parade to SD (no changes to Wii)\n");
        printf(" 2  : Restore newest backup\n");
        printf(" +  : When parade is full, replace oldest: %s\n", evict ? "ON " : "OFF");
        printf(" -  : Include 'private' (mingle-off) Miis: %s\n", allow_private ? "ON " : "OFF");
        printf(" HOME: Exit\n");

        u32 b = wait_buttons();
        if (b & WPAD_BUTTON_HOME) quit(0);
        if (b & WPAD_BUTTON_PLUS) { evict = !evict; continue; }
        if (b & WPAD_BUTTON_MINUS) { allow_private = !allow_private; continue; }
        if (b & WPAD_BUTTON_1) { printf("\n"); archive_parade(&h); printf("Press any button.\n"); wait_buttons(); continue; }

        if (b & WPAD_BUTTON_2) {
            int i = latest_backup();
            if (i < 0) { printf("\nNo backups found.\nPress any button.\n"); wait_buttons(); continue; }
            char p[96]; size_t bl = 0;
            snprintf(p, sizeof p, "%s/backup/RFL_DB_%03d.dat", root, i);
            uint8_t *tmp = read_file(p, &bl);
            RflDb t;
            uint8_t *al = (uint8_t *)memalign(32, RFL_FILE_SIZE);
            if (tmp && bl == RFL_FILE_SIZE && al) { memcpy(al, tmp, bl); }
            if (!tmp || !al || rfl_open(&t, al, bl) != RFL_OK) { printf("\nBackup %03d is not valid.\nPress any button.\n", i); free(tmp); free(al); wait_buttons(); continue; }
            printf("\nRestore backup %03d?  A = yes, B = no\n", i);
            u32 c = wait_buttons();
            if (c & WPAD_BUTTON_A) {
                if (commit(&t, db, RFL_FILE_SIZE) == 0) { memcpy(db, al, RFL_FILE_SIZE); memcpy(orig, db, RFL_FILE_SIZE); rfl_open(&h, db, RFL_FILE_SIZE); printf("Restored.\n"); }
                printf("Press any button.\n"); wait_buttons();
            }
            free(tmp); free(al);
            continue;
        }

        if (b & WPAD_BUTTON_A) {
            printf("\n");
            Scan s;
            memset(&s, 0, sizeof s);
            IdSet seen;
            s.h = &h; s.seen = &seen; s.allow_private = allow_private;
            s.recs = (uint8_t *)malloc((size_t)MAX_NEW * RFL_HREC_SIZE);
            if (!s.recs || idset_init(&seen, IDSET_CAP) != 0) { printf("Out of memory.\n"); wait_buttons(); free(s.recs); continue; }
            rfl_official_ids(&h, &seen);
            rfl_hidden_ids(&h, &seen);

            char p[96];
            snprintf(p, sizeof p, "%s/archive", root);   scan_dir(&s, p, 0);
            snprintf(p, sizeof p, "%s/import", root);    scan_dir(&s, p, 0);
            printf("\rScanned %d files. %d new Miis found.%s\n", s.files, s.nrecs, s.stopped ? " (limit reached)" : "");
            show_rejects(&s);

            int free_slots = RFL_HIDDEN_MAX - h.count;
            int will = evict ? (s.nrecs > RFL_HIDDEN_MAX ? RFL_HIDDEN_MAX : s.nrecs) : (s.nrecs < free_slots ? s.nrecs : free_slots);
            if (will <= 0) {
                printf("\nNothing to add (%s).\nPress any button.\n", s.nrecs ? "parade is full - press + to allow replacing oldest" : "no new Miis");
                wait_buttons();
            } else {
                printf("\nAdd %d Miis to the parade?  A = yes, B = no\n", will);
                if (wait_buttons() & WPAD_BUTTON_A) {
                    if (make_backup(orig, RFL_FILE_SIZE) != 0) {
                        printf("Could not write a verified backup to SD. Aborting.\n");
                    } else {
                        int added = 0;
                        /* with replace-oldest, the newest 'will' records win */
                        int start = evict ? s.nrecs - will : 0;
                        for (int i = start; i < s.nrecs; i++) {
                            int ar = rfl_hidden_add(&h, s.recs + (size_t)i * RFL_HREC_SIZE, evict);
                            if (ar == 1) added++; else if (ar == 0) break; else { printf("Add error.\n"); break; }
                        }
                        if (commit(&h, orig, RFL_FILE_SIZE) == 0) {
                            printf("Added %d Miis. Parade now %d.\n", added, h.count);
                            memcpy(orig, db, RFL_FILE_SIZE);
                            archive_parade(&h);
                        } else {
                            memcpy(db, orig, RFL_FILE_SIZE);
                            rfl_open(&h, db, RFL_FILE_SIZE);
                        }
                    }
                    printf("Press any button.\n");
                    wait_buttons();
                }
            }
            idset_free(&seen);
            free(s.recs);
        }
    }
    return 0;
}
