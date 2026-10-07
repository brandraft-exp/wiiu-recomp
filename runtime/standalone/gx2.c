/* Guest ABI for GX2. Layout uses the attributed Cemu implementation in gx2/.
 * Scan-buffer sizing: Cemu GX2.cpp::_GX2InitScanBuffer,
 * gx2Export_GX2CalcTVSize, gx2Export_GX2CalcDRCSize. */
#include "rt.h"
#include "gx2/surface.h"
#define A(n) c->r[3 + (n)]
#define RET(v) (c->r[3] = (uint32_t)(v))

static void GX2CalcSurfaceSizeAndAlignment(ppc_ctx *c) {
    uint32_t a = A(0), words[29];
    for (unsigned i = 0; i < 29; i++) words[i] = ld32(a + 4 * i);
    if (!gx2_surface_layout(words))
        rt_fatal(c, "invalid GX2 surface at %08x: dim=%u size=%ux%ux%u format=%x mips=%u tile=%u aa=%u",
                 a, words[0], words[1], words[2], words[3], words[5], words[4], words[12], words[6]);
    for (unsigned i = 0; i < 29; i++) st32(a + 4 * i, words[i]);
}
static void scan_size(ppc_ctx *c, uint32_t width, uint32_t height, unsigned buffers, unsigned interlace) {
    uint32_t words[29] = {1, width, height, 1, 1, A(1), 0, 3};
    if (!buffers || !gx2_surface_layout(words)) rt_fatal(c, "invalid scan buffer parameters");
    uint32_t size = words[8], align = words[14], pad = (align - size % align) % align;
    uint64_t total = (uint64_t)(size + pad) * buffers * interlace - pad;
    if (total > UINT32_MAX) rt_fatal(c, "scan buffer size overflow");
    if (A(3)) st32(A(3), (uint32_t)total);
    if (A(4)) st32(A(4), 0);
    RET(0);
}
static void GX2CalcTVSize(ppc_ctx *c) {
    static const uint32_t sizes[][2] = {{0,0}, {640,480}, {854,480}, {1280,720}, {1280,720}, {1920,1080}};
    uint32_t mode = A(0);
    if (!mode || mode >= sizeof sizes / sizeof *sizes) rt_fatal(c, "invalid TV render mode %u", mode);
    scan_size(c, sizes[mode][0], sizes[mode][1], mode < 3 ? 4 : A(2), mode == 4 ? 2 : 1);
}
static void GX2CalcDRCSize(ppc_ctx *c) { scan_size(c, 854, 480, A(2), 1); }
static void GX2GetSurfaceFormatBits(ppc_ctx *c) { RET(gx2_format_bits(A(0))); }
static void GX2GetSystemTVScanMode(ppc_ctx *c) { RET(7); }
static void GX2GetSystemTVAspectRatio(ppc_ctx *c) { RET(1); }
/* Cemu GX2.cpp::gx2Export_GX2TempGetGPUVersion. */
static void GX2TempGetGPUVersion(ppc_ctx *c) { RET(2); }
static void mip_info(ppc_ctx *c, uint32_t a, uint32_t level, uint32_t info[8]) {
    uint32_t words[29];
    for (unsigned i = 0; i < 29; i++) words[i] = ld32(a + 4 * i);
    if (!gx2_mip_layout(words, level, info)) rt_fatal(c, "invalid GX2 mip surface %08x level %u", a, level);
}
static void GX2GetSurfaceMipPitch(ppc_ctx *c) { uint32_t info[8]; mip_info(c, A(0), A(1), info); RET(info[0]); }
/* Cemu GX2_Texture.cpp::GX2InitTextureRegs and LatteReg.h register fields. */
static void GX2InitTextureRegs(ppc_ctx *c) {
    uint32_t a = A(0), w = ld32(a + 4), h = ld32(a + 8), d = ld32(a + 12);
    uint32_t fmt = ld32(a + 0x14), hw = fmt & 63, dim = ld32(a), pitch = ld32(a + 0x3c);
    uint32_t first = ld32(a + 0x74), levels = ld32(a + 0x78), slice = ld32(a + 0x7c), slices = ld32(a + 0x80);
    uint32_t sel = ld32(a + 0x84), aa = ld32(a + 0x18);
    if (!h) { h = 1; st32(a + 8, h); }
    if (!d) { d = 1; st32(a + 12, d); }
    if (!ld32(a + 16)) st32(a + 16, 1);
    if (!levels) { levels = 1; st32(a + 0x78, 1); }
    if (!slices) { slices = 1; st32(a + 0x80, 1); }
    if (hw >= 49 && hw <= 53) pitch *= 4;
    uint32_t r0 = (dim & 7) | ((ld32(a + 0x30) & 15) << 3) | ((ld32(a + 0x1c) & 4) ? 128 : 0);
    r0 |= (((pitch >> 3) - 1) & 0x7ff) << 8; r0 |= ((w - 1) & 0x1fff) << 19;
    uint32_t depth = dim == 3 ? d / 6 - 1 : (dim == 2 || dim == 4 || dim == 5 || dim == 7) ? d - 1 : 0;
    uint32_t r1 = ((h - 1) & 0x1fff) | ((depth & 0x1fff) << 13) | (hw << 26);
    uint32_t r4 = (fmt & 0x200 ? 0x55 : 0) | (fmt & 0x800 ? 2u << 8 : fmt & 0x100 ? 1u << 8 : 0);
    if (fmt & 0x400) r4 |= 1u << 11;
    r4 |= 2u << 14 | ((sel >> 24) & 7) << 16 | ((sel >> 16) & 7) << 19;
    r4 |= ((sel >> 8) & 7) << 22 | (sel & 7) << 25 | (first & 15) << 28;
    uint32_t r5 = (first + levels - 1) & 15;
    if (aa >= 1 && aa <= 3) r5 = aa;
    r5 |= (slice & 0x1fff) << 4 | ((slice + slices - 1) & 0x1fff) << 17;
    if (dim == 3 && d / 6 != 1) r5 |= 1u << 30;
    st32(a + 0x88, r0); st32(a + 0x8c, r1); st32(a + 0x90, r4);
    st32(a + 0x94, r5); st32(a + 0x98, 4u << 2 | 7u << 5 | 2u << 30);
}
/* Cemu GX2_RenderTarget.cpp::gx2Export_GX2InitColorBufferRegs. */
static void GX2InitColorBufferRegs(ppc_ctx *c) {
    uint32_t a = A(0), info[8]; mip_info(c, a, ld32(a + 0x74), info);
    st32(a + 0x88, ((info[0] / 8 - 1) & 1023) | (((info[0] * info[1] / 64 - 1) & 0xfffff) << 10));
    uint32_t fmt = ld32(a + 0x14), hw = fmt & 63, type;
    switch (fmt & 0xf00) {
    case 0: type = 0; break; case 0x100: type = 4; break; case 0x200: type = 1; break;
    case 0x300: type = 5; break; case 0x400: type = 6; break; case 0x800: type = 7; break;
    default: rt_fatal(c, "unsupported color format %x", fmt);
    }
    uint32_t reg = hw << 2 | info[3] << 8 | type << 12;
    if (hw == 12 || hw == 27) reg |= 2u << 16;
    if (ld32(a + 0x18)) reg |= 2u << 18;
    /* GX2_Surface.cpp::GetSurfaceColorBufferExportFormat */
    static const uint8_t normalized[64] = {
        1,1,1,1,1,0,1,1,1,1,1,1,1,0,0,0,1,0,0,0,0,1,1,1,1,1,1,1,0,0,0,0,
        1,0,0,0,1,1,1,0,0,0,0,0,1,0,0,0,0,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0};
    if (!(fmt & 0x100)) reg |= (uint32_t)normalized[hw] << 27;
    if (fmt & 0x100 || fmt == 0x11 || fmt == 0x811 || fmt == 0x81c) reg |= 1u << 22;
    else if (type == 0 || type == 1 || type == 6) reg |= 1u << 20;
    if (fmt & 0x800) reg |= 1u << 25;
    st32(a + 0x8c, reg);
    uint32_t first = ld32(a + 0x78), count = ld32(a + 0x7c);
    st32(a + 0x90, ld32(a + 0x30) == 16 ? 0 : (first & 0x7ff) | ((first + count - 1) & 0x7ff) << 13);
    st32(a + 0x94, 0);
}
static void GX2InitDepthBufferRegs(ppc_ctx *c) {
    uint32_t a = A(0), info[8]; mip_info(c, a, ld32(a + 0x74), info);
    st32(a + 0x90, ((info[0] / 8 - 1) & 1023) | (((info[0] * info[1] / 64 - 1) & 0xfffff) << 10));
}

static void GX2AllocateTilingApertureEx(ppc_ctx *c);
static void GX2FreeTilingAperture(ppc_ctx *c);
hle_fn hle_gx2_lookup(const char *name) {
#define EXPORT(n) if (!strcmp(name, #n)) return n
    EXPORT(GX2CalcSurfaceSizeAndAlignment); EXPORT(GX2CalcTVSize); EXPORT(GX2CalcDRCSize);
    EXPORT(GX2GetSurfaceFormatBits); EXPORT(GX2GetSystemTVScanMode); EXPORT(GX2GetSystemTVAspectRatio);
    EXPORT(GX2TempGetGPUVersion);
    EXPORT(GX2AllocateTilingApertureEx); EXPORT(GX2FreeTilingAperture);
    EXPORT(GX2GetSurfaceMipPitch); EXPORT(GX2InitTextureRegs); EXPORT(GX2InitColorBufferRegs); EXPORT(GX2InitDepthBufferRegs);
    return NULL;
#undef EXPORT
}

/* Cemu GX2_TilingAperture.cpp allocation/free lifecycle. Dedicated 32 MiB
 * guest aperture range, as in Cafe/HW/MMU/MMU.h. */
typedef struct {
    uint32_t words[29], level, slice, addr, size, handle;
} aperture;
static aperture apertures[32];
static uint32_t next_handle = 1;
static void aperture_transfer(ppc_ctx *c, aperture *a, int write_back) {
    uint32_t ptr = a->level ? a->words[11] : a->words[9];
    uint32_t size = a->level ? a->words[10] : a->words[8];
    uint32_t off = a->level > 1 ? a->words[16 + a->level - 1] : 0;
    if (!ptr || off > size || (uint64_t)ptr + size > (1ull << 32) ||
        !gx2_aperture_copy(a->words, a->level, a->slice, g_mem + a->addr, a->size,
                           g_mem + ptr + off, size - off, write_back))
        rt_fatal(c, "unsupported GX2 aperture transfer: format=%x tile=%u mip=%u slice=%u",
                 a->words[5], a->words[12], a->level, a->slice);
}
static void GX2AllocateTilingApertureEx(ppc_ctx *c) {
    aperture a = {0};
    for (unsigned i = 0; i < 29; i++) a.words[i] = ld32(A(0) + i * 4);
    a.level = A(1); a.slice = A(2);
    uint32_t info[8];
    if (A(3) || !gx2_mip_layout(a.words, a.level, info)) rt_fatal(c, "unsupported GX2 aperture parameters");
    uint64_t size = (uint64_t)info[0] * ((info[1] + 3) & ~3u) * (info[4] / 8);
    if (!size || size > 0x02000000) rt_fatal(c, "GX2 aperture size overflow");
    a.size = (uint32_t)((size + 4095) & ~4095ull);
    unsigned slot = 32;
    for (unsigned i = 0; i < 32; i++) if (!apertures[i].handle) { slot = i; break; }
    uint32_t addr = 0xe8000000;
    for (;;) {
        uint32_t next = addr;
        for (unsigned i = 0; i < 32; i++) {
            aperture *b = &apertures[i];
            if (b->handle && addr < b->addr + b->size && addr + a.size > b->addr)
                if (b->addr + b->size > next) next = b->addr + b->size;
        }
        if (next == addr) break;
        addr = next;
    }
    if (slot == 32 || (uint64_t)addr + a.size > 0xea000000) {
        st32(A(4), 0); st32(A(5), 0); RET(0); return;
    }
    a.addr = addr; a.handle = next_handle++;
    if (!next_handle) next_handle = 1;
    memset(g_mem + addr, 0, a.size);
    aperture_transfer(c, &a, 0);
    apertures[slot] = a;
    st32(A(4), a.handle); st32(A(5), a.addr); RET(0);
}
static void GX2FreeTilingAperture(ppc_ctx *c) {
    for (unsigned i = 0; i < 32; i++) if (apertures[i].handle == A(0) && A(0)) {
        aperture_transfer(c, &apertures[i], 1); apertures[i].handle = 0; break;
    }
    RET(0);
}
