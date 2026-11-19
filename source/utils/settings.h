/*
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  settings.h
 * @brief Loader settings that can be set via a configurator app.
 */

#ifndef SOLOADER_SETTINGS_H
#define SOLOADER_SETTINGS_H

#include "stdbool.h"

#ifdef __cplusplus
extern "C" {
#endif

// 0 = no MSAA, 1 = 2x, 2 = 4x  (ux0:data/ridgeracer/config.txt: "msaa 1")
extern int  setting_msaa;
// 1 = rewrite hot -O0 helpers of the game with optimized code (source/fastpath.c)
extern int  setting_fastpath;
// 1 = pin the game thread to CPU core 0 and move helper threads to cores 1-2
extern int  setting_cores;
// 1 = C rewrites of big engine functions (CRenderNodeMesh::Collect, ...), needs fastpath
extern int  setting_rewrite;
// 0 = plain vsync, 1 = steady 30 fps when 60 can't be reached (default), 2 = always 30 fps
extern int  setting_framepacing;
// 1 = open the OBB once and serve every fopen() of it from that handle (obbfile.c)
extern int  setting_obbcache;
// 0 = hide the Facebook / share widgets (default), 1 = show them as in the original
extern int  setting_social;
// individual switches of the big rewrites (all need rewrite 1), and libdeflate
extern int  setting_rw_collect;
extern int  setting_rw_begin;
extern int  setting_rw_vertex;
extern int  setting_rw_render;
extern int  setting_rw_findfile;
extern int  setting_rw_cfile;
extern int  setting_deflate;
extern int  setting_libpng;
extern int  setting_prefetch;

void settings_load();
void settings_save();
void settings_reset();

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_SETTINGS_H
