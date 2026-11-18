// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Optimized implementation of sub_82453768 - vertex transformation for 3D objects.
// Clanker function, but 80% faster. 

#include <stdafx.h>
#include <region/region.h>

#ifdef PPC_EXTERN_FUNC
PPC_EXTERN_FUNC(sub_8215A588);
PPC_EXTERN_FUNC(sub_82449820);
PPC_EXTERN_FUNC(sub_82449A98);
#else
extern void sub_8215A588(PPCContext& __restrict__ ctx, uint8_t* base);
extern void sub_82449820(PPCContext& __restrict__ ctx, uint8_t* base);
extern void sub_82449A98(PPCContext& __restrict__ ctx, uint8_t* base);
#endif

namespace sub_82453768_detail
{
    using V = simde__m128;

    inline V LoadVec(uint8_t* base, uint32_t ea)
    {
        return simde_mm_castsi128_ps(simde_mm_shuffle_epi8(
            simde_mm_load_si128((const simde__m128i*)(base + (ea & ~0xFu))),
            simde_mm_load_si128((const simde__m128i*)VectorMaskL)));
    }

    inline void StoreVec(uint8_t* base, uint32_t ea, V v)
    {
        simde_mm_store_si128((simde__m128i*)(base + (ea & ~0xFu)),
            simde_mm_shuffle_epi8(simde_mm_castps_si128(v),
                simde_mm_load_si128((const simde__m128i*)VectorMaskL)));
    }

    // Broadcast guest element E. Lanes are reversed (element E lives in lane 3-E), like vspltw.
    template <int E>
    inline V Splat(V v)
    {
        return simde_mm_castsi128_ps(
            simde_mm_shuffle_epi32(simde_mm_castps_si128(v), (3 - E) * 0x55));
    }

    // m0*a[0] + m1*a[1] + m2*a[2] + m3*a[3], unfused, left to right
    // (identical rounding to vmulfp128 followed by three vmaddfp).
    inline V MulRow(V a, V m0, V m1, V m2, V m3)
    {
        V r = simde_mm_mul_ps(m0, Splat<0>(a));
        r = simde_mm_add_ps(simde_mm_mul_ps(m1, Splat<1>(a)), r);
        r = simde_mm_add_ps(simde_mm_mul_ps(m2, Splat<2>(a)), r);
        return simde_mm_add_ps(simde_mm_mul_ps(m3, Splat<3>(a)), r);
    }

    inline float Lane0(V v) { return simde_mm_cvtss_f32(v); }
}

SB_HOOK(sub_82453768)
{
    using namespace sub_82453768_detail;

    constexpr uint32_t kCache     = 0x82A2D190; // { blk, matA, self } of the previous call
    constexpr uint32_t kIndexAddr = 0x82A2D1A0; // table record index
    constexpr uint32_t kTable     = 0x82C40CC0; // 384-byte records, matrix T at +0
    constexpr uint32_t kTable2    = 0x82C40C20; // same records, source for sub_8215A588 (+16 included)
    constexpr uint32_t kGlobalPtr = 0x82A2C4F8; // -> struct, matrix W at +288
    constexpr uint32_t kConstAddr = 0x82063038; // float constant used as w of the position

    const uint32_t self = ctx.r3.u32;
    const uint32_t matA = ctx.r4.u32;
    const uint32_t blk  = ctx.r5.u32;

    // Pointer cache: identical arguments as last time -> nothing to do.
    const uint32_t changed =
        (PPC_LOAD_U32(kCache + 8) - self) |
        (PPC_LOAD_U32(kCache + 0) - blk)  |
        (PPC_LOAD_U32(kCache + 4) - matA);
    PPC_STORE_U32(kCache + 0, blk);
    PPC_STORE_U32(kCache + 4, matA);
    PPC_STORE_U32(kCache + 8, self);
    if (changed == 0)
        return;

    // Same 384-byte guest frame as the original: the callees read their matrices from it.
    const auto     savedLr = ctx.lr;
    const uint32_t savedSp = ctx.r1.u32;
    const uint32_t sp      = savedSp - 384;
    PPC_STORE_U32(sp, savedSp); // back chain (stwu)
    ctx.r1.u32 = sp;

    // One flush-mode switch instead of five; the original also leaves it enabled.
    ctx.fpscr.enableFlushMode();

    const uint32_t rec = PPC_LOAD_U32(kIndexAddr) * 384u;

    V A[4], P[4], Q[4], T[4];
    for (int i = 0; i < 4; ++i)
    {
        A[i] = LoadVec(base, matA + i * 16);
        P[i] = LoadVec(base, blk + 240 + i * 16);
        Q[i] = LoadVec(base, blk + 304 + i * 16);
        T[i] = LoadVec(base, kTable + rec + i * 16);
    }

    for (int i = 0; i < 4; ++i)
    {
        StoreVec(base, sp + 128 + i * 16, MulRow(A[i], P[0], P[1], P[2], P[3]));
        StoreVec(base, sp + 192 + i * 16, MulRow(A[i], Q[0], Q[1], Q[2], Q[3]));
        StoreVec(base, sp + 256 + i * 16, MulRow(A[i], T[0], T[1], T[2], T[3]));
    }

    // Position (x, y, z, const) in guest element order, then the dot products with the
    // rows (A_i.xyz, -dot(A3.xyz, A_i.xyz)).
    const uint32_t constBits = PPC_LOAD_U32(kConstAddr);
    const V pos = simde_mm_castsi128_ps(simde_mm_set_epi32(
        (int)PPC_LOAD_U32(blk + 48), (int)PPC_LOAD_U32(blk + 52),
        (int)PPC_LOAD_U32(blk + 56), (int)constBits));
    const V W    = LoadVec(base, PPC_LOAD_U32(kGlobalPtr) + 288);
    const V e3   = simde_mm_set_ps(0.f, 0.f, 0.f, 1.f); // guest {0,0,0,1}
    const V xyz  = simde_mm_set_ps(1.f, 1.f, 1.f, 0.f); // guest {1,1,1,0}
    const V negA3 = simde_mm_mul_ps(A[3], simde_mm_set1_ps(-1.0f));

    float s[3], t[3];
    for (int i = 0; i < 3; ++i)
    {
        const V d   = simde_mm_mul_ps(simde_mm_dp_ps(negA3, A[i], 0xEF), e3);
        const V row = simde_mm_add_ps(simde_mm_mul_ps(xyz, A[i]), d);
        s[i] = Lane0(simde_mm_dp_ps(pos, row, 0xFF));
        t[i] = Lane0(simde_mm_dp_ps(W,   row, 0xFF));
    }
    float c;
    std::memcpy(&c, &constBits, sizeof(c));
    const float w3 = Lane0(W); // guest element 3 of W

    StoreVec(base, sp + 80,  simde_mm_set_ps(s[0], s[1], s[2], c));
    StoreVec(base, sp + 112, simde_mm_set_ps(t[0], t[1], t[2], w3));

    // Hand the results to the renderer. [self+12] / [self+28] are re-read before each use, like the original.
    const auto dev = [&] { return PPC_LOAD_U32(self + 12); };
    const auto gfx = [&] { return PPC_LOAD_U32(self + 28); };
    const auto call = [&](auto fn, uint32_t slot, uint32_t data)
    {
        ctx.r3.u64 = gfx();
        ctx.r4.u64 = slot;
        ctx.r5.u64 = data;
        fn(ctx, base);
    };

    if (const uint32_t slot = PPC_LOAD_U32(dev() + 572))
        call(sub_82449A98, slot, sp + 128);                 // A x P

    if (const uint32_t slot = PPC_LOAD_U32(dev() + 568))
        call(sub_82449A98, slot, matA);                     // A itself

    if (const uint32_t slot = PPC_LOAD_U32(dev() + 564))
    {
        ctx.r3.u64 = sp + 320;
        ctx.r4.u64 = kTable2 + rec;
        sub_8215A588(ctx, base);
        call(sub_82449820, slot, sp + 320);
    }

    if (const uint32_t slot = PPC_LOAD_U32(dev() + 468))
        call(sub_82449A98, slot, sp + 256);                 // A x T

    if (const uint32_t slot = PPC_LOAD_U32(dev() + 576))
        call(sub_82449A98, slot, sp + 192);                 // A x Q

    {
        const uint32_t d    = dev();
        const uint32_t s284 = PPC_LOAD_U32(d + 284);
        const uint32_t s292 = PPC_LOAD_U32(d + 292);
        if (s284)
            call(sub_82449820, s284, sp + 80);              // out1
        if (s292)
            call(sub_82449820, PPC_LOAD_U32(dev() + 292), sp + 112); // out2
    }

    ctx.r1.u32 = savedSp;
    ctx.lr = savedLr;
}