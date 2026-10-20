#ifndef SPU_HELPERS_VEC_H
#define SPU_HELPERS_VEC_H
#if defined(__SSE4_1__) && !defined(SPU_HELPERS_VEC_DISABLE)
#include <smmintrin.h>
#if defined(__AVX2__)
#include <immintrin.h>
#endif

static inline __m128i spu__vi(u128 a) { __m128i x; memcpy(&x, &a, 16); return x; }
static inline u128 spu__vo(__m128i x) { u128 a; memcpy(&a, &x, 16); return a; }
#define SPU__V1(name, expr) static inline u128 spu__v_##name(u128 a_) { const __m128i A = spu__vi(a_); return spu__vo(expr); }
#define SPU__V2(name, expr) static inline u128 spu__v_##name(u128 a_, u128 b_) { const __m128i A = spu__vi(a_), B = spu__vi(b_); return spu__vo(expr); }
#define SPU__V3(name, expr) static inline u128 spu__v_##name(u128 a_, u128 b_, u128 c_) { const __m128i A = spu__vi(a_), B = spu__vi(b_), C = spu__vi(c_); return spu__vo(expr); }
#define SPU__VI(name, T, expr) static inline u128 spu__v_##name(u128 a_, T imm) { const __m128i A = spu__vi(a_); return spu__vo(expr); }
#define SPU__ONES _mm_set1_epi32(-1)
#define SPU__S32 _mm_set1_epi32((int)0x80000000u)
#define SPU__S16 _mm_set1_epi16((short)0x8000)
#define SPU__S8  _mm_set1_epi8((char)0x80)

#define SPU__LO16S(X) _mm_srai_epi32(_mm_slli_epi32((X), 16), 16)
#define SPU__HI16S(X) _mm_srai_epi32((X), 16)
#define SPU__LO16U(X) _mm_and_si128((X), _mm_set1_epi32(0xFFFF))

static inline u128 spu__v_splat_u32(uint32_t v) { return spu__vo(_mm_set1_epi32((int)v)); }
static inline u128 spu__v_il(int32_t imm)    { return spu__vo(_mm_set1_epi32(imm)); }
static inline u128 spu__v_ila(uint32_t i18)  { return spu__vo(_mm_set1_epi32((int)(i18 & 0x3FFFF))); }
static inline u128 spu__v_ilhu(uint16_t imm) { return spu__vo(_mm_set1_epi32((int)((uint32_t)imm << 16))); }
static inline u128 spu__v_ilh(uint16_t imm)  { return spu__vo(_mm_set1_epi16((short)imm)); }
static inline u128 spu__v_link(uint32_t addr){ return spu__vo(_mm_cvtsi32_si128((int)addr)); }
static inline u128 spu__v_pref_u32(uint32_t v){ return spu__vo(_mm_cvtsi32_si128((int)v)); }
static inline u128 spu__v_iohl(u128 a_, uint16_t imm) { return spu__vo(_mm_or_si128(spu__vi(a_), _mm_set1_epi32((int)(uint32_t)imm))); }

SPU__V2(a,   _mm_add_epi32(A, B))
SPU__V2(sf,  _mm_sub_epi32(B, A))
SPU__V2(ah,  _mm_add_epi16(A, B))
SPU__V2(sfh, _mm_sub_epi16(B, A))
SPU__VI(ai,   int32_t, _mm_add_epi32(A, _mm_set1_epi32(imm)))
SPU__VI(sfi,  int32_t, _mm_sub_epi32(_mm_set1_epi32(imm), A))
SPU__VI(ahi,  int32_t, _mm_add_epi16(A, _mm_set1_epi16((short)(uint16_t)imm)))
SPU__VI(sfhi, int32_t, _mm_sub_epi16(_mm_set1_epi16((short)(uint16_t)imm), A))
SPU__V3(addx, _mm_add_epi32(_mm_add_epi32(A, B), _mm_and_si128(C, _mm_set1_epi32(1))))
SPU__V3(sfx,  _mm_add_epi32(_mm_add_epi32(B, _mm_xor_si128(A, SPU__ONES)), _mm_and_si128(C, _mm_set1_epi32(1))))

SPU__V2(cg,  _mm_and_si128(_mm_cmpgt_epi32(_mm_xor_si128(A, SPU__S32), _mm_xor_si128(_mm_add_epi32(A, B), SPU__S32)), _mm_set1_epi32(1)))
SPU__V2(bg,  _mm_andnot_si128(_mm_cmpgt_epi32(_mm_xor_si128(A, SPU__S32), _mm_xor_si128(B, SPU__S32)), _mm_set1_epi32(1)))

SPU__V2(and,  _mm_and_si128(A, B))
SPU__V2(or,   _mm_or_si128(A, B))
SPU__V2(xor,  _mm_xor_si128(A, B))
SPU__V2(andc, _mm_andnot_si128(B, A))
SPU__V2(orc,  _mm_or_si128(A, _mm_xor_si128(B, SPU__ONES)))
SPU__V2(nor,  _mm_xor_si128(_mm_or_si128(A, B), SPU__ONES))
SPU__V2(nand, _mm_xor_si128(_mm_and_si128(A, B), SPU__ONES))
SPU__V2(eqv,  _mm_xor_si128(_mm_xor_si128(A, B), SPU__ONES))
SPU__V3(selb, _mm_or_si128(_mm_andnot_si128(C, A), _mm_and_si128(B, C)))
SPU__VI(andi,  int32_t, _mm_and_si128(A, _mm_set1_epi32(imm)))
SPU__VI(ori,   int32_t, _mm_or_si128(A, _mm_set1_epi32(imm)))
SPU__VI(xori,  int32_t, _mm_xor_si128(A, _mm_set1_epi32(imm)))
SPU__VI(andhi, int32_t, _mm_and_si128(A, _mm_set1_epi16((short)(uint16_t)imm)))
SPU__VI(orhi,  int32_t, _mm_or_si128(A, _mm_set1_epi16((short)(uint16_t)imm)))
SPU__VI(xorhi, int32_t, _mm_xor_si128(A, _mm_set1_epi16((short)(uint16_t)imm)))
SPU__VI(andbi, int32_t, _mm_and_si128(A, _mm_set1_epi8((char)(uint8_t)imm)))
SPU__VI(orbi,  int32_t, _mm_or_si128(A, _mm_set1_epi8((char)(uint8_t)imm)))
SPU__VI(xorbi, int32_t, _mm_xor_si128(A, _mm_set1_epi8((char)(uint8_t)imm)))

SPU__V2(ceq,  _mm_cmpeq_epi32(A, B))
SPU__V2(ceqh, _mm_cmpeq_epi16(A, B))
SPU__V2(ceqb, _mm_cmpeq_epi8(A, B))
SPU__V2(cgt,  _mm_cmpgt_epi32(A, B))
SPU__V2(clgt, _mm_cmpgt_epi32(_mm_xor_si128(A, SPU__S32), _mm_xor_si128(B, SPU__S32)))
SPU__V2(clgth,_mm_cmpgt_epi16(_mm_xor_si128(A, SPU__S16), _mm_xor_si128(B, SPU__S16)))
SPU__VI(ceqi,  int32_t, _mm_cmpeq_epi32(A, _mm_set1_epi32(imm)))
SPU__VI(ceqhi, int32_t, _mm_cmpeq_epi16(A, _mm_set1_epi16((short)(int16_t)imm)))
SPU__VI(ceqbi, int32_t, _mm_cmpeq_epi8(A, _mm_set1_epi8((char)(uint8_t)imm)))
SPU__VI(cgti,  int32_t, _mm_cmpgt_epi32(A, _mm_set1_epi32(imm)))
SPU__VI(cgthi, int32_t, _mm_cmpgt_epi16(A, _mm_set1_epi16((short)(int16_t)imm)))
SPU__VI(cgtbi, int32_t, _mm_cmpgt_epi8(A, _mm_set1_epi8((char)(int8_t)imm)))
SPU__VI(clgti, int32_t, _mm_cmpgt_epi32(_mm_xor_si128(A, SPU__S32), _mm_set1_epi32((int)((uint32_t)imm ^ 0x80000000u))))
SPU__VI(clgthi,int32_t, _mm_cmpgt_epi16(_mm_xor_si128(A, SPU__S16), _mm_set1_epi16((short)(uint16_t)((uint16_t)imm ^ 0x8000u))))
SPU__VI(clgtbi,int32_t, _mm_cmpgt_epi8(_mm_xor_si128(A, SPU__S8), _mm_set1_epi8((char)(uint8_t)((uint8_t)imm ^ 0x80u))))

SPU__VI(shli,   int, _mm_sll_epi32(A, _mm_cvtsi32_si128(imm & 0x3F)))
SPU__VI(shlhi,  int, _mm_sll_epi16(A, _mm_cvtsi32_si128(imm & 0x1F)))
SPU__VI(rotmi,  int, _mm_srl_epi32(A, _mm_cvtsi32_si128((0 - imm) & 0x3F)))
SPU__VI(rotmai, int, _mm_sra_epi32(A, _mm_cvtsi32_si128((0 - imm) & 0x3F)))
SPU__VI(rothmi, int, _mm_srl_epi16(A, _mm_cvtsi32_si128((0 - imm) & 0x1F)))
SPU__VI(rotmhi, int, _mm_srl_epi16(A, _mm_cvtsi32_si128((0 - imm) & 0x1F)))
SPU__VI(rotmahi,int, _mm_sra_epi16(A, _mm_cvtsi32_si128((0 - imm) & 0x1F)))
SPU__VI(roti,   int, _mm_or_si128(_mm_sll_epi32(A, _mm_cvtsi32_si128(imm & 31)), _mm_srl_epi32(A, _mm_cvtsi32_si128(32 - (imm & 31)))))
#if defined(__AVX2__)

SPU__V2(shl,  _mm_sllv_epi32(A, _mm_and_si128(B, _mm_set1_epi32(0x3F))))
SPU__V2(rotm, _mm_srlv_epi32(A, _mm_and_si128(_mm_sub_epi32(_mm_setzero_si128(), B), _mm_set1_epi32(0x3F))))
SPU__V2(rotma,_mm_srav_epi32(A, _mm_and_si128(_mm_sub_epi32(_mm_setzero_si128(), B), _mm_set1_epi32(0x3F))))
SPU__V2(rot,  _mm_or_si128(_mm_sllv_epi32(A, _mm_and_si128(B, _mm_set1_epi32(31))),
                           _mm_srlv_epi32(A, _mm_sub_epi32(_mm_set1_epi32(32), _mm_and_si128(B, _mm_set1_epi32(31))))))
#define spu_shl   spu__v_shl
#define spu_rotm  spu__v_rotm
#define spu_rotma spu__v_rotma
#define spu_rot   spu__v_rot
#endif

SPU__V1(xsbh, _mm_srai_epi16(_mm_slli_epi16(A, 8), 8))
SPU__V1(xshw, _mm_srai_epi32(_mm_slli_epi32(A, 16), 16))

SPU__V2(mpy,   _mm_mullo_epi32(SPU__LO16S(A), SPU__LO16S(B)))
SPU__V2(mpyu,  _mm_mullo_epi32(SPU__LO16U(A), SPU__LO16U(B)))
SPU__V2(mpyh,  _mm_slli_epi32(_mm_mullo_epi32(SPU__HI16S(A), SPU__LO16S(B)), 16))
SPU__V2(mpyhh, _mm_mullo_epi32(SPU__HI16S(A), SPU__HI16S(B)))
SPU__V2(mpys,  _mm_srai_epi32(_mm_mullo_epi32(SPU__LO16S(A), SPU__LO16S(B)), 16))
SPU__V3(mpya,  _mm_add_epi32(_mm_mullo_epi32(SPU__LO16S(A), SPU__LO16S(B)), C))
SPU__VI(mpyi,  int32_t, _mm_mullo_epi32(SPU__LO16S(A), _mm_set1_epi32((int32_t)(int16_t)imm)))
SPU__VI(mpyui, int32_t, _mm_mullo_epi32(SPU__LO16U(A), _mm_set1_epi32((int32_t)(uint32_t)(uint16_t)imm)))

static inline u128 spu__v_fsm(u128 a_) {
    const __m128i bits = _mm_setr_epi32(8, 4, 2, 1);
    const __m128i v = _mm_and_si128(_mm_set1_epi32(_mm_cvtsi128_si32(spu__vi(a_)) & 0xF), bits);
    return spu__vo(_mm_cmpeq_epi32(v, bits));
}

#define spu_splat_u32 spu__v_splat_u32
#define spu_il     spu__v_il
#define spu_ila    spu__v_ila
#define spu_ilhu   spu__v_ilhu
#define spu_ilh    spu__v_ilh
#define spu_link   spu__v_link
#define spu_pref_u32 spu__v_pref_u32
#define spu_iohl   spu__v_iohl
#define spu_a      spu__v_a
#define spu_sf     spu__v_sf
#define spu_ah     spu__v_ah
#define spu_sfh    spu__v_sfh
#define spu_ai     spu__v_ai
#define spu_sfi    spu__v_sfi
#define spu_ahi    spu__v_ahi
#define spu_sfhi   spu__v_sfhi
#define spu_addx   spu__v_addx
#define spu_sfx    spu__v_sfx
#define spu_cg     spu__v_cg
#define spu_bg     spu__v_bg
#define spu_and    spu__v_and
#define spu_or     spu__v_or
#define spu_xor    spu__v_xor
#define spu_andc   spu__v_andc
#define spu_orc    spu__v_orc
#define spu_nor    spu__v_nor
#define spu_nand   spu__v_nand
#define spu_eqv    spu__v_eqv
#define spu_selb   spu__v_selb
#define spu_andi   spu__v_andi
#define spu_ori    spu__v_ori
#define spu_xori   spu__v_xori
#define spu_andhi  spu__v_andhi
#define spu_orhi   spu__v_orhi
#define spu_xorhi  spu__v_xorhi
#define spu_andbi  spu__v_andbi
#define spu_orbi   spu__v_orbi
#define spu_xorbi  spu__v_xorbi
#define spu_ceq    spu__v_ceq
#define spu_ceqh   spu__v_ceqh
#define spu_ceqb   spu__v_ceqb
#define spu_cgt    spu__v_cgt
#define spu_clgt   spu__v_clgt
#define spu_clgth  spu__v_clgth
#define spu_ceqi   spu__v_ceqi
#define spu_ceqhi  spu__v_ceqhi
#define spu_ceqbi  spu__v_ceqbi
#define spu_cgti   spu__v_cgti
#define spu_cgthi  spu__v_cgthi
#define spu_cgtbi  spu__v_cgtbi
#define spu_clgti  spu__v_clgti
#define spu_clgthi spu__v_clgthi
#define spu_clgtbi spu__v_clgtbi
#define spu_shli   spu__v_shli
#define spu_shlhi  spu__v_shlhi
#define spu_rotmi  spu__v_rotmi
#define spu_rotmai spu__v_rotmai
#define spu_rothmi spu__v_rothmi
#define spu_rotmhi spu__v_rotmhi
#define spu_rotmahi spu__v_rotmahi
#define spu_roti   spu__v_roti
#define spu_xsbh   spu__v_xsbh
#define spu_xshw   spu__v_xshw
#define spu_mpy    spu__v_mpy
#define spu_mpyu   spu__v_mpyu
#define spu_mpyh   spu__v_mpyh
#define spu_mpyhh  spu__v_mpyhh
#define spu_mpys   spu__v_mpys
#define spu_mpya   spu__v_mpya
#define spu_mpyi   spu__v_mpyi
#define spu_mpyui  spu__v_mpyui
#define spu_fsm    spu__v_fsm

#if !defined(SPU_RC_LS_PROBES)
static inline u128 spu__v_ls_rd(const uint8_t* ls, uint32_t lsa) {
    const __m128i x = _mm_loadu_si128((const __m128i*)(ls + (lsa & (SPU_LS_MASK & ~0xFu))));
    return spu__vo(_mm_shuffle_epi8(x, _mm_setr_epi8(3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12)));
}
static inline void spu__v_ls_wr(uint8_t* ls, uint32_t lsa, u128 val) {
    _mm_storeu_si128((__m128i*)(ls + (lsa & (SPU_LS_MASK & ~0xFu))),
                     _mm_shuffle_epi8(spu__vi(val), _mm_setr_epi8(3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12)));
}

#define spu_ls_read128(c, lsa)       spu__v_ls_rd(spu_rc_ls, (lsa))
#define spu_ls_write128(c, lsa, val) spu__v_ls_wr(spu_rc_ls, (lsa), (val))
#endif

#endif
#endif
