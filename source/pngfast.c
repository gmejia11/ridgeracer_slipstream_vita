/*
 * pngfast.c -- send the game's PNG decoding to the SDK's libpng 1.6.
 *
 * The game bundles libpng 1.5.10 built at -O0 (row filters, row copies and
 * interlace handling all unoptimised). In a race load PNG decoding was about
 * 20 ms per frame (5-6 s per load in total). The SDK libpng 1.6 is -O2 and
 * has NEON row filters; inflate already goes to the SDK zlib (patch_zlib).
 *
 * The game only uses the public API (CTextureManager::CreateTextureFromFile
 * and CImage::CreateImageFromFile, plus their png_get_io_ptr read callbacks),
 * with no transforms, so the decoded rows are the same with either version.
 * All 844 PNGs of the game (OBB + assets) were checked on the host: valid
 * CRCs, complete zlib streams, exact IDAT sizes, no unknown critical chunks.
 *
 * Redirection is all-or-nothing: a 1.6 png_struct must never reach a 1.5
 * function. Two wrappers adapt the differences:
 *  - png_create_read_struct: the game passes "1.5.10", which 1.6 rejects
 *    (major.minor must match), so our own version string is used; the error
 *    and warning callbacks are ours (log, then longjmp like the default).
 *  - png_set_longjmp_fn: the game passes the size of bionic's jmp_buf (256)
 *    but setjmp/longjmp here are newlib's, so the size given to libpng is
 *    newlib's sizeof(jmp_buf) (the buffer is then the one inside png_struct).
 *
 * Config: "libpng 0" keeps the game's own libpng.
 */
#include <string.h>
#include <stdint.h>
#include <setjmp.h>
#include <png.h>
#include <so_util/so_util.h>

#include "utils/logger.h"
#include "utils/settings.h"

static unsigned png_errors, png_warnings;

static void pf_error(png_structp p, png_const_charp msg) {
    if (png_errors++ < 16) l_warn("libpng error: %s", msg);
    png_longjmp(p, 1);
}

static void pf_warning(png_structp p, png_const_charp msg) {
    (void)p;
    if (png_warnings++ < 16) l_info("libpng warning: %s", msg);
}

static png_structp pf_create_read_struct(png_const_charp ver, png_voidp err_ptr,
                                         png_error_ptr err_fn, png_error_ptr warn_fn) {
    (void)ver; (void)err_fn; (void)warn_fn;   // the game passes NULL callbacks
    return png_create_read_struct(PNG_LIBPNG_VER_STRING, err_ptr, pf_error, pf_warning);
}

static jmp_buf *pf_set_longjmp_fn(png_structp p, png_longjmp_ptr fn, size_t size) {
    (void)size;
    return png_set_longjmp_fn(p, fn, sizeof(jmp_buf));
}

unsigned png_fast_errors(void) { return png_errors; }

int png_patch(so_module *m) {
    if (!setting_libpng) {
        l_info("pngfast: disabled in config");
        return 0;
    }
    static const struct { const char *sym; void *fn; } t[] = {
        { "png_create_read_struct", (void *)pf_create_read_struct },
        { "png_set_longjmp_fn", (void *)pf_set_longjmp_fn },
        { "png_sig_cmp", (void *)png_sig_cmp },
        { "png_create_info_struct", (void *)png_create_info_struct },
        { "png_set_read_fn", (void *)png_set_read_fn },
        { "png_get_io_ptr", (void *)png_get_io_ptr },
        { "png_set_sig_bytes", (void *)png_set_sig_bytes },
        { "png_read_info", (void *)png_read_info },
        { "png_get_image_width", (void *)png_get_image_width },
        { "png_get_image_height", (void *)png_get_image_height },
        { "png_get_bit_depth", (void *)png_get_bit_depth },
        { "png_get_channels", (void *)png_get_channels },
        { "png_read_update_info", (void *)png_read_update_info },
        { "png_read_image", (void *)png_read_image },
        { "png_destroy_read_struct", (void *)png_destroy_read_struct },
    };
    enum { N = sizeof(t) / sizeof(t[0]) };
    uintptr_t addr[N];
    for (int i = 0; i < N; i++) {
        addr[i] = (uintptr_t)so_symbol(m, t[i].sym);
        if (!addr[i] || (addr[i] & 3)) {
            l_warn("pngfast: %s not found, keeping the game's libpng", t[i].sym);
            return 0;
        }
    }
    for (int i = 0; i < N; i++)
        hook_addr(addr[i], (uintptr_t)t[i].fn);
    l_info("pngfast: libpng %s in use (%d functions redirected)", PNG_LIBPNG_VER_STRING, N);
    return N;
}
