/* CPU state and instruction helpers targeted by recompiled code.
 * Semantics follow the PowerPC 750CL (Espresso) user-mode ISA. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

typedef struct ppc_ctx {
    uint32_t r[32];
    uint64_t f[32];   /* ps0, IEEE double bit pattern */
    uint64_t ps1[32]; /* ps1, IEEE double bit pattern */
    uint8_t cr[8];    /* per field: LT=8 GT=4 EQ=2 SO=1 */
    uint32_t lr, ctr;
    uint8_t xer_so, xer_ov, xer_ca, xer_bc;
    uint32_t fpscr;
    uint32_t resv_addr, resv_val;
    uint8_t resv;
    uint32_t npc; /* next pc; only used by single-instruction test harness */
    void *host;   /* owning runtime thread, opaque here */
} ppc_ctx;

typedef void (*ppc_fn)(ppc_ctx *);

/* Guest address space base: guest address A lives at g_mem + A. */
extern uint8_t *g_mem;

/* Runtime hooks, provided by the runtime (or the test harness). */
void ppc_call_indirect(ppc_ctx *c, uint32_t target);
void ppc_branch_mid(ppc_ctx *c, uint32_t target, uint32_t from);
void ppc_syscall(ppc_ctx *c, uint32_t addr);
void ppc_trap(ppc_ctx *c, uint32_t addr);
uint64_t ppc_timebase(ppc_ctx *c);

/* ---- memory (big-endian guest) ---------------------------------------- */
static inline uint8_t ld8(uint32_t a) { return g_mem[a]; }
static inline uint16_t ld16(uint32_t a) { uint16_t v; memcpy(&v, g_mem + a, 2); return __builtin_bswap16(v); }
static inline uint32_t ld32(uint32_t a) { uint32_t v; memcpy(&v, g_mem + a, 4); return __builtin_bswap32(v); }
static inline uint64_t ld64(uint32_t a) { uint64_t v; memcpy(&v, g_mem + a, 8); return __builtin_bswap64(v); }
static inline void st8(uint32_t a, uint8_t v) { g_mem[a] = v; }
static inline void st16(uint32_t a, uint16_t v) { v = __builtin_bswap16(v); memcpy(g_mem + a, &v, 2); }
static inline void st32(uint32_t a, uint32_t v) { v = __builtin_bswap32(v); memcpy(g_mem + a, &v, 4); }
static inline void st64(uint32_t a, uint64_t v) { v = __builtin_bswap64(v); memcpy(g_mem + a, &v, 8); }

/* ---- condition register ----------------------------------------------- */
static inline uint8_t cr_cmp_s(ppc_ctx *c, int32_t a, int32_t b) {
    return (a < b ? 8 : a > b ? 4 : 2) | c->xer_so;
}
static inline uint8_t cr_cmp_u(ppc_ctx *c, uint32_t a, uint32_t b) {
    return (a < b ? 8 : a > b ? 4 : 2) | c->xer_so;
}
#define RC0(v) (c->cr[0] = cr_cmp_s(c, (int32_t)(v), 0))
static inline int cr_bit(ppc_ctx *c, int bi) { return (c->cr[bi >> 2] >> (3 - (bi & 3))) & 1; }
static inline void cr_setbit(ppc_ctx *c, int bi, int v) {
    uint8_t m = 1 << (3 - (bi & 3));
    c->cr[bi >> 2] = v ? (c->cr[bi >> 2] | m) : (c->cr[bi >> 2] & ~m);
}
static inline uint32_t cr_get(ppc_ctx *c) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint32_t)c->cr[i] << (28 - 4 * i);
    return v;
}
static inline void cr_put(ppc_ctx *c, uint32_t v, uint32_t fxm) {
    for (int i = 0; i < 8; i++)
        if (fxm & (0x80 >> i)) c->cr[i] = (v >> (28 - 4 * i)) & 0xF;
}

/* ---- integer helpers ---------------------------------------------------- */
static inline uint32_t rotl32(uint32_t v, uint32_t n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }
static inline uint32_t add_ca(ppc_ctx *c, uint32_t a, uint32_t b, uint32_t ci) {
    uint64_t s = (uint64_t)a + b + ci;
    c->xer_ca = (uint8_t)(s >> 32);
    return (uint32_t)s;
}
static inline uint32_t sraw(ppc_ctx *c, uint32_t s, uint32_t n) {
    int32_t v = (int32_t)s;
    if (n & 0x20) { c->xer_ca = v < 0; return v < 0 ? 0xFFFFFFFFu : 0; }
    n &= 31;
    c->xer_ca = v < 0 && n && (s & ((1u << n) - 1));
    return (uint32_t)(v >> n);
}
/* Undefined-result cases follow Dolphin's hardware-verified 750 behaviour. */
static inline uint32_t divw(uint32_t a, uint32_t b) {
    if (UNLIKELY(b == 0 || (a == 0x80000000u && b == 0xFFFFFFFFu)))
        return ((int32_t)a < 0) ? 0xFFFFFFFFu : 0;
    return (uint32_t)((int32_t)a / (int32_t)b);
}
static inline uint32_t divwu(uint32_t a, uint32_t b) { return b ? a / b : 0; }

/* ---- floating point ------------------------------------------------------ */
static inline double fd(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }
static inline uint64_t db(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }
static inline float ff(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static inline uint32_t fb(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }

/* Single -> double as the FPU does on lfs (exact, keeps signalling NaNs). */
static inline uint64_t to_double(uint32_t s) {
    if (UNLIKELY((s & 0x7F800000u) == 0x7F800000u))
        return ((uint64_t)(s >> 31) << 63) | (0x7FFull << 52) | ((uint64_t)(s & 0x7FFFFF) << 29);
    return db((double)ff(s));
}
/* Double -> single as stfs does: bit selection, no rounding. */
static inline uint32_t to_single(uint64_t d) {
    uint32_t exp = (d >> 52) & 0x7FF;
    if (LIKELY(exp > 896))
        return (uint32_t)(((d >> 62) & 3) << 30) | (uint32_t)((d >> 29) & 0x3FFFFFFF);
    uint32_t r = (uint32_t)(d >> 63) << 31;
    if (exp >= 874) r |= (uint32_t)(((1ull << 52) | (d & 0xFFFFFFFFFFFFFull)) >> (896 + 30 - exp));
    return r;
}

static inline int is_nan(uint64_t b) { return (b & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull; }
/* PowerPC NaN result: first NaN operand in priority order, quieted; else default NaN. */
static inline uint64_t ppc_nan(uint64_t a, uint64_t b, uint64_t cc) {
    if (is_nan(a)) return a | 0x0008000000000000ull;
    if (is_nan(b)) return b | 0x0008000000000000ull;
    if (is_nan(cc)) return cc | 0x0008000000000000ull;
    return 0x7FF8000000000000ull;
}
static inline uint64_t fix_nan(double r, uint64_t a, uint64_t b, uint64_t cc) {
    return UNLIKELY(r != r) ? ppc_nan(a, b, cc) : db(r);
}
/* Round a double-format result to single precision, as single-precision ops do. */
static inline uint64_t rnd_single(uint64_t b) {
    if (UNLIKELY(is_nan(b))) return to_double(to_single(b) | 0x00400000u);
    return db((double)(float)fd(b));
}

/* Arithmetic. Operand order follows the instruction (A, B, C priority for NaNs). */
static inline uint64_t f_add(uint64_t a, uint64_t b) { return fix_nan(fd(a) + fd(b), a, b, 0); }
static inline uint64_t f_sub(uint64_t a, uint64_t b) { return fix_nan(fd(a) - fd(b), a, b, 0); }
static inline uint64_t f_mul(uint64_t a, uint64_t cc) { return fix_nan(fd(a) * fd(cc), a, 0, cc); }
static inline uint64_t f_div(uint64_t a, uint64_t b) { return fix_nan(fd(a) / fd(b), a, b, 0); }
static inline uint64_t f_madd(uint64_t a, uint64_t cc, uint64_t b) { return fix_nan(fma(fd(a), fd(cc), fd(b)), a, b, cc); }
static inline uint64_t f_msub(uint64_t a, uint64_t cc, uint64_t b) { return fix_nan(fma(fd(a), fd(cc), -fd(b)), a, b, cc); }
static inline uint64_t f_neg_unless_nan(uint64_t r) { return is_nan(r) ? r : r ^ 0x8000000000000000ull; }

static inline uint64_t fctiwz(uint64_t b) {
    double x = fd(b);
    int32_t r;
    if (x != x || x < -2147483648.0) r = INT32_MIN;
    else if (x >= 2147483648.0) r = INT32_MAX;
    else r = (int32_t)x;
    return 0xFFF8000000000000ull | (uint32_t)r;
}
static inline uint64_t fctiw(uint64_t b) {
    double x = fd(b);
    int32_t r;
    if (x != x || x < -2147483648.0) r = INT32_MIN;
    else if (x >= 2147483648.0) r = INT32_MAX;
    else r = (int32_t)nearbyint(x);
    return 0xFFF8000000000000ull | (uint32_t)r;
}
static inline uint8_t fcmp(uint64_t a, uint64_t b) {
    double x = fd(a), y = fd(b);
    if (x != x || y != y) return 1;
    return x < y ? 8 : x > y ? 4 : 2;
}
