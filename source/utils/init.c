/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021-2022 Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/init.h"

#include "utils/dialog.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/utils.h"
#include "utils/settings.h"

#include <reimpl/controls.h>

#include <string.h>

#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/kernel/clib.h>
#include <psp2/power.h>
#include <psp2/kernel/sysmem.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>
#include <fios/fios.h>

// Module layout, relative to a base address found at runtime (each module
// has a 64KB patch arena right below its load address):
//   base+0x0000000 .. +0x011F000  libfmodex.so      (load at +0x10000)
//   base+0x0200000 .. +0x026B000  libfmodevent.so   (load at +0x210000)
//   base+0x0400000 .. +0x1D91000  libRidgeRacer.so  (load at +0x410000)
#define MODULES_SPAN       0x1E00000
#define OFF_FMODEX         0x0010000
#define OFF_FMODEVENT      0x0210000
#define OFF_RIDGERACER     0x0410000
#define FALLBACK_BASE      0x97BF0000

// Ask the kernel for a free range big enough for all modules plus a margin,
// then release it so so_util can allocate its blocks there at fixed
// addresses. The first FILE_BUF_MARGIN bytes are left for so_util's temporary
// "so block" (the raw .so file buffer, ~10.4MB for libRidgeRacer.so), which
// the allocator places at the lowest free address, i.e. right at the start
// of this range. Fixed addresses like 0x98000000 are not usable on Vita3K.
#define FILE_BUF_MARGIN 0x1000000

static uintptr_t find_modules_base(void) {
    SceUID blk = sceKernelAllocMemBlock("so_probe", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                        FILE_BUF_MARGIN + MODULES_SPAN, NULL);
    if (blk < 0) {
        l_warn("Base probe failed (0x%08X), using fallback 0x%08X", blk, FALLBACK_BASE);
        return FALLBACK_BASE;
    }
    void *base = NULL;
    sceKernelGetMemBlockBase(blk, &base);
    sceKernelFreeMemBlock(blk);
    uintptr_t b = (uintptr_t)base + FILE_BUF_MARGIN;
    l_info("Free range at %p, modules base: 0x%08X", base, b);
    return b;
}

extern so_module so_mod;
so_module fmodex_mod, fmodevent_mod;

void resolve_imports_generic(so_module *mod);
void fmod_patch(void);

static void load_dep(so_module *mod, const char *path, uintptr_t addr) {
    if (!file_exists(path))
        fatal_error("Missing file: %s", path);
    int r = so_file_load(mod, path, addr);
    if (r < 0)
        fatal_error("Error: could not load %s (0x%08X) at 0x%08X.", path, r, addr);
}

void soloader_init_all() {
	// Launch `app0:configurator.bin` on `-config` init param
    sceAppUtilInit(&(SceAppUtilInitParam){}, &(SceAppUtilBootParam){});
    SceAppUtilAppEventParam eventParam;
    sceClibMemset(&eventParam, 0, sizeof(SceAppUtilAppEventParam));
    sceAppUtilReceiveAppEvent(&eventParam);
    if (eventParam.type == 0x05) {
        char buffer[2048];
        sceAppUtilAppEventParseLiveArea(&eventParam, buffer);
        if (strstr(buffer, "-config"))
            sceAppMgrLoadExec("app0:/configurator.bin", NULL, NULL);
    }

    // Set default overclock values
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

#ifdef USE_SCELIBC_IO
    if (fios_init(DATA_PATH) == 0)
        l_success("FIOS initialized.");
#endif

    if (!module_loaded("kubridge")) {
        l_fatal("kubridge is not loaded.");
        fatal_error("Error: kubridge.skprx is not installed.");
    }
    l_success("kubridge check passed.");

    if (!file_exists(SO_PATH)) {
        fatal_error("Looks like you haven't installed the data files for this "
                    "port, or they are in an incorrect location. Please make "
                    "sure that you have %s file exactly at that path.", SO_PATH);
    }

    if (!file_exists(OBB_PATH)) {
        fatal_error("Missing OBB. Copy main.2319.com.namcobandaigames.ridgeracerss.obb "
                    "to %s", OBB_PATH);
    }

    // Dependencies first (FMOD Ex -> FMOD Event), then the game itself.
    uintptr_t base = find_modules_base();

    // Main game library first (largest one).
    load_dep(&so_mod, SO_PATH, base + OFF_RIDGERACER);
    l_success("libRidgeRacer.so loaded.");

    load_dep(&fmodex_mod, FMODEX_PATH, base + OFF_FMODEX);
    load_dep(&fmodevent_mod, FMODEVENT_PATH, base + OFF_FMODEVENT);
    l_success("FMOD libraries loaded.");

    settings_load();
    l_success("Settings loaded.");

    so_relocate(&fmodex_mod);
    so_relocate(&fmodevent_mod);
    so_relocate(&so_mod);
    l_success("SO relocated.");

    resolve_imports_generic(&fmodex_mod);
    resolve_imports_generic(&fmodevent_mod);
    resolve_imports(&so_mod);
    l_success("SO imports resolved.");

    fmod_patch();
    so_patch();
    l_success("SO patched.");

    so_flush_caches(&fmodex_mod);
    so_flush_caches(&fmodevent_mod);
    so_flush_caches(&so_mod);
    l_success("SO caches flushed.");

    so_initialize(&fmodex_mod);
    so_initialize(&fmodevent_mod);
    so_initialize(&so_mod);
    l_success("SO initialized.");

    gl_preload();
    l_success("OpenGL preloaded.");

    jni_init();
    l_success("FalsoJNI initialized.");

#ifndef NDK_PORT
    controls_init();
    l_success("Controls initialized.");
#endif
}
