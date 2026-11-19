/*
 * perf.c -- lightweight frame profiler (CMake option PERF_PROFILE)
 *
 * - Game functions are timed with so_util hooks.
 * - Every GL function the game imports is wrapped (in the dynlib table, before
 *   libRidgeRacer's imports are resolved) and timed individually.
 * - A summary is appended every PERF_WINDOW frames to
 *   ux0:data/ridgeracer/perf.txt, in ms per frame averaged over the window.
 *
 * Wrappers are signature-agnostic: they forward r0-r3 plus eight stack words
 * and return r0:r1. The game is softfp, so floats travel as raw words. Reading
 * a few extra words of the caller's stack is harmless.
 */

#ifdef PERF_PROFILE

#include <pthread.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include <so_util/so_util.h>
#include <vitaGL.h>
#include <kubridge.h>

extern so_module so_mod, fmodex_mod, fmodevent_mod;

#define PERF_WINDOW 120
#define MAX_SLOTS 192

typedef uint64_t (*fn8)(uint32_t, uint32_t, uint32_t, uint32_t,
                        uint32_t, uint32_t, uint32_t, uint32_t,
                        uint32_t, uint32_t, uint32_t, uint32_t);

static const char *slot_name[MAX_SLOTS];
static volatile uint64_t slot_time[MAX_SLOTS];
static volatile uint32_t slot_calls[MAX_SLOTS];
static int nslots;

static inline uint64_t now_us(void) { return sceKernelGetProcessTimeWide(); }

static int new_slot(const char *name) {
    if (nslots >= MAX_SLOTS) return -1;
    slot_name[nslots] = name;
    return nslots++;
}

static int S_FRAME, S_SWAP, S_AUDIO, S_FILEOPS, S_OBBIO, S_ZLIB;

void perf_fileop(void) { slot_calls[S_FILEOPS]++; }
// obbfile.c: time spent opening/reading the OBB, calls = virtual opens
static uint32_t deflate_ok, deflate_bad;
static uint64_t obb_bytes; static uint32_t obb_reads;
void perf_obb_bytes(uint32_t b) { obb_bytes += b; obb_reads++; }
/* where each card read starts relative to the end of the previous one (any
 * thread): tells whether merging neighbouring files into bigger reads pays */
static int64_t loc_last_end = -1;
static unsigned cache_hit, cache_miss, cache_seq;
/* wall-clock time with at least one card read in flight (the OBB I/O slot
 * sums the waits of all threads, so overlapping reads are counted twice) */
static pthread_mutex_t card_lock = PTHREAD_MUTEX_INITIALIZER;
static int card_inflight;
static uint64_t card_since, card_busy_us;
static double bench_small, bench_big;
void perf_card_begin(void) {
    pthread_mutex_lock(&card_lock);
    if (card_inflight++ == 0) card_since = sceKernelGetProcessTimeWide();
    pthread_mutex_unlock(&card_lock);
}
void perf_card_end(void) {
    pthread_mutex_lock(&card_lock);
    if (--card_inflight == 0) card_busy_us += sceKernelGetProcessTimeWide() - card_since;
    pthread_mutex_unlock(&card_lock);
}
void perf_card_bench(double s, double b) { bench_small = s; bench_big = b; }
static uint64_t pf_b_read, pf_b_hit, pf_b_drop;
static unsigned pf_n_hit, pf_matches, pf_aborts, pf_saves; static int pf_last_rec = -1;
void perf_prefetch(int what, uint32_t b) {
    switch (what) {
    case 0: pf_b_read += b; break;
    case 1: pf_b_hit += b; pf_n_hit++; break;
    case 2: pf_b_drop += b; break;
    case 3: pf_matches++; pf_last_rec = (int)b; break;
    case 4: pf_aborts++; break;
    case 5: pf_saves++; break;
    }
}
void perf_obb_cache(int hit, int seq) { if (hit) cache_hit++; else { cache_miss++; if (seq) cache_seq++; } }
static unsigned loc_cont, loc_ahead64, loc_ahead1m, loc_behind, loc_far;
/* card bytes in 4 KB blocks that had already been read from the card before:
 * how much a bigger cache could still save (OBB < 1 GB -> 32 KB bitmap) */
static uint8_t obb_seen[(1u << 30) / 4096 / 8];
static uint64_t loc_reread;
void perf_obb_locality(int64_t off, int len) {
    for (int64_t b = (off + 4095) / 4096; (b + 1) * 4096 <= off + len && b < (int64_t)sizeof(obb_seen) * 8; b++) {
        if (obb_seen[b >> 3] & (1 << (b & 7))) loc_reread += 4096;
        else obb_seen[b >> 3] |= 1 << (b & 7);
    }
    int64_t d = off - loc_last_end;
    if (loc_last_end < 0) loc_far++;
    else if (d == 0) loc_cont++;
    else if (d > 0 && d <= 65536) loc_ahead64++;
    else if (d > 0 && d <= 1048576) loc_ahead1m++;
    else if (d < 0 && d >= -1048576) loc_behind++;
    else loc_far++;
    loc_last_end = off + len;
}
void perf_deflate_check(int ok) { if (ok) deflate_ok++; else deflate_bad++; }
void perf_zlib(uint64_t us) { slot_time[S_ZLIB] += us; slot_calls[S_ZLIB]++; }
void perf_obb_io(uint64_t us, int opens) { slot_time[S_OBBIO] += us; if (opens) slot_calls[S_OBBIO]++; }
void perf_audio_mix(uint64_t us) { slot_time[S_AUDIO] += us; slot_calls[S_AUDIO]++; }

/* REP8(M, b) expands M(b, 0) .. M(b, 7); the slot index is b * 8 + k */
#define REP8(M, b)  M(b, 0) M(b, 1) M(b, 2) M(b, 3) M(b, 4) M(b, 5) M(b, 6) M(b, 7)
/* 12 words: glTexImage2D alone takes 9 arguments */
#define ARGS8 uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g, uint32_t h, uint32_t i, uint32_t j, uint32_t k, uint32_t l

/* ---------------- GL wrappers ---------------- */
#define MAX_GL 96
static fn8 gl_target[MAX_GL];
static int gl_slot[MAX_GL];

#define GLW(B, K) static uint64_t glw_##B##_##K(ARGS8) { \
    const int n = B * 8 + K; \
    uint64_t t = now_us(); \
    uint64_t r = gl_target[n](a, b, c, d, e, f, g, h, i, j, k, l); \
    slot_time[gl_slot[n]] += now_us() - t; slot_calls[gl_slot[n]]++; \
    return r; }
REP8(GLW, 0) REP8(GLW, 1) REP8(GLW, 2) REP8(GLW, 3) REP8(GLW, 4) REP8(GLW, 5)
REP8(GLW, 6) REP8(GLW, 7) REP8(GLW, 8) REP8(GLW, 9) REP8(GLW, 10) REP8(GLW, 11)
#undef GLW
#define GLW(B, K) (uintptr_t)glw_##B##_##K,
static const uintptr_t gl_wrap[MAX_GL] = {
    REP8(GLW, 0) REP8(GLW, 1) REP8(GLW, 2) REP8(GLW, 3) REP8(GLW, 4) REP8(GLW, 5)
    REP8(GLW, 6) REP8(GLW, 7) REP8(GLW, 8) REP8(GLW, 9) REP8(GLW, 10) REP8(GLW, 11)
};

static EGLBoolean perf_eglSwapBuffers(EGLDisplay d, EGLSurface s);
EGLBoolean eglSwapBuffers_soloader(EGLDisplay d, EGLSurface s);

/* GL functions libRidgeRacer.so imports (nm -D --undefined-only) */
static const char *game_gl[] = {
    "glActiveTexture",
    "glAttachShader",
    "glBindAttribLocation",
    "glBindBuffer",
    "glBindFramebuffer",
    "glBindRenderbuffer",
    "glBindTexture",
    "glBlendFunc",
    "glBufferData",
    "glBufferSubData",
    "glCheckFramebufferStatus",
    "glClear",
    "glClearColor",
    "glClearDepthf",
    "glClearStencil",
    "glColorMask",
    "glCompileShader",
    "glCompressedTexImage2D",
    "glCreateProgram",
    "glCreateShader",
    "glCullFace",
    "glDeleteBuffers",
    "glDeleteFramebuffers",
    "glDeleteProgram",
    "glDeleteRenderbuffers",
    "glDeleteShader",
    "glDeleteTextures",
    "glDepthFunc",
    "glDepthMask",
    "glDisable",
    "glDisableVertexAttribArray",
    "glDrawArrays",
    "glDrawElements",
    "glEnable",
    "glEnableVertexAttribArray",
    "glFramebufferRenderbuffer",
    "glFramebufferTexture2D",
    "glGenBuffers",
    "glGenFramebuffers",
    "glGenRenderbuffers",
    "glGenTextures",
    "glGetActiveUniform",
    "glGetAttribLocation",
    "glGetFloatv",
    "glGetIntegerv",
    "glGetProgramInfoLog",
    "glGetProgramiv",
    "glGetShaderInfoLog",
    "glGetShaderiv",
    "glGetString",
    "glGetUniformLocation",
    "glIsEnabled",
    "glLinkProgram",
    "glPixelStorei",
    "glReadPixels",
    "glRenderbufferStorage",
    "glScissor",
    "glShaderSource",
    "glTexImage2D",
    "glTexParameterf",
    "glTexParameteri",
    "glUniform1f",
    "glUniform1fv",
    "glUniform1i",
    "glUniform1iv",
    "glUniform2f",
    "glUniform2fv",
    "glUniform2iv",
    "glUniform3f",
    "glUniform3fv",
    "glUniform3iv",
    "glUniform4f",
    "glUniform4fv",
    "glUniform4iv",
    "glUniformMatrix2fv",
    "glUniformMatrix3fv",
    "glUniformMatrix4fv",
    "glUseProgram",
    "glValidateProgram",
    "glVertexAttribPointer",
    "glViewport",
};

static int is_game_gl(const char *s) {
    for (unsigned i = 0; i < sizeof(game_gl) / sizeof(game_gl[0]); i++)
        if (strcmp(s, game_gl[i]) == 0) return 1;
    return 0;
}

/* Called on the dynlib table right before libRidgeRacer's imports are resolved. */
void perf_wrap_dynlib(so_default_dynlib *tbl, int count) {
    S_FRAME = new_slot("frame");
    S_SWAP = new_slot("swap (gpu wait)");
    S_AUDIO = new_slot("audio mix (thread)");
    S_FILEOPS = new_slot("file lookups");
    S_OBBIO = new_slot("OBB I/O (opens)");
    S_ZLIB = new_slot("zlib decompress");

    int ngl = 0;
    for (int i = 0; i < count; i++) {
        const char *s = tbl[i].symbol;
        if (!s) continue;
        if (strcmp(s, "eglSwapBuffers") == 0) {
            tbl[i].func = (uintptr_t)&perf_eglSwapBuffers;
            continue;
        }
        if (s[0] != 'g' || s[1] != 'l' || !is_game_gl(s)) continue;
        if (ngl >= MAX_GL) continue;
        int sl = new_slot(s);
        if (sl < 0) continue;
        gl_target[ngl] = (fn8)tbl[i].func;
        gl_slot[ngl] = sl;
        tbl[i].func = gl_wrap[ngl];
        ngl++;
    }
}

/* ---------------- game hooks ---------------- */
#define MAX_HOOKS 32
static so_hook ghook[MAX_HOOKS];
static int ghook_slot[MAX_HOOKS];

#define GH(B, K) static uint64_t gh_##B##_##K(ARGS8) { \
    const int n = B * 8 + K; \
    uint64_t t = now_us(); \
    uint64_t r = SO_CONTINUE(uint64_t, ghook[n], a, b, c, d, e, f, g, h, i, j, k, l); \
    slot_time[ghook_slot[n]] += now_us() - t; slot_calls[ghook_slot[n]]++; \
    return r; }
REP8(GH, 0) REP8(GH, 1) REP8(GH, 2) REP8(GH, 3)
#undef GH
#define GH(B, K) (uintptr_t)gh_##B##_##K,
static const uintptr_t gh_fn[MAX_HOOKS] = { REP8(GH, 0) REP8(GH, 1) REP8(GH, 2) REP8(GH, 3) };

/*
 * Timed trampolines (much cheaper than SO_CONTINUE, which flushes caches twice
 * per call). The function's first two (position-independent) prologue
 * instructions are displaced into an entry trampoline that records the entry
 * time and swaps LR for a shared exit stub; the exit stub records the time
 * and returns to the real caller. A small per-thread shadow stack keeps the
 * original return addresses (handles recursion and other threads).
 */
static int safe_prologue(uint32_t a, uint32_t b);

#define SHADOW_DEPTH 256
#define SHADOW_THREADS 8
typedef struct { uint32_t idx, lr; uint64_t t0; } shadow_ent;
static struct { SceUID thid; int depth; shadow_ent e[SHADOW_DEPTH]; } shadow[SHADOW_THREADS];

static inline __attribute__((always_inline)) int shadow_slot(void) {
    SceUID me = sceKernelGetThreadId();
    for (int i = 0; i < SHADOW_THREADS; i++) {
        if (shadow[i].thid == me) return i;
        if (shadow[i].thid == 0) { shadow[i].thid = me; return i; }
    }
    return -1;
}

static int tramp_slot[128];
static int tramp_active[128]; // recursion depth per hook: only the outermost call is timed

// Called from the entry trampoline: r0 = hook index, r1 = original LR.
void perf_enter(uint32_t idx, uint32_t lr) {
    int t = shadow_slot();
    if (t < 0) return; // never happens with < 8 threads
    int d = shadow[t].depth++;
    if (d < SHADOW_DEPTH) {
        shadow[t].e[d].idx = idx;
        shadow[t].e[d].lr = lr;
        shadow[t].e[d].t0 = now_us();
    }
    tramp_active[idx]++;
}

// Called from the exit stub: returns the original LR.
uint32_t perf_exit(void) {
    uint64_t now = now_us();
    int t = shadow_slot();
    int d = --shadow[t].depth;
    shadow_ent *e = &shadow[t].e[d];
    if (--tramp_active[e->idx] == 0)
        slot_time[tramp_slot[e->idx]] += now - e->t0;
    slot_calls[tramp_slot[e->idx]]++;
    return e->lr;
}

static uint32_t *tramp_mem, *exit_stub;
static int ntramp;

static int tramp_init(void) {
    if (tramp_mem) return 1;
    SceKernelAllocMemBlockKernelOpt opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    SceUID blk = kuKernelAllocMemBlock("perf_timed", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 0x10000, &opt);
    if (blk < 0) return 0;
    sceKernelGetMemBlockBase(blk, (void **)&tramp_mem);
    kuKernelMemProtect(tramp_mem, 0x10000, KU_KERNEL_PROT_EXEC | KU_KERNEL_PROT_WRITE | KU_KERNEL_PROT_READ);
    exit_stub = tramp_mem;
    exit_stub[0] = 0xE92D000F;   // push {r0-r3}
    exit_stub[1] = 0xE59FC00C;   // ldr r12, [pc, #12]  -> [6]
    exit_stub[2] = 0xE12FFF3C;   // blx r12
    exit_stub[3] = 0xE1A0C000;   // mov r12, r0
    exit_stub[4] = 0xE8BD000F;   // pop {r0-r3}
    exit_stub[5] = 0xE12FFF1C;   // bx r12
    exit_stub[6] = (uint32_t)&perf_exit;
    return 1;
}

static int timed_hook(uintptr_t a, int slot) {
    if (!tramp_init() || ntramp >= 128 || (a & 3)) return 0;
    uint32_t *f = (uint32_t *)a;
    if (!safe_prologue(f[0], f[1])) return 0;
    uint32_t *t = tramp_mem + 8 + ntramp * 14;
    t[0] = 0xE92D500F;   // push {r0-r3, r12, lr}
    t[1] = 0xE59F0020;   // ldr r0, [pc, #32]   -> t[11] idx
    t[2] = 0xE1A0100E;   // mov r1, lr
    t[3] = 0xE59FC01C;   // ldr r12, [pc, #28]  -> t[12] perf_enter
    t[4] = 0xE12FFF3C;   // blx r12
    t[5] = 0xE8BD500F;   // pop {r0-r3, r12, lr}
    t[6] = 0xE59FE014;   // ldr lr, [pc, #20]   -> t[13] exit stub
    t[7] = f[0];         // displaced prologue
    t[8] = f[1];
    t[9] = 0xE51FF004;   // ldr pc, [pc, #-4]
    t[10] = a + 8;
    t[11] = ntramp;
    t[12] = (uint32_t)&perf_enter;
    t[13] = (uint32_t)exit_stub;
    tramp_slot[ntramp] = slot;
    f[0] = 0xE51FF004;   // ldr pc, [pc, #-4]
    f[1] = (uint32_t)t;
    kuKernelFlushCaches(t, 14 * 4);
    kuKernelFlushCaches(f, 8);
    ntramp++;
    return 1;
}

static int nhooks;
static void add_hook(so_module *m, const char *sym, const char *label) {
    uintptr_t a = so_symbol(m, sym);
    if (!a) return;
    int sl = new_slot(label);
    if (sl < 0) return;
    if (timed_hook(a, sl)) { nhooks++; return; }
    if (nhooks >= MAX_HOOKS) return;   // fallback: SO_CONTINUE hook (thumb code etc.)
    ghook_slot[nhooks] = sl;
    ghook[nhooks] = hook_addr(a, gh_fn[nhooks]);
    nhooks++;
}

static void perf_count_install(so_module *m);

/*
 * Menu navigation log (ux0:data/ridgeracer/menu.txt): every
 * MenuController::DoTransition with the current slot before/after, the
 * number of links the slot has in that direction (0 left, 1 right, 2 up,
 * 3 down, 4 accept, 5-8 right stick) and the "basic configuration" flag.
 */
static SceUID menu_log = -1;
static uint32_t menu_frame;

// called by controls_rr.c for every menu transition
void perf_menu_log(void *mc, int t_in, int t_used, void *before, void *after, int links) {
    if (menu_log < 0) return;
    char b[176];
    static const char *dn[] = { "left", "right", "up", "down", "accept", "rs-left", "rs-right", "rs-up", "rs-down" };
    int n = snprintf(b, sizeof(b), "frame %u: %s, slot %p -> %p%s | links of the slot: left %d right %d up %d down %d\n",
                     menu_frame, (t_in >= 0 && t_in <= 8) ? dn[t_in] : "?", before, after,
                     before == after ? " (did not move)" : "",
                     links & 15, (links >> 4) & 15, (links >> 8) & 15, (links >> 12) & 15);
    (void)t_used;
    sceIoWrite(menu_log, b, n);
}

void perf_menu_note(const char *s) { if (menu_log >= 0) sceIoWrite(menu_log, s, strlen(s)); }

static void menu_log_install(so_module *m) {
    (void)m;
    menu_log = sceIoOpen("ux0:data/ridgeracer/menu.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
}

void perf_patch(void) {
    // Main loop: CTitanEngine::FrameStep and everything it calls
    add_hook(&so_mod, "_ZN12CTitanEngine9FrameStepEf", "[FrameStep total]");
    add_hook(&so_mod, "_ZN12SoundManager6UpdateEf", " SoundManager::Update");
    add_hook(&so_mod, "_ZN11IGameObject13PreUpdateNodeEf", " PreUpdateNode");
    add_hook(&so_mod, "_ZN11IGameObject10UpdateNodeEf", " UpdateNode");
    add_hook(&so_mod, "_ZN11IGameObject14PostUpdateNodeEf", " PostUpdateNode");
    add_hook(&so_mod, "_ZN11IGameObject15Post2UpdateNodeEf", " Post2UpdateNode");
    add_hook(&so_mod, "_ZN11IUpdateNode11UpdateNodesEf", " UpdateNodes");
    add_hook(&so_mod, "_ZN8CFrame2D12UpdateFramesEf", " Frame2D::UpdateFrames");
    add_hook(&so_mod, "_ZN14CRenderManager6RenderEv", " RenderManager::Render");
    // Inner pieces (inclusive times, nested inside the above)
    add_hook(&so_mod, "_ZN12CRegularRace3RunEf", "  RegularRace::Run");
    add_hook(&so_mod, "_ZN12CRegularRace6UpdateEf", "  RegularRace::Update");
    add_hook(&so_mod, "_ZN11CTrackScene3RunEf", "  TrackScene::Run");
    add_hook(&so_mod, "_ZN23btDiscreteDynamicsWorld14stepSimulationEfif", "  bullet step");
    add_hook(&so_mod, "_ZN4CCar6UpdateEf", "  CCar::Update");
    add_hook(&so_mod, "_ZN12CGameVehicle6UpdateEf", "  CGameVehicle::Update");
    add_hook(&so_mod, "_ZN6CTrack6UpdateEf", "  CTrack::Update");
    add_hook(&so_mod, "_ZN11CTrackScene9PreUpdateEf", "  TrackScene::PreUpdate");
    add_hook(&so_mod, "_ZN11CTrackScene10PostUpdateEf", "  TrackScene::PostUpdate");
    add_hook(&so_mod, "_ZN11CTrackScene13UpdatePhysicsEf", "   TrackScene::UpdatePhysics");
    add_hook(&so_mod, "_ZN4CCar9PreUpdateEf", "  CCar::PreUpdate");
    add_hook(&so_mod, "_ZN4CCar10PostUpdateEf", "  CCar::PostUpdate");
    add_hook(&so_mod, "_ZN4CCar27UpdateSplineInfoAndMatricesEf", "   CCar::UpdateSplineInfo");
    add_hook(&so_mod, "_ZN4CCar26pupdateWheelsAndSuspensionEf", "   CCar::WheelsAndSuspension");
    add_hook(&so_mod, "_ZN4CCar25pupdateBodyStateInPhysicsEf", "   CCar::BodyStateInPhysics");
    add_hook(&so_mod, "_ZN4CCar16pupdateCollisionEf", "   CCar::Collision");
    add_hook(&so_mod, "_ZN4CCar14pupdateCamerasEf", "   CCar::Cameras");
    add_hook(&so_mod, "_ZN7CDriver9PreUpdateEf", "  CDriver::PreUpdate");
    add_hook(&so_mod, "_ZN7CArtInt9PreUpdateEf", "  CArtInt::PreUpdate");
    add_hook(&so_mod, "_ZN7CArtInt10PostUpdateEf", "  CArtInt::PostUpdate");
    add_hook(&so_mod, "_ZN7CArtInt29_prepareSlipStreamAndOvertakeEf", "   CArtInt::SlipStreamAndOvertake");
    add_hook(&so_mod, "_ZN7CArtInt10_letsDriveEf", "   CArtInt::_letsDrive");
    add_hook(&so_mod, "_ZN7CArtInt18UpdateOvertakePlanEf", "   CArtInt::UpdateOvertakePlan");
    // loading
    add_hook(&so_mod, "_ZN13CMenuControll9LoadSceneE9eGameModeR9RaceSetup", " [load] MenuControll::LoadScene");
    add_hook(&so_mod, "_ZN11CTrackScene4InitEP9RaceSetup", " [load] TrackScene::Init");
    // inside TrackScene::Init (2026-11-18: ~5 s per heavy load, one 8 s frame)
    add_hook(&so_mod, "_ZN12CRenderScene14LoadFromMemoryERN4pugi12xml_documentEPKc", "  [init] RenderScene::LoadFromMemory");
    add_hook(&so_mod, "_ZN12CRenderScene14LoadShadowMeshEPKc", "   [init] RenderScene::LoadShadowMesh");
    add_hook(&so_mod, "_ZN12CRenderScene20CalculateBoundingBoxEv", "   [init] RenderScene::CalcBoundingBox");
    add_hook(&so_mod, "_ZN6CImage19CreateImageFromFileEPKc", "   [init] CImage::CreateImageFromFile");
    add_hook(&so_mod, "_ZN15CTextureManager21CreateCubeMapFromFileEPKcb", "   [init] TextureManager::CreateCubeMap");
    add_hook(&so_mod, "_Z15OpenXMLFromFileRN4pugi12xml_documentEPKc", "  [init] OpenXMLFromFile");
    add_hook(&so_mod, "_ZN4CCarC1EP11CTrackSceneRK6GameIDiP7CDriverP16ITriggerCallback", "  [init] CCar::CCar");
    add_hook(&so_mod, "_ZN4CCar4initEP16ITriggerCallbacki", "   [init] CCar::init");
    add_hook(&so_mod, "_ZN12CGameVehicleC1EP12CRenderSceneP13iBulletSolverP16ITriggerCallback", "    [init] CGameVehicle::CGameVehicle");
    add_hook(&so_mod, "_ZN15CCarDefinitions3GetERK6GameID", "   [init] CarDefinitions::Get");
    add_hook(&so_mod, "_ZN7CArtIntC1EP11IGameObject", "  [init] CArtInt::CArtInt");
    add_hook(&so_mod, "_ZN7CSplineC1EPKc", "  [init] CSpline::CSpline(file)");
    add_hook(&so_mod, "_Z18GetTokenFromStringPKciRf", "   [init] GetTokenFromString(float)");
    add_hook(&so_mod, "_ZN13CMatrixSpline18_constructFromSPL2Eb", "  [init] MatrixSpline::_constructFromSPL2");
    add_hook(&so_mod, "_ZN13CMatrixSpline7_reinitEi", "  [init] MatrixSpline::_reinit");
    add_hook(&so_mod, "_ZN12SoundManager9AddReverbEPKcfffff", "  [init] SoundManager::AddReverb");
    // inside CCar::CCar (~0.5 s per car): the car's model / paint / tyres load synchronously
    add_hook(&so_mod, "_ZN4CCar20pcreateHandlingOfCarEv", "    [init] CCar::pcreateHandlingOfCar");
    add_hook(&so_mod, "_ZN4CCar14pcreateCamerasEv", "    [init] CCar::pcreateCameras");
    add_hook(&so_mod, "_ZN4CCar6iNitroEb", "    [init] CCar::iNitro");
    add_hook(&so_mod, "_ZN12CGameVehicle11SetPaintJobEi", "    [init] GameVehicle::SetPaintJob");
    add_hook(&so_mod, "_ZN12CGameVehicle8SetTiresEi", "    [init] GameVehicle::SetTires");
    add_hook(&so_mod, "_ZN12CGameVehicle12SetTuningSetEi", "    [init] GameVehicle::SetTuningSet");
    add_hook(&so_mod, "_ZN14CRenderManager18CreateMeshFromFileEPKcjP11CRenderMesh", "     [init] RenderManager::CreateMeshFromFile (sync)");
    add_hook(&so_mod, "_ZN14CCarDefinition21ppreCreatePhysicalCarEv", "    [init] CarDefinition::ppreCreatePhysicalCar");
    add_hook(&so_mod, "_ZN14CCarDefinition19pcollectTuningUnitsEv", "    [init] CarDefinition::pcollectTuningUnits");
    add_hook(&so_mod, "_ZN10CCarTuning27pcalculateAccelerationCurveEv", "    [init] CarTuning::pcalculateAccelerationCurve");
    add_hook(&so_mod, "_ZN10CCarTuning22pupdatePerformanceDataEv", "    [init] CarTuning::pupdatePerformanceData");
    // mesh loading pieces not yet measured
    add_hook(&so_mod, "_ZN15CRenderMaterialC1EPhjb", "      [load] RenderMaterial ctor (parse)");
    add_hook(&so_mod, "_ZN11CRenderMesh10AddSectionEP15CRenderMaterialP15CRenderGeometry", "      [load] RenderMesh::AddSection");
    add_hook(&so_mod, "_ZN15CTextureManager26CreateTextureFromFileAsyncEPKc", "      [load] TextureManager::CreateAsync (queue)");
    add_hook(&so_mod, "_ZN12CRenderScene12LoadFromFileEPKc", "  [load] RenderScene::LoadFromFile");
    add_hook(&so_mod, "_ZN11CRenderMesh12LoadFromFileEPKcj", "  [load] RenderMesh::LoadFromFile");
    add_hook(&so_mod, "_ZN11CRenderMesh14LoadFromMemoryER8sScxDataj", "   [load] RenderMesh::LoadFromMemory");
    add_hook(&so_mod, "_ZN15CTextureManager17LoadAsyncTexturesEv", "  [load] TextureManager::LoadAsync");
    add_hook(&so_mod, "_ZN14CRenderManager15LoadAsyncMeshesEv", "  [load] RenderManager::LoadAsyncMeshes");
    add_hook(&so_mod, "_ZN13CRenderShader4LoadEPKc", "  [load] RenderShader::Load");
    add_hook(&so_mod, "_ZN10ConfigNode18ReadFromJSONStringEPKc", "  [load] ConfigNode::ReadFromJSON");
    add_hook(&so_mod, "_ZN12SoundManager13LoadSoundBankEPKc", "  [load] SoundManager::LoadSoundBank");
    add_hook(&so_mod, "_ZN12SoundManager17LoadSoundFromFileEPKcS1_bb", "   [load] SoundManager::LoadSoundFromFile");
    add_hook(&so_mod, "_ZN12SoundManager17LoadEventFromFileEPKcS1_S1_", "   [load] SoundManager::LoadEventFromFile");
    add_hook(&fmodex_mod, "_ZN4FMOD6System11createSoundEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE", "    [load] FMOD System::createSound");
    add_hook(&fmodevent_mod, "_ZN4FMOD11EventSystem4loadEPKcP19FMOD_EVENT_LOADINFOPPNS_12EventProjectE", "    [load] FMOD EventSystem::load");
    add_hook(&so_mod, "_ZN5CFile4OpenEi", "  [load] CFile::Open");
    add_hook(&so_mod, "_ZN5CFile4ReadEPvj", "  [load] CFile::Read");
    add_hook(&so_mod, "_ZN12CPackManager11GetFileInfoEPKc", "   [load] PackManager::GetFileInfo");
    add_hook(&so_mod, "_ZN5CFile6ExistsEPKci", "   [load] CFile::Exists");
    add_hook(&so_mod, "_ZN15CRenderGeometry15LoadVertexChunkEPcjj", "    [load] Geometry::LoadVertexChunk");
    add_hook(&so_mod, "_ZN15CRenderGeometry14LoadVertexDataEjjjPvj", "     [load] Geometry::LoadVertexData");
    add_hook(&so_mod, "_ZN15CRenderGeometry19LoadVertexAnimChunkEPcjj", "    [load] Geometry::LoadVertexAnimChunk");
    add_hook(&so_mod, "_ZN15CRenderGeometry26CalculateVertexAnimNormalsEj", "    [load] Geometry::CalcVertexAnimNormals");
    add_hook(&so_mod, "_ZN15CRenderGeometry13LoadIndexDataEjPvj", "    [load] Geometry::LoadIndexData");
    add_hook(&so_mod, "_ZN15CRenderGeometry9CreateVAOEP15CRenderMaterial", "    [load] Geometry::CreateVAO");
    add_hook(&so_mod, "_ZN15CTextureManager21CreateTextureFromFileEPKcbi", "  [load] TextureManager::CreateFromFile");
    add_hook(&so_mod, "_ZN15CTextureManager13CreateTextureEPhiiiPKc", "   [load] TextureManager::CreateTexture");
    add_hook(&so_mod, "_ZN15CTextureManager8FindFileEPKci", "   [load] TextureManager::FindFile");
    add_hook(&so_mod, "_ZN9CFileDataC1EPKcS1_i", "   [load] CFileData (read whole file)");
    add_hook(&so_mod, "_ZN15CTextureManager13unpackPVRDataEPhj", "   [load] tex unpackPVRData");
    add_hook(&so_mod, "_ZN15CTextureManager14unpackPVRData2EPhj", "   [load] tex unpackPVRData2");
    add_hook(&so_mod, "png_read_image", "   [load] tex PNG decode");
    add_hook(&so_mod, "jpeg_read_scanlines", "   [load] tex JPEG decode");
    add_hook(&so_mod, "_ZN8CTexture6UpdateEiPhj", "   [load] tex CTexture::Update (GL upload)");
    add_hook(&so_mod, "_ZN11CTrackScene6UpdateEf", "  CTrackScene::Update");
    add_hook(&so_mod, "_ZN11CController6UpdateEf", "  Controller::Update");
    add_hook(&so_mod, "_ZN13CRenderCamera6RenderEv", "  RenderCamera::Render");
    add_hook(&so_mod, "_ZN13CRenderCamera15UpdateForRenderEf", "   RenderCamera::UpdateForRender");
    add_hook(&so_mod, "_ZN13CRenderCamera12RenderQueuesEv", "   RenderCamera::RenderQueues");
    add_hook(&so_mod, "_ZN13CRenderCamera18RenderShadowQueuesEv", "   RenderCamera::RenderShadowQueues");
    add_hook(&so_mod, "_ZN15CRenderNodeMesh7CollectEv", "    NodeMesh::Collect");
    add_hook(&so_mod, "_ZN15CRenderNodeMesh6RenderEjP17CRenderShaderPass", "    NodeMesh::Render");
    add_hook(&so_mod, "_ZN17CRenderShaderPass5BeginEP15CRenderMaterial", "    ShaderPass::Begin");
    add_hook(&so_mod, "_ZN17CRenderShaderPass3EndEP15CRenderMaterial", "    ShaderPass::End");
    add_hook(&so_mod, "_ZN15CRenderGeometry5BeginEi", "    Geometry::Begin");
    add_hook(&so_mod, "_ZN15CRenderGeometry4DrawEi", "    Geometry::Draw");
    add_hook(&so_mod, "_ZN15CRenderGeometry3EndEv", "    Geometry::End");
    add_hook(&so_mod, "_ZSt16__introsort_loopIN9__gnu_cxx17__normal_iteratorIP16sRenderQueueItemSt6vectorIS2_SaIS2_EEEEi19sRenderQueueCompareEvT_S9_T0_T1_", "    sort queue (introsort)");
    add_hook(&so_mod, "_ZSt22__final_insertion_sortIN9__gnu_cxx17__normal_iteratorIP16sRenderQueueItemSt6vectorIS2_SaIS2_EEEE19sRenderQueueCompareEvT_S9_T0_", "    sort queue (insertion)");
    add_hook(&so_mod, "_ZN12CRenderScene25GetLightingFactorFromMeshERK8CVector3IfE", "    Scene::LightingFromMesh");
    add_hook(&so_mod, "_ZN12CRenderScene24GetLightingFactorFromMapERK8CVector3IfE", "    Scene::LightingFromMap");
    add_hook(&so_mod, "_ZN11CRenderNode6UpdateEv", "    RenderNode::Update");
    add_hook(&so_mod, "_ZN19CRenderNodeParticle6RenderEjP17CRenderShaderPass", "    NodeParticle::Render");
    add_hook(&so_mod, "_ZN17CRenderNodeSprite6RenderEjP17CRenderShaderPass", "    NodeSprite::Render");
    add_hook(&so_mod, "_ZN15CRenderNodeText6RenderEjP17CRenderShaderPass", "    NodeText::Render");
    add_hook(&so_mod, "_ZN20CRenderNodeLensflare6RenderEjP17CRenderShaderPass", "    NodeLensflare::Render");
    add_hook(&so_mod, "_ZN8CFrame2D18TransformAndRenderEv", "    Frame2D::TransformAndRender");
    add_hook(&so_mod, "_ZN4CCar21GetNearestSplinePointERK8CVector3IfEP18CSplineNearestInfofPS1_PK13CMatrixSpline", "   CCar::GetNearestSplinePoint");
    add_hook(&so_mod, "_ZN9CSkidmark6UpdateEf", "   Skidmark::Update");
    add_hook(&so_mod, "_ZN15CSmoothedBuffer6updateEff", "   SmoothedBuffer::update");
    add_hook(&so_mod, "_ZN7MeshLOD6SetLodEf", "   MeshLOD::SetLod");
    add_hook(&so_mod, "_ZN19CRenderNodeParticle6UpdateEf", "  Particle::Update");
    add_hook(&fmodevent_mod, "_ZN4FMOD11EventSystem6updateEv", "  FMOD EventSystem::update");

    // The per-function call counter adds overhead to every call and skews the
    // timings: only enable it when ux0:data/ridgeracer/perf_count exists.
    menu_log_install(&so_mod);

    SceIoStat st;
    if (sceIoGetstat("ux0:data/ridgeracer/perf_count", &st) >= 0)
        perf_count_install(&so_mod); // after timed hooks: they skip already-hooked functions

    SceUID f = sceIoOpen("ux0:data/ridgeracer/perf.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (f >= 0) {
        char hdr[160];
        int n = snprintf(hdr, sizeof(hdr), "perf v2, window %d frames, ms/frame (calls/frame), %d hooks\n",
                         PERF_WINDOW, nhooks);
        sceIoWrite(f, hdr, n);
        sceIoClose(f);
    }
}


/* ---------------- call counter for every game function ----------------
 * libRidgeRacer.so is built at -O0, so nearly every ARM function starts with
 * two position-independent prologue instructions (push/str + add/mov/vpush).
 * We redirect those 8 bytes to a per-function trampoline that bumps a counter,
 * runs the two displaced instructions and jumps back to func+8.
 */
#include <kubridge.h>

#define TRAMP_WORDS 11
typedef struct { uint32_t addr, size; const char *name; } cfunc;
static cfunc *cf;
static uint32_t *cf_count;
static int ncf;

static int safe_prologue(uint32_t a, uint32_t b) {
    // already redirected by fastpath (ldr pc, [pc, #-4]; .word dst): moving
    // both words into the trampoline keeps working, it just jumps to dst
    if (a == 0xE51FF004) return 1;
    int ok_a = ((a & 0xFFFF0000) == 0xE92D0000 && !(a & 0x8000)) ||   // push {..} without pc
               ((a & 0xFFFF0FFF) == 0xE52D0004 && ((a >> 12) & 0xF) != 15); // str rX, [sp, #-4]!
    int ok_b = ((b & 0xFFFF0000) == 0xE28D0000 && ((b >> 12) & 0xF) != 15) || // add rX, sp, #imm
               ((b & 0xFFFF0FF0) == 0xE1A00000 && ((b >> 12) & 0xF) != 15 && (b & 0xF) != 15) || // mov rX, rY
               ((b & 0xFFBF0F00) == 0xED2D0B00) ||                            // vpush {dN..}
               ((b & 0xFFF00000) == 0xE2800000 && ((b >> 12) & 0xF) != 15 && ((b >> 16) & 0xF) != 15); // add rX, rY, #imm
    return ok_a && ok_b;
}

static int cf_cmp(const void *x, const void *y) {
    const cfunc *a = x, *b = y;
    return (a->addr > b->addr) - (a->addr < b->addr);
}

static void perf_count_install(so_module *m) {
    int n = 0;
    cf = malloc(sizeof(cfunc) * m->num_dynsym);
    for (uint32_t i = 0; i < m->num_dynsym; i++) {
        Elf32_Sym *sym = &m->dynsym[i];
        if (ELF32_ST_TYPE(sym->st_info) != STT_FUNC || sym->st_shndx == 0) continue;
        if (sym->st_value & 3) continue;           // thumb or odd
        if (sym->st_size < 12) continue;
        uint32_t addr = m->load_addr + sym->st_value;
        if (addr < m->text_base || addr + 8 > m->text_base + m->text_size) continue;
        cf[n].addr = addr; cf[n].size = sym->st_size; cf[n].name = m->dynstr + sym->st_name;
        n++;
    }
    qsort(cf, n, sizeof(cfunc), cf_cmp);
    // dedupe aliases (C1/C2 constructors etc.)
    int w = 0;
    for (int i = 0; i < n; i++)
        if (w == 0 || cf[i].addr != cf[w - 1].addr) cf[w++] = cf[i];
    n = w;

    size_t tsize = (n * TRAMP_WORDS * 4 + 0xFFFF) & ~0xFFFF;
    SceKernelAllocMemBlockKernelOpt opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    SceUID blk = kuKernelAllocMemBlock("perf_tramp", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, tsize, &opt);
    if (blk < 0) return;
    uint32_t *tr;
    sceKernelGetMemBlockBase(blk, (void **)&tr);
    kuKernelMemProtect(tr, tsize, KU_KERNEL_PROT_EXEC | KU_KERNEL_PROT_WRITE | KU_KERNEL_PROT_READ);
    cf_count = calloc(n, sizeof(uint32_t));

    int k = 0;
    for (int i = 0; i < n; i++) {
        uint32_t *f = (uint32_t *)cf[i].addr;
        if (!safe_prologue(f[0], f[1])) continue;
        uint32_t *t = tr + k * TRAMP_WORDS;
        t[0] = 0xE92D0003;           // push {r0, r1}
        t[1] = 0xE59F001C;           // ldr r0, [pc, #28]  -> t[10]
        t[2] = 0xE5901000;           // ldr r1, [r0]
        t[3] = 0xE2811001;           // add r1, r1, #1
        t[4] = 0xE5801000;           // str r1, [r0]
        t[5] = 0xE8BD0003;           // pop {r0, r1}
        t[6] = f[0];                 // displaced prologue
        t[7] = f[1];
        t[8] = 0xE51FF004;           // ldr pc, [pc, #-4]
        t[9] = (uint32_t)(f + 2);
        t[10] = (uint32_t)&cf_count[k];
        cf[k] = cf[i];               // compact: index k now owns this function
        f[0] = 0xE51FF004;           // ldr pc, [pc, #-4]
        f[1] = (uint32_t)t;
        k++;
    }
    ncf = k;
    kuKernelFlushCaches(tr, tsize);
    kuKernelFlushCaches((void *)m->text_base, m->text_size);
}

static void perf_count_dump(char *buf, int *pn, int cap, double fr) {
    enum { TOP = 40 };
    int top_calls[TOP], top_cost[TOP];
    for (int j = 0; j < TOP; j++) top_calls[j] = top_cost[j] = -1;
    uint64_t total = 0;
    for (int i = 0; i < ncf; i++) {
        uint32_t c = cf_count[i];
        if (!c) continue;
        total += c;
        uint64_t cost = (uint64_t)c * cf[i].size;
        for (int j = 0; j < TOP; j++) {
            if (top_calls[j] < 0 || c > cf_count[top_calls[j]]) {
                memmove(&top_calls[j + 1], &top_calls[j], (TOP - 1 - j) * sizeof(int));
                top_calls[j] = i; break;
            }
        }
        for (int j = 0; j < TOP; j++) {
            if (top_cost[j] < 0 || cost > (uint64_t)cf_count[top_cost[j]] * cf[top_cost[j]].size) {
                memmove(&top_cost[j + 1], &top_cost[j], (TOP - 1 - j) * sizeof(int));
                top_cost[j] = i; break;
            }
        }
    }
    int n = *pn;
    n += snprintf(buf + n, cap - n, "  -- %d funcs counted, %.0f calls/frame total\n  -- top by calls/frame (calls, bytes, name)\n",
                  ncf, (double)total / fr);
    for (int j = 0; j < TOP && top_calls[j] >= 0 && n < cap - 200; j++) {
        int i = top_calls[j];
        n += snprintf(buf + n, cap - n, "  C %9.0f %5u %s\n", cf_count[i] / fr, (unsigned)cf[i].size, cf[i].name);
    }
    n += snprintf(buf + n, cap - n, "  -- top by calls*bytes\n");
    for (int j = 0; j < TOP && top_cost[j] >= 0 && n < cap - 200; j++) {
        int i = top_cost[j];
        n += snprintf(buf + n, cap - n, "  W %9.0f %5u %s\n", cf_count[i] / fr, (unsigned)cf[i].size, cf[i].name);
    }
    *pn = n;
    memset(cf_count, 0, ncf * sizeof(uint32_t));
}

/* ---------------- output ---------------- */
static uint64_t last_swap;
static int frames;
static uint32_t frame_max_us, frames_over40, frames_over60;

static void perf_dump(void) {
    static char buf[32768];
    int n = 0;
    double fr = frames;
    n += snprintf(buf + n, sizeof(buf) - n, "=== fps %.1f | frame %.2f ms | file lookups/frame %.1f | max %.1f ms, >40ms %u, >60ms %u\n",
                  slot_time[S_FRAME] ? fr * 1000000.0 / (double)slot_time[S_FRAME] : 0.0,
                  (double)slot_time[S_FRAME] / 1000.0 / fr, (double)slot_calls[S_FILEOPS] / fr,
                  frame_max_us / 1000.0, frames_over40, frames_over60);
    {
        void stubs_snap_stats(int *builds, int *count, int *state);
        int sb, sc, ss;
        stubs_snap_stats(&sb, &sc, &ss);
        n += snprintf(buf + n, sizeof(buf) - n, "  file snapshot: built %d times, %d entries, state %d\n", sb, sc, ss);
    }
    if (obb_reads) {
        double sec = (double)slot_time[S_OBBIO] / 1000000.0;
        n += snprintf(buf + n, sizeof(buf) - n, "  OBB read: %.1f MB in %u reads, %.1f MB/s while reading\n",
                      obb_bytes / 1048576.0, obb_reads, sec > 0 ? obb_bytes / 1048576.0 / sec : 0.0);
        pthread_mutex_lock(&card_lock);
        uint64_t busy = card_busy_us; card_busy_us = 0;
        pthread_mutex_unlock(&card_lock);
        n += snprintf(buf + n, sizeof(buf) - n, "  card busy (wall clock): %.2f s = %.1f MB/s real | card benchmark at boot: %.1f MB/s (32 KB reads), %.1f MB/s (512 KB reads)\n",
                      busy / 1e6, busy ? obb_bytes / 1048576.0 / (busy / 1e6) : 0.0, bench_small, bench_big);
        obb_bytes = 0; obb_reads = 0;
        n += snprintf(buf + n, sizeof(buf) - n, "  OBB read start vs previous end: same %u, +64KB %u, +1MB %u, back<1MB %u, far %u\n",
                      loc_cont, loc_ahead64, loc_ahead1m, loc_behind, loc_far);
        loc_cont = loc_ahead64 = loc_ahead1m = loc_behind = loc_far = 0;
        n += snprintf(buf + n, sizeof(buf) - n, "  OBB re-read from card (already read before): %.1f MB\n", loc_reread / 1048576.0);
        loc_reread = 0;
    }
    if (pf_b_read || pf_n_hit || pf_matches || pf_aborts || pf_saves) {
        n += snprintf(buf + n, sizeof(buf) - n, "  prefetch: read ahead %.1f MB, used %.1f MB (%u reads served), dropped %.1f MB | matched %u (recording %d), aborted %u, recordings saved %u\n",
                      pf_b_read / 1048576.0, pf_b_hit / 1048576.0, pf_n_hit, pf_b_drop / 1048576.0, pf_matches, pf_last_rec, pf_aborts, pf_saves);
        pf_b_read = pf_b_hit = pf_b_drop = 0; pf_n_hit = pf_matches = pf_aborts = pf_saves = 0;
    }
    if (cache_hit || cache_miss) {
        n += snprintf(buf + n, sizeof(buf) - n, "  OBB cache: %u hits, %u card reads (%u continue a previous read)\n",
                      cache_hit, cache_miss, cache_seq);
        cache_hit = cache_miss = cache_seq = 0;
    }
    if (deflate_ok || deflate_bad)
        n += snprintf(buf + n, sizeof(buf) - n, "  libdeflate check: %u chunks identical to zlib, %u DIFFERENT\n",
                      deflate_ok, deflate_bad);
    frame_max_us = frames_over40 = frames_over60 = 0;
    for (int i = 0; i < nslots && n < (int)sizeof(buf) - 128; i++) {
        if (i == S_FRAME || i == S_FILEOPS) continue;
        double ms = (double)slot_time[i] / 1000.0 / fr;
        // Skip GL functions that cost nothing; always print game hooks
        if (slot_name[i][0] == 'g' && ms < 0.05) continue;
        n += snprintf(buf + n, sizeof(buf) - n, "  %-34s %7.2f  (%.1f)\n", slot_name[i], ms,
                      (double)slot_calls[i] / fr);
    }
    if (ncf) perf_count_dump(buf, &n, sizeof(buf), fr);
    SceUID f = sceIoOpen("ux0:data/ridgeracer/perf.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (f >= 0) {
        sceIoWrite(f, buf, n);
        sceIoClose(f);
    }
    for (int i = 0; i < nslots; i++) { slot_time[i] = 0; slot_calls[i] = 0; }
    frames = 0;
}

static EGLBoolean perf_eglSwapBuffers(EGLDisplay d, EGLSurface s) {
    uint64_t t = now_us();
    EGLBoolean r = eglSwapBuffers_soloader(d, s);
    uint64_t e = now_us();
    slot_time[S_SWAP] += e - t;
    slot_calls[S_SWAP]++;
    menu_frame++;
    if (last_swap) {
        uint32_t ft = (uint32_t)(e - last_swap);
        if (ft > frame_max_us) frame_max_us = ft;
        if (ft > 40000) frames_over40++;
        if (ft > 60000) frames_over60++;
        slot_time[S_FRAME] += e - last_swap;
        if (++frames >= PERF_WINDOW) perf_dump();
    }
    last_swap = now_us();
    return r;
}

#else

void perf_patch(void) {}
void perf_fileop(void) {}

#endif
