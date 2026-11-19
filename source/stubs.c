/*
 * stubs.c -- Game-specific reimplementations / stubs for Ridge Racer Slipstream
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <setjmp.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

#include <so_util/so_util.h>
#include <falso_ndk/FalsoNDK.h>

#include "utils/logger.h"

extern so_module so_mod, fmodex_mod, fmodevent_mod;

/* ------------------------------------------------------------------------
 * C++ exception support for the loaded .so modules
 *
 * libgcc's ARM EHABI unwinder calls the weak __gnu_Unwind_Find_exidx() (if
 * defined) to find the exception index table for a given PC. Without this,
 * any `throw` inside libRidgeRacer.so would end up in std::terminate().
 * ---------------------------------------------------------------------- */

extern char __exidx_start[] __attribute__((weak));
extern char __exidx_end[] __attribute__((weak));

uintptr_t __gnu_Unwind_Find_exidx(uintptr_t pc, int *pcount) {
    so_module *mods[] = { &so_mod, &fmodex_mod, &fmodevent_mod };
    for (unsigned i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
        so_module *m = mods[i];
        if (m->exidx_base && pc >= m->text_base && pc < m->text_base + m->text_size) {
            *pcount = m->exidx_size / 8;
            return m->exidx_base;
        }
    }
    *pcount = (__exidx_end - __exidx_start) / 8;
    return (uintptr_t)__exidx_start;
}

/* ------------------------------------------------------------------------
 * NMALib (Namco ads library) -- not loaded, just stubbed
 * ---------------------------------------------------------------------- */

typedef struct { void *vtbl; void *delegate; char pad[56]; } NMALibrary;
static NMALibrary nma_instance_obj;
NMALibrary *_ZN10NMALibrary9_instanceE = &nma_instance_obj;

// typeinfo for NMALibraryCppDelegate (the game derives from it). A minimal
// __class_type_info: { vtable ptr, name }.
extern void *_ZTVN10__cxxabiv117__class_type_infoE;
void *_ZTI21NMALibraryCppDelegate[2] __attribute__((aligned(8))) = {
    (void *)((uintptr_t)&_ZTVN10__cxxabiv117__class_type_infoE + 8),
    (void *)"21NMALibraryCppDelegate",
};

void NMALibrary_setAppDelegate(NMALibrary *self, void *delegate) {
    if (self) self->delegate = delegate;
}

void NMALibrary_LaunchInterstitial(NMALibrary *self, const char *placement) {
}

void NMALibrary_LaunchRewardedVideo(NMALibrary *self, const char *a, const char *b, int c) {
}

/* ------------------------------------------------------------------------
 * Input filtering: FalsoNDK reports analog sticks as AINPUT_SOURCE_JOYSTICK
 * motion events, which this game would interpret as screen touches.
 * The sticks are read through the controller delegate instead (java.c).
 * ---------------------------------------------------------------------- */

int32_t AInputQueue_getEvent_rr(AInputQueue *queue, AInputEvent **outEvent) {
    int32_t r;
    while ((r = AInputQueue_getEvent(queue, outEvent)) >= 0) {
        if (AInputEvent_getType(*outEvent) == AINPUT_EVENT_TYPE_MOTION &&
            AInputEvent_getSource(*outEvent) == AINPUT_SOURCE_JOYSTICK) {
            AInputQueue_finishEvent(queue, *outEvent, 1);
            continue;
        }
        return r;
    }
    return r;
}

/* ------------------------------------------------------------------------
 * libc odds and ends
 * ---------------------------------------------------------------------- */

int ret0_rr(void) { return 0; }
int retm1_rr(void) { errno = ENOSYS; return -1; }
void *retnull_rr(void) { return NULL; }

int sigsetjmp_rr(jmp_buf env, int savemask) {
    return setjmp(env);
}

void siglongjmp_rr(jmp_buf env, int val) {
    longjmp(env, val);
}

struct iovec_rr { void *iov_base; size_t iov_len; };

extern int fndk_write(int fd, const void *buf, size_t count);

ssize_t writev_rr(int fd, const struct iovec_rr *iov, int iovcnt) {
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        int w = fndk_write(fd, iov[i].iov_base, iov[i].iov_len);
        if (w < 0) return total ? total : w;
        total += w;
        if ((size_t)w < iov[i].iov_len) break;
    }
    return total;
}

// Minimal fnmatch: supports '*' and '?', no character classes.
int fnmatch_rr(const char *pattern, const char *string, int flags) {
    const char *p = pattern, *s = string, *star = NULL, *ss = NULL;
    while (*s) {
        if (*p == '?' || *p == *s) { p++; s++; }
        else if (*p == '*') { star = p++; ss = s; }
        else if (star) { p = star + 1; s = ++ss; }
        else return 1; // FNM_NOMATCH
    }
    while (*p == '*') p++;
    return *p ? 1 : 0;
}

int chown_rr(const char *path, int owner, int group) {
    return 0;
}

/*
 * CFile::Delete safety net: never let the game delete the OBB, even if
 * its version check fails.
 */
static so_hook cfile_delete_hook;

int CFile_Delete_hook(const char *path, int flags) {
    if (path && (strstr(path, ".obb") || strstr(path, "data.pak"))) {
        l_error("Game tried to delete %s -- blocked.", path);
        return 0;
    }
    return SO_CONTINUE(int, cfile_delete_hook, path, flags);
}

void rr_patch(void) {
    // Prefer the game's own (statically linked gnustl) class_type_info vtable
    uintptr_t cti = so_symbol(&so_mod, "_ZTVN10__cxxabiv117__class_type_infoE");
    if (cti)
        _ZTI21NMALibraryCppDelegate[0] = (void *)(cti + 8);

    uintptr_t del = so_symbol(&so_mod, "_ZN5CFile6DeleteEPKci");
    if (del)
        cfile_delete_hook = hook_addr(del, (uintptr_t)&CFile_Delete_hook);
    else
        l_warn("CFile::Delete not found");
}

/* ------------------------------------------------------------------------
 * Negative lookup cache for read-only game data.
 *
 * The game probes several locations for every asset (files/<x>/PVRTC/,
 * files/<x>/ETC/, files/<x>, assets/...) and some missing sprites (e.g.
 * GUI/Sprites/Common/Menu/mainmenu_inapp_plus.png) are requested *every
 * frame*, causing ~16 failing file operations per frame. On real hardware
 * that is a big performance hit, so remember paths that do not exist.
 * Only paths of read-only data are cached (never save files).
 * ---------------------------------------------------------------------- */

#include <pthread.h>
#include <fcntl.h>

#define NEG_CACHE_SIZE 16384 // a track load probes thousands of distinct missing paths
static uint32_t neg_hash[NEG_CACHE_SIZE];
static char *neg_path[NEG_CACHE_SIZE];
static pthread_mutex_t neg_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t path_hash(const char *s) {
    uint32_t h = 5381;
    while (*s) h = ((h << 5) + h) ^ (uint8_t)*s++;
    return h ? h : 1;
}

// Any path can be cached: the whole cache is dropped on every write, create,
// rename or delete (dir_cache_clear), so a file that appears later (a save)
// is never reported missing.
static int neg_cacheable(const char *p) {
    return p && *p;
}

static int neg_lookup(const char *p) {
    if (!neg_cacheable(p)) return 0;
    uint32_t h = path_hash(p), i = h % NEG_CACHE_SIZE;
    int found = 0;
    pthread_mutex_lock(&neg_lock);
    for (int n = 0; n < 8; n++, i = (i + 1) % NEG_CACHE_SIZE) {
        if (!neg_hash[i]) break;
        if (neg_hash[i] == h && strcmp(neg_path[i], p) == 0) { found = 1; break; }
    }
    pthread_mutex_unlock(&neg_lock);
    return found;
}

static void neg_insert(const char *p) {
    if (!neg_cacheable(p)) return;
    uint32_t h = path_hash(p), i = h % NEG_CACHE_SIZE;
    pthread_mutex_lock(&neg_lock);
    for (int n = 0; n < 8; n++, i = (i + 1) % NEG_CACHE_SIZE) {
        if (!neg_hash[i]) {
            neg_path[i] = strdup(p);
            neg_hash[i] = h;
            break;
        }
        if (neg_hash[i] == h && strcmp(neg_path[i], p) == 0) break;
    }
    pthread_mutex_unlock(&neg_lock);
}

/*
 * Directory-level cache: the game probes files/<dir>/PVRTC/, files/<dir>/ETC/,
 * assets/<dir>/... for every texture of a track (~40k probes per load), but
 * almost none of those directories exist on disk (everything is in the OBB).
 * Remember which directories under DATA_PATH exist so lookups inside missing
 * directories fail instantly. Invalidated on mkdir.
 */
#include <psp2/io/stat.h>

#define DIR_CACHE_SIZE 512
static uint32_t dir_hash[DIR_CACHE_SIZE];
static char *dir_path[DIR_CACHE_SIZE];
static uint8_t dir_state[DIR_CACHE_SIZE]; // 1 = exists, 2 = missing


/*
 * Snapshot of everything that exists under DATA_PATH (files and directories).
 * Almost all game data is inside the OBB, so the loose-file probes of
 * CFile::Exists / FindFile nearly always miss; answering them from memory
 * avoids one memory-card stat per probe. Built on first use, rebuilt after
 * any write/create/rename/delete (snap_invalidate from dir_cache_clear).
 * Names are compared case-insensitively like the Vita file system.
 */
#include <psp2/io/dirent.h>
#include <ctype.h>

#define SNAP_SIZE 8192
static uint32_t snap_hash_tab[SNAP_SIZE];
static char *snap_path[SNAP_SIZE];
static uint8_t snap_flag[SNAP_SIZE];   // 1 = exists, 0 = deleted since the scan
static int snap_builds;
static int snap_state;      // 0 = not built, 1 = valid, 2 = disabled (too many files)
static int snap_count;

static uint32_t snap_hash(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (uint8_t)tolower((uint8_t)*s); h *= 16777619u; }
    return h ? h : 1;
}

// normalized key: lowercase, no repeated or trailing '/'; 0 if not representable
static int snap_norm(const char *p, char *out, size_t n) {
    size_t o = 0;
    for (const char *c = p; *c; c++) {
        if (*c == '/' && o && out[o - 1] == '/') continue;
        if (o + 1 >= n) return 0;
        out[o++] = (char)tolower((uint8_t)*c);
    }
    while (o && out[o - 1] == '/') o--;
    out[o] = 0;
    if (strstr(out, "/./") || strstr(out, "/../")) return 0;
    return 1;
}

static void snap_add(const char *key) {
    if (snap_count >= SNAP_SIZE * 3 / 4) { snap_state = 2; return; }
    uint32_t h = snap_hash(key);
    for (int n = 0, i = h % SNAP_SIZE; n < SNAP_SIZE; n++, i = (i + 1) % SNAP_SIZE) {
        if (!snap_hash_tab[i]) {
            snap_path[i] = strdup(key);
            if (!snap_path[i]) { snap_state = 2; return; }
            snap_hash_tab[i] = h;
            snap_flag[i] = 1;
            snap_count++;
            return;
        }
        if (snap_hash_tab[i] == h && strcmp(snap_path[i], key) == 0) { snap_flag[i] = 1; return; }
    }
}

static void snap_scan(const char *dir, int depth) {
    if (depth > 16 || snap_state == 2) return;
    SceUID d = sceIoDopen(dir);
    if (d < 0) return;
    SceIoDirent e;
    char path[512], key[512];
    while (snap_state != 2 && sceIoDread(d, &e) > 0) {
        if (!strcmp(e.d_name, ".") || !strcmp(e.d_name, "..")) continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e.d_name);
        if (snap_norm(path, key, sizeof(key))) snap_add(key);
        if (SCE_S_ISDIR(e.d_stat.st_mode)) snap_scan(path, depth + 1);
    }
    sceIoDclose(d);
}

static void snap_invalidate_locked(void) {
    for (int i = 0; i < SNAP_SIZE; i++) {
        free(snap_path[i]);
        snap_path[i] = NULL;
        snap_hash_tab[i] = 0;
    }
    snap_count = 0;
    snap_state = 0;
}

// 1 = exists, 0 = does not exist, -1 = unknown (not under DATA_PATH / disabled)
static int snap_query(const char *p) {
    static const char root[] = DATA_PATH;   // "ux0:data/ridgeracer/"
    if (!p || strncasecmp(p, root, sizeof(root) - 1) != 0) return -1;
    char key[512];
    if (!snap_norm(p, key, sizeof(key))) return -1;
    int r = -1;
    pthread_mutex_lock(&neg_lock);
    if (snap_state == 0) {
        char top[sizeof(root)];
        memcpy(top, root, sizeof(root));
        top[sizeof(root) - 2] = 0;            // strip the trailing '/'
        snap_state = 1;
        snap_builds++;
        snap_scan(top, 0);
        if (snap_state == 2) snap_invalidate_locked(), snap_state = 2;
    }
    if (snap_state == 1) {
        r = 0;
        uint32_t h = snap_hash(key);
        for (int n = 0, i = h % SNAP_SIZE; n < SNAP_SIZE; n++, i = (i + 1) % SNAP_SIZE) {
            if (!snap_hash_tab[i]) break;
            if (snap_hash_tab[i] == h && strcmp(snap_path[i], key) == 0) { r = snap_flag[i]; break; }
        }
        // the data root itself always exists
        char rootkey[sizeof(root)];
        if (snap_norm(root, rootkey, sizeof(rootkey)) && !strcmp(rootkey, key)) r = 1;
    }
    pthread_mutex_unlock(&neg_lock);
    return r;
}

// Keep the snapshot in sync after a successful file-system change (no rescan).
// A path that is created also makes its parent directories exist.
static void snap_mark(const char *p, int exists) {
    static const char root[] = DATA_PATH;
    if (!p || strncasecmp(p, root, sizeof(root) - 1) != 0) return;
    char key[512];
    pthread_mutex_lock(&neg_lock);
    if (snap_state == 1) {
        if (!snap_norm(p, key, sizeof(key))) {
            snap_invalidate_locked();               // unusual path: rescan next time
        } else if (exists) {
            snap_add(key);
            for (char *sl = strrchr(key, '/'); sl && sl > key + sizeof(root) - 2; sl = strrchr(key, '/')) {
                *sl = 0;
                snap_add(key);
            }
            if (snap_state == 2) snap_invalidate_locked(), snap_state = 2;
        } else {
            uint32_t h = snap_hash(key);
            for (int n = 0, i = h % SNAP_SIZE; n < SNAP_SIZE; n++, i = (i + 1) % SNAP_SIZE) {
                if (!snap_hash_tab[i]) break;
                if (snap_hash_tab[i] == h && strcmp(snap_path[i], key) == 0) { snap_flag[i] = 0; break; }
            }
        }
    }
    pthread_mutex_unlock(&neg_lock);
}

static void snap_invalidate(void) {
    pthread_mutex_lock(&neg_lock);
    if (snap_state == 1) snap_invalidate_locked();
    pthread_mutex_unlock(&neg_lock);
}

void stubs_snap_stats(int *builds, int *count, int *state) {
    *builds = snap_builds; *count = snap_count; *state = snap_state;
}

// Drops both caches (directory states and missing files); called before
// anything that can create, rename or delete files.
static void dir_cache_clear(void) {
    pthread_mutex_lock(&neg_lock);
    for (int i = 0; i < DIR_CACHE_SIZE; i++) {
        if (dir_path[i]) free(dir_path[i]);
        dir_path[i] = NULL;
        dir_hash[i] = 0;
        dir_state[i] = 0;
    }
    for (int i = 0; i < NEG_CACHE_SIZE; i++) {
        if (neg_path[i]) free(neg_path[i]);
        neg_path[i] = NULL;
        neg_hash[i] = 0;
    }
    pthread_mutex_unlock(&neg_lock);
}

// Returns 1 if the parent directory of `p` (under DATA_PATH) does not exist.
static int parent_dir_missing(const char *p) {
    if (!p) return 0;
    const char *base = strstr(p, "data/ridgeracer/");
    if (!base) return 0;
    const char *slash = strrchr(p, '/');
    if (!slash || slash <= base + 15) return 0; // DATA_PATH itself always exists

    char dir[512];
    size_t len = slash - p;
    if (len >= sizeof(dir)) return 0;
    memcpy(dir, p, len);
    dir[len] = 0;

    uint32_t h = path_hash(dir), i = h % DIR_CACHE_SIZE;
    int state = 0, slot = -1;
    pthread_mutex_lock(&neg_lock);
    for (int n = 0; n < 8; n++, i = (i + 1) % DIR_CACHE_SIZE) {
        if (!dir_hash[i]) { slot = i; break; }
        if (dir_hash[i] == h && strcmp(dir_path[i], dir) == 0) { state = dir_state[i]; break; }
    }
    pthread_mutex_unlock(&neg_lock);
    if (state) return state == 2;

    SceIoStat st;
    int exists = sceIoGetstat(dir, &st) >= 0;
    if (slot >= 0) {
        pthread_mutex_lock(&neg_lock);
        if (!dir_hash[slot]) {
            dir_path[slot] = strdup(dir);
            dir_state[slot] = exists ? 1 : 2;
            dir_hash[slot] = h;
        }
        pthread_mutex_unlock(&neg_lock);
    }
    return !exists;
}

void perf_fileop(void);
static int lookup_missing(const char *p) {
    perf_fileop();
    int st = snap_query(p);
    if (st >= 0) return !st;
    return neg_lookup(p) || parent_dir_missing(p);
}

extern FILE *fopen_soloader(const char *filename, const char *mode);
extern int open_soloader(const char *path, int oflag, ...);
extern int stat_soloader(const char *path, void *buf);

int obb_is_path(const char *path);
FILE *obb_fopen(void);

FILE *fopen_rr(const char *filename, const char *mode) {
    int reading = mode && !strpbrk(mode, "wa+");
    if (reading && obb_is_path(filename)) {   // shared OBB handle (obbfile.c)
        FILE *f = obb_fopen();
        if (f) return f;
    }
    if (reading && lookup_missing(filename)) { errno = ENOENT; return NULL; }
    if (!reading) dir_cache_clear(); // a write may create things we cached as missing
    FILE *f = fopen_soloader(filename, mode);
    if (!f && reading) neg_insert(filename);
    if (f && !reading) snap_mark(filename, 1);
    return f;
}

int open_rr(const char *path, int oflag, int mode) {
    int reading = (oflag & 3) == 0; // O_RDONLY
    if (reading && lookup_missing(path)) { errno = ENOENT; return -1; }
    if (!reading) dir_cache_clear();
    int r = open_soloader(path, oflag, mode);
    if (r < 0 && reading) neg_insert(path);
    if (r >= 0 && !reading) snap_mark(path, 1);
    return r;
}

int stat_rr(const char *path, void *buf) {
    if (lookup_missing(path)) { errno = ENOENT; return -1; }
    int r = stat_soloader(path, buf);
    if (r < 0) neg_insert(path);
    return r;
}

int access_rr(const char *path, int amode) {
    if (lookup_missing(path)) { errno = ENOENT; return -1; }
    int r = access(path, amode);
    if (r < 0 && errno == ENOENT) neg_insert(path);
    return r;
}

// file-system changes: drop the lookup caches first
int rename_rr(const char *a, const char *b) {
    dir_cache_clear();
    int r = rename(a, b);
    if (r == 0) snap_invalidate();   // may move a whole directory: rescan on next lookup
    return r;
}
int remove_rr(const char *p) { dir_cache_clear(); int r = remove(p); if (r == 0) snap_mark(p, 0); return r; }
int unlink_rr(const char *p) { dir_cache_clear(); int r = unlink(p); if (r == 0) snap_mark(p, 0); return r; }
int rmdir_rr(const char *p) { dir_cache_clear(); int r = rmdir(p); if (r == 0) snap_mark(p, 0); return r; }

int mkdir_rr(const char *path, int mode) {
    dir_cache_clear();
    int r = mkdir(path, mode);
    if (r == 0) snap_mark(path, 1);
    return r;
}

AAsset *AAssetManager_open_rr(AAssetManager *mgr, const char *filename, int mode) {
    char full[512];
    snprintf(full, sizeof(full), DATA_PATH "assets/%s", filename ? filename : "");
    if (neg_lookup(filename)) return NULL;
    int st = snap_query(full);   // in-memory listing of DATA_PATH (no card access)
    if (st == 0 || (st < 0 && parent_dir_missing(full))) return NULL;
    AAsset *a = AAssetManager_open(mgr, filename, mode);
    if (!a) neg_insert(filename);
    return a;
}

/* FalsoNDK logs every AAsset_read/seek/close; keep only errors in release. */
#ifndef DEBUG_SOLOADER
int sceClibPrintf(const char *fmt, ...);
void fndk_log(int severity, const char *message) {
    if (severity >= 2)
        sceClibPrintf("E/FalsoNDK: %s\n", message);
}
#endif
