/*
 * fastpath_asm.S -- optimized bodies for hot -O0 leaf functions of
 * libRidgeRacer.so. Each routine is position-independent ARM code with no
 * literal pools; fastpath.c copies it over the original function body.
 *
 * ABI notes (softfp, ARM EABI):
 *  - constructors return `this` in r0
 *  - CVector3 returned by value goes through a hidden pointer in r0,
 *    `this` is then r1 and the first argument r2
 *  - floats are returned in r0
 *  - only s0-s15 (caller-saved) are used
 * Float operations keep the original evaluation order, so results are
 * bit-identical to the original code.
 */

    .syntax unified
    .arm
    .fpu vfpv3
    .text

#define ROUTINE(name) .global name; .global name##_end; .type name, %function; .balign 4; name:
#define END(name) name##_end:

/* CVector3<float>::CVector3(float x, float y, float z) */
ROUTINE(fp_v3_ctor3)
    stm     r0, {r1, r2, r3}
    bx      lr
END(fp_v3_ctor3)

/* CVector3<float>::CVector3()  -> zero */
ROUTINE(fp_v3_ctor0)
    mov     r1, #0
    mov     r2, #0
    mov     r3, #0
    stm     r0, {r1, r2, r3}
    bx      lr
END(fp_v3_ctor0)

/* CVector3<float>& CVector3<float>::operator=(const CVector3<float>&) */
ROUTINE(fp_v3_assign)
    ldm     r1, {r2, r3, r12}
    stm     r0, {r2, r3, r12}
    bx      lr
END(fp_v3_assign)

/* CVector2<float>::CVector2(float x, float y) */
ROUTINE(fp_v2_ctor2)
    stm     r0, {r1, r2}
    bx      lr
END(fp_v2_ctor2)

/* CVector3 CVector3::operator+(const CVector3&) const  (r0 = ret, r1 = this, r2 = rhs) */
ROUTINE(fp_v3_add)
    vldmia  r1, {s0-s2}
    vldmia  r2, {s4-s6}
    vadd.f32 s0, s0, s4
    vadd.f32 s1, s1, s5
    vadd.f32 s2, s2, s6
    vstmia  r0, {s0-s2}
    bx      lr
END(fp_v3_add)

/* CVector3 CVector3::operator-(const CVector3&) const */
ROUTINE(fp_v3_sub)
    vldmia  r1, {s0-s2}
    vldmia  r2, {s4-s6}
    vsub.f32 s0, s0, s4
    vsub.f32 s1, s1, s5
    vsub.f32 s2, s2, s6
    vstmia  r0, {s0-s2}
    bx      lr
END(fp_v3_sub)

/* CVector3 CVector3::operator*(const float&) const */
ROUTINE(fp_v3_mulf)
    vldmia  r1, {s0-s2}
    vldr    s4, [r2]
    vmul.f32 s0, s0, s4
    vmul.f32 s1, s1, s4
    vmul.f32 s2, s2, s4
    vstmia  r0, {s0-s2}
    bx      lr
END(fp_v3_mulf)

/* float CVector3::Dot(const CVector3&) const  = (x*x' + y*y') + z*z' */
ROUTINE(fp_v3_dot)
    vldmia  r0, {s0-s2}
    vldmia  r1, {s4-s6}
    vmul.f32 s0, s0, s4
    vmul.f32 s1, s1, s5
    vmul.f32 s2, s2, s6
    vadd.f32 s0, s0, s1
    vadd.f32 s0, s0, s2
    vmov    r0, s0
    bx      lr
END(fp_v3_dot)

/* float CVector2::Dot(const CVector2&) */
ROUTINE(fp_v2_dot)
    vldmia  r0, {s0-s1}
    vldmia  r1, {s4-s5}
    vmul.f32 s0, s0, s4
    vmul.f32 s1, s1, s5
    vadd.f32 s0, s0, s1
    vmov    r0, s0
    bx      lr
END(fp_v2_dot)

/* float CVector2::GetLength()  (original: (float)sqrt((double)(x*x + y*y)),
   which is exactly vsqrt.f32 because sqrt is correctly rounded) */
ROUTINE(fp_v2_len)
    vldmia  r0, {s0-s1}
    vmul.f32 s0, s0, s0
    vmul.f32 s1, s1, s1
    vadd.f32 s0, s0, s1
    vsqrt.f32 s0, s0
    vmov    r0, s0
    bx      lr
END(fp_v2_len)

/* void CVector2::Normalize()  x /= len; y /= len */
ROUTINE(fp_v2_normalize)
    vldmia  r0, {s0-s1}
    vmul.f32 s2, s0, s0
    vmul.f32 s3, s1, s1
    vadd.f32 s2, s2, s3
    vsqrt.f32 s2, s2
    vdiv.f32 s0, s0, s2
    vdiv.f32 s1, s1, s2
    vstmia  r0, {s0-s1}
    bx      lr
END(fp_v2_normalize)

/* void CMatrix::Identity()  (16 floats, row-major) */
ROUTINE(fp_mat_identity)
    mov     r1, #0x3f800000
    mov     r2, #0
    mov     r3, #0
    mov     r12, #0
    stm     r0, {r1, r2, r3, r12}
    str     r2, [r0, #16]
    str     r1, [r0, #20]
    str     r2, [r0, #24]
    str     r2, [r0, #28]
    str     r2, [r0, #32]
    str     r2, [r0, #36]
    str     r1, [r0, #40]
    str     r2, [r0, #44]
    str     r2, [r0, #48]
    str     r2, [r0, #52]
    str     r2, [r0, #56]
    str     r1, [r0, #60]
    bx      lr
END(fp_mat_identity)

/* bool std::less<int>::operator()(const int&, const int&) const */
ROUTINE(fp_less_int)
    ldr     r1, [r1]
    ldr     r2, [r2]
    cmp     r1, r2
    movge   r0, #0
    movlt   r0, #1
    bx      lr
END(fp_less_int)

/* bool std::less<unsigned>::operator()(const unsigned&, const unsigned&) const */
ROUTINE(fp_less_uint)
    ldr     r1, [r1]
    ldr     r2, [r2]
    cmp     r1, r2
    movcs   r0, #0
    movcc   r0, #1
    bx      lr
END(fp_less_uint)

/* CMatrix::CMatrix(const CMatrix&)  copy 16 floats, return this */
ROUTINE(fp_mat_copy)
    vldmia  r1, {s0-s15}
    vstmia  r0, {s0-s15}
    bx      lr
END(fp_mat_copy)

/* float CVector3::GetLength()  (float)sqrt((double)((x*x + y*y) + z*z)) */
ROUTINE(fp_v3_len)
    vldmia  r0, {s0-s2}
    vmul.f32 s0, s0, s0
    vmul.f32 s1, s1, s1
    vmul.f32 s2, s2, s2
    vadd.f32 s0, s0, s1
    vadd.f32 s0, s0, s2
    vsqrt.f32 s0, s0
    vmov    r0, s0
    bx      lr
END(fp_v3_len)

/* bool sRenderQueueCompare::operator()(const sRenderQueueItem &a, const sRenderQueueItem &b)
   original: vcmpe a.key, b.key; movpl 0; movmi 1  ->  a.key < b.key */
ROUTINE(fp_rq_compare)
    vldr    s0, [r1]
    vldr    s1, [r2]
    vcmpe.f32 s0, s1
    vmrs    APSR_nzcv, fpscr
    movpl   r0, #0
    movmi   r0, #1
    bx      lr
END(fp_rq_compare)

/* CVector3 CMatrix::GetRight/GetUp/GetAt() const  (r0 = ret, r1 = this) */
ROUTINE(fp_mat_right)
    ldm     r1, {r1, r2, r3}
    stm     r0, {r1, r2, r3}
    bx      lr
END(fp_mat_right)
ROUTINE(fp_mat_up)
    add     r1, r1, #16
    ldm     r1, {r1, r2, r3}
    stm     r0, {r1, r2, r3}
    bx      lr
END(fp_mat_up)
ROUTINE(fp_mat_at)
    add     r1, r1, #32
    ldm     r1, {r1, r2, r3}
    stm     r0, {r1, r2, r3}
    bx      lr
END(fp_mat_at)

/* std::vector<std::vector<Plane>>::operator[](n)  begin + n * 12 */
ROUTINE(fp_vec12_index)
    ldr     r2, [r0]
    add     r1, r1, r1, lsl #1
    add     r0, r2, r1, lsl #2
    bx      lr
END(fp_vec12_index)

/* CVector3 CMatrix::VecRotate(const CVector3 &v) const   (r0 = out, r1 = this, r2 = v)
   out.i = (v.x*m[i] + v.y*m[4+i]) + v.z*m[8+i] */
ROUTINE(fp_mat_vecrotate)
    vldmia  r2, {s0-s2}
    vldr    s4, [r1]
    vldr    s5, [r1, #16]
    vldr    s6, [r1, #32]
    vmul.f32 s8, s0, s4
    vmul.f32 s9, s1, s5
    vadd.f32 s8, s8, s9
    vmul.f32 s9, s2, s6
    vadd.f32 s8, s8, s9
    vldr    s4, [r1, #4]
    vldr    s5, [r1, #20]
    vldr    s6, [r1, #36]
    vmul.f32 s10, s0, s4
    vmul.f32 s9, s1, s5
    vadd.f32 s10, s10, s9
    vmul.f32 s9, s2, s6
    vadd.f32 s10, s10, s9
    vldr    s4, [r1, #8]
    vldr    s5, [r1, #24]
    vldr    s6, [r1, #40]
    vmul.f32 s12, s0, s4
    vmul.f32 s9, s1, s5
    vadd.f32 s12, s12, s9
    vmul.f32 s9, s2, s6
    vadd.f32 s12, s12, s9
    vstr    s8, [r0]
    vstr    s10, [r0, #4]
    vstr    s12, [r0, #8]
    bx      lr
END(fp_mat_vecrotate)

/* CRenderMaterial *CRenderMesh::GetMaterial(unsigned i)  sections[i].material
   (mesh+12: vector<sRenderMeshSection>, 8-byte { material, geometry }) */
ROUTINE(fp_mesh_getmat)
    ldr     r3, [r0, #12]
    ldr     r0, [r3, r1, lsl #3]
    bx      lr
END(fp_mesh_getmat)

/* CRenderGeometry *CRenderMesh::GetGeometry(unsigned i)  sections[i].geometry */
ROUTINE(fp_mesh_getgeom)
    ldr     r3, [r0, #12]
    add     r3, r3, r1, lsl #3
    ldr     r0, [r3, #4]
    bx      lr
END(fp_mesh_getgeom)

/* float CMatrix::Get(int row, int col) const   m[row*4 + col] */
ROUTINE(fp_mat_get)
    add     r3, r0, r1, lsl #4
    ldr     r0, [r3, r2, lsl #2]
    bx      lr
END(fp_mat_get)
