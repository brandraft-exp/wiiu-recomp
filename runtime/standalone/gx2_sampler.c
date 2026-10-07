/* Cemu GX2_Texture.cpp::GX2InitSampler* and LatteReg.h sampler fields. */
#include "rt.h"
#define A(n) c->r[3 + (n)]
static void field(uint32_t addr, unsigned shift, uint32_t mask, uint32_t value) {
    st32(addr, (ld32(addr) & ~(mask << shift)) | ((value & mask) << shift));
}
static void init(ppc_ctx *c) {
    uint32_t clamp = A(1) & 7, filter = A(2) & 7;
    st32(A(0), clamp | clamp << 3 | clamp << 6 | filter << 9 | filter << 12 | 1u << 15 | 1u << 17 | 1u << 25);
    st32(A(0) + 4, 1023u << 10); st32(A(0) + 8, 1u << 31);
}
static void xy(ppc_ctx *c) {
    uint32_t mag = A(1), min = A(2), aniso = A(3);
    if (aniso) { mag = mag <= 1 ? mag + 4 : 0; min = min <= 1 ? min + 4 : 0; }
    field(A(0), 9, 7, mag); field(A(0), 12, 7, min); field(A(0), 19, 7, aniso);
}
static void zm(ppc_ctx *c) { field(A(0), 15, 3, A(1)); field(A(0), 17, 3, A(2)); }
static void clamping(ppc_ctx *c) { field(A(0), 0, 7, A(1)); field(A(0), 3, 7, A(2)); field(A(0), 6, 7, A(3)); }
static void border(ppc_ctx *c) { field(A(0), 22, 3, A(1)); }
static void compare(ppc_ctx *c) { field(A(0), 26, 7, A(1)); }
static int fixed(double value, int low, int high) {
    value = floor(value * 64.0);
    if (isnan(value)) return 0;
    if (value < low) return low;
    if (value > high) return high;
    return (int)value;
}
static void lod(ppc_ctx *c) {
    double v[3]; for (unsigned i = 0; i < 3; i++) memcpy(&v[i], &c->f[i + 1], 8);
    uint32_t min = fixed(v[0], 0, 1023), max = fixed(v[1], 0, 1023), bias = fixed(v[2], -2048, 2047);
    st32(A(0) + 4, min | max << 10 | (bias & 4095) << 20);
}
hle_fn hle_gx2_sampler_lookup(const char *name) {
#define EXPORT(n, f) if (!strcmp(name, n)) return f
    EXPORT("GX2InitSampler", init); EXPORT("GX2InitSamplerXYFilter", xy); EXPORT("GX2InitSamplerZMFilter", zm);
    EXPORT("GX2InitSamplerClamping", clamping); EXPORT("GX2InitSamplerBorderType", border);
    EXPORT("GX2InitSamplerDepthCompare", compare); EXPORT("GX2InitSamplerLOD", lod);
    return NULL;
#undef EXPORT
}
