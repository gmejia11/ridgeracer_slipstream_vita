/*
 * fastpath.c -- speed up libRidgeRacer.so, which was built at -O0.
 *
 * At -O0 every tiny helper (vector constructors, getters, std::vector
 * accessors...) spills all its arguments to the stack and reloads them, and
 * the game calls these ~400k times per frame in a race. We rewrite them in
 * place, before the module is initialized:
 *
 *  1. pattern rewrites: functions whose machine code matches a known -O0
 *     shape ("return this->field", "return this", "return arg", vector
 *     size()/operator[]) are replaced by the equivalent 2-5 instructions;
 *  2. hand-written routines from fastpath_asm.S for vector math;
 *  3. C replacements (hooked) for the 4x4 matrix helpers.
 *
 * Float evaluation order is preserved everywhere, so results are identical.
 * Disable with "fastpath 0" in ux0:data/ridgeracer/config.txt.
 */

#pragma GCC optimize ("O3", "no-fast-math")

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <kubridge.h>
#include <time.h>
#include <pthread.h>
#include <zlib.h>
#include "libdeflate.h"
#include <vitaGL.h>
#include <psp2/kernel/sysmem.h>
#include <so_util/so_util.h>

#include "utils/logger.h"
#include "utils/settings.h"

extern so_module so_mod;

/* ---------------- 1. pattern rewrites ---------------- */

#define PRO0 0xE52DB004  // push {fp}
#define PRO1 0xE28DB000  // add fp, sp, #0
#define PRO2 0xE24DD00C  // sub sp, sp, #12
#define PRO3 0xE50B0008  // str r0, [fp, #-8]
#define EPI0 0xE1A00003  // mov r0, r3
#define EPI1 0xE24BD000  // sub sp, fp, #0
#define EPI2 0xE49DB004  // pop {fp}
#define EPI3 0xE12FFF1E  // bx lr
#define BX_LR 0xE12FFF1E

static int has_pro(const uint32_t *f) {
    return f[0] == PRO0 && f[1] == PRO1 && f[2] == PRO2 && f[3] == PRO3;
}
static int has_epi(const uint32_t *e) {
    return e[0] == EPI0 && e[1] == EPI1 && e[2] == EPI2 && e[3] == EPI3;
}

#define PUSH_FP_LR 0xE92D4800  // push {fp, lr}
#define ADD_FP_4   0xE28DB004  // add fp, sp, #4
#define EPI_LR0    0xE24BD004  // sub sp, fp, #4
#define EPI_LR1    0xE8BD8800  // pop {fp, pc}

enum { K_NONE, K_RET_THIS, K_CTOR_DEREF, K_CTOR_VALUE };

static uint32_t *fn_lo, *fn_hi; // text bounds, for bl target checks

// Classify a tiny function (original code) used as a callee by wrappers.
static int classify(const uint32_t *f) {
    if (f < fn_lo || f + 14 > fn_hi || !has_pro(f)) return K_NONE;
    const uint32_t *b = f + 4;
    if (b[0] == 0xE51B3008 && has_epi(b + 1)) return K_RET_THIS;
    if (b[0] == 0xE50B100C && b[1] == 0xE51B300C && b[2] == 0xE5932000 && b[3] == 0xE51B3008 &&
        b[4] == 0xE5832000 && b[5] == 0xE51B3008 && has_epi(b + 6)) return K_CTOR_DEREF;
    if (b[0] == 0xE50B100C && b[1] == 0xE51B3008 && b[2] == 0xE51B200C && b[3] == 0xE5832000 &&
        b[4] == 0xE51B3008 && has_epi(b + 5)) return K_CTOR_VALUE;
    return K_NONE;
}

static const uint32_t *bl_target(const uint32_t *at) {
    uint32_t w = *at;
    if ((w & 0xFF000000) != 0xEB000000) return NULL;
    int32_t off = (int32_t)(w << 8) >> 6; // sign-extended imm24 * 4
    return (const uint32_t *)((uintptr_t)at + 8 + off);
}

static int is_cond_mov01(uint32_t x) { // mov<cond> r3, #0 / #1
    return (x & 0x0FFFFFFE) == 0x03A03000 && (x >> 28) != 0xE && (x >> 28) != 0xF;
}

// Returns number of words written to `out`, 0 if no pattern matched.
static int match_pattern(const uint32_t *f, uint32_t nwords, uint32_t *out) {
    // ---- functions with push {fp, lr} that only wrap another tiny call ----
    if (nwords >= 13 && f[0] == PUSH_FP_LR && f[1] == ADD_FP_4) {
        // begin()/end(): return iterator(this->field) or iterator(this + K)
        if (f[2] == 0xE24DD010 && f[3] == 0xE50B0010 && f[4] == 0xE51B3010) {
            uint32_t k = 0;
            const uint32_t *p = f + 5;
            if ((p[0] & 0xFFFFF000) == 0xE2833000) { k = p[0] & 0xFFF; p++; }
            if (p[0] == 0xE24B2008 && p[1] == 0xE1A00002 && p[2] == 0xE1A01003 &&
                p[4] == 0xE51B3008 && p[5] == 0xE1A00003 && p[6] == EPI_LR0 && p[7] == EPI_LR1 &&
                (uint32_t)(p + 8 - f) == nwords) {
                int c = classify(bl_target(p + 3));
                if (c == K_CTOR_DEREF && (k & 0xF00) == 0) { out[0] = 0xE5900000 | k; out[1] = BX_LR; return 2; }
                if (c == K_CTOR_VALUE) { out[0] = 0xE2800000 | k; out[1] = BX_LR; return 2; } // add r0, r0, #k
            }
        }
        // Rb_tree_iterator::operator->(): return &(node + K) via an identity helper
        if (nwords == 13 && f[2] == 0xE24DD008 && f[3] == 0xE50B0008 && f[4] == 0xE51B3008 &&
            f[5] == 0xE5933000 && (f[6] & 0xFFFFF000) == 0xE2833000 && f[7] == 0xE1A00003 &&
            f[9] == 0xE1A03000 && f[10] == 0xE1A00003 && f[11] == EPI_LR0 && f[12] == EPI_LR1 &&
            classify(bl_target(f + 8)) == K_RET_THIS) {
            out[0] = 0xE5900000;                 // ldr r0, [r0]
            out[1] = 0xE2800000 | (f[6] & 0xFFF); // add r0, r0, #K
            out[2] = BX_LR;
            return 3;
        }
    }
    // converting iterator ctor: __normal_iterator<const T*>(const __normal_iterator<T*> &o) : cur(o.base())
    if (nwords == 15 && f[0] == PUSH_FP_LR && f[1] == ADD_FP_4 && f[2] == 0xE24DD008 &&
        f[3] == 0xE50B0008 && f[4] == 0xE50B100C && f[5] == 0xE51B000C && f[7] == 0xE1A03000 &&
        f[8] == 0xE5932000 && f[9] == 0xE51B3008 && f[10] == 0xE5832000 && f[11] == 0xE51B3008 &&
        f[12] == 0xE1A00003 && f[13] == EPI_LR0 && f[14] == EPI_LR1 &&
        classify(bl_target(f + 6)) == K_RET_THIS) {
        out[0] = 0xE5912000; // ldr r2, [r1]
        out[1] = 0xE5802000; // str r2, [r0]
        out[2] = BX_LR;
        return 3;
    }
    // __normal_iterator operator!= / operator== : a.base() vs b.base()
    if (nwords == 20 && f[0] == 0xE92D4810 && f[1] == 0xE28DB008 && f[2] == 0xE24DD00C &&
        f[3] == 0xE50B0010 && f[4] == 0xE50B1014 && f[5] == 0xE51B0010 && f[7] == 0xE1A03000 &&
        f[8] == 0xE5934000 && f[9] == 0xE51B0014 && f[11] == 0xE1A03000 && f[12] == 0xE5933000 &&
        f[13] == 0xE1540003 && is_cond_mov01(f[14]) && is_cond_mov01(f[15]) && f[16] == 0xE6EF3073 &&
        f[17] == 0xE1A00003 && f[18] == 0xE24BD008 && f[19] == 0xE8BD8810 &&
        classify(bl_target(f + 6)) == K_RET_THIS && classify(bl_target(f + 10)) == K_RET_THIS) {
        out[0] = 0xE5902000;               // ldr r2, [r0]
        out[1] = 0xE5913000;               // ldr r3, [r1]
        out[2] = 0xE1520003;               // cmp r2, r3
        out[3] = f[14] & ~0xF000;          // mov<c> r0, #x
        out[4] = f[15] & ~0xF000;
        out[5] = BX_LR;
        return 6;
    }
    // pure forwarders: f(a[, b, c]) { return g(a[, b, c]); }  ->  b g
    if (nwords >= 8 && f[0] == PUSH_FP_LR && f[1] == ADD_FP_4) {
        static const uint32_t m1[] = { 0xE24DD008, 0xE50B0008, 0xE51B0008 };
        static const uint32_t m1b[] = { 0xE24DD008, 0xE50B0008, 0xE51B3008, 0xE1A00003 };
        static const uint32_t m2[] = { 0xE24DD008, 0xE50B0008, 0xE50B100C, 0xE51B0008, 0xE51B100C };
        static const uint32_t m3[] = { 0xE24DD010, 0xE50B0008, 0xE50B100C, 0xE50B2010, 0xE51B0008, 0xE51B100C, 0xE51B2010 };
        const uint32_t *mids[] = { m1, m1b, m2, m3 };
        const uint32_t lens[] = { 3, 4, 5, 7 };
        for (int k = 0; k < 4; k++) {
            uint32_t ml = lens[k];
            if (nwords != 2 + ml + 5) continue;
            if (memcmp(f + 2, mids[k], ml * 4)) continue;
            const uint32_t *tail = f + 2 + ml;
            if ((tail[0] & 0xFF000000) != 0xEB000000 || tail[1] != 0xE1A03000 || tail[2] != 0xE1A00003 ||
                tail[3] != EPI_LR0 || tail[4] != EPI_LR1) continue;
            const uint32_t *target = bl_target(tail);
            if (target < fn_lo || target >= fn_hi) continue;
            int32_t off = ((int32_t)((uintptr_t)target - ((uintptr_t)f + 8))) >> 2;
            out[0] = 0xEA000000 | ((uint32_t)off & 0xFFFFFF); // b target
            return 1;
        }
    }
    // global singleton getter: return *(T **)GOT[x]
    if (nwords == 13 && f[0] == PRO0 && f[1] == PRO1 && (f[2] & 0xFFFFF000) == 0xE59F2000 &&
        f[3] == 0xE08F2002 && (f[4] & 0xFFFFF000) == 0xE59F3000 && f[5] == 0xE7923003 &&
        f[6] == 0xE5933000 && f[7] == EPI0 && f[8] == EPI1 && f[9] == EPI2 && f[10] == EPI3) {
        uint32_t lit_a = *(const uint32_t *)((uintptr_t)(f + 2) + 8 + (f[2] & 0xFFF));
        uint32_t lit_b = *(const uint32_t *)((uintptr_t)(f + 4) + 8 + (f[4] & 0xFFF));
        uintptr_t got = (uintptr_t)(f + 3) + 8 + lit_a;
        uint32_t var = *(const uint32_t *)(got + lit_b);
        if (var) {
            out[0] = 0xE59F0004;           // ldr r0, [pc, #4]
            out[1] = 0xE5900000;           // ldr r0, [r0]
            out[2] = BX_LR;
            out[3] = var;
            return 4;
        }
    }

    // ---- leaf functions: push {fp}; ...; pop {fp}; bx lr ----
    if (nwords < 9 || !has_pro(f) || !has_epi(f + nwords - 4)) return 0;
    const uint32_t *b = f + 4;      // body
    uint32_t nb = nwords - 8;

    // return this;
    if (nb == 1 && b[0] == 0xE51B3008) {
        out[0] = BX_LR;
        return 1;
    }
    if (nb == 2 && b[0] == 0xE51B3008) {
        uint32_t x = b[1];
        // return this->field;  (ldr / ldrb with immediate offset)
        if ((x & 0xFFFFF000) == 0xE5933000) { out[0] = 0xE5900000 | (x & 0xFFF); out[1] = BX_LR; return 2; }
        if ((x & 0xFFFFF000) == 0xE5D33000) { out[0] = 0xE5D00000 | (x & 0xFFF); out[1] = BX_LR; return 2; }
        // return this + K;
        if ((x & 0xFFFFF000) == 0xE2833000) { out[0] = 0xE2800000 | (x & 0xFFF); out[1] = BX_LR; return 2; }
    }
    // return arg1;
    if (nb == 2 && b[0] == 0xE50B100C && b[1] == 0xE51B300C) {
        out[0] = 0xE1A00001; // mov r0, r1
        out[1] = BX_LR;
        return 2;
    }
    // iterator(const T *&p): *this = *p; return this
    if (nb == 6 && b[0] == 0xE50B100C && b[1] == 0xE51B300C && b[2] == 0xE5932000 &&
        b[3] == 0xE51B3008 && b[4] == 0xE5832000 && b[5] == 0xE51B3008) {
        out[0] = 0xE5912000; // ldr r2, [r1]
        out[1] = 0xE5802000; // str r2, [r0]
        out[2] = BX_LR;
        return 3;
    }
    // iterator(node *p): *this = p; return this
    if (nb == 5 && b[0] == 0xE50B100C && b[1] == 0xE51B3008 && b[2] == 0xE51B200C &&
        b[3] == 0xE5832000 && b[4] == 0xE51B3008) {
        out[0] = 0xE5801000; // str r1, [r0]
        out[1] = BX_LR;
        return 2;
    }
    // iterator ++ / -- : *this += K; return this
    if (nb == 6 && b[0] == 0xE51B3008 && b[1] == 0xE5933000 &&
        ((b[2] & 0xFFFFF000) == 0xE2832000 || (b[2] & 0xFFFFF000) == 0xE2432000) &&
        b[3] == 0xE51B3008 && b[4] == 0xE5832000 && b[5] == 0xE51B3008) {
        out[0] = 0xE5903000;                     // ldr r3, [r0]
        out[1] = (b[2] & ~0xF000) | 0x3000;      // add/sub r3, r3, #K
        out[2] = 0xE5803000;                     // str r3, [r0]
        out[3] = BX_LR;
        return 4;
    }
    // Rb_tree_iterator operator!= / == : node vs other.node
    if (nb == 9 && b[0] == 0xE50B100C && b[1] == 0xE51B3008 && b[2] == 0xE5932000 &&
        b[3] == 0xE51B300C && b[4] == 0xE5933000 && b[5] == 0xE1520003 &&
        is_cond_mov01(b[6]) && is_cond_mov01(b[7]) && b[8] == 0xE6EF3073) {
        out[0] = 0xE5902000; // ldr r2, [r0]
        out[1] = 0xE5913000; // ldr r3, [r1]
        out[2] = 0xE1520003; // cmp r2, r3
        out[3] = b[6] & ~0xF000;
        out[4] = b[7] & ~0xF000;
        out[5] = BX_LR;
        return 6;
    }
    // std::vector<T>::size()  (end - begin) >> n
    if (nb == 7 && b[0] == 0xE51B3008 && b[1] == 0xE5933004 && b[2] == 0xE1A02003 &&
        b[3] == 0xE51B3008 && b[4] == 0xE5933000 && b[5] == 0xE0633002 &&
        (b[6] & 0xFFFFF07F) == 0xE1A03043) {
        uint32_t sh = (b[6] >> 7) & 0x1F;
        out[0] = 0xE5901004;              // ldr r1, [r0, #4]
        out[1] = 0xE5900000;              // ldr r0, [r0]
        out[2] = 0xE0410000;              // sub r0, r1, r0
        out[3] = 0xE1A00040 | (sh << 7);  // asr r0, r0, #sh
        out[4] = BX_LR;
        return 5;
    }
    // std::vector<T>::operator[](n)  begin + (n << k)
    if (nb == 6 && b[0] == 0xE50B100C && b[1] == 0xE51B3008 && b[2] == 0xE5932000 &&
        b[3] == 0xE51B300C && (b[4] & 0xFFFFF07F) == 0xE1A03003 && b[5] == 0xE0823003) {
        uint32_t sh = (b[4] >> 7) & 0x1F;
        out[0] = 0xE5902000;              // ldr r2, [r0]
        out[1] = 0xE0820001 | (sh << 7);  // add r0, r2, r1, lsl #sh
        out[2] = BX_LR;
        return 3;
    }
    return 0;
}

/* ---------------- 2. hand-written routines ---------------- */

#define R(n) extern const uint32_t n[], n##_end[];
R(fp_v3_ctor3) R(fp_v3_ctor0) R(fp_v3_assign) R(fp_v2_ctor2) R(fp_v3_add) R(fp_v3_sub)
R(fp_v3_mulf) R(fp_v3_dot) R(fp_v2_dot) R(fp_v2_len) R(fp_v2_normalize) R(fp_mat_identity)
R(fp_less_int) R(fp_less_uint) R(fp_mat_copy) R(fp_v3_len) R(fp_rq_compare)
R(fp_mat_right) R(fp_mat_up) R(fp_mat_at) R(fp_vec12_index)
R(fp_mat_vecrotate) R(fp_mesh_getmat) R(fp_mesh_getgeom) R(fp_mat_get)
#undef R

typedef struct { const char *sym; const uint32_t *start, *end; } routine;
#define E(s, n) { s, n, n##_end }
static const routine routines[] = {
    E("_ZN8CVector3IfEC2Efff", fp_v3_ctor3),
    E("_ZN8CVector3IfEC2Ev", fp_v3_ctor0),
    E("_ZN8CVector3IfEaSERKS0_", fp_v3_assign),
    E("_ZN8CVector2IfEC2Eff", fp_v2_ctor2),
    E("_ZNK8CVector3IfEplERKS0_", fp_v3_add),
    E("_ZNK8CVector3IfEmiERKS0_", fp_v3_sub),
    E("_ZNK8CVector3IfEmlERKf", fp_v3_mulf),
    E("_ZNK8CVector3IfE3DotERKS0_", fp_v3_dot),
    E("_ZN8CVector2IfE3DotERKS0_", fp_v2_dot),
    E("_ZN8CVector2IfE9GetLengthEv", fp_v2_len),
    E("_ZN8CVector2IfE9NormalizeEv", fp_v2_normalize),
    E("_ZN7CMatrix8IdentityEv", fp_mat_identity),
    E("_ZNKSt4lessIiEclERKiS2_", fp_less_int),
    E("_ZNKSt4lessIjEclERKjS2_", fp_less_uint),
    E("_ZN7CMatrixC2ERKS_", fp_mat_copy),
    E("_ZN8CVector3IfE9GetLengthEv", fp_v3_len),
    E("_ZN19sRenderQueueCompareclERK16sRenderQueueItemS2_", fp_rq_compare),
    E("_ZNK7CMatrix8GetRightEv", fp_mat_right),
    E("_ZNK7CMatrix5GetUpEv", fp_mat_up),
    E("_ZNK7CMatrix5GetAtEv", fp_mat_at),
    E("_ZNSt6vectorIS_I5PlaneSaIS0_EESaIS2_EEixEj", fp_vec12_index),
    E("_ZNK7CMatrix9VecRotateERK8CVector3IfE", fp_mat_vecrotate),
    E("_ZN11CRenderMesh11GetMaterialEj", fp_mesh_getmat),
    E("_ZN11CRenderMesh11GetGeometryEj", fp_mesh_getgeom),
    E("_ZNK7CMatrix3GetEii", fp_mat_get),
};
#undef E

/* ---------------- 3. C replacements ---------------- */

// CMatrix CMatrix::MatrixMultiply(const CMatrix &a, const CMatrix &b) const
// (hidden return pointer in r0, `this` unused)
static float *mat_multiply(float *out, const void *self, const float *a, const float *b) {
    float r[16];
    for (int i = 0; i < 4; i++) {
        const float *ar = a + i * 4;
        for (int j = 0; j < 4; j++) {
            float s = ar[0] * b[j];
            s = s + ar[1] * b[4 + j];
            s = s + ar[2] * b[8 + j];
            s = s + ar[3] * b[12 + j];
            r[i * 4 + j] = s;
        }
    }
    memcpy(out, r, sizeof(r));
    return out;
}

// CVector3 CMatrix::VecTransform(const CVector3 &v) const  (projective)
static float *mat_vec_transform(float *out, const float *m, const float *v) {
    float x = ((v[0] * m[0] + v[1] * m[4]) + v[2] * m[8]) + m[12];
    float y = ((v[0] * m[1] + v[1] * m[5]) + v[2] * m[9]) + m[13];
    float z = ((v[0] * m[2] + v[1] * m[6]) + v[2] * m[10]) + m[14];
    float w = ((v[0] * m[3] + v[1] * m[7]) + v[2] * m[11]) + m[15];
    out[0] = x / w;
    out[1] = y / w;
    out[2] = z / w;
    return out;
}

// bool CRenderCamera::IsAreaVisible(int area)
static int cam_is_area_visible(const uint8_t *cam, int area) {
    if (area == -2) return 1;
    const int *begin = *(int *const *)(cam + 160);
    const int *end = *(int *const *)(cam + 164);
    int n = (int)(end - begin);
    for (int i = 0; i < n; i++)
        if (begin[i] == area) return 1;
    return 0;
}

// bool CRenderCamera::IsVisible(const CMatrix &m, const sBoundingBox &bb)
// Oriented box (center bb+24, size bb+36, transformed by m) against the six
// frustum planes stored at cam+320 (normal xyz, d at +12, 16 bytes each).
static void (*ComputeFrustumPlanes)(void *cam);

static int cam_is_visible(uint8_t *cam, const float *m, const uint8_t *bb) {
    if (*(uint32_t *)(cam + 188) & 4)
        ComputeFrustumPlanes(cam);

    float c[3];
    mat_vec_transform(c, m, (const float *)(bb + 24));
    const float *size = (const float *)(bb + 36);
    float h[3] = { size[0] * 0.5f, size[1] * 0.5f, size[2] * 0.5f };
    float R[3] = { m[0] * h[0], m[1] * h[0], m[2] * h[0] };
    float U[3] = { m[4] * h[1], m[5] * h[1], m[6] * h[1] };
    float A[3] = { m[8] * h[2], m[9] * h[2], m[10] * h[2] };

    // 8 corners, same order and same ((c +/- R) +/- U) +/- A evaluation as the original
    static const signed char sg[8][3] = {
        {-1, 1, 1}, {-1, 1, -1}, {-1, -1, -1}, {-1, -1, 1},
        { 1, 1, 1}, { 1, 1, -1}, { 1, -1, -1}, { 1, -1, 1},
    };
    float corner[8][3];
    for (int k = 0; k < 8; k++) {
        for (int j = 0; j < 3; j++) {
            float v = sg[k][0] > 0 ? c[j] + R[j] : c[j] - R[j];
            v = sg[k][1] > 0 ? v + U[j] : v - U[j];
            v = sg[k][2] > 0 ? v + A[j] : v - A[j];
            corner[k][j] = v;
        }
    }

    for (int i = 0; i < 6; i++) {
        const float *pl = (const float *)(cam + 320 + i * 16);
        float d = pl[3];
        int inside = 0;
        for (int k = 0; k < 8; k++) {
            float dot = (pl[0] * corner[k][0] + pl[1] * corner[k][1]) + pl[2] * corner[k][2];
            if (dot + d >= 0.0f) { inside = 1; break; }
        }
        if (!inside) return 0;
    }
    return 1;
}

// Point-to-segment distance in 2D (static helper of CGeomData, at +0x28 of
// GetIndexCount in the original binary).
static float seg_dist(float ax, float ay, float bx, float by, float px, float py) {
    float abx = ax - bx, aby = ay - by;
    float l = abx * abx + aby * aby;
    float pax = px - ax, pay = py - ay;
    if (l == 0.0f) return __builtin_sqrtf(pax * pax + pay * pay);
    float bax = bx - ax, bay = by - ay;
    float t = (pax * bax + pay * bay) / l;
    if (t < 0.0f) return __builtin_sqrtf(pax * pax + pay * pay);
    if (t > 1.0f) {
        float x = px - bx, y = py - by;
        return __builtin_sqrtf(x * x + y * y);
    }
    float qx = ax + bax * t, qy = ay + bay * t;
    float x = px - qx, y = py - qy;
    return __builtin_sqrtf(x * x + y * y);
}

// float CRenderScene::GetLightingFactorFromMesh(const CVector3 &p)
// Shadow factor from a "shadow mesh": triangles above the point darken it,
// fading out within `radius` (scene+252) of their edges, in the XZ plane.
/*
 * Per-triangle XZ bounds of the shadow mesh, so far-away triangles are
 * rejected with 4 compares. A triangle farther than `radius` from p can't be
 * "inside" nor give a distance < radius, so skipping it doesn't change the
 * result. Degenerate / sliver triangles are never skipped (their edge tests
 * are dominated by rounding in the original code).
 */
typedef struct { float minx, maxx, minz, maxz; } tri_box;
static struct {
    const void *g, *verts, *ind;
    int ntri;
    tri_box *box;
} light_cache;

static tri_box *light_boxes(const void *g, const float *verts, const int *ind, int ntri) {
    if (light_cache.g == g && light_cache.verts == verts && light_cache.ind == ind &&
        light_cache.ntri == ntri)
        return light_cache.box;
    free(light_cache.box);
    light_cache.box = NULL;
    light_cache.g = g; light_cache.verts = verts; light_cache.ind = ind; light_cache.ntri = ntri;
    tri_box *b = malloc(sizeof(tri_box) * (ntri > 0 ? ntri : 1));
    if (!b) { light_cache.g = NULL; return NULL; }
    for (int t = 0; t < ntri; t++) {
        const float *v[3];
        for (int k = 0; k < 3; k++)
            v[k] = verts + (uint16_t)ind[t * 3 + k] * 3;
        float minx = v[0][0], maxx = v[0][0], minz = v[0][2], maxz = v[0][2];
        for (int k = 1; k < 3; k++) {
            if (v[k][0] < minx) minx = v[k][0];
            if (v[k][0] > maxx) maxx = v[k][0];
            if (v[k][2] < minz) minz = v[k][2];
            if (v[k][2] > maxz) maxz = v[k][2];
        }
        float e1x = v[1][0] - v[0][0], e1z = v[1][2] - v[0][2];
        float e2x = v[2][0] - v[0][0], e2z = v[2][2] - v[0][2];
        float e3x = v[2][0] - v[1][0], e3z = v[2][2] - v[1][2];
        float area2 = __builtin_fabsf(e1x * e2z - e1z * e2x);
        float l1 = e1x * e1x + e1z * e1z, l2 = e2x * e2x + e2z * e2z, l3 = e3x * e3x + e3z * e3z;
        float lmax = l1 > l2 ? l1 : l2;
        if (l3 > lmax) lmax = l3;
        int ok = (minx == minx) && (maxx == maxx) && (minz == minz) && (maxz == maxz) &&
                 __builtin_isfinite(minx) && __builtin_isfinite(maxx) &&
                 __builtin_isfinite(minz) && __builtin_isfinite(maxz) &&
                 area2 > 1e-3f * lmax && area2 > 0.0f;
        if (ok) {
            b[t].minx = minx; b[t].maxx = maxx; b[t].minz = minz; b[t].maxz = maxz;
        } else { // never reject
            b[t].minx = -__builtin_inff(); b[t].maxx = __builtin_inff();
            b[t].minz = -__builtin_inff(); b[t].maxz = __builtin_inff();
        }
    }
    light_cache.box = b;
    return b;
}

static float scene_light_from_mesh(uint8_t *scene, const float *p) {
    float result = 1.0f;
    const uint8_t *g = *(const uint8_t **)(scene + 244);
    if (!g) return 1.0f;
    float *radius = (float *)(scene + 252);
    if (*radius < 0.1f) *radius = 0.1f;

    const float *verts = *(const float *const *)(g + 8);
    const int *ind = *(const int *const *)(g + 12);
    int ntri = *(const int *)(g + 20) / 3;

    const tri_box *box = light_boxes(g, verts, ind, ntri);
    float m = *radius * 1.01f + 0.05f;
    float qx0 = p[0] + m, qx1 = p[0] - m, qz0 = p[2] + m, qz1 = p[2] - m;
    int use_box = box && (p[0] == p[0]) && (p[2] == p[2]) && (m == m);

    for (int t = 0; t < ntri; t++) {
        if (use_box && (box[t].minx > qx0 || box[t].maxx < qx1 ||
                        box[t].minz > qz0 || box[t].maxz < qz1))
            continue;
        const float *v[3];
        for (int k = 0; k < 3; k++)
            v[k] = verts + (uint16_t)ind[t * 3 + k] * 3;

        float avg = ((v[0][1] + v[1][1]) + v[2][1]) / 3.0f;
        if (!(p[1] < avg)) continue;

        int inside = 1;
        int mask = 0;
        for (int e = 0; e < 3; e++) {
            const float *a = v[e], *b = v[(e + 1) % 3];
            float nx = b[2] - a[2], ny = a[0] - b[0];
            float len = __builtin_sqrtf(nx * nx + ny * ny);
            nx = nx / len;
            ny = ny / len;
            float dist = (nx * a[0] + ny * a[2]) - (nx * p[0] + ny * p[2]);
            if (dist > 0.0f) {
                inside = 0;
                if (*radius >= dist) mask |= 1 << e;
            }
            if (*radius < dist) { mask = 0; break; }
        }
        if (inside) { result = 0.0f; break; }
        if (!mask) continue;

        float best = *radius;
        for (int e = 0; e < 3; e++) {
            if (!((mask >> e) & 1)) continue;
            const float *a = v[e], *b = v[(e + 1) % 3];
            float d = seg_dist(a[0], a[2], b[0], b[2], p[0], p[2]);
            if (d < best) best = d;
        }
        if (*radius > best) {
            float frac = 1.0f - best / *radius;
            if (1.0f - frac < result) result = 1.0f - frac;
        }
    }
    float base = *(float *)(scene + 248);
    return base + (1.0f - base) * result;
}

// libstdc++ red-black trees with int / unsigned keys (key at node+16,
// left child at +8, right at +12; tree: header at this+4, root at this+8).
typedef struct rb_node { int color; struct rb_node *parent, *left, *right; uint32_t key; } rb_node;

// _M_lower_bound(x, y, k): first node whose key is not less than k
static rb_node *rb_lower_bound_i(void *self, rb_node *x, rb_node *y, const int *k) {
    int key = *k;
    while (x) {
        if (!((int)x->key < key)) { y = x; x = x->left; }
        else x = x->right;
    }
    return y;
}
static rb_node *rb_lower_bound_u(void *self, rb_node *x, rb_node *y, const uint32_t *k) {
    uint32_t key = *k;
    while (x) {
        if (!(x->key < key)) { y = x; x = x->left; }
        else x = x->right;
    }
    return y;
}
// find(k)
static rb_node *rb_find_i(uint8_t *self, const int *k) {
    rb_node *end = (rb_node *)(self + 4);
    rb_node *j = rb_lower_bound_i(self, *(rb_node **)(self + 8), end, k);
    return (j == end || *k < (int)j->key) ? end : j;
}
static rb_node *rb_find_u(uint8_t *self, const uint32_t *k) {
    rb_node *end = (rb_node *)(self + 4);
    rb_node *j = rb_lower_bound_u(self, *(rb_node **)(self + 8), end, k);
    return (j == end || *k < j->key) ? end : j;
}

// Hook every _Rb_tree<int|unsigned, ..., less<int|unsigned>> find/_M_lower_bound
static int patch_rb_trees(so_module *m) {
    int n = 0;
    for (uint32_t i = 0; i < m->num_dynsym; i++) {
        Elf32_Sym *s = &m->dynsym[i];
        if (ELF32_ST_TYPE(s->st_info) != STT_FUNC || !s->st_shndx) continue;
        const char *name = m->dynstr + s->st_name;
        const char *p;
        if (!strncmp(name, "_ZNSt8_Rb_treeI", 15)) p = name + 15;
        else if (!strncmp(name, "_ZNKSt8_Rb_treeI", 16)) p = name + 16;
        else continue;
        char kt = *p;
        if (kt != 'i' && kt != 'j') continue;
        if (!strstr(name, kt == 'i' ? "St4lessIiE" : "St4lessIjE")) continue;
        uintptr_t fn = 0;
        if (strstr(name, "E14_M_lower_boundE"))
            fn = kt == 'i' ? (uintptr_t)&rb_lower_bound_i : (uintptr_t)&rb_lower_bound_u;
        else if (strstr(name, "E4findE"))
            fn = kt == 'i' ? (uintptr_t)&rb_find_i : (uintptr_t)&rb_find_u;
        if (!fn) continue;
        uint32_t *f = (uint32_t *)(m->load_addr + s->st_value);
        if ((uintptr_t)f & 3) continue;
        if (f[0] != 0xE92D4800 && (f[0] & 0xFFFF0000) != 0xE92D0000) continue; // still original
        hook_addr((uintptr_t)f, fn);
        n++;
    }
    return n;
}

// void CMatrix::DoMatrixPass(CMatrix m, eMatrixPass pass)  (m by invisible reference)
static void mat_do_pass(float *self, const float *m, int pass) {
    if (pass == 0) memcpy(self, m, 64);
    else if (pass == 1) mat_multiply(self, NULL, m, self);   // this = m * this
    else if (pass == 2) mat_multiply(self, NULL, self, m);   // this = this * m
}

// bool CRenderMesh::IsSkyDome(): any section whose material shader has ID 29
static int mesh_is_skydome(uint8_t *mesh) {
    uint8_t **sec = *(uint8_t ***)(mesh + 12), **end = *(uint8_t ***)(mesh + 16);
    for (; sec < end; sec += 2) {                 // sRenderMeshSection = { material, geometry }
        uint8_t *shader = *(uint8_t **)(sec[0] + 136);
        if (shader && *(int *)(shader + 4) == 29) return 1;
    }
    return 0;
}

// bool CRenderMesh::IsInstance(): every section's geometry has vertex flag 0x400000
static int mesh_is_instance(uint8_t *mesh) {
    uint8_t **sec = *(uint8_t ***)(mesh + 12), **end = *(uint8_t ***)(mesh + 16);
    for (; sec < end; sec += 2)
        if (!(*(uint32_t *)(sec[1] + 40) & 0x400000)) return 0;
    return 1;
}

// int CRenderScene::GetVisibilityAreaIndex(const CVector3 &p)
// scene+8: vector<vector<Plane>>, Plane = { nx, ny, nz, d }. First area whose
// planes all have the point on their positive side, else -1.
static int scene_visibility_area(uint8_t *scene, const float *p) {
    const uint8_t *areas = *(const uint8_t **)(scene + 8);
    const uint8_t *aend = *(const uint8_t **)(scene + 12);
    int n = (int)(aend - areas) / 12;
    for (int i = 0; i < n; i++) {
        const float *pl = *(const float **)(areas + i * 12);
        const float *pe = *(const float **)(areas + i * 12 + 4);
        int inside = 1;
        for (; pl != pe; pl += 4) {
            float v = ((pl[0] * p[0] + pl[1] * p[1]) + pl[2] * p[2]) + pl[3];
            if (v < 0.0f) { inside = 0; break; }
        }
        if (inside) return i;
    }
    return -1;
}

// float CRenderScene::GetLightingFactorFromMap(const CVector3 &p)
// scene+228: vector<ShadowPlane> (28 bytes: minx, minz, maxx, maxz, scalex,
// scalez, CSmartPtr<CImage>); bilinear-ish lookup in an 8-bit shadow image.
static float scene_light_from_map(uint8_t *scene, const float *p) {
    const uint8_t *sp = *(const uint8_t **)(scene + 228);
    const uint8_t *se = *(const uint8_t **)(scene + 232);
    for (; sp != se; sp += 28) {
        const float *f = (const float *)sp;
        if (p[0] < f[0]) continue;
        if (!(p[0] < f[2])) continue;
        if (p[2] < f[1]) continue;
        if (!(p[2] < f[3])) continue;
        const uint8_t *img = *(const uint8_t **)(sp + 24);
        int w = *(const int *)(img + 8);
        int h = *(const int *)(img + 12);
        const uint8_t *buf = *(const uint8_t **)(img + 20);
        float dx = p[0] - f[0], dz = p[2] - f[1];
        int ix = (int)(dx * f[4]);
        int iz = (int)(dz * f[5]);
        int ix1 = ix + 1, iz1 = iz + 1;
        if (!(ix1 < w)) ix1 = ix;
        if (!(iz1 < h)) iz1 = iz;
        float ax = (float)ix / f[4], az = (float)iz / f[5];
        float bx = (float)ix1 / f[4], bz = (float)iz1 / f[5];
        float ux = bx - ax, uz = bz - az;
        float len1 = __builtin_sqrtf(ux * ux + uz * uz);
        float vx = dx - ax, vz = dz - az;
        float len2 = __builtin_sqrtf(vx * vx + vz * vz);
        float t = len2 / len1;
        float v0 = (float)buf[iz * w + ix];
        float v1 = (float)buf[iz1 * w + ix1];
        return ((v1 - v0) * t + v0) / 255.0f;
    }
    return 1.0f;
}

/*
 * IGameObject::{Pre,,Post,Post2}UpdateNode(float dt) -- the game object tree
 * walk, identical in all four except the vtable slot of the per-object step:
 *   if (active) { this->Step(dt); vector copy = children;
 *                 for (c : copy) if (find(children, c) != end) c->XUpdateNode(dt); }
 * At -O0 this copies the vector (malloc/free) and runs std::find through
 * iterator helpers for every node. obj+8: vector<IGameObject*>, obj+20: active.
 */
typedef void (*gobj_step)(void *self, float dt);

static void gobj_walk(uint8_t *self, float dt, int slot) {
    if (!self[20]) return;
    ((gobj_step)(*(void ***)self)[slot])(self, dt);

    void **b = *(void ***)(self + 8), **e = *(void ***)(self + 12);
    int n = e - b;
    if (n <= 0) return;
    void *stack_copy[32];
    void **copy = n <= 32 ? stack_copy : malloc(n * sizeof(void *));
    if (!copy) return;
    memcpy(copy, b, n * sizeof(void *));
    for (int i = 0; i < n; i++) {
        void *c = copy[i];
        void **p = *(void ***)(self + 8), **pe = *(void ***)(self + 12);
        for (; p != pe; p++) if (*p == c) break;
        if (p != pe) gobj_walk(c, dt, slot);
    }
    if (copy != stack_copy) free(copy);
}

static void gobj_pre_update_node(uint8_t *self, float dt)   { gobj_walk(self, dt, 2); }
static void gobj_update_node(uint8_t *self, float dt)       { gobj_walk(self, dt, 3); }
static void gobj_post_update_node(uint8_t *self, float dt)  { gobj_walk(self, dt, 4); }
static void gobj_post2_update_node(uint8_t *self, float dt) { gobj_walk(self, dt, 5); }

/*
 * void sBoundingBox::AddPoint(float x, float y, float z)
 * bb: min[0..2], max[3..5], center[6..8], size[9..11], radius[12], valid (byte @52)
 */
static void bbox_add_point(float *bb, float x, float y, float z) {
    uint8_t *valid = (uint8_t *)bb + 52;
    if (*valid) {
        if (bb[0] > x) bb[0] = x;
        if (bb[1] > y) bb[1] = y;
        if (bb[2] > z) bb[2] = z;
        if (bb[3] < x) bb[3] = x;
        if (bb[4] < y) bb[4] = y;
        if (bb[5] < z) bb[5] = z;
    } else {
        bb[0] = bb[3] = x;
        bb[1] = bb[4] = y;
        bb[2] = bb[5] = z;
        *valid = 1;
    }
    float sx = bb[3] - bb[0], sy = bb[4] - bb[1], sz = bb[5] - bb[2];
    bb[9] = sx; bb[10] = sy; bb[11] = sz;
    bb[6] = bb[0] + sx / 2.0f;
    bb[7] = bb[1] + sy / 2.0f;
    bb[8] = bb[2] + sz / 2.0f;
    bb[12] = __builtin_sqrtf((sx * sx + sy * sy) + sz * sz) / 2.0f;
}

/*
 * void CRenderNodeMesh::Collect() -- per visible mesh: frustum/area culling
 * (also per instance), optional occlusion-query bookkeeping, then one render
 * queue item per section x shader pass (+ shadow pass). Same control flow and
 * calls as the -O0 original; only the glue is optimized. Non-trivial helpers
 * are still the game's own functions.
 *
 * node: +4 scene, +8 area, +16 world matrix, +96 vector<InstanceInfo> (140 B:
 * local matrix @0, world @64, area @128, light @132, dirty @136),
 * +108 vector<CMatrix> (visible instance matrices), +148 occlusion state,
 * +152 query pending, +156 query id, +164 CSmartPtr<CRenderMesh>
 */
typedef struct { uint8_t *b, *e, *c; } rvec;
static void *(*rc_GetActiveCamera)(void);
static void *(*rc_GetBoundingBox)(void *mesh);
static void (*rc_MatVecClear)(void *v);
static void (*rc_MatVecPush)(void *v, const void *m);
static void (*rc_ItemPush)(void *v, const void *item);
static void (*rc_NodePtrPush)(void *v, void *const *node);
static void *(*rc_RMSingleton)(void);
static int (*rc_IsExtSupported)(void *rm, int ext);
static float (*rc_GetZNear)(void *cam);
static void (*rc_VecSubF)(float *out, const float *v, const float *f);
static void (*rc_VecAddF)(float *out, const float *v, const float *f);
static void *(*rc_GetInvView)(void *cam);
static void (*rc_GetPos)(float *out, const void *m);
static void (*rc_Inverse)(float *out, void *m);
static void (*rc_VecTransform)(float *out, const void *m, const float *v);
static void *(*rc_GetOcclusionQueue)(void *cam);
static void *(*rc_GetShader)(void *mat);
static unsigned (*rc_GetPassCount)(void *sh);
static void *(*rc_GetPass)(void *sh, unsigned i);
static int (*rc_PassGetQueue)(void *pass);
static void *(*rc_GetActiveQueue)(int q);
static unsigned (*rc_PassGetID)(void *pass);
static int (*rc_GetIntParam)(void *mat, unsigned i);
static void *(*rc_GeomGetBB)(void *geom);
static int (*rc_ShaderGetID)(void *sh);
static int (*rc_ShadowMapAvail)(void *rm);
static int (*rc_IsCastShadow)(void *node);
static void *(*rc_GetActiveScene)(void);
static void *(*rc_GetShadowShader)(void *scene);
static void (**rc_glGetQueryObjectuiv)(unsigned id, unsigned pname, unsigned *out);

static inline void rq_push(void *q, uint8_t *node, unsigned sec, void *pass, float key) {
    struct { float key; void *node; unsigned sec; void *pass; } item = { key, node, sec, pass };
    rvec *v = q;
    if (v->e != v->c) {            // vector<sRenderQueueItem>::push_back fast path
        if (v->e) memcpy(v->e, &item, 16);
        v->e += 16;
    } else {
        rc_ItemPush(q, &item);
    }
}

static inline void mat_push(rvec *v, const void *m, float light) {
    if (v->e != v->c) {
        if (v->e) memcpy(v->e, m, 64);
        v->e += 64;
    } else {
        rc_MatVecPush(v, m);
    }
    memcpy(v->e - 64 + 60, &light, 4);   // back()[15] = light
}

static void node_mesh_collect(uint8_t *self) {
    uint8_t *mesh = *(uint8_t **)(self + 164);
    if (!mesh) return;
    void *cam = rc_GetActiveCamera();
    int visible = 0;
    int sky = mesh_is_skydome(mesh);
    int inst = mesh_is_instance(mesh);
    uint8_t *scene = *(uint8_t **)(self + 4);

    if (!inst) {
        if (cam_is_area_visible(cam, *(int *)(self + 8))) {
            if (!sky) {
                if (cam_is_visible(cam, (const float *)(self + 16), rc_GetBoundingBox(mesh))) visible = 1;
            } else {
                visible = 1;
            }
        }
    } else {
        rvec *iv = (rvec *)(self + 96);
        int n = (int)((unsigned)(iv->e - iv->b) / 140u);
        if (n != 0) {
            uint8_t *base = iv->b;
            rvec *mv = (rvec *)(self + 108);
            rc_MatVecClear(mv);
            for (int i = 0; i < n; i++) {
                uint8_t *ii = base + i * 140;
                if (ii[136]) {
                    float tmp[16], pos[3];
                    mat_multiply(tmp, NULL, (const float *)ii, (const float *)(self + 16));
                    memcpy(ii + 64, tmp, 64);
                    rc_GetPos(pos, ii + 64);
                    if (*(int *)(ii + 128) != -2)
                        *(int *)(ii + 128) = scene_visibility_area(scene, pos);
                    float lmap = scene_light_from_map(scene, pos);
                    *(float *)(ii + 132) = lmap;
                    float lmesh = scene_light_from_mesh(scene, pos);
                    if (*(float *)(ii + 132) > lmesh) *(float *)(ii + 132) = lmesh;
                    ii[136] = 0;
                }
                if (cam_is_area_visible(cam, *(int *)(ii + 128))) {
                    if (!sky) {
                        if (cam_is_visible(cam, (const float *)(ii + 64), rc_GetBoundingBox(mesh))) {
                            mat_push(mv, ii + 64, *(float *)(ii + 132));
                            visible = 1;
                        }
                    } else {
                        mat_push(mv, ii + 64, *(float *)(ii + 132));
                        visible = 1;
                    }
                }
            }
        }
    }
    if (!visible) return;

    if (rc_IsExtSupported(rc_RMSingleton(), 8) && !inst && !sky) {
        if (self[152]) {
            unsigned avail = 0;
            (*rc_glGetQueryObjectuiv)(*(unsigned *)(self + 156), 0x8867, &avail);
            if (avail) {
                unsigned res = 0;
                (*rc_glGetQueryObjectuiv)(*(unsigned *)(self + 156), 0x8866, &res);
                *(int *)(self + 148) = res ? 2 : 0;
                self[152] = 0;
            }
        }
        float *bb = rc_GetBoundingBox(mesh);
        float zn = rc_GetZNear(cam);
        float f = zn + zn;
        float bmin[3], bmax[3], cp[3], inv[16], t[3];
        rc_VecSubF(bmin, bb, &f);
        rc_VecAddF(bmax, bb + 3, &f);
        rc_GetPos(cp, rc_GetInvView(cam));
        rc_Inverse(inv, self + 16);
        rc_VecTransform(t, inv, cp);
        memcpy(cp, t, 12);
        if (!(cp[0] < bmin[0]) && (cp[0] <= bmax[0]) &&
            !(cp[1] < bmin[1]) && (cp[1] <= bmax[1]) &&
            !(cp[2] < bmin[2]) && (cp[2] <= bmax[2])) {
            *(int *)(self + 148) = 2;
        } else if (!self[152]) {
            uint8_t *me = self;
            rc_NodePtrPush(rc_GetOcclusionQueue(cam), (void *const *)&me);
        }
    }

    if (*(int *)(self + 148) != 2) return;

    for (unsigned sec = 0; sec < (unsigned)((*(uint8_t **)(mesh + 16) - *(uint8_t **)(mesh + 12)) >> 3); sec++) {
        uint8_t *secp = *(uint8_t **)(mesh + 12) + sec * 8;
        void *mat = *(void **)secp, *geom = *(void **)(secp + 4);
        if (!mat || !rc_GetShader(mat)) continue;
        void *sh = rc_GetShader(mat);
        for (unsigned p = 0; p < rc_GetPassCount(sh); p++) {
            void *pass = rc_GetPass(sh, p);
            void *q = rc_GetActiveQueue(rc_PassGetQueue(pass));
            float key = (float)rc_PassGetID(pass);
            int qt = rc_PassGetQueue(pass);
            if (qt == 8) {
                key = (float)rc_GetIntParam(mat, 0);
            } else if (qt == 32) {
                float *gbb = rc_GeomGetBB(geom);
                float c[3], camp[3];
                rc_VecTransform(c, self + 16, gbb + 6);
                rc_GetPos(camp, rc_GetInvView(cam));
                float dx = c[0] - camp[0], dy = c[1] - camp[1], dz = c[2] - camp[2];
                key = (dx * dx + dy * dy) + dz * dz;
                if (rc_ShaderGetID(sh) == 9) key = key - 2.0f;
            }
            rq_push(q, self, sec, pass, key);
            if (rc_ShadowMapAvail(rc_RMSingleton()) && rc_IsCastShadow(self)) {
                void *spass = rc_GetPass(rc_GetShadowShader(rc_GetActiveScene()), inst ? 1 : 0);
                rq_push(rc_GetActiveQueue(64), self, sec, spass, key);
            }
        }
    }
}

static int install_collect(so_module *m) {
#define RS(var, sym) if (!(*(void **)&var = (void *)so_symbol(m, sym))) { l_warn("fastpath: collect missing %s", sym); return 0; }
    RS(rc_GetActiveCamera, "_ZN13CRenderCamera15GetActiveCameraEv");
    RS(rc_GetBoundingBox, "_ZN11CRenderMesh14GetBoundingBoxEv");
    RS(rc_MatVecClear, "_ZNSt6vectorI7CMatrixSaIS0_EE5clearEv");
    RS(rc_MatVecPush, "_ZNSt6vectorI7CMatrixSaIS0_EE9push_backERKS0_");
    RS(rc_ItemPush, "_ZNSt6vectorI16sRenderQueueItemSaIS0_EE9push_backERKS0_");
    RS(rc_NodePtrPush, "_ZNSt6vectorIP15CRenderNodeMeshSaIS1_EE9push_backERKS1_");
    RS(rc_RMSingleton, "_ZN10CSingletonI14CRenderManagerE12GetSingletonEv");
    RS(rc_IsExtSupported, "_ZN14CRenderManager20IsExtensionSupportedE16eRenderExtension");
    RS(rc_GetZNear, "_ZN13CRenderCamera8GetZNearEv");
    RS(rc_VecSubF, "_ZNK8CVector3IfEmiERKf");
    RS(rc_VecAddF, "_ZNK8CVector3IfEplERKf");
    RS(rc_GetInvView, "_ZN13CRenderCamera20GetInverseViewMatrixEv");
    RS(rc_GetPos, "_ZNK7CMatrix6GetPosEv");
    RS(rc_Inverse, "_ZN7CMatrix7InverseEv");
    RS(rc_VecTransform, "_ZNK7CMatrix12VecTransformERK8CVector3IfE");
    RS(rc_GetOcclusionQueue, "_ZN13CRenderCamera17GetOcclusionQueueEv");
    RS(rc_GetShader, "_ZNK15CRenderMaterial9GetShaderEv");
    RS(rc_GetPassCount, "_ZNK13CRenderShader12GetPassCountEv");
    RS(rc_GetPass, "_ZNK13CRenderShader7GetPassEj");
    RS(rc_PassGetQueue, "_ZN17CRenderShaderPass8GetQueueEv");
    RS(rc_GetActiveQueue, "_ZN13CRenderCamera14GetActiveQueueE12eRenderQueue");
    RS(rc_PassGetID, "_ZN17CRenderShaderPass5GetIDEv");
    RS(rc_GetIntParam, "_ZNK15CRenderMaterial11GetIntParamEj");
    RS(rc_GeomGetBB, "_ZNK15CRenderGeometry14GetBoundingBoxEv");
    RS(rc_ShaderGetID, "_ZN13CRenderShader5GetIDEv");
    RS(rc_ShadowMapAvail, "_ZNK14CRenderManager20IsShadowMapAvailableEv");
    RS(rc_IsCastShadow, "_ZN11CRenderNode12IsCastShadowEv");
    RS(rc_GetActiveScene, "_ZN12CRenderScene14GetActiveSceneEv");
    RS(rc_GetShadowShader, "_ZNK12CRenderScene15GetShadowShaderEv");
#undef RS
    uintptr_t collect = so_symbol(m, "_ZN15CRenderNodeMesh7CollectEv");
    // the original's GOT slot (0xa4fdac in 2.5.4) holding &glGetQueryObjectuivEXT pointer
    const uint32_t *f = (const uint32_t *)collect;
    if (!collect || f[0] != 0xE92D4830 || f[4] != 0xE59F4AA8 || f[0x4bc / 4] != 0xE59F3600) {
        l_warn("fastpath: Collect() not the expected 2.5.4 code, skipped");
        return 0;
    }
    uintptr_t got = collect + 0x14 + 8 + f[0xac0 / 4];                     // add r4, pc, r4
    rc_glGetQueryObjectuiv = *(void **)(got + (int32_t)f[0xac4 / 4]);    // ldr r3, [r4, r3]
    if (!setting_rw_collect) return 1;   // pointers resolved for Begin/render path, Collect not hooked
    hook_addr(collect, (uintptr_t)&node_mesh_collect);
    return 1;
}

/*
 * void CRenderShaderPass::Begin(CRenderMaterial *mat) -- binds the pass'
 * program, its textures (units 0-2 from the material, 3 = reflection cube,
 * 4 = shadow depth) and uploads its uniforms. The per-object uniforms are
 * skipped when the previous node drawn with this program is the same node
 * (map program -> last node). Passes using uniform types 18 (bones), 19
 * (sprite animation) or unknown ones go to the original code.
 *
 * pass: +4 program, +8 vector<uint> {type, location}..., +20/+24 blend
 * src/dst, +28 depth-write flag, +32 texture unit mask
 */
static void *(*sb_GetCurrentNode)(void);
static float (*sb_GetCurrentTime)(void *scene);
static void **(*sb_MapFind)(void *map, const unsigned *k);   // returns iterator (node ptr)
static void *(*sb_MapEnd)(void *map);
static void **(*sb_MapIndex)(void *map, const unsigned *k);  // returns &value
static void *(*sb_GetShadowRT)(void *scene);
static void (*sb_GetDepthTexture)(void **out_sp, void *rt);
static void (*sb_TexSPDtor)(void **sp);
static unsigned (*sb_TexGetID)(void *tex);
static void *(*sb_NodeGetRefl)(void *node);
static const char *(*sb_GetStringParam)(void *mat, unsigned i);
static void *(*sb_SceneGetReflNamed)(void *scene, const char *n);
static void *(*sb_SceneGetRefl)(void *scene);
static void (*sb_NodeSetRefl)(void *node, void *cube);
static void *(*sb_GetChecker)(void *rm, int i);
static const float *(*sb_GetZFactor)(void *cam);
static const float *(*sb_GetWorld)(void *node);
static const float *(*sb_GetViewProj)(void *cam);
static const float *(*sb_GetView)(void *cam);
static const float *(*sb_GetShadowVP)(void *scene);
static const float *(*sb_GetShadowTextVP)(void *scene);
static unsigned (*sb_GetInstanceCount)(void *node);
static const float *(*sb_GetFogColor)(void *scene);
static const float *(*sb_GetFogFactor)(void *scene);
static void *(*sb_GetColorParam)(void *mat, unsigned i);
static const float *(*sb_ColorToF)(void *c);
static float (*sb_GetFloatParam)(void *mat, unsigned i);
static const float *(*sb_GetSunDir)(void *scene);
static float (*sb_GetLightMul)(void *node);
static const float *(*sb_GetSH)(void *scene);
static void (*sb_SHMul)(float *out, const void *sh, float f);
static void *(*sb_GetDiffuse)(void *mat);
static void (*sb_NodeGetColor)(float *out, void *node);
static void (*sb_ColorMul)(float *out, const void *a, const void *b);
static const float *(*sb_GetFloat4Param)(void *mat, unsigned i);
static float (*sb_GetVColorOffset)(void *scene);
static unsigned (*sb_GetCaps)(void *rm);
static void (*sb_SetCaps)(void *rm, unsigned caps);
static void (*sb_MatCtor16)(float *m, float, float, float, float, float, float, float, float,
                            float, float, float, float, float, float, float, float);
static void (*sb_orig)(uint8_t *pass, uint8_t *mat);   // trampoline to the original
static void *sb_map;          // std::map<unsigned, CRenderNode*>
static unsigned *sb_cur_prog; // last glUseProgram
static int *sb_bound;         // bound texture id per unit [5]
static float sb_bias[16];

static void shader_pass_begin(uint8_t *self, uint8_t *mat) {
    if (!mat) return;

    const unsigned *u = *(const unsigned **)(self + 8), *ue = *(const unsigned **)(self + 12);
    unsigned nu = (unsigned)(ue - u);
    for (unsigned k = 0; k < nu; k += 2) {
        unsigned t = u[k];
        if (t == 18 || t == 19 || t > 27) { sb_orig(self, mat); return; }
    }

    void *last = NULL;
    void *node = sb_GetCurrentNode();
    void *scene = rc_GetActiveScene();
    void *cam = rc_GetActiveCamera();
    float now = scene ? sb_GetCurrentTime(scene) : 0.0f;

    void **it = sb_MapFind(sb_map, (const unsigned *)(self + 4));
    if ((void *)it != sb_MapEnd(sb_map)) {
        last = ((void **)((uint8_t *)it + 16))[1];
        ((void **)((uint8_t *)it + 16))[1] = node;
    } else {
        *sb_MapIndex(sb_map, (const unsigned *)(self + 4)) = node;
    }

    unsigned prog = *(unsigned *)(self + 4);
    if (prog != *sb_cur_prog) {
        glUseProgram(prog);
        *sb_cur_prog = *(unsigned *)(self + 4);
    }

    unsigned mask = *(unsigned *)(self + 32);
    for (int i = 0; i <= 4; i++) {
        if (!(((int)mask >> i) & 1)) continue;
        if (i == 4) {
            glActiveTexture(GL_TEXTURE4);
            if (scene && sb_GetShadowRT(scene)) {
                void *sp = NULL;
                sb_GetDepthTexture(&sp, sb_GetShadowRT(scene));
                void *tex = sp;
                sb_TexSPDtor(&sp);
                if (tex) {
                    unsigned id = sb_TexGetID(tex);
                    if ((unsigned)sb_bound[i] != id) {
                        glBindTexture(GL_TEXTURE_2D, id);
                        sb_bound[i] = id;
                    }
                } else {
                    sb_bound[i] = -1;
                }
            } else {
                sb_bound[i] = -1;
            }
        } else if (i == 3) {
            glActiveTexture(GL_TEXTURE3);
            void *refl = sb_NodeGetRefl(node);
            if (!refl) {
                const char *name = sb_GetStringParam(mat, 0);
                refl = (name && scene) ? sb_SceneGetReflNamed(scene, name) : NULL;
                if (!refl) refl = scene ? sb_SceneGetRefl(scene) : NULL;
                sb_NodeSetRefl(node, refl);
            }
            unsigned id = refl ? sb_TexGetID(refl) : 0;
            if (id) {
                if ((unsigned)sb_bound[i] != id) {
                    glBindTexture(GL_TEXTURE_CUBE_MAP, id);
                    sb_bound[i] = id;
                }
            } else {
                sb_bound[i] = -1;
            }
        } else {
            glActiveTexture(GL_TEXTURE0 + i);
            void *tex = *(void **)(mat + 8 + i * 4);
            unsigned id = tex ? sb_TexGetID(tex) : 0;
            if (id) {
                if ((unsigned)sb_bound[i] != id) {
                    glBindTexture(GL_TEXTURE_2D, id);
                    sb_bound[i] = id;
                }
            } else {
                sb_bound[i] = -1;
                int tm = (int)time(NULL);
                glBindTexture(GL_TEXTURE_2D, sb_TexGetID(sb_GetChecker(rc_RMSingleton(), tm % 2)));
            }
            unsigned wrap = *(unsigned *)(mat + 28 + i * 4);
            if (!(wrap & 1)) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            if (!(wrap & 2)) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
    }

    float tmp[16];
    for (unsigned k = 0; k < (unsigned)(*(const unsigned **)(self + 12) - *(const unsigned **)(self + 8)); k += 2) {
        const unsigned *uv = *(const unsigned **)(self + 8);
        int loc = (int)uv[k + 1];
        switch (uv[k]) {
        case 0:  // world * viewproj
            if (last != node) {
                mat_multiply(tmp, NULL, sb_GetWorld(node), sb_GetViewProj(cam));
                glUniformMatrix4fv(loc, 1, GL_FALSE, tmp);
            }
            break;
        case 1:  // world
            if (last != node) glUniformMatrix4fv(loc, 1, GL_FALSE, sb_GetWorld(node));
            break;
        case 2:  // view
            if (!last) glUniformMatrix4fv(loc, 1, GL_FALSE, sb_GetView(cam));
            break;
        case 3:  // viewproj
            if (!last) glUniformMatrix4fv(loc, 1, GL_FALSE, sb_GetViewProj(cam));
            break;
        case 4:  // world * view
            if (last != node) {
                mat_multiply(tmp, NULL, sb_GetWorld(node), sb_GetView(cam));
                glUniformMatrix4fv(loc, 1, GL_FALSE, tmp);
            }
            break;
        case 5:  // view-space depth row
            if (last != node) {
                if (sb_GetInstanceCount(node)) {
                    const float *v = sb_GetView(cam);
                    glUniform4f(loc, v[2], v[6], v[10], v[14]);
                } else {
                    mat_multiply(tmp, NULL, sb_GetWorld(node), sb_GetView(cam));
                    glUniform4f(loc, tmp[2], tmp[6], tmp[10], tmp[14]);
                }
            }
            break;
        case 6:  // camera position
            if (!last) {
                const float *iv = rc_GetInvView(cam);
                glUniform4f(loc, iv[12], iv[13], iv[14], 1.0f);
            }
            break;
        case 7:
            if (!last) {
                const float *c = sb_GetFogColor(scene);
                glUniform4f(loc, c[0], c[1], c[2], 1.0f);
            }
            break;
        case 8:
            if (!last) {
                const float *f = sb_GetFogFactor(scene);
                glUniform2f(loc, f[0], f[1]);
            }
            break;
        case 9: {
            const float *z = sb_GetZFactor(cam);
            glUniform2f(loc, z[0], z[1]);
            break;
        }
        case 10:
            glUniform3fv(loc, 1, sb_ColorToF(sb_GetColorParam(mat, 1)));
            break;
        case 11: {
            float a = sb_GetFloatParam(mat, 0) * 0.01f;
            float b = sb_GetFloatParam(mat, 1);
            glUniform2f(loc, a, b);
            break;
        }
        case 12:  // sun direction in view space
            if (!last) {
                const float *d = sb_GetSunDir(scene);
                const float *v = sb_GetView(cam);
                float x = (d[0] * v[0] + d[1] * v[4]) + d[2] * v[8];
                float y = (d[0] * v[1] + d[1] * v[5]) + d[2] * v[9];
                float z = (d[0] * v[2] + d[1] * v[6]) + d[2] * v[10];
                glUniform3f(loc, x, y, z);
            }
            break;
        case 13:  // spherical harmonics, scaled by the node light multiplier
            if (node != last) {
                if (sb_GetLightMul(node) != 1.0f) {
                    float sh[32];   // cSH2 = 3 x vec4 (48 bytes), margin
                    const float *base = sb_GetSH(scene);
                    sb_SHMul(sh, base, sb_GetLightMul(node));
                    glUniform4fv(loc, 3, sh);
                } else {
                    glUniform4fv(loc, 3, sb_GetSH(scene));
                }
            }
            break;
        case 14:
            if (!last) glUniform4f(loc, now, 0.0f, 0.0f, 0.0f);
            break;
        case 15:
            glUniform1f(loc, (float)rc_GetIntParam(mat, 0) / 255.0f);
            break;
        case 16: {
            float nc[8], prod[8];   // CColor = 4 floats, margin
            void *dc = sb_GetDiffuse(mat);
            sb_NodeGetColor(nc, node);
            sb_ColorMul(prod, dc, nc);
            glUniform4fv(loc, 1, prod);
            break;
        }
        case 17:  // world position
            if (node != last) {
                const float *w = sb_GetWorld(node);
                glUniform3f(loc, w[12], w[13], w[14]);
            }
            break;
        case 20:
            glUniform4fv(loc, 1, sb_GetFloat4Param(mat, 0));
            break;
        case 21:
            if (!last) glUniform1f(loc, sb_GetVColorOffset(scene));
            break;
        case 22:
            if (last != node) glUniform1f(loc, sb_GetLightMul(node));
            break;
        case 23:
            break;
        case 24:
            if (last != node) glUniformMatrix4fv(loc, 1, GL_FALSE, sb_GetShadowVP(scene));
            break;
        case 25:
            if (last != node) {
                mat_multiply(tmp, NULL, sb_GetWorld(node), sb_GetShadowVP(scene));
                glUniformMatrix4fv(loc, 1, GL_FALSE, tmp);
            }
            break;
        case 26:
            if (last != node) {
                mat_multiply(tmp, NULL, sb_GetWorld(node), sb_GetShadowTextVP(scene));
                glUniformMatrix4fv(loc, 1, GL_FALSE, tmp);
            }
            break;
        case 27:  // viewproj * texture bias
            mat_multiply(tmp, NULL, sb_GetViewProj(cam), sb_bias);
            glUniformMatrix4fv(loc, 1, GL_FALSE, tmp);
            break;
        }
    }

    unsigned src = *(unsigned *)(self + 20), dst = *(unsigned *)(self + 24);
    if (!(src == GL_SRC_ALPHA && dst == GL_ONE_MINUS_SRC_ALPHA))
        glBlendFunc(src, dst);
    if (*(unsigned *)(self + 28) == 0) {
        void *rm = rc_RMSingleton();
        sb_SetCaps(rm, sb_GetCaps(rc_RMSingleton()) & ~0xB44u);
    }
}

static void *exec_alloc(unsigned words) {
    static uint32_t *mem;
    static unsigned used;
    if (!mem) {
        SceKernelAllocMemBlockKernelOpt opt;
        memset(&opt, 0, sizeof(opt));
        opt.size = sizeof(opt);
        SceUID blk = kuKernelAllocMemBlock("fastpath_tramp", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 0x1000, &opt);
        if (blk < 0) return NULL;
        sceKernelGetMemBlockBase(blk, (void **)&mem);
        kuKernelMemProtect(mem, 0x1000, KU_KERNEL_PROT_EXEC | KU_KERNEL_PROT_WRITE | KU_KERNEL_PROT_READ);
    }
    if ((used + words) * 4 > 0x1000) return NULL;
    void *p = mem + used;
    used += words;
    return p;
}

static int install_shader_pass_begin(so_module *m) {
#define RS(var, sym) if (!(*(void **)&var = (void *)so_symbol(m, sym))) { l_warn("fastpath: Begin missing %s", sym); return 0; }
    RS(sb_GetCurrentNode, "_ZN11CRenderNode14GetCurrentNodeEv");
    RS(sb_GetCurrentTime, "_ZNK12CRenderScene14GetCurrentTimeEv");
    RS(sb_MapFind, "_ZNSt3mapIjP11CRenderNodeSt4lessIjESaISt4pairIKjS1_EEE4findERS5_");
    RS(sb_MapEnd, "_ZNSt3mapIjP11CRenderNodeSt4lessIjESaISt4pairIKjS1_EEE3endEv");
    RS(sb_MapIndex, "_ZNSt3mapIjP11CRenderNodeSt4lessIjESaISt4pairIKjS1_EEEixERS5_");
    RS(sb_GetShadowRT, "_ZNK12CRenderScene21GetShadowRenderTargetEv");
    RS(sb_GetDepthTexture, "_ZNK13CRenderTarget15GetDepthTextureEv");
    RS(sb_TexSPDtor, "_ZN9CSmartPtrI8CTextureED1Ev");
    RS(sb_TexGetID, "_ZNK8ITexture5GetIDEv");
    RS(sb_NodeGetRefl, "_ZN11CRenderNode16GetReflectionMapEv");
    RS(sb_GetStringParam, "_ZNK15CRenderMaterial14GetStringParamEj");
    RS(sb_SceneGetReflNamed, "_ZN12CRenderScene16GetReflectionMapEPKc");
    RS(sb_SceneGetRefl, "_ZN12CRenderScene16GetReflectionMapEv");
    RS(sb_NodeSetRefl, "_ZN11CRenderNode16SetReflectionMapEP12CTextureCube");
    RS(sb_GetChecker, "_ZNK14CRenderManager17GetCheckerTextureEi");
    RS(sb_GetZFactor, "_ZNK13CRenderCamera10GetZFactorEv");
    RS(sb_GetWorld, "_ZNK11CRenderNode14GetWorldMatrixEv");
    RS(sb_GetViewProj, "_ZN13CRenderCamera17GetViewProjMatrixEv");
    RS(sb_GetView, "_ZNK13CRenderCamera13GetViewMatrixEv");
    RS(sb_GetShadowVP, "_ZNK12CRenderScene17GetShadowViewProjEv");
    RS(sb_GetShadowTextVP, "_ZNK12CRenderScene21GetShadowTextViewProjEv");
    RS(sb_GetInstanceCount, "_ZN11CRenderNode16GetInstanceCountEv");
    RS(sb_GetFogColor, "_ZN12CRenderScene11GetFogColorEv");
    RS(sb_GetFogFactor, "_ZN12CRenderScene12GetFogFactorEv");
    RS(sb_GetColorParam, "_ZN15CRenderMaterial13GetColorParamEj");
    RS(sb_ColorToF, "_ZNK6CColorcvPKfEv");
    RS(sb_GetFloatParam, "_ZNK15CRenderMaterial13GetFloatParamEj");
    RS(sb_GetSunDir, "_ZN12CRenderScene15GetSunDirectionEv");
    RS(sb_GetLightMul, "_ZN11CRenderNode18GetLightMultiplierEv");
    RS(sb_GetSH, "_ZN12CRenderScene5GetSHEv");
    RS(sb_SHMul, "_ZNK4cSH2mlEf");
    RS(sb_GetDiffuse, "_ZN15CRenderMaterial15GetDiffuseColorEv");
    RS(sb_NodeGetColor, "_ZNK11CRenderNode8GetColorEv");
    RS(sb_ColorMul, "_ZNK6CColormlERKS_");
    RS(sb_GetFloat4Param, "_ZN15CRenderMaterial14GetFloat4ParamEj");
    RS(sb_GetVColorOffset, "_ZNK12CRenderScene20GetVertexColorOffsetEv");
    RS(sb_GetCaps, "_ZN14CRenderManager13GetCapabiliesEv");
    RS(sb_SetCaps, "_ZN14CRenderManager13SetCapabiliesEj");
    RS(sb_MatCtor16, "_ZN7CMatrixC1Effffffffffffffff");
#undef RS
    uintptr_t a = so_symbol(m, "_ZN17CRenderShaderPass5BeginEP15CRenderMaterial");
    const uint32_t *f = (const uint32_t *)a;
    // exact 2.5.4 code: prologue, GOT base load, the three global literals
    if (!a || f[0] != 0xE92D4870 || f[1] != 0xED2D8B04 || f[7] != 0xE59F4FBC || f[8] != 0xE08F4004) {
        l_warn("fastpath: ShaderPass::Begin not the expected 2.5.4 code, skipped");
        return 0;
    }
    uintptr_t got = a + 0x20 + 8 + f[0xfe0 / 4];
    sb_map      = *(void **)(got + (int32_t)f[0xfe4 / 4]);
    sb_cur_prog = *(unsigned **)(got + (int32_t)f[0xfec / 4]);
    sb_bound    = *(int **)(got + (int32_t)f[0xff0 / 4]);

    uint32_t *t = exec_alloc(4);
    if (!t) return 0;
    t[0] = f[0];          // push {r4, r5, r6, fp, lr}
    t[1] = f[1];          // vpush {d8-d9}
    t[2] = 0xE51FF004;    // ldr pc, [pc, #-4]
    t[3] = a + 8;
    kuKernelFlushCaches(t, 16);
    sb_orig = (void *)t;

    sb_MatCtor16(sb_bias, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f, 0.0f,
                 0.0f, 0.0f, 0.5f, 0.0f, 0.5f, 0.5f, 0.5f, 1.0f);
    if (!setting_rw_begin) return 0;
    hook_addr(a, (uintptr_t)&shader_pass_begin);
    return 1;
}

/*
 * void CRenderGeometry::LoadVertexData(unsigned flags, unsigned count,
 *                                      unsigned stride, void *data, unsigned extra)
 * Converts interleaved source vertices to the GPU layout, grows the bounding
 * box and uploads a VBO. Covers every format of the original (bone weights
 * 1-3 + indices, float or 10:10:10:2 normals, color, float/half texcoords,
 * 30x instancing replicas, 0.01 position scale): mesh loading and the
 * skidmarks (rebuilt every frame while they grow) both go through here.
 *
 * geom: +28 VBO, +32 vertex buffer (kept), +36 count, +40 format flags,
 * +44 GPU stride, +48 color offset, +56.. texcoord offsets, +100 sBoundingBox
 */
static void (*lv_orig)(uint8_t *g, unsigned flags, unsigned count, unsigned stride, const uint8_t *data, unsigned extra) __attribute__((unused));
static void (*lv_Unload)(uint8_t *g);
static void (*lv_BBoxCtor)(void *bb);
static void (*lv_BBoxAssign)(void *dst, const void *src);
static void *(*lv_NewArr)(unsigned size);

static inline uint32_t f16_bits(uint32_t x) {   // == ConvertToFloat16 (truncating)
    uint32_t sign = (x >> 16) & 0x8000;
    uint32_t e = (((x & 0x7F800000u) + 0xC8000000u) >> 13) & 0x7C00;
    uint32_t m = (x >> 13) & 0x3FF;
    return (sign | e | m) & 0xFFFF;
}

// == ConvertTo1010102(x, y, z): clamp to [-1,1], ceil((v*1023 - 1) * 0.5), pack 10:10:10
static inline int c1010102_comp(float v) {
    if (v < -1.0f) v = -1.0f;
    else if (v > 1.0f) v = 1.0f;
    return (int)__builtin_ceil((double)((v * 1023.0f - 1.0f) * 0.5f));
}
static inline uint32_t convert_1010102(float x, float y, float z) {
    uint32_t ix = (uint32_t)c1010102_comp(x), iy = (uint32_t)c1010102_comp(y), iz = (uint32_t)c1010102_comp(z);
    return (ix << 22) | ((iy << 12) & 0x3FF000u) | ((iz << 2) & 0xFFCu);
}

typedef struct { float w; int idx; } boneinfo;
static inline int bone_less(const boneinfo *a, const boneinfo *b) { return a->w > b->w; }

// std::sort on 4 elements == libstdc++ __insertion_sort (n < 16)
static void bone_sort4(boneinfo *f) {
    for (int i = 1; i < 4; i++) {
        boneinfo v = f[i];
        if (bone_less(&v, &f[0])) {
            for (int j = i; j > 0; j--) f[j] = f[j - 1];
            f[0] = v;
        } else {
            int j = i;
            while (bone_less(&v, &f[j - 1])) { f[j] = f[j - 1]; j--; }
            f[j] = v;
        }
    }
}

static void geom_load_vertex_data(uint8_t *g, unsigned flags, unsigned count, unsigned stride,
                                  const uint8_t *data, unsigned extra) {
    lv_Unload(g);
    *(unsigned *)(g + 40) = 0;
    uint8_t bb[128];
    lv_BBoxCtor(bb);
    lv_BBoxAssign(g + 100, bb);

    int ext1 = -1, ext2 = -1;
#define EXT1() (ext1 < 0 ? (ext1 = rc_IsExtSupported(rc_RMSingleton(), 1) ? 1 : 0) : ext1)
#define EXT2() (ext2 < 0 ? (ext2 = rc_IsExtSupported(rc_RMSingleton(), 2) ? 1 : 0) : ext2)

    unsigned dst = 0, src = 0, vf = 0;
    unsigned pos_src = 0, w_src = 0, bi_src = 0, n_src = 0, col_src = 0, uv_src[3] = { 0, 0, 0 };
    if (flags & 1) { pos_src = src; src += 12; vf |= 1; dst += 12; }
    if (flags & 4)       { w_src = src; src += 4; }
    else if (flags & 8)  { w_src = src; src += 8; }
    else if (flags & 16) { w_src = src; src += 12; }
    if (flags & 32) {
        *(unsigned *)(g + 68) = dst; dst += 4; vf |= 32;
        bi_src = src; src += 4;
    } else if (flags & 4) {
        *(unsigned *)(g + 68) = dst; dst += 4; vf |= 32;
    }
    if (flags & 64) {
        *(unsigned *)(g + 52) = dst; n_src = src; src += 12; vf |= 64;
        dst += EXT1() ? 4 : 12;
    }
    if (flags & 128) { *(unsigned *)(g + 48) = dst; col_src = src; src += 4; vf |= 128; dst += 4; }
    if (flags & 256) { src += 4; }   // skipped (never written)
    unsigned repl = 1;
    if (extra & 32) {
        repl = 30;
        *(unsigned *)(g + 72) = dst; vf |= 0x400000; dst += 4;
    }
    for (int t = 0; t <= 7; t++) {
        if (!(flags & (512u << t))) continue;
        if (t <= 2) {
            *(unsigned *)(g + (14 + t) * 4) = dst;
            uv_src[t] = src;
            vf |= 512u << t;
            dst += EXT2() ? 4 : 8;
        }
        src += 8;
    }
    *(unsigned *)(g + 40) = vf;
    (void)src;

    unsigned vsz = dst * count;   // one replica
    uint8_t *buf = lv_NewArr(vsz * repl);
    *(unsigned *)(g + 36) = count;
    *(unsigned *)(g + 44) = dst;

    uint8_t *d = buf;
    const uint8_t *sp = data;
    for (unsigned i = 0; i < count; i++, sp += stride) {
        if (vf & 1) {
            for (unsigned k = 0; k < repl; k++) {
                float *p = (float *)(d + k * vsz);
                memcpy(p, sp + pos_src, 12);
                if (extra & 2) {
                    p[0] = p[0] * 0.01f;
                    p[1] = p[1] * 0.01f;
                    p[2] = p[2] * 0.01f;
                }
            }
            float q[3];
            memcpy(q, d, 12);
            bbox_add_point((float *)(g + 100), q[0], q[1], q[2]);
            d += 12;
        }
        if (vf & 32) {
            boneinfo bi[4] = { { 1.0f, 0 }, { 0.0f, 0 }, { 0.0f, 0 }, { 0.0f, 0 } };
            if (flags & 2) {
                uint32_t iw; memcpy(&iw, sp + bi_src, 4);
                bi[0].idx = iw & 0xFF;
            } else if (flags & 4) {
                float w; memcpy(&w, sp + w_src, 4);
                bi[0].w = w;
                bi[1].w = 1.0f - bi[0].w;
                if (flags & 32) {
                    uint32_t iw; memcpy(&iw, sp + bi_src, 4);
                    bi[0].idx = iw & 0xFF;
                    bi[1].idx = (iw >> 8) & 0xFF;
                } else {
                    bi[0].idx = 0;
                    bi[1].idx = 1;
                }
            } else if (flags & 8) {
                float w[2]; memcpy(w, sp + w_src, 8);
                bi[0].w = w[0];
                bi[1].w = w[1];
                bi[2].w = (1.0f - bi[0].w) - bi[1].w;
                uint32_t iw; memcpy(&iw, sp + bi_src, 4);
                bi[0].idx = iw & 0xFF;
                bi[1].idx = (iw >> 8) & 0xFF;
                bi[2].idx = (iw >> 16) & 0xFF;
            } else if (flags & 16) {
                float w[3]; memcpy(w, sp + w_src, 12);
                bi[0].w = w[0];
                bi[1].w = w[1];
                bi[2].w = w[2];
                bi[3].w = ((1.0f - bi[0].w) - bi[1].w) - bi[2].w;
                uint32_t iw; memcpy(&iw, sp + bi_src, 4);
                bi[0].idx = iw & 0xFF;
                bi[1].idx = (iw >> 8) & 0xFF;
                bi[2].idx = (iw >> 16) & 0xFF;
                bi[3].idx = (int)(iw >> 24);
            }
            bone_sort4(bi);
            uint32_t packed = ((uint32_t)bi[0].idx & 0xFF) | (((uint32_t)bi[1].idx << 8) & 0xFFFF) |
                              (((uint32_t)(int)(bi[0].w * 255.0f) & 0xFF) << 16);
            for (unsigned k = 0; k < repl; k++) memcpy(d + k * vsz, &packed, 4);
            d += 4;
        }
        if (vf & 64) {
            if (EXT1()) {
                float n[3]; memcpy(n, sp + n_src, 12);
                uint32_t packed = convert_1010102(n[0], n[1], n[2]);
                for (unsigned k = 0; k < repl; k++) memcpy(d + k * vsz, &packed, 4);
                d += 4;
            } else {
                for (unsigned k = 0; k < repl; k++) memcpy(d + k * vsz, sp + n_src, 12);
                d += 12;
            }
        }
        if (vf & 128) {
            uint32_t v;
            memcpy(&v, sp + col_src, 4);
            v = (v & 0xFF000000u) | ((v & 0xFFu) << 16) | (v & 0xFF00u) | ((v & 0xFF0000u) >> 16);
            for (unsigned k = 0; k < repl; k++) memcpy(d + k * vsz, &v, 4);
            d += 4;
        }
        if (vf & 0x400000) {
            for (unsigned k = 0; k < repl; k++) { float f = (float)(int)k; memcpy(d + k * vsz, &f, 4); }
            d += 4;
        }
        for (int t = 0; t <= 2; t++) {
            if (!(vf & (512u << t))) continue;
            if (EXT2()) {
                uint32_t uv[2];
                memcpy(uv, sp + uv_src[t], 8);
                uint32_t packed = (f16_bits(uv[1]) << 16) | f16_bits(uv[0]);
                for (unsigned k = 0; k < repl; k++) memcpy(d + k * vsz, &packed, 4);
                d += 4;
            } else {
                for (unsigned k = 0; k < repl; k++) memcpy(d + k * vsz, sp + uv_src[t], 8);
                d += 8;
            }
        }
    }
#undef EXT1
#undef EXT2

    glGenBuffers(1, (GLuint *)(g + 28));
    glBindBuffer(GL_ARRAY_BUFFER, *(GLuint *)(g + 28));
    glBufferData(GL_ARRAY_BUFFER, repl * count * dst, buf, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    *(void **)(g + 32) = buf;
}

static int install_load_vertex_data(so_module *m) {
#define RS(var, sym) if (!(*(void **)&var = (void *)so_symbol(m, sym))) { l_warn("fastpath: LoadVertexData missing %s", sym); return 0; }
    RS(lv_Unload, "_ZN15CRenderGeometry16UnloadVertexDataEv");
    RS(lv_BBoxCtor, "_ZN12sBoundingBoxC1Ev");
    RS(lv_BBoxAssign, "_ZN12sBoundingBoxaSERKS_");
    RS(lv_NewArr, "_Znaj");
    RS(rc_RMSingleton, "_ZN10CSingletonI14CRenderManagerE12GetSingletonEv");
    RS(rc_IsExtSupported, "_ZN14CRenderManager20IsExtensionSupportedE16eRenderExtension");
#undef RS
    uintptr_t a = so_symbol(m, "_ZN15CRenderGeometry14LoadVertexDataEjjjPvj");
    const uint32_t *f = (const uint32_t *)a;
    // exact 2.5.4 code: prologue, 292-byte frame, the 0x281-relevant layout code
    if (!a || f[0] != 0xE92D4810 || f[1] != 0xE28DB008 || f[2] != 0xE24DDF49 ||
        f[(0x631d48 - 0x631480) / 4] != 0xEDDF7A0A) {
        l_warn("fastpath: LoadVertexData not the expected 2.5.4 code, skipped");
        return 0;
    }
    uint32_t *t = exec_alloc(4);
    if (!t) return 0;
    t[0] = f[0];          // push {r4, fp, lr}
    t[1] = f[1];          // add fp, sp, #8
    t[2] = 0xE51FF004;    // ldr pc, [pc, #-4]
    t[3] = a + 8;
    kuKernelFlushCaches(t, 16);
    lv_orig = (void *)t;
    if (!setting_rw_vertex) return 0;
    hook_addr(a, (uintptr_t)&geom_load_vertex_data);
    return 1;
}

/*
 * Per-draw path: CRenderNodeMesh::Render -> CRenderGeometry::Begin/BindVBO,
 * CRenderShaderPass::Begin/UpdateForInstance, CRenderGeometry::Draw,
 * CRenderShaderPass::End, CRenderGeometry::End. ~540 calls per frame on heavy
 * tracks. Same GL call sequence as the -O0 originals.
 *
 * geom: +8 vector<sVAO {attrs, vao}>, +20 used attrs, +24 bound VAO, +28 VBO,
 * +36 vertex count, +40 vertex flags, +44 stride, +48 color, +52 normal,
 * +56.. texcoord offsets, +68 bone weights?, +72 instance id, +76
 * CSmartPtr<cIndexData> (+8 IBO, +16 index count), +84 primitive
 */
static void (**gp_BindVAO)(unsigned vao);     // in Begin
static void (**gp_BindVAO_end)(unsigned vao); // in End (same variable)
static void (*gp_AddPolys)(void *rm, unsigned n);
static unsigned (*gp_UsedAttrs)(void *pass);
static void (*gp_PassBegin)(void *pass, void *mat);
static void (*gp_PassEnd)(void *pass, void *mat);
static void (*gp_GeomBegin)(void *g, unsigned attrs);
static void (*gp_GeomDraw)(void *g, int n);
static void (*gp_GeomEnd)(void *g);
static void (*gp_UpdForInst)(void *pass, unsigned n, void *mats);
static void **gp_cur_node;
static unsigned *gp_cur_section;

static void geom_bind_vbo(uint8_t *g, unsigned attrs) {
    if (!*(unsigned *)(g + 28)) return;
    unsigned vf = *(unsigned *)(g + 40), st = *(unsigned *)(g + 44);
    glBindBuffer(GL_ARRAY_BUFFER, *(unsigned *)(g + 28));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, st, (void *)0);
    if (attrs & 4) {
        glEnableVertexAttribArray(2);
        if (vf & 64) {
            if (rc_IsExtSupported(rc_RMSingleton(), 1))
                glVertexAttribPointer(2, 3, 0x8DF7 /* GL_INT_10_10_10_2_OES */, GL_TRUE, st, (void *)*(uintptr_t *)(g + 52));
            else
                glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, st, (void *)*(uintptr_t *)(g + 52));
        } else {
            glVertexAttribPointer(2, 3, GL_BYTE, GL_TRUE, 0, (void *)0);
        }
    } else {
        glDisableVertexAttribArray(2);
    }
    if (attrs & 8) {
        glEnableVertexAttribArray(3);
        if (vf & 128) glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, (void *)*(uintptr_t *)(g + 48));
        else          glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, (void *)0);
    } else {
        glDisableVertexAttribArray(3);
    }
    if (attrs & 128) {
        glEnableVertexAttribArray(7);
        if (vf & 0x400000) glVertexAttribPointer(7, 1, GL_FLOAT, GL_TRUE, st, (void *)*(uintptr_t *)(g + 72));
        else               glVertexAttribPointer(7, 1, GL_FLOAT, GL_TRUE, 0, (void *)0);
    } else {
        glDisableVertexAttribArray(7);
    }
    for (int t = 0; t <= 2; t++) {
        if (vf & (512u << t)) {
            glEnableVertexAttribArray(4 + t);
            void *off = (void *)*(uintptr_t *)(g + (14 + t) * 4);
            if (rc_IsExtSupported(rc_RMSingleton(), 2))
                glVertexAttribPointer(4 + t, 2, 0x8D61 /* GL_HALF_FLOAT_OES */, GL_FALSE, st, off);
            else
                glVertexAttribPointer(4 + t, 2, GL_FLOAT, GL_FALSE, st, off);
        } else {
            glDisableVertexAttribArray(4 + t);
        }
    }
    if (attrs & 2) {
        glEnableVertexAttribArray(1);
        if (vf & 32) glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_FALSE, st, (void *)*(uintptr_t *)(g + 68));
        else         glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, (void *)0);
    } else {
        glDisableVertexAttribArray(1);
    }
}

static void geom_begin(uint8_t *g, unsigned attrs) {
    *(unsigned *)(g + 24) = 0;
    *(unsigned *)(g + 20) = attrs;
    const unsigned *vao = *(const unsigned **)(g + 8), *vend = *(const unsigned **)(g + 12);
    unsigned n = (unsigned)(vend - vao) / 2;
    uint8_t *idx = *(uint8_t **)(g + 76);
    for (unsigned i = 0; i < n; i++) {
        if (vao[i * 2] == attrs) {
            *(unsigned *)(g + 24) = vao[1];          // (sic) the first entry's VAO
            (*gp_BindVAO)(vao[1]);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, *(unsigned *)(idx + 8));
            return;
        }
    }
    geom_bind_vbo(g, attrs);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, *(unsigned *)(idx + 8));
}

static void geom_draw(uint8_t *g, int n) {
    uint8_t *idx = *(uint8_t **)(g + 76);
    if (*(unsigned *)(idx + 8)) {
        glDrawElements(*(unsigned *)(g + 84), n * *(unsigned *)(idx + 16), GL_UNSIGNED_SHORT, (void *)0);
        gp_AddPolys(rc_RMSingleton(), (unsigned)(n * *(unsigned *)(idx + 16)) / 3u);
    } else {
        glDrawArrays(*(unsigned *)(g + 84), 0, n * *(unsigned *)(g + 36));
        gp_AddPolys(rc_RMSingleton(), (unsigned)(n * *(unsigned *)(g + 36)) / 3u);
    }
}

static void geom_end(uint8_t *g) {
    if (*(unsigned *)(g + 24)) { (*gp_BindVAO_end)(0); return; }
    unsigned a = *(unsigned *)(g + 20), vf = *(unsigned *)(g + 40);
    glDisableVertexAttribArray(0);
    if (a & 4) glDisableVertexAttribArray(2);
    if (a & 8) glDisableVertexAttribArray(3);
    if (a & 128) glDisableVertexAttribArray(7);
    for (int t = 0; t <= 2; t++)
        if (vf & (512u << t)) glDisableVertexAttribArray(4 + t);
    if (a & 2) glDisableVertexAttribArray(1);
}

static void pass_end(uint8_t *self, uint8_t *mat) {
    unsigned mask = *(unsigned *)(self + 32);
    for (int i = 0; i <= 4; i++) {
        if (!(((int)mask >> i) & 1) || i > 2) continue;
        if (!*(void **)(mat + 8 + i * 4)) continue;
        unsigned wrap = *(unsigned *)(mat + 28 + i * 4);
        if (wrap & 1) continue;
        glActiveTexture(GL_TEXTURE0 + i);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        if (!(wrap & 2)) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }
    if (!(*(unsigned *)(self + 20) == GL_SRC_ALPHA && *(unsigned *)(self + 24) == GL_ONE_MINUS_SRC_ALPHA))
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (*(unsigned *)(self + 28) == 0) {
        void *rm = rc_RMSingleton();
        sb_SetCaps(rm, sb_GetCaps(rc_RMSingleton()) | 0xB44u);
    }
}

static void pass_update_for_instance(uint8_t *self, unsigned n, void *mats) {
    for (unsigned k = 0; k < (unsigned)(*(const unsigned **)(self + 12) - *(const unsigned **)(self + 8)); k += 2) {
        const unsigned *uv = *(const unsigned **)(self + 8);
        if (uv[k] == 23) {
            if (n) glUniformMatrix4fv((int)uv[k + 1], n, GL_FALSE, mats);
            return;
        }
    }
}

static void node_mesh_render(uint8_t *self, unsigned sec, uint8_t *pass) {
    *gp_cur_node = self;
    *gp_cur_section = sec;
    uint8_t *mesh = *(uint8_t **)(self + 164);
    if (mesh) {
        uint8_t *secp = *(uint8_t **)(mesh + 12) + sec * 8;
        uint8_t *geom = *(uint8_t **)(secp + 4), *mat = *(uint8_t **)secp;
        gp_GeomBegin(geom, gp_UsedAttrs(pass));
        if (*(unsigned *)(geom + 40) & 0x400000) {
            *(unsigned *)(self + 120) = 0;
            rvec *mv = (rvec *)(self + 108);
            int n = (int)((unsigned)(mv->e - mv->b) / 64u);
            gp_PassBegin(pass, mat);
            while (n) {
                int k = n > 30 ? 30 : n;
                gp_UpdForInst(pass, k, ((rvec *)(self + 108))->b + *(unsigned *)(self + 120) * 64);
                gp_GeomDraw(geom, k);
                *(unsigned *)(self + 120) += k;
                n -= k;
            }
        } else {
            gp_PassBegin(pass, mat);
            gp_GeomDraw(geom, 1);
        }
        gp_PassEnd(pass, mat);
        gp_GeomEnd(geom);
    }
    *gp_cur_node = NULL;
}

static uintptr_t got_var(uintptr_t fn, unsigned add_off, unsigned base_lit_off, unsigned var_lit_off) {
    const uint32_t *f = (const uint32_t *)fn;
    uintptr_t got = fn + add_off + 8 + f[base_lit_off / 4];
    return *(uintptr_t *)(got + (int32_t)f[var_lit_off / 4]);
}

static int check_fn(uintptr_t a, uint32_t w0, uint32_t w1) {
    const uint32_t *f = (const uint32_t *)a;
    return a && f[0] == w0 && f[1] == w1;
}

static int install_render_path(so_module *m) {
    uintptr_t nmr = so_symbol(m, "_ZN15CRenderNodeMesh6RenderEjP17CRenderShaderPass");
    uintptr_t gb = so_symbol(m, "_ZN15CRenderGeometry5BeginEi");
    uintptr_t gd = so_symbol(m, "_ZN15CRenderGeometry4DrawEi");
    uintptr_t ge = so_symbol(m, "_ZN15CRenderGeometry3EndEv");
    uintptr_t gv = so_symbol(m, "_ZN15CRenderGeometry7BindVBOEi");
    uintptr_t pe = so_symbol(m, "_ZN17CRenderShaderPass3EndEP15CRenderMaterial");
    uintptr_t pu = so_symbol(m, "_ZN17CRenderShaderPass17UpdateForInstanceEjPv");
    uintptr_t pb = so_symbol(m, "_ZN17CRenderShaderPass5BeginEP15CRenderMaterial");
    if (!check_fn(nmr, 0xE92D4830, 0xE28DB00C) || ((uint32_t *)nmr)[6] != 0xE59F41EC || ((uint32_t *)nmr)[7] != 0xE08F4004 ||
        !check_fn(gb, 0xE92D4810, 0xE28DB008) || ((uint32_t *)gb)[5] != 0xE59F412C || ((uint32_t *)gb)[6] != 0xE08F4004 ||
        !check_fn(ge, 0xE92D4800, 0xE28DB004) || ((uint32_t *)ge)[4] != 0xE59F20F8 || ((uint32_t *)ge)[5] != 0xE08F2002 ||
        !check_fn(gd, 0xE92D4810, 0xE28DB008) || !check_fn(gv, 0xE92D4800, 0xE28DB004) ||
        !check_fn(pe, 0xE92D4810, 0xE28DB008) || !check_fn(pu, 0xE92D4800, 0xE28DB004) || !pb) {
        l_warn("fastpath: render path not the expected 2.5.4 code, skipped");
        return 0;
    }
    if (!(*(void **)&gp_AddPolys = (void *)so_symbol(m, "_ZN14CRenderManager18AddToDrawPolyCountEj")) ||
        !(*(void **)&gp_UsedAttrs = (void *)so_symbol(m, "_ZNK17CRenderShaderPass17GetUsedAttributesEv")) ||
        !sb_GetCaps || !sb_SetCaps || !rc_RMSingleton || !rc_IsExtSupported) {
        l_warn("fastpath: render path missing helpers, skipped");
        return 0;
    }
    // globals referenced through the GOT (offsets of the add-pc and literals in 2.5.4)
    gp_cur_node    = (void **)got_var(nmr, 0x1c, 0x20c, 0x210);
    gp_cur_section = (unsigned *)got_var(nmr, 0x1c, 0x20c, 0x214);
    gp_BindVAO     = (void *)got_var(gb, 0x18, 0x148, 0x14c);
    gp_BindVAO_end = (void *)got_var(ge, 0x14, 0x110, 0x114);

    // callers inside the path go through the (hooked) entry points, so the
    // profiler still sees each function
    gp_PassBegin = (void *)pb;  gp_PassEnd = (void *)pe;
    gp_GeomBegin = (void *)gb;  gp_GeomDraw = (void *)gd;  gp_GeomEnd = (void *)ge;
    gp_UpdForInst = (void *)pu;

    if (!setting_rw_render) return 0;
    hook_addr(gv, (uintptr_t)&geom_bind_vbo);
    hook_addr(gb, (uintptr_t)&geom_begin);
    hook_addr(gd, (uintptr_t)&geom_draw);
    hook_addr(ge, (uintptr_t)&geom_end);
    hook_addr(pe, (uintptr_t)&pass_end);
    hook_addr(pu, (uintptr_t)&pass_update_for_instance);
    hook_addr(nmr, (uintptr_t)&node_mesh_render);
    return 7;
}

/*
 * CTextureManager::FindFile(name, flags) -> std::string: probes the ETC /
 * PVRTC variants and every common texture path (CFile::Exists on loose files
 * and in the pack) to resolve a texture name. Nothing it looks at changes
 * while the game runs, except the directory lists, so the result is memoized
 * per (manager, name, flags) and the memo is dropped whenever
 * AddCommonTexturesPath / SetPVRTCETCDir change those lists.
 * The result is rebuilt with the game's own std::string() + assign(const char*).
 */
static void *(*ff_orig)(void *out, void *self, const char *name, int flags);
static void (*ff_add_orig)(void *self, const char *path);
static void (*ff_dir_orig)(void *self, const char *a, const char *b);
static void (*ff_str_ctor)(void *s);
static void (*ff_str_assign)(void *s, const char *c);

#define FF_SIZE 8192
typedef struct { uint32_t h; int flags; void *self; char *name, *res; } ff_ent;
static ff_ent ff_tab[FF_SIZE];
static int ff_used;
static pthread_mutex_t ff_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t ff_hash(const char *s, int flags, void *self) {
    uint32_t h = 2166136261u ^ (uint32_t)flags ^ (uint32_t)(uintptr_t)self;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h ? h : 1;
}

static void ff_clear(void) {
    pthread_mutex_lock(&ff_lock);
    for (int i = 0; i < FF_SIZE; i++) {
        free(ff_tab[i].name); free(ff_tab[i].res);
        ff_tab[i].name = ff_tab[i].res = NULL; ff_tab[i].h = 0;
    }
    ff_used = 0;
    pthread_mutex_unlock(&ff_lock);
}

static void *find_file(void *out, void *self, const char *name, int flags) {
    if (!name) return ff_orig(out, self, name, flags);
    uint32_t h = ff_hash(name, flags, self);
    pthread_mutex_lock(&ff_lock);
    for (int n = 0, i = h % FF_SIZE; n < 16; n++, i = (i + 1) % FF_SIZE) {
        ff_ent *e = &ff_tab[i];
        if (!e->h) break;
        if (e->h == h && e->flags == flags && e->self == self && strcmp(e->name, name) == 0) {
            char *res = e->res;
            pthread_mutex_unlock(&ff_lock);
            ff_str_ctor(out);
            ff_str_assign(out, res);
            return out;
        }
    }
    pthread_mutex_unlock(&ff_lock);

    ff_orig(out, self, name, flags);
    const char *res = *(const char **)out;   // COW std::string: pointer to the chars
    if (!res) return out;
    pthread_mutex_lock(&ff_lock);
    if (ff_used < FF_SIZE * 3 / 4) {
        for (int n = 0, i = h % FF_SIZE; n < 16; n++, i = (i + 1) % FF_SIZE) {
            ff_ent *e = &ff_tab[i];
            if (e->h) continue;
            e->name = strdup(name);
            e->res = strdup(res);
            if (e->name && e->res) {
                e->flags = flags; e->self = self; e->h = h; ff_used++;
            } else {
                free(e->name); free(e->res); e->name = e->res = NULL;
            }
            break;
        }
    }
    pthread_mutex_unlock(&ff_lock);
    return out;
}

static void ff_add_common_path(void *self, const char *path) { ff_clear(); ff_add_orig(self, path); }
static void ff_set_dirs(void *self, const char *a, const char *b) { ff_clear(); ff_dir_orig(self, a, b); }

static void *make_tramp(uintptr_t a) {
    const uint32_t *f = (const uint32_t *)a;
    uint32_t *t = exec_alloc(4);
    if (!t) return NULL;
    t[0] = f[0]; t[1] = f[1];
    t[2] = 0xE51FF004;    // ldr pc, [pc, #-4]
    t[3] = a + 8;
    kuKernelFlushCaches(t, 16);
    return t;
}

// for other files (controls_rr.c): trampoline to the original code at a
void *fp_make_tramp(uintptr_t a) { return make_tramp(a); }

static int install_find_file_memo(so_module *m) {
    uintptr_t ff = so_symbol(m, "_ZN15CTextureManager8FindFileEPKci");
    uintptr_t add = so_symbol(m, "_ZN15CTextureManager21AddCommonTexturesPathEPKc");
    uintptr_t dir = so_symbol(m, "_ZN15CTextureManager14SetPVRTCETCDirEPKcS1_");
    uintptr_t ctor = m->load_addr + 0x817028, asg = m->load_addr + 0x817f14;
    const uint32_t *c = (const uint32_t *)ctor, *g = (const uint32_t *)asg;
    // 2.5.4 only: prologues (push + add fp) and the two libstdc++ string helpers
    if (!check_fn(ff, 0xE92D4810, 0xE28DB008) || !check_fn(add, 0xE92D4830, 0xE28DB00C) ||
        !check_fn(dir, 0xE92D4800, 0xE28DB004) ||
        c[0] != 0xE59F300C || c[1] != 0xE08F3003 || c[2] != 0xE283300C || c[3] != 0xE5803000 ||
        g[0] != 0xE92D4038 || g[1] != 0xE1A05000 || g[2] != 0xE1A00001) {
        l_warn("fastpath: FindFile memo: unexpected code, skipped");
        return 0;
    }
    ff_str_ctor = (void *)ctor;
    ff_str_assign = (void *)asg;
    if (!(*(void **)&ff_orig = make_tramp(ff)) || !(*(void **)&ff_add_orig = make_tramp(add)) ||
        !(*(void **)&ff_dir_orig = make_tramp(dir)))
        return 0;
    if (!setting_rw_findfile) return 0;
    hook_addr(add, (uintptr_t)&ff_add_common_path);
    hook_addr(dir, (uintptr_t)&ff_set_dirs);
    hook_addr(ff, (uintptr_t)&find_file);
    return 3;
}

/*
 * CFile::Read(void *buf, unsigned n) for compressed pack entries (TEPK: zlib
 * chunks of up to 64 KB). The original reads + decompresses every chunk that
 * overlaps the request on *every* call and throws it away, so FMOD (which
 * reads sounds and streams music through CFile in 2 KB pieces) makes it
 * decompress each 64 KB chunk ~32 times. Same algorithm here, but the last
 * decompressed chunks are kept in a small cache keyed by the chunk's position
 * in the pack and its sizes: the OBB is read-only, so a cached chunk is always
 * valid (no dependence on CFile / pack entry pointers being reused).
 * Uncompressed / loose / asset files go to the original code.
 *
 * CFile: +8 FILE*, +20 pack entry (+4 compressed flag, +8 base offset),
 * +24 compress data {chunk table (12 B: off, usize, csize), nchunks,
 * max chunk size, total size, position}
 */
static unsigned (*cf_read_orig)(uint8_t *f, uint8_t *buf, unsigned n);
static int fast_uncompress(Bytef *dest, uLongf *destLen, const Bytef *src, uLong srcLen);
size_t fread_rr(void *ptr, size_t size, size_t count, FILE *f);
int fseek_rr(FILE *f, long off, int whence);

#define CK_N 8
// key = where the chunk is in the pack + its sizes: content-addressed, the OBB never changes
typedef struct { uint32_t abs_off, csize, usize; unsigned len, stamp, cap; uint8_t *data; } ck_ent;
static ck_ent ck[CK_N];
static unsigned ck_clock;
static pthread_mutex_t ck_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned cfile_read(uint8_t *f, uint8_t *buf, unsigned n) {
    FILE *fp = *(FILE **)(f + 8);
    uint8_t *pi = *(uint8_t **)(f + 20);
    if (!fp || !pi || !*(int *)(pi + 4)) return cf_read_orig(f, buf, n);

    uint32_t *cd = *(uint32_t **)(f + 24);
    const uint32_t *chunks = (const uint32_t *)cd[0];
    unsigned nch = cd[1], maxbuf = cd[2], total = cd[3];
    if (cd[4] + n > total) n = total - cd[4];

    int start = -1;
    unsigned acc = 0;
    for (unsigned i = 0; i < nch; i++) {
        if ((int)cd[4] < (int)acc) break;
        start = (int)i;
        acc += chunks[i * 3 + 1];
    }
    if (start < 0) return 0;

    unsigned got = 0, ustart = 0;
    uint8_t *dst = buf;
    uint8_t *cbuf = NULL, *ubuf = NULL;   // only allocated on a cache miss
    for (unsigned k = 0; k < nch; k++) {
        const uint32_t *ch = &chunks[k * 3];
        if ((unsigned)start <= k) {
            const uint8_t *data = NULL;
            unsigned dl = 0;
            uint32_t abs_off = *(uint32_t *)(pi + 8) + ch[0];
            pthread_mutex_lock(&ck_lock);
            for (int i = 0; i < CK_N; i++) {
                if (ck[i].data && ck[i].abs_off == abs_off && ck[i].csize == ch[2] && ck[i].usize == ch[1]) {
                    ck[i].stamp = ++ck_clock;
                    // copy out under the lock: the slot may be recycled by another thread
                    dl = ck[i].len;
                    unsigned offin = cd[4] - ustart, c = n;
                    if (offin + c > dl) c = dl - offin;
                    memcpy(dst, ck[i].data + offin, c);
                    pthread_mutex_unlock(&ck_lock);
                    data = (const uint8_t *)1;
                    cd[4] += c; got += c; dst += c; n -= c;
                    break;
                }
            }
            if (!data) {
                pthread_mutex_unlock(&ck_lock);
                if (!cbuf) {
                    cbuf = malloc(maxbuf);
                    ubuf = malloc(maxbuf);
                    if (!cbuf || !ubuf) { free(cbuf); free(ubuf); return got; }
                }
                fseek_rr(fp, abs_off, SEEK_SET);
                unsigned r = (unsigned)fread_rr(cbuf, 1, ch[2], fp);
                if (r != ch[2]) break;
                uLongf ul = maxbuf;
                fast_uncompress(ubuf, &ul, cbuf, r);
                dl = (unsigned)ul;
                if (ch[1] != dl) break;
                unsigned offin = cd[4] - ustart, c = n;
                if (offin + c > dl) c = dl - offin;
                memcpy(dst, ubuf + offin, c);
                cd[4] += c; got += c; dst += c; n -= c;

                // remember it (least recently used slot)
                pthread_mutex_lock(&ck_lock);
                int v = 0;
                for (int i = 1; i < CK_N; i++) if (ck[i].stamp < ck[v].stamp) v = i;
                if (!ck[v].data || ck[v].cap < dl) {
                    free(ck[v].data);
                    ck[v].data = malloc(dl > 65536 ? dl : 65536);
                    ck[v].cap = ck[v].data ? (dl > 65536 ? dl : 65536) : 0;
                }
                if (ck[v].data) {
                    memcpy(ck[v].data, ubuf, dl);
                    ck[v].abs_off = abs_off; ck[v].csize = ch[2]; ck[v].usize = ch[1];
                    ck[v].len = dl; ck[v].stamp = ++ck_clock;
                }
                pthread_mutex_unlock(&ck_lock);
            }
            if (n == 0) break;
        }
        ustart += ch[1];
    }
    free(cbuf);
    free(ubuf);
    return got;
}

static int install_cfile_read(so_module *m) {
    uintptr_t a = so_symbol(m, "_ZN5CFile4ReadEPvj");
    const uint32_t *f = (const uint32_t *)a;
    // 2.5.4: push {fp, lr}; add fp, sp, #4; sub sp, sp, #88; str r0, [fp, #-80]
    if (!a || f[0] != 0xE92D4800 || f[1] != 0xE28DB004 || f[2] != 0xE24DD058 || f[3] != 0xE50B0050) {
        l_warn("fastpath: CFile::Read not the expected 2.5.4 code, skipped");
        return 0;
    }
    if (!setting_rw_cfile) return 0;
    if (!(*(void **)&cf_read_orig = make_tramp(a))) return 0;
    hook_addr(a, (uintptr_t)&cfile_read);
    return 1;
}

static uint32_t sym_size(so_module *m, const char *name, uintptr_t *addr) {
    for (uint32_t i = 0; i < m->num_dynsym; i++) {
        Elf32_Sym *s = &m->dynsym[i];
        if (s->st_shndx && strcmp(m->dynstr + s->st_name, name) == 0) {
            *addr = m->load_addr + s->st_value;
            return s->st_size;
        }
    }
    return 0;
}

/*
 * The game links its own zlib 1.2.8 built at -O0; every asset read from the
 * OBB goes through its inflate(), which makes track loading take 20+ s.
 * Route the whole decompression API to the SDK's optimized zlib. z_stream is
 * unchanged between 1.2.8 and 1.3, and the internal inflate state is only
 * touched by inflate* functions, so all of them must move together.
 * Pure helpers (crc32, adler32) and uncompress go too. deflate is left alone.
 */
/*
 * uncompress() (used for the 64 KB TEPK chunks of the OBB) through libdeflate,
 * which is much faster than zlib's inflate. Any result other than success is
 * redone with zlib so errors and odd streams behave exactly as before.
 * Decompressors are not thread-safe: a small pool shared by the loader threads.
 */
#define LD_POOL 8
static struct libdeflate_decompressor *ld_pool[LD_POOL];
static int ld_free = 0;
static pthread_mutex_t ld_lock = PTHREAD_MUTEX_INITIALIZER;

static struct libdeflate_decompressor *ld_take(void) {
    struct libdeflate_decompressor *d = NULL;
    pthread_mutex_lock(&ld_lock);
    if (ld_free > 0) d = ld_pool[--ld_free];
    pthread_mutex_unlock(&ld_lock);
    return d ? d : libdeflate_alloc_decompressor();
}

static void ld_give(struct libdeflate_decompressor *d) {
    if (!d) return;
    pthread_mutex_lock(&ld_lock);
    if (ld_free < LD_POOL) { ld_pool[ld_free++] = d; d = NULL; }
    pthread_mutex_unlock(&ld_lock);
    if (d) libdeflate_free_decompressor(d);
}

static int fast_uncompress(Bytef *dest, uLongf *destLen, const Bytef *src, uLong srcLen) {
    if (!setting_deflate) return uncompress(dest, destLen, src, srcLen);
    struct libdeflate_decompressor *d = ld_take();
    if (d && dest && destLen && src) {
        size_t in_used = 0, out_used = 0;
        enum libdeflate_result r = libdeflate_zlib_decompress_ex(d, src, srcLen, dest, *destLen,
                                                                 &in_used, &out_used);
        ld_give(d);
        if (r == LIBDEFLATE_SUCCESS) { *destLen = out_used; return Z_OK; }
    } else {
        ld_give(d);
    }
    return uncompress(dest, destLen, src, srcLen);
}

#ifdef PERF_PROFILE
#include <psp2/kernel/processmgr.h>
void perf_zlib(uint64_t us);
void perf_deflate_check(int ok);
static int zl_inflate(z_streamp s, int f) {
    uint64_t t = sceKernelGetProcessTimeWide();
    int r = inflate(s, f);
    perf_zlib(sceKernelGetProcessTimeWide() - t);
    return r;
}
static int zl_uncompress(Bytef *d, uLongf *dl, const Bytef *src, uLong sl) {
    uLongf cap = dl ? *dl : 0;
    uint64_t t = sceKernelGetProcessTimeWide();
    int r = fast_uncompress(d, dl, src, sl);
    perf_zlib(sceKernelGetProcessTimeWide() - t);
    // profiler build: verify the first chunks against zlib (not timed); verified
    // identical on hardware, so only a sample to keep load timings meaningful
    static volatile int checked;
    if (r == Z_OK && cap && cap <= 4 * 1024 * 1024 && checked < 2000) {
        checked++;
        Bytef *ref = malloc(cap);
        if (ref) {
            uLongf rl = cap;
            int rr = uncompress(ref, &rl, src, sl);
            perf_deflate_check(rr == Z_OK && rl == *dl && memcmp(ref, d, rl) == 0);
            free(ref);
        }
    }
    return r;
}
#define ZL_INFLATE zl_inflate
#define ZL_UNCOMPRESS zl_uncompress
#else
#define ZL_INFLATE inflate
#define ZL_UNCOMPRESS fast_uncompress
#endif

static int patch_zlib(so_module *m) {
    static const struct { const char *sym; void *fn; } z[] = {
        { "inflateInit_", (void *)inflateInit_ },
        { "inflateInit2_", (void *)inflateInit2_ },
        { "inflate", (void *)ZL_INFLATE },
        { "inflateEnd", (void *)inflateEnd },
        { "inflateReset", (void *)inflateReset },
        { "inflateReset2", (void *)inflateReset2 },
        { "inflateResetKeep", (void *)inflateResetKeep },
        { "inflateSetDictionary", (void *)inflateSetDictionary },
        { "inflateGetHeader", (void *)inflateGetHeader },
        { "inflateSync", (void *)inflateSync },
        { "inflateSyncPoint", (void *)inflateSyncPoint },
        { "inflateCopy", (void *)inflateCopy },
        { "inflatePrime", (void *)inflatePrime },
        { "inflateUndermine", (void *)inflateUndermine },
        { "inflateMark", (void *)inflateMark },
        { "uncompress", (void *)ZL_UNCOMPRESS },
        { "crc32", (void *)crc32 },
        { "adler32", (void *)adler32 },
    };
    // all-or-nothing: a half-redirected inflate API would corrupt streams
    uintptr_t addr[sizeof(z) / sizeof(z[0])];
    for (unsigned i = 0; i < sizeof(z) / sizeof(z[0]); i++) {
        if (!sym_size(m, z[i].sym, &addr[i]) || (addr[i] & 3)) {
            l_warn("fastpath: zlib %s not found, keeping the game's zlib", z[i].sym);
            return 0;
        }
    }
    for (unsigned i = 0; i < sizeof(z) / sizeof(z[0]); i++)
        hook_addr(addr[i], (uintptr_t)z[i].fn);
    return sizeof(z) / sizeof(z[0]);
}

int png_patch(so_module *m);

void fastpath_patch(void) {
    if (!setting_fastpath) {
        l_info("fastpath: disabled in config");
        return;
    }
    so_module *m = &so_mod;
    int n_pat = 0, n_rt = 0, n_c = 0;

    // 1. pattern rewrites over every function. Two passes: all matches are
    //    computed on the original code first (wrapper patterns inspect their
    //    callees), then applied.
    fn_lo = (uint32_t *)m->text_base;
    fn_hi = (uint32_t *)(m->text_base + m->text_size);
    typedef struct { uint32_t *f; uint8_t n; uint32_t w[6]; } rewrite;
    rewrite *rw = malloc(sizeof(rewrite) * 8192);
    int nrw = 0;
    for (uint32_t i = 0; i < m->num_dynsym && rw && nrw < 8192; i++) {
        Elf32_Sym *s = &m->dynsym[i];
        if (ELF32_ST_TYPE(s->st_info) != STT_FUNC || !s->st_shndx) continue;
        if ((s->st_value & 3) || s->st_size < 36 || (s->st_size & 3)) continue;
        uint32_t *f = (uint32_t *)(m->load_addr + s->st_value);
        if (f < fn_lo || f + s->st_size / 4 > fn_hi) continue;
        int dup = 0;
        for (int j = nrw - 1; j >= 0 && j >= nrw - 4; j--) if (rw[j].f == f) dup = 1;
        if (dup) continue;
        uint32_t out[8];
        int nw = match_pattern(f, s->st_size / 4, out);
        if (nw) {
            rw[nrw].f = f;
            rw[nrw].n = nw;
            memcpy(rw[nrw].w, out, nw * 4);
            nrw++;
        }
    }
    for (int i = 0; i < nrw; i++) memcpy(rw[i].f, rw[i].w, rw[i].n * 4);
    n_pat = nrw;
    free(rw);

    // 2. hand-written routines (only if the original still has its -O0 prologue)
    for (unsigned i = 0; i < sizeof(routines) / sizeof(routines[0]); i++) {
        uintptr_t a;
        uint32_t sz = sym_size(m, routines[i].sym, &a);
        uint32_t len = (routines[i].end - routines[i].start) * 4;
        uint32_t *f = (uint32_t *)a;
        if (!sz || (a & 3) || len > sz) { l_warn("fastpath: skip %s", routines[i].sym); continue; }
        if (f[0] != PRO0 && f[0] != 0xE92D4800 && f[0] != 0xE92D0810) { l_warn("fastpath: %s already patched?", routines[i].sym); continue; }
        memcpy(f, routines[i].start, len);
        n_rt++;
    }

    // CMatrix::CMatrix(bool identity): if (identity) Identity(); return this;
    {
        uintptr_t ctor, ident;
        uint32_t csz = sym_size(m, "_ZN7CMatrixC2Eb", &ctor);
        uint32_t isz = sym_size(m, "_ZN7CMatrix8IdentityEv", &ident);
        if (csz >= 12 && isz && !(ctor & 3) && !(ident & 3)) {
            uint32_t *f = (uint32_t *)ctor;
            uintptr_t b_pc = ctor + 8;
            f[0] = 0xE31100FF;                                   // tst r1, #255
            f[1] = 0x012FFF1E;                                   // bxeq lr
            f[2] = 0xEA000000 | (((ident - (b_pc + 8)) >> 2) & 0xFFFFFF); // b Identity
            n_rt++;
        }
    }

    // 3. C replacements
    uintptr_t a;
    if (sym_size(m, "_ZNK7CMatrix14MatrixMultiplyERKS_S1_", &a)) { hook_addr(a, (uintptr_t)&mat_multiply); n_c++; }
    if (sym_size(m, "_ZNK7CMatrix12VecTransformERK8CVector3IfE", &a)) { hook_addr(a, (uintptr_t)&mat_vec_transform); n_c++; }
    if (sym_size(m, "_ZN13CRenderCamera13IsAreaVisibleEi", &a)) { hook_addr(a, (uintptr_t)&cam_is_area_visible); n_c++; }
    if (sym_size(m, "_ZN12CRenderScene25GetLightingFactorFromMeshERK8CVector3IfE", &a)) { hook_addr(a, (uintptr_t)&scene_light_from_mesh); n_c++; }
    if (sym_size(m, "_ZN12CRenderScene22GetVisibilityAreaIndexERK8CVector3IfE", &a)) { hook_addr(a, (uintptr_t)&scene_visibility_area); n_c++; }
    if (sym_size(m, "_ZN12CRenderScene24GetLightingFactorFromMapERK8CVector3IfE", &a)) { hook_addr(a, (uintptr_t)&scene_light_from_map); n_c++; }
    if (sym_size(m, "_ZN7CMatrix12DoMatrixPassES_NS_11eMatrixPassE", &a)) { hook_addr(a, (uintptr_t)&mat_do_pass); n_c++; }
    if (sym_size(m, "_ZN11CRenderMesh9IsSkyDomeEv", &a)) { hook_addr(a, (uintptr_t)&mesh_is_skydome); n_c++; }
    if (sym_size(m, "_ZN11CRenderMesh10IsInstanceEv", &a)) { hook_addr(a, (uintptr_t)&mesh_is_instance); n_c++; }
    if (sym_size(m, "_ZN11IGameObject13PreUpdateNodeEf", &a)) { hook_addr(a, (uintptr_t)&gobj_pre_update_node); n_c++; }
    if (sym_size(m, "_ZN11IGameObject10UpdateNodeEf", &a)) { hook_addr(a, (uintptr_t)&gobj_update_node); n_c++; }
    if (sym_size(m, "_ZN11IGameObject14PostUpdateNodeEf", &a)) { hook_addr(a, (uintptr_t)&gobj_post_update_node); n_c++; }
    if (sym_size(m, "_ZN11IGameObject15Post2UpdateNodeEf", &a)) { hook_addr(a, (uintptr_t)&gobj_post2_update_node); n_c++; }
    if (sym_size(m, "_ZN12sBoundingBox8AddPointEfff", &a)) { hook_addr(a, (uintptr_t)&bbox_add_point); n_c++; }
    n_c += patch_rb_trees(m);
    n_c += patch_zlib(m);
    n_c += png_patch(m);   // pngfast.c: game's -O0 libpng 1.5 -> SDK libpng 1.6
    ComputeFrustumPlanes = (void *)so_symbol(m, "_ZN13CRenderCamera20ComputeFrustumPlanesEv");
    if (ComputeFrustumPlanes && sym_size(m, "_ZN13CRenderCamera9IsVisibleERK7CMatrixRK12sBoundingBox", &a)) {
        hook_addr(a, (uintptr_t)&cam_is_visible);
        n_c++;
        if (setting_rewrite) {
            int ok = install_collect(m);   // uses cam_is_visible & co. directly
            n_c += ok;
            if (ok) n_c += install_shader_pass_begin(m);   // shares rc_* pointers
            n_c += install_load_vertex_data(m);
            if (ok) n_c += install_render_path(m);         // needs sb_GetCaps/SetCaps
            n_c += install_find_file_memo(m);
            n_c += install_cfile_read(m);
        }
    }

    kuKernelFlushCaches((void *)m->text_base, m->text_size);
    l_info("fastpath: %d pattern rewrites, %d routines, %d C replacements", n_pat, n_rt, n_c);
}
