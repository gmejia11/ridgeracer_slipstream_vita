/*
 * obbfile.c -- read the OBB (TLIB pack, ~790 MB) through one shared handle.
 *
 * CFile::Open() does fopen(main.obb) + fseek + fread + fclose for every
 * asset taken from the pack, thousands of times while a race loads. Here the
 * OBB is opened once; each fopen() of it returns a lightweight virtual FILE
 * that only keeps a position plus a read-ahead buffer (up to 256 KB, never past
 * the end of the pack file being read, using the TLIB index)
 * and reads with sceIoPread, which is position-independent and thread-safe.
 *
 * Every FILE* function the game / FMOD import is wrapped: virtual handles are
 * recognized by address (they live in obb_pool), anything else goes to the
 * normal SceLibc bridge. Disable with "obbcache 0" in config.txt.
 */

#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>

#include <libc_bridge/libc_bridge.h>

#include "utils/logger.h"
#include "utils/settings.h"

#define OBB_MAX_HANDLES 64
#define OBB_SMALL 4096          // fallback buffer when the big one can't be allocated
#define OBB_READAHEAD (256 * 1024)

typedef struct {
    int used;
    int64_t pos;     // logical position
    int unget;       // pushed-back char or -1
    int64_t buf_off; // file offset of buf[0], -1 = empty
    int buf_len;
    uint8_t *big;    // OBB_READAHEAD bytes, allocated on first use, freed on fclose
    uint8_t small[OBB_SMALL];
} obb_file;

static obb_file obb_pool[OBB_MAX_HANDLES];
static SceUID obb_fd = -1;
static int64_t obb_size;
static pthread_mutex_t obb_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * TLIB index (offset, size of every file in the pack), so the read-ahead
 * never goes past the end of the file being read: reading a 2 KB json must
 * not pull 256 KB of unrelated data from the card.
 * Format: "TLIBVER1", u32 version, u32 hash, u32 ndirs, ndirs x (u32 len,
 * char[len]), u32 nfiles, nfiles x (u32 len, char[len], u32 flag, u32 off,
 * u32 size).
 */
typedef struct { uint32_t off, size; } obb_ent;
static obb_ent *obb_idx;
static int obb_nidx;

static int ent_cmp(const void *a, const void *b) {
    uint32_t x = ((const obb_ent *)a)->off, y = ((const obb_ent *)b)->off;
    return (x > y) - (x < y);
}

static void obb_load_index(void) {
    size_t cap = 1024 * 1024;
    uint8_t *d = NULL;
    int len = 0;
    for (int tries = 0; tries < 4; tries++, cap *= 2) {
        free(d);
        d = malloc(cap);
        if (!d) return;
        len = sceIoPread(obb_fd, d, cap, 0);
        if (len <= 0) { free(d); return; }
        size_t o = 0;
#define NEED(n) if (o + (n) > (size_t)len) goto more
        NEED(20);
        if (memcmp(d, "TLIBVER1", 8)) { free(d); return; }
        uint32_t ndirs; memcpy(&ndirs, d + 16, 4); o = 20;
        for (uint32_t i = 0; i < ndirs; i++) {
            uint32_t l; NEED(4); memcpy(&l, d + o, 4); o += 4; NEED(l); o += l;
        }
        uint32_t nf; NEED(4); memcpy(&nf, d + o, 4); o += 4;
        if (nf > 200000) { free(d); return; }
        obb_ent *e = malloc(sizeof(obb_ent) * (nf ? nf : 1));
        if (!e) { free(d); return; }
        int ok = 1, sorted = 1;
        for (uint32_t i = 0; i < nf; i++) {
            uint32_t l;
            if (o + 4 > (size_t)len) { ok = 0; break; }
            memcpy(&l, d + o, 4); o += 4;
            if (o + l + 12 > (size_t)len) { ok = 0; break; }
            o += l;
            memcpy(&e[i].off, d + o + 4, 4);
            memcpy(&e[i].size, d + o + 8, 4);
            o += 12;
            if (i && e[i].off < e[i - 1].off) sorted = 0;
        }
        if (!ok) { free(e); continue; }
        if (!sorted) qsort(e, nf, sizeof(obb_ent), ent_cmp);
        obb_idx = e;
        obb_nidx = (int)nf;
        free(d);
        l_info("obbfile: index of %d files loaded", obb_nidx);
        return;
#undef NEED
more:
        continue;
    }
    free(d);
}

// end of the pack file containing `pos` (or pos + OBB_SMALL if unknown)
static int64_t obb_extent_end(int64_t pos) {
    int lo = 0, hi = obb_nidx - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if ((int64_t)obb_idx[mid].off <= pos) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (best >= 0) {
        int64_t end = (int64_t)obb_idx[best].off + obb_idx[best].size;
        if (pos < end) return end;
    }
    return pos + OBB_SMALL;
}

#ifdef PERF_PROFILE
void perf_obb_io(uint64_t us, int opens);
void perf_obb_bytes(uint32_t bytes);
void perf_obb_locality(int64_t off, int len);
#define IO_BYTES(n) perf_obb_bytes(n)
#define IO_LOC(o, n) perf_obb_locality((o), (n))
#else
#define IO_BYTES(n)
#define IO_LOC(o, n)
#endif
#ifdef PERF_PROFILE
#define IO_T0() uint64_t _t0 = sceKernelGetProcessTimeWide()
#define IO_T1(o) perf_obb_io(sceKernelGetProcessTimeWide() - _t0, (o))
#else
#define IO_T0()
#define IO_T1(o)
#endif

#ifdef PERF_PROFILE
void perf_card_begin(void);
void perf_card_end(void);
void perf_card_bench(double mbs_small, double mbs_big);
#endif
static int obb_pread(void *dst, SceSize n, int64_t pos) {
#ifdef PERF_PROFILE
    perf_card_begin();
    int r = sceIoPread(obb_fd, dst, n, pos);
    perf_card_end();
    return r;
#else
    return sceIoPread(obb_fd, dst, n, pos);
#endif
}

#ifdef PERF_PROFILE
// raw card speed, single thread, before the game reads anything: 8 MB in
// 32 KB reads and 8 MB in 512 KB reads, from two different places of the OBB
static void obb_card_bench(void) {
    uint8_t *b = malloc(512 * 1024);
    if (!b || obb_size < 64LL * 1024 * 1024) { free(b); return; }
    int64_t base = obb_size / 3 & ~0xFFFFLL;
    uint64_t t0 = sceKernelGetProcessTimeWide();
    for (int i = 0; i < 256; i++) sceIoPread(obb_fd, b, 32 * 1024, base + (int64_t)i * 32 * 1024);
    uint64_t t1 = sceKernelGetProcessTimeWide();
    base = (obb_size / 3) * 2 & ~0xFFFFLL;
    for (int i = 0; i < 16; i++) sceIoPread(obb_fd, b, 512 * 1024, base + (int64_t)i * 512 * 1024);
    uint64_t t2 = sceKernelGetProcessTimeWide();
    free(b);
    perf_card_bench(8.0 / ((t1 - t0) / 1e6), 8.0 / ((t2 - t1) / 1e6));
}
#endif

static void pf_init(void);

static inline obb_file *as_obb(FILE *f) {
    uint8_t *p = (uint8_t *)f;
    if (p >= (uint8_t *)obb_pool && p < (uint8_t *)(obb_pool + OBB_MAX_HANDLES))
        return (obb_file *)f;
    return NULL;
}

int obb_is_path(const char *path) {
    return setting_obbcache && path && strcmp(path, OBB_PATH) == 0;
}

FILE *obb_fopen(void) {
    IO_T0();
    pthread_mutex_lock(&obb_lock);
    if (obb_fd < 0) {
        obb_fd = sceIoOpen(OBB_PATH, SCE_O_RDONLY, 0);
        if (obb_fd >= 0) {
            obb_size = sceIoLseek(obb_fd, 0, SCE_SEEK_END);
            l_info("obbfile: %s opened once, %lld bytes", OBB_PATH, (long long)obb_size);
            obb_load_index();
#ifdef PERF_PROFILE
            obb_card_bench();
#endif
            pf_init();
        }
    }
    obb_file *h = NULL;
    if (obb_fd >= 0) {
        for (int i = 0; i < OBB_MAX_HANDLES; i++) {
            if (!obb_pool[i].used) { h = &obb_pool[i]; break; }
        }
        if (h) {
            h->used = 1;
            h->pos = 0;
            h->unget = -1;
            h->buf_off = -1;
            h->buf_len = 0;
            h->big = NULL;
        }
    }
    pthread_mutex_unlock(&obb_lock);
    IO_T1(1);
    return (FILE *)h;   // NULL -> caller falls back to a real fopen
}

/*
 * Shared read cache (2026-11-18). Measured in a race load: 52% of the card
 * reads started exactly where the previous one ended and 14% re-read data
 * read shortly before, all in ~30 KB pieces. So every card read goes through
 * a small LRU of recently read segments (re-reads come from memory).
 * The OBB is read-only, so a cached segment never goes stale. Segments are
 * copied out under obb_cache_lock, so eviction never frees memory in use.
 *
 * Tried and removed: sequential read-ahead across file boundaries (128 ->
 * 512 KB). The SD2Vita card is bandwidth bound (boot benchmark: 10.0 MB/s with
 * 32 KB reads, 10.1 MB/s with 512 KB reads), so extra bytes only cost time.
 * Reads stay up to the end of the file being read.
 */
#define OBB_CACHE_SEGS 32
#define OBB_CACHE_BYTES (6 * 1024 * 1024)
#define OBB_BIG_READ (512 * 1024)   // reads this big go straight to the caller

typedef struct { int64_t off; int len; unsigned stamp; uint8_t *data; } obb_seg;
static obb_seg obb_cache[OBB_CACHE_SEGS];
static int obb_cache_total;
static unsigned obb_cache_clock;
static pthread_mutex_t obb_cache_lock = PTHREAD_MUTEX_INITIALIZER;

#ifdef PERF_PROFILE
void perf_obb_cache(int hit, int seq);
void perf_prefetch(int what, uint32_t bytes);
#define IO_CACHE(h, s) perf_obb_cache((h), (s))
#define PF_STAT(w, b) perf_prefetch((w), (b))
#else
#define IO_CACHE(h, s)
#define PF_STAT(w, b)
#endif
enum { PF_ST_READ, PF_ST_HIT, PF_ST_DROP, PF_ST_MATCH, PF_ST_ABORT, PF_ST_SAVE };

// copy from a cached segment covering pos (lock held); returns bytes copied
static int cache_lookup(int64_t pos, uint8_t *dst, int want) {
    for (int i = 0; i < OBB_CACHE_SEGS; i++) {
        obb_seg *s = &obb_cache[i];
        if (s->data && pos >= s->off && pos < s->off + s->len) {
            int c = (int)(s->off + s->len - pos);
            if (c > want) c = want;
            memcpy(dst, s->data + (pos - s->off), c);
            s->stamp = ++obb_cache_clock;
            return c;
        }
    }
    return 0;
}

static int cache_has(int64_t pos) {
    for (int i = 0; i < OBB_CACHE_SEGS; i++)
        if (obb_cache[i].data && pos >= obb_cache[i].off && pos < obb_cache[i].off + obb_cache[i].len)
            return 1;
    return 0;
}

// take ownership of data (lock held); frees it if it cannot be cached
static void cache_insert(int64_t off, int len, uint8_t *data) {
    if (len > OBB_CACHE_BYTES / 2) { free(data); return; }
    for (;;) {
        int free_slot = -1, lru = -1, used = 0;
        for (int i = 0; i < OBB_CACHE_SEGS; i++) {
            if (!obb_cache[i].data) { if (free_slot < 0) free_slot = i; continue; }
            used++;
            if (lru < 0 || (int)(obb_cache[i].stamp - obb_cache[lru].stamp) < 0) lru = i;
        }
        if (free_slot >= 0 && obb_cache_total + len <= OBB_CACHE_BYTES) {
            obb_seg *s = &obb_cache[free_slot];
            s->off = off; s->len = len; s->data = data; s->stamp = ++obb_cache_clock;
            obb_cache_total += len;
            return;
        }
        if (!used) { free(data); return; }
        obb_cache_total -= obb_cache[lru].len;
        free(obb_cache[lru].data);
        obb_cache[lru].data = NULL;
    }
}

/*
 * Recorded prefetch (2026-11-18). A heavy race load took ~30 s but the card
 * was busy only ~11 s of it: the game reads a file, processes it, then reads
 * the next one (car models load synchronously on the main thread), so the
 * card and the CPU rarely work at the same time.
 *
 * - A "session" is a burst of card reads (it ends after PF_IDLE_US without
 *   one). Every read that the LRU could not serve is logged as (offset, size).
 *   Sessions with at least PF_MIN_SAVE reads (race loads) are kept, up to
 *   PF_MAX_REC of them, in PF_PATH.
 * - While a session runs, its most recent reads (up to 128) are looked up in
 *   every stored recording, anywhere in it (a session may start with the
 *   garage and go on with the race, or not), at 16, 32, 64, 128 reads and
 *   then every 128. When >= 75% of them are in one recording (the longest one
 *   if several do), a background thread reads that recording ahead into
 *   memory from where the game is in it (at most PF_MAX_BYTES not yet used),
 *   skipping what this session already read.
 * - The game is served from those segments. Entries the game skipped (e.g.
 *   other cars than last time) are dropped once the game is PF_STALE entries
 *   past them; if the game keeps reading things that are not in the
 *   recording (48 in a row without using a prefetched read, or far more
 *   misses than hits), prefetching stops
 *   for that session. How far ahead it reads grows with what the game
 *   actually used (2 MB + half of it, up to PF_MAX_BYTES), so a wrong guess
 *   wastes little card time. Worst case: plain reads.
 * - Every session is recorded again, so the stored list follows the game.
 * Config: "prefetch 0" disables it (no recording, no prefetch).
 */
#ifndef PF_PATH
#define PF_PATH "ux0:data/ridgeracer/prefetch.bin"
#endif
#ifndef PF_IDLE_US
#define PF_IDLE_US 4000000
#endif
#define PF_MAX_REC 16
#define PF_MAX_ENT 6144
#define PF_MIN_SAVE 100
#define PF_MAX_BYTES (16 * 1024 * 1024)
#define PF_SEGS 1024
#define PF_STALE 96
#define PF_SEEN 16384   // power of two

typedef struct { uint32_t off, len; } pf_ent;
typedef struct { uint32_t n, stamp; pf_ent *e; } pf_rec;
typedef struct { uint32_t off, len; int idx; uint8_t *data; } pf_seg;

static pf_rec pf_recs[PF_MAX_REC];
static uint32_t pf_clock;
static int pf_loaded, pf_thread_started;

static int sess_active;
static uint64_t sess_last_us;
static pf_ent *sess_log;
static uint32_t sess_n;
static uint32_t *sess_seen;          // open addressing set of offset+1
static int sess_match = -1;          // recording being prefetched, -1 none
static int sess_best_rec = -1;       // recording that matched this session (replaced on save if similar)
static uint32_t sess_id;             // changes with every new session
static uint32_t pf_check_at;         // session read count at which to look for a match again
static int sess_aborted;
static uint32_t pf_next;             // next entry of the recording to prefetch
static int pf_used_max = -1;         // highest recording index the game used
static uint32_t pf_misses_since, pf_hits_since, pf_miss_streak;
static uint32_t pf_used_bytes;       // prefetched bytes the game used this session

static pf_seg pf_store[PF_SEGS];
static uint32_t pf_store_bytes;

static uint64_t pf_now(void) { return sceKernelGetProcessTimeWide(); }

static int seen_has(uint32_t off) {
    uint32_t k = off + 1, i = (k * 2654435761u) & (PF_SEEN - 1);
    for (int n = 0; n < PF_SEEN && sess_seen[i]; n++, i = (i + 1) & (PF_SEEN - 1))
        if (sess_seen[i] == k) return 1;
    return 0;
}
static void seen_add(uint32_t off) {
    uint32_t k = off + 1, i = (k * 2654435761u) & (PF_SEEN - 1);
    for (int n = 0; n < PF_SEEN / 2; n++, i = (i + 1) & (PF_SEEN - 1)) {
        if (sess_seen[i] == k) return;
        if (!sess_seen[i]) { sess_seen[i] = k; return; }
    }
}

static void pf_store_clear(void) {
    for (int i = 0; i < PF_SEGS; i++) {
        if (pf_store[i].data) {
            PF_STAT(PF_ST_DROP, pf_store[i].len);
            free(pf_store[i].data);
            pf_store[i].data = NULL;
        }
    }
    pf_store_bytes = 0;
}

static int pf_store_find(int64_t pos) {
    for (int i = 0; i < PF_SEGS; i++)
        if (pf_store[i].data && pos >= pf_store[i].off && pos < (int64_t)pf_store[i].off + pf_store[i].len)
            return i;
    return -1;
}

/* ---- recordings file: "RRPF", u32 version 1, u32 count, then per
 *      recording: u32 n, u32 stamp, n x (u32 off, u32 len) ---- */
static void pf_load(void) {
    pf_loaded = 1;
    SceUID fd = sceIoOpen(PF_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) return;
    uint32_t h[3];
    if (sceIoRead(fd, h, 12) == 12 && h[0] == 0x46505252 && h[1] == 1 && h[2] <= PF_MAX_REC) {
        for (uint32_t r = 0; r < h[2]; r++) {
            uint32_t rh[2];
            if (sceIoRead(fd, rh, 8) != 8 || rh[0] > PF_MAX_ENT) break;
            pf_ent *e = malloc(sizeof(pf_ent) * (rh[0] ? rh[0] : 1));
            if (!e) break;
            if (sceIoRead(fd, e, sizeof(pf_ent) * rh[0]) != (int)(sizeof(pf_ent) * rh[0])) { free(e); break; }
            int ok = 1;
            for (uint32_t i = 0; i < rh[0]; i++)
                if (!e[i].len || (int64_t)e[i].off + e[i].len > obb_size) ok = 0;   // other OBB?
            if (!ok) { free(e); continue; }
            pf_recs[r].n = rh[0]; pf_recs[r].stamp = rh[1]; pf_recs[r].e = e;
            if (rh[1] > pf_clock) pf_clock = rh[1];
        }
    }
    sceIoClose(fd);
}

static void pf_save(void) {   // called without obb_cache_lock (file I/O)
    static pf_rec snap[PF_MAX_REC];
    pthread_mutex_lock(&obb_cache_lock);
    uint32_t cnt = 0;
    for (int r = 0; r < PF_MAX_REC; r++) {
        snap[r] = pf_recs[r];
        if (pf_recs[r].e) {
            snap[r].e = malloc(sizeof(pf_ent) * pf_recs[r].n);
            if (snap[r].e) { memcpy(snap[r].e, pf_recs[r].e, sizeof(pf_ent) * pf_recs[r].n); cnt++; }
        }
    }
    pthread_mutex_unlock(&obb_cache_lock);
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s.tmp", PF_PATH);
    SceUID fd = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    int ok = fd >= 0;
    if (ok) {
        uint32_t h[3] = { 0x46505252, 1, cnt };
        ok = sceIoWrite(fd, h, 12) == 12;
        for (int r = 0; ok && r < PF_MAX_REC; r++) {
            if (!snap[r].e) continue;
            uint32_t rh[2] = { snap[r].n, snap[r].stamp };
            ok = sceIoWrite(fd, rh, 8) == 8 &&
                 sceIoWrite(fd, snap[r].e, sizeof(pf_ent) * snap[r].n) == (int)(sizeof(pf_ent) * snap[r].n);
        }
        sceIoClose(fd);
    }
    for (int r = 0; r < PF_MAX_REC; r++) free(snap[r].e);
    if (ok) {
        sceIoRemove(PF_PATH);
        ok = sceIoRename(tmp, PF_PATH) >= 0;
        if (ok) PF_STAT(PF_ST_SAVE, 0);
    }
    if (!ok) sceIoRemove(tmp);
}

// session bookkeeping for a read the LRU could not serve (lock held)
static void pf_note(uint32_t off, uint32_t len, int pf_hit) {
    if (!setting_prefetch || !sess_log || !sess_seen) return;
    sess_last_us = pf_now();
    if (!sess_active) {
        sess_active = 1;
        sess_n = 0;
        memset(sess_seen, 0, sizeof(uint32_t) * PF_SEEN);
        sess_match = sess_best_rec = -1;
        sess_aborted = 0;
        sess_id++;
        pf_check_at = 16;
        pf_next = 0;
        pf_used_max = -1;
        pf_misses_since = pf_hits_since = pf_miss_streak = 0;
        pf_used_bytes = 0;
    }
    if (sess_n < PF_MAX_ENT) sess_log[sess_n++] = (pf_ent){ off, len };
    seen_add(off);
    if (sess_match >= 0 && !pf_hit && pf_next >= pf_recs[sess_match].n && !pf_store_bytes) {
        // the recording is used up and the session goes on: find the next one
        sess_match = -1;
        pf_check_at = sess_n + 16;
    } else if (sess_match >= 0) {
        // stop only when the game clearly left the recording: no prefetched
        // read used in the last 48, or far more misses than hits. Misses mixed
        // with hits are normal (rival cars get other paint jobs every race).
        if (pf_hit) { pf_hits_since++; pf_miss_streak = 0; pf_used_bytes += len; }
        else if (++pf_misses_since > 64 + pf_hits_since * 3 ||
                 (++pf_miss_streak >= 48 && pf_store_bytes)) {   // the game left the recording
            sess_match = -1;
            sess_aborted = 1;
            pf_check_at = sess_n + 64;    // maybe another recording fits later
            pf_store_clear();
            PF_STAT(PF_ST_ABORT, 0);
        }
    }
}

/*
 * Look the session's recent reads up in every recording (pf_thread only, no
 * lock: recordings change only in pf_thread). Returns the recording and, in
 * *pos, the median index of the matched reads (where the game is now; the
 * highest one can be far ahead when a file is also read later).
 */
#define PF_HASH 16384   // > 2 * PF_MAX_ENT, power of two
static uint32_t pf_hkey[PF_HASH], pf_hidx[PF_HASH];
static int pf_find_recording(const uint32_t *offs, int k, uint32_t *pos) {
    int best = -1, bc = -1;
    uint32_t bpos = 0;
    for (int r = 0; r < PF_MAX_REC; r++) {
        pf_rec *rec = &pf_recs[r];
        if (!rec->e) continue;
        memset(pf_hkey, 0, sizeof(pf_hkey));
        for (uint32_t i = 0; i < rec->n; i++) {
            uint32_t key = rec->e[i].off + 1, h = (key * 2654435761u) & (PF_HASH - 1);
            while (pf_hkey[h] && pf_hkey[h] != key) h = (h + 1) & (PF_HASH - 1);
            pf_hkey[h] = key; pf_hidx[h] = i;   // last occurrence wins
        }
        int c = 0;
        static uint32_t found[128];
        for (int i = 0; i < k; i++) {
            uint32_t key = offs[i] + 1, h = (key * 2654435761u) & (PF_HASH - 1);
            while (pf_hkey[h] && pf_hkey[h] != key) h = (h + 1) & (PF_HASH - 1);
            if (pf_hkey[h]) found[c++] = pf_hidx[h];
        }
        for (int i = 1; i < c; i++)   // insertion sort (c <= 128) for the median
            for (int j = i; j > 0 && found[j] < found[j - 1]; j--) { uint32_t t = found[j]; found[j] = found[j - 1]; found[j - 1] = t; }
        uint32_t mx = c ? found[c / 2] : 0;
        // a better score wins; a nearly equal one wins if the recording is longer
        if (best < 0 || c > bc + k / 20 || (c >= bc - k / 20 && rec->n > pf_recs[best].n)) {
            best = r; bc = c; bpos = mx;
        }
    }
    if (best < 0 || bc * 4 < k * 3) return -1;
    *pos = bpos;
    return best;
}

// end of session (lock held): keep the log as a recording if it is a load
static int pf_end_session(void) {
    sess_active = 0;
    sess_match = -1;
    pf_store_clear();
    if (sess_n < PF_MIN_SAVE) return 0;
    // replace the matched recording only if this session is a similar load
    // (a short garage visit must not overwrite the garage + race recording)
    int slot = sess_best_rec >= 0 && (uint64_t)sess_n * 10 >= (uint64_t)pf_recs[sess_best_rec].n * 6 ?
               sess_best_rec : -1;
    if (slot < 0) {
        for (int r = 0; r < PF_MAX_REC; r++)
            if (!pf_recs[r].e) { slot = r; break; }
        if (slot < 0) {   // replace the least recently used recording
            slot = 0;
            for (int r = 1; r < PF_MAX_REC; r++)
                if ((int32_t)(pf_recs[r].stamp - pf_recs[slot].stamp) < 0) slot = r;
        }
    }
    pf_ent *e = malloc(sizeof(pf_ent) * sess_n);
    if (!e) return 0;
    memcpy(e, sess_log, sizeof(pf_ent) * sess_n);
    free(pf_recs[slot].e);
    pf_recs[slot].e = e;
    pf_recs[slot].n = sess_n;
    pf_recs[slot].stamp = ++pf_clock;
    return 1;
}

static void *pf_thread(void *arg) {
    (void)arg;
    for (;;) {
        int save = 0, work = 0;
        uint32_t off = 0, len = 0;
        int idx = -1;
        pthread_mutex_lock(&obb_cache_lock);
        if (sess_active && pf_now() - sess_last_us > PF_IDLE_US)
            save = pf_end_session();
        if (sess_active && sess_match < 0 && sess_n >= pf_check_at) {
            static uint32_t offs[128];
            int k = sess_n < 128 ? (int)sess_n : 128;
            for (int i = 0; i < k; i++) offs[i] = sess_log[sess_n - k + i].off;
            uint32_t id = sess_id;
            pf_check_at = sess_n < 128 ? sess_n * 2 : sess_n + 128;
            pthread_mutex_unlock(&obb_cache_lock);
            uint32_t pos = 0;
            int m = pf_find_recording(offs, k, &pos);
            pthread_mutex_lock(&obb_cache_lock);
            if (m >= 0 && sess_active && sess_id == id && sess_match < 0) {
                sess_match = sess_best_rec = m;
                sess_aborted = 0;
                pf_next = pos > 32 ? pos - 32 : 0;
                pf_used_max = (int)pf_next;
                pf_misses_since = pf_hits_since = pf_miss_streak = 0;
                PF_STAT(PF_ST_MATCH, m);
            }
        }
        if (sess_active && sess_match >= 0) {
            pf_rec *r = &pf_recs[sess_match];
            // drop segments the game went past without using
            for (int i = 0; i < PF_SEGS; i++)
                if (pf_store[i].data && pf_store[i].idx < pf_used_max - PF_STALE) {
                    PF_STAT(PF_ST_DROP, pf_store[i].len);
                    pf_store_bytes -= pf_store[i].len;
                    free(pf_store[i].data);
                    pf_store[i].data = NULL;
                }
            uint32_t cap = 2 * 1024 * 1024 + pf_used_bytes / 2;
            if (cap > PF_MAX_BYTES) cap = PF_MAX_BYTES;
            while (pf_next < r->n && pf_store_bytes + r->e[pf_next].len <= cap) {
                pf_ent e = r->e[pf_next];
                if (seen_has(e.off) || cache_has(e.off) || pf_store_find(e.off) >= 0 ||
                    e.len > OBB_BIG_READ) { pf_next++; continue; }
                off = e.off; len = e.len; idx = (int)pf_next++;
                work = 1;
                break;
            }
        }
        pthread_mutex_unlock(&obb_cache_lock);
        if (save) pf_save();
        if (!work) { usleep(2000); continue; }

        uint8_t *buf = malloc(len);
        int rd = buf ? obb_pread(buf, len, off) : -1;
        pthread_mutex_lock(&obb_cache_lock);
        int slot = -1;
        for (int i = 0; i < PF_SEGS && rd == (int)len; i++) if (!pf_store[i].data) { slot = i; break; }
        if (slot >= 0 && sess_match >= 0) {
            pf_store[slot] = (pf_seg){ off, len, idx, buf };
            pf_store_bytes += len;
            buf = NULL;
            PF_STAT(PF_ST_READ, len);
        }
        pthread_mutex_unlock(&obb_cache_lock);
        if (rd > 0) { IO_BYTES(rd); IO_LOC(off, rd); }
        free(buf);   // not stored (session over, store full or short read)
    }
    return NULL;
}

static void pf_init(void) {   // obb_lock held, OBB just opened
    if (!setting_prefetch || pf_thread_started) return;
    sess_log = malloc(sizeof(pf_ent) * PF_MAX_ENT);
    sess_seen = calloc(PF_SEEN, sizeof(uint32_t));
    if (!sess_log || !sess_seen) { free(sess_log); free(sess_seen); sess_log = NULL; sess_seen = NULL; return; }
    pf_load();
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 64 * 1024);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, pf_thread, NULL) == 0) pf_thread_started = 1;
    pthread_attr_destroy(&a);
}

// fill dst with up to `want` bytes at pos (want = what the handle would read
// ahead: up to the end of the pack file). Returns bytes stored, <= 0 on error.
static int obb_fetch(int64_t pos, uint8_t *dst, int want) {
    pthread_mutex_lock(&obb_cache_lock);
    int c = cache_lookup(pos, dst, want);
    if (c > 0) {
        pthread_mutex_unlock(&obb_cache_lock);
        IO_CACHE(1, 0);
        return c;
    }
    int k = pf_thread_started ? pf_store_find(pos) : -1;
    if (k >= 0) {   // prefetched: hand it over to the LRU (re-reads) and serve it
        pf_seg s = pf_store[k];
        pf_store[k].data = NULL;
        pf_store_bytes -= s.len;
        if (s.idx > pf_used_max) pf_used_max = s.idx;
        pf_note(s.off, s.len, 1);
        c = (int)(s.off + s.len - pos);
        if (c > want) c = want;
        memcpy(dst, s.data + (pos - s.off), c);
        cache_insert(s.off, s.len, s.data);
        pthread_mutex_unlock(&obb_cache_lock);
        PF_STAT(PF_ST_HIT, s.len);
        IO_CACHE(1, 0);
        return c;
    }
    pthread_mutex_unlock(&obb_cache_lock);

    if (want >= OBB_BIG_READ) {   // big read: straight into the destination, not cached
        int r = obb_pread(dst, want, pos);
        if (r <= 0) return r;
        IO_BYTES(r);
        IO_LOC(pos, r);
        IO_CACHE(0, 0);
        return r;
    }

    int64_t len = want;
    if (pos + len > obb_size) len = obb_size - pos;
    if (len < want) want = (int)len;
    if (len <= 0) return 0;
    IO_CACHE(0, 0);

    uint8_t *buf = malloc((size_t)len);
    if (!buf) {   // no memory: plain read, uncached
        int r = obb_pread(dst, want, pos);
        if (r > 0) { IO_BYTES(r); IO_LOC(pos, r); }
        return r;
    }
    int r = obb_pread(buf, (SceSize)len, pos);
    if (r <= 0) { free(buf); return r; }
    IO_BYTES(r);
    IO_LOC(pos, r);
    c = r < want ? r : want;
    memcpy(dst, buf, c);
    pthread_mutex_lock(&obb_cache_lock);
    if (pf_thread_started && pos < 0xFFFFFFFFLL) pf_note((uint32_t)pos, (uint32_t)r, 0);
    cache_insert(pos, r, buf);
    pthread_mutex_unlock(&obb_cache_lock);
    return c;
}

static size_t obb_read(obb_file *h, void *dst, size_t n) {
    uint8_t *d = dst;
    size_t got = 0;
    if (!n) return 0;
    if (h->unget >= 0) {
        *d++ = (uint8_t)h->unget;
        h->unget = -1;
        h->pos++;
        got = 1;
        n--;
    }
    while (n) {
        uint8_t *buf = h->big ? h->big : h->small;
        // serve from the handle's buffer
        if (h->buf_off >= 0 && h->pos >= h->buf_off && h->pos < h->buf_off + h->buf_len) {
            size_t avail = (size_t)(h->buf_off + h->buf_len - h->pos);
            size_t c = n < avail ? n : avail;
            memcpy(d, buf + (h->pos - h->buf_off), c);
            d += c; h->pos += c; got += c; n -= c;
            continue;
        }
        if (h->pos >= obb_size) break;

        // how much to read ahead: up to the end of the current pack file
        int64_t want = obb_idx ? obb_extent_end(h->pos) - h->pos : OBB_SMALL;
        if (want < (int64_t)n) want = n;
        if (want > OBB_READAHEAD) want = OBB_READAHEAD;
        if (want > OBB_SMALL && !h->big) {
            h->big = malloc(OBB_READAHEAD);
            h->buf_off = -1;
            if (!h->big) want = OBB_SMALL;
        }
        if (!h->big && want > OBB_SMALL) want = OBB_SMALL;

        if ((int64_t)n >= want) {     // straight into the destination
            int r = obb_fetch(h->pos, d, (int)n);
            if (r <= 0) break;
            d += r; h->pos += r; got += r; n -= r;
            continue;
        }
        buf = h->big ? h->big : h->small;
        int r = obb_fetch(h->pos, buf, (int)want);
        if (r <= 0) { h->buf_off = -1; break; }
        h->buf_off = h->pos;
        h->buf_len = r;
    }
    return got;
}

/* ---- FILE* wrappers (dynlib) ---- */

size_t fread_rr(void *ptr, size_t size, size_t count, FILE *f) {
    obb_file *h = as_obb(f);
    if (!h) return sceLibcBridge_fread(ptr, size, count, f);
    if (!size || !count) return 0;
    IO_T0();
    size_t got = obb_read(h, ptr, size * count);
    IO_T1(0);
    return got / size;
}

static int obb_seek(obb_file *h, int64_t off, int whence) {
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? h->pos : obb_size;
    int64_t np = base + off;
    if (np < 0) { errno = EINVAL; return -1; }
    h->pos = np;
    h->unget = -1;
    return 0;
}

int fseek_rr(FILE *f, long off, int whence) {
    obb_file *h = as_obb(f);
    if (!h) return sceLibcBridge_fseek(f, off, whence);
    return obb_seek(h, off, whence);
}

long ftell_rr(FILE *f) {
    obb_file *h = as_obb(f);
    if (!h) return sceLibcBridge_ftell(f);
    return (long)h->pos;
}

// FMOD imports these; the non-OBB path keeps the previous newlib mapping
int fseeko_rr(FILE *f, long off, int whence) {
    obb_file *h = as_obb(f);
    if (!h) return fseeko(f, off, whence);
    return obb_seek(h, off, whence);
}

long ftello_rr(FILE *f) {
    obb_file *h = as_obb(f);
    if (!h) return (long)ftello(f);
    return (long)h->pos;
}

int fclose_soloader(FILE *f);
int fclose_rr(FILE *f) {
    obb_file *h = as_obb(f);
    if (!h) return fclose_soloader(f);
    free(h->big);
    h->big = NULL;
    pthread_mutex_lock(&obb_lock);
    h->used = 0;
    pthread_mutex_unlock(&obb_lock);
    return 0;
}

int getc_rr(FILE *f) {
    obb_file *h = as_obb(f);
    if (!h) return sceLibcBridge_getc(f);
    uint8_t c;
    return obb_read(h, &c, 1) == 1 ? c : EOF;
}

int ungetc_rr(int c, FILE *f) {
    obb_file *h = as_obb(f);
    if (!h) return sceLibcBridge_ungetc(c, f);
    if (c == EOF || h->pos <= 0) return EOF;
    h->unget = (uint8_t)c;
    h->pos--;
    return (uint8_t)c;
}

char *fgets_rr(char *s, int n, FILE *f) {
    obb_file *h = as_obb(f);
    if (!h) return sceLibcBridge_fgets(s, n, f);
    if (n <= 0) return NULL;
    int i = 0;
    while (i < n - 1) {
        uint8_t c;
        if (obb_read(h, &c, 1) != 1) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (!i) return NULL;
    s[i] = 0;
    return s;
}

int fflush_rr(FILE *f) {
    if (f && as_obb(f)) return 0;
    return sceLibcBridge_fflush(f);
}

int setvbuf_rr(FILE *f, char *buf, int mode, size_t size) {
    if (as_obb(f)) return 0;
    return sceLibcBridge_setvbuf(f, buf, mode, size);
}

// writes to the (read-only) OBB never happen; refuse them safely
size_t fwrite_rr(const void *ptr, size_t size, size_t count, FILE *f) {
    if (as_obb(f)) { errno = EBADF; return 0; }
    return sceLibcBridge_fwrite(ptr, size, count, f);
}

int fputc_rr(int c, FILE *f) {
    if (as_obb(f)) return EOF;
    return sceLibcBridge_fputc(c, f);
}

int fputs_rr(const char *s, FILE *f) {
    if (as_obb(f)) return EOF;
    return sceLibcBridge_fputs(s, f);
}

int vfprintf_rr(FILE *f, const char *fmt, va_list ap) {
    if (as_obb(f)) return -1;
    return sceLibcBridge_vfprintf(f, fmt, ap);
}

int fprintf_rr(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf_rr(f, fmt, ap);
    va_end(ap);
    return r;
}
