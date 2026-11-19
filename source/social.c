/*
 * social.c -- hide the social network widgets (Facebook and Google Play
 * login/logout, share buttons, Google Play Games / Game Center button and the
 * Facebook / Game Center icons), which can't work on the Vita.
 *
 * GUI layouts are XML files in the OBB parsed with pugixml's
 * xml_document::load_buffer, which copies the text before parsing. We hook it
 * and, when the text contains one of those widgets, parse a patched copy in
 * which each of them gets visible="0" and an off-screen pos (the element's
 * own pos/visible attributes are renamed in place to po_/visibl_ so there are
 * no duplicates and the XML stays well-formed). Everything else in the layout
 * is untouched. Show them again with "social 1" in config.txt.
 *
 * pugi::xml_parse_result xml_document::load_buffer(const void *contents,
 *     size_t size, unsigned options, xml_encoding enc)
 *   r0 = &result (hidden), r1 = this, r2 = contents, r3 = size, stack: options, enc
 */

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <kubridge.h>
#include <so_util/so_util.h>

#include "utils/logger.h"
#include "utils/settings.h"

extern so_module so_mod;

static void *(*xml_load_orig)(void *res, void *doc, const void *contents, size_t size,
                              unsigned options, int enc);

// an element is hidden when its tag contains one of these
static const char *const social_marks[] = {
    "name=\"FBLogin\"",
    "name=\"FBLogout\"",
    "name=\"BFbShare\"",
    "name=\"BShareIntent\"",
    "challange_facebook_icon.png\"",
    // 2026-11-18: Google Play / Game Center and the remaining network icons
    "name=\"BGpLogin\"",            // ProfileGUI: Google Play login / logout
    "name=\"BGpLogout\"",
    "name=\"BGameCenter\"",         // TopGUI: Google Play Games / Game Center button
    "name=\"SChallengeFbIcon\"",    // ResultsGUI: challenge via Facebook / Game Center icons
    "name=\"SChallengeGCIcon\"",
    "challange_gamecenter_icon.png\"", // Templates: Game Center icon on friend lines
};
#define N_MARKS (sizeof(social_marks) / sizeof(social_marks[0]))

static const char hide_attrs[] = " visible=\"0\" pos=\"-4000,-4000\"";

static const char *mem_find(const char *h, size_t hl, const char *n, size_t nl) {
    if (nl > hl) return NULL;
    for (size_t i = 0; i + nl <= hl; i++)
        if (h[i] == n[0] && memcmp(h + i, n, nl) == 0) return h + i;
    return NULL;
}

// rename attribute `name` (same length, so offsets don't move) inside tag [b, e)
static void rename_attr(char *b, char *e, const char *name, const char *repl) {
    size_t nl = strlen(name);
    for (char *p = b; p + nl + 2 <= e; p++) {
        if ((p == b || p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\r' || p[-1] == '\n') &&
            memcmp(p, name, nl) == 0 && p[nl] == '=' && p[nl + 1] == '"')
            memcpy(p, repl, nl);
    }
}

// returns a malloc'ed patched copy (and its size) or NULL if nothing to hide
static char *patch_layout(const char *src, size_t len, size_t *out_len) {
    char *buf = NULL;            // mutable copy, created on the first match
    size_t ins[64];
    int n_ins = 0;
    for (unsigned m = 0; m < N_MARKS; m++) {
        size_t ml = strlen(social_marks[m]);
        const char *base = buf ? buf : src;
        const char *p = base;
        while ((p = mem_find(p, len - (p - base), social_marks[m], ml)) != NULL) {
            const char *lt = p;
            while (lt > base && *lt != '<' && *lt != '>') lt--;
            const char *gt = p;
            while (gt < base + len && *gt != '>') gt++;
            if (*lt == '<' && gt < base + len) {
                if (!buf) {
                    buf = malloc(len);
                    if (!buf) return NULL;
                    memcpy(buf, src, len);
                    p = buf + (p - base); lt = buf + (lt - base); gt = buf + (gt - base);
                    base = buf;
                }
                // the original pos/visible become inert attributes: no duplicates
                rename_attr((char *)lt, (char *)gt, "pos", "po_");
                rename_attr((char *)lt, (char *)gt, "visible", "visibl_");
                const char *e = lt + 1;
                while (e < base + len && *e != ' ' && *e != '\t' && *e != '\r' && *e != '\n' &&
                       *e != '/' && *e != '>')
                    e++;
                size_t at = e - base;
                int dup = 0;
                for (int i = 0; i < n_ins; i++) if (ins[i] == at) dup = 1;
                if (!dup && n_ins < 64) ins[n_ins++] = at;
            }
            p += ml;
        }
    }
    if (!n_ins) { free(buf); return NULL; }

    for (int i = 1; i < n_ins; i++)
        for (int j = i; j > 0 && ins[j] < ins[j - 1]; j--) { size_t t = ins[j]; ins[j] = ins[j - 1]; ins[j - 1] = t; }

    size_t add = sizeof(hide_attrs) - 1;
    char *out = malloc(len + n_ins * add + 1);
    if (!out) { free(buf); return NULL; }
    size_t o = 0, from = 0;
    for (int i = 0; i < n_ins; i++) {
        memcpy(out + o, buf + from, ins[i] - from);
        o += ins[i] - from;
        memcpy(out + o, hide_attrs, add);
        o += add;
        from = ins[i];
    }
    memcpy(out + o, buf + from, len - from);
    o += len - from;
    out[o] = 0;
    *out_len = o;
    free(buf);
    return out;
}

static void *xml_load_buffer(void *res, void *doc, const void *contents, size_t size,
                             unsigned options, int enc) {
    if (contents && size) {
        size_t nl;
        char *patched = patch_layout(contents, size, &nl);
        if (patched) {
            void *r = xml_load_orig(res, doc, patched, nl, options, enc);   // pugixml copies it
            free(patched);
            return r;
        }
    }
    return xml_load_orig(res, doc, contents, size, options, enc);
}

static void *exec_tramp(uintptr_t a) {
    static uint32_t *mem;
    if (!mem) {
        SceKernelAllocMemBlockKernelOpt opt;
        memset(&opt, 0, sizeof(opt));
        opt.size = sizeof(opt);
        SceUID blk = kuKernelAllocMemBlock("social_tramp", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 0x1000, &opt);
        if (blk < 0) return NULL;
        sceKernelGetMemBlockBase(blk, (void **)&mem);
        kuKernelMemProtect(mem, 0x1000, KU_KERNEL_PROT_EXEC | KU_KERNEL_PROT_WRITE | KU_KERNEL_PROT_READ);
    }
    const uint32_t *f = (const uint32_t *)a;
    mem[0] = f[0]; mem[1] = f[1];
    mem[2] = 0xE51FF004;   // ldr pc, [pc, #-4]
    mem[3] = a + 8;
    kuKernelFlushCaches(mem, 16);
    return mem;
}

void social_patch(void) {
    if (setting_social) return;
    uintptr_t a = so_symbol(&so_mod, "_ZN4pugi12xml_document11load_bufferEPKvjjNS_12xml_encodingE");
    const uint32_t *f = (const uint32_t *)a;
    // 2.5.4: push {fp, lr}; add fp, sp, #4; sub sp, sp, #32
    if (!a || f[0] != 0xE92D4800 || f[1] != 0xE28DB004 || f[2] != 0xE24DD020) {
        l_warn("social: pugixml load_buffer not the expected code, widgets left as they are");
        return;
    }
    if (!(*(void **)&xml_load_orig = exec_tramp(a))) return;
    hook_addr(a, (uintptr_t)&xml_load_buffer);
    kuKernelFlushCaches((void *)a, 8);
}
