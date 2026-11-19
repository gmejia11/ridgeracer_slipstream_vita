/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdio.h>
#include <string.h>
#include "settings.h"

#define CONFIG_FILE_PATH DATA_PATH"config.txt"

int  setting_msaa;
int  setting_fastpath;
int  setting_cores;
int  setting_rewrite;
int  setting_framepacing;
int  setting_obbcache;
int  setting_social;
int  setting_rw_collect;
int  setting_rw_begin;
int  setting_rw_vertex;
int  setting_rw_render;
int  setting_rw_findfile;
int  setting_rw_cfile;
int  setting_deflate;
int  setting_libpng;
int  setting_prefetch;

void settings_reset() {
    setting_msaa = 1; // 2x by default: tested 4x on HW (2026-11-13), GPU-bound: menus drop to 30, race stutters
    setting_fastpath = 1;
    setting_cores = 1;
    setting_rewrite = 1;
    setting_framepacing = 1;
    setting_obbcache = 1;
    setting_social = 0;
    setting_rw_collect = 1;
    setting_rw_begin = 1;
    setting_rw_vertex = 1;
    setting_rw_render = 1;
    setting_rw_findfile = 1;
    setting_rw_cfile = 1;
    setting_deflate = 1;
    setting_libpng = 1;
    setting_prefetch = 1;
}

void settings_load() {
    settings_reset();

    char buffer[30];
    int value;

    FILE *config = fopen(CONFIG_FILE_PATH, "r");

    if (config) {
        while (EOF != fscanf(config, "%[^ ] %d\n", buffer, &value)) {
            if (strcmp("msaa", buffer) == 0 && value >= 0 && value <= 2) setting_msaa = value;
            if (strcmp("fastpath", buffer) == 0) setting_fastpath = value != 0;
            if (strcmp("cores", buffer) == 0) setting_cores = value != 0;
            if (strcmp("rewrite", buffer) == 0) setting_rewrite = value != 0;
            if (strcmp("framepacing", buffer) == 0 && value >= 0 && value <= 2) setting_framepacing = value;
            if (strcmp("obbcache", buffer) == 0) setting_obbcache = value != 0;
            if (strcmp("social", buffer) == 0) setting_social = value != 0;
            if (strcmp("rw_collect", buffer) == 0) setting_rw_collect = value != 0;
            if (strcmp("rw_begin", buffer) == 0) setting_rw_begin = value != 0;
            if (strcmp("rw_vertex", buffer) == 0) setting_rw_vertex = value != 0;
            if (strcmp("rw_render", buffer) == 0) setting_rw_render = value != 0;
            if (strcmp("rw_findfile", buffer) == 0) setting_rw_findfile = value != 0;
            if (strcmp("rw_cfile", buffer) == 0) setting_rw_cfile = value != 0;
            if (strcmp("deflate", buffer) == 0) setting_deflate = value != 0;
            if (strcmp("libpng", buffer) == 0) setting_libpng = value != 0;
            if (strcmp("prefetch", buffer) == 0) setting_prefetch = value != 0;
        }
        fclose(config);
    }
}

void settings_save() {
    FILE *config = fopen(CONFIG_FILE_PATH, "w+");

    if (config) {
        fprintf(config, "%s %d\n", "msaa", setting_msaa);
        fprintf(config, "%s %d\n", "fastpath", setting_fastpath);
        fprintf(config, "%s %d\n", "cores", setting_cores);
        fprintf(config, "%s %d\n", "rewrite", setting_rewrite);
        fprintf(config, "%s %d\n", "framepacing", setting_framepacing);
        fprintf(config, "%s %d\n", "obbcache", setting_obbcache);
        fprintf(config, "%s %d\n", "social", setting_social);
        fprintf(config, "%s %d\n", "rw_collect", setting_rw_collect);
        fprintf(config, "%s %d\n", "rw_begin", setting_rw_begin);
        fprintf(config, "%s %d\n", "rw_vertex", setting_rw_vertex);
        fprintf(config, "%s %d\n", "rw_render", setting_rw_render);
        fprintf(config, "%s %d\n", "rw_findfile", setting_rw_findfile);
        fprintf(config, "%s %d\n", "rw_cfile", setting_rw_cfile);
        fprintf(config, "%s %d\n", "deflate", setting_deflate);
        fprintf(config, "%s %d\n", "libpng", setting_libpng);
        fprintf(config, "%s %d\n", "prefetch", setting_prefetch);
        fclose(config);
    }
}
