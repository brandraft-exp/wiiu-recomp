/* Synchronous CPU DMA backend. Cemu dmae.cpp::dmaeExport_DMAECopyMem,
 * DMAEFillMem, DMAEGetRetiredTimeStamp. Count is in 32-bit words, not bytes. */
#include "rt.h"
static uint64_t retired;
static void ret64(ppc_ctx *c, uint64_t value) { c->r[3] = value >> 32; c->r[4] = value; }
static void retire(ppc_ctx *c) {
    uint64_t now = ppc_timebase(c);
    retired = now > retired ? now : retired + 1;
    ret64(c, retired);
}
static void check_range(ppc_ctx *c, uint32_t addr, uint64_t size) {
    if ((size && !addr) || (uint64_t)addr + size > (1ull << 32)) rt_fatal(c, "invalid DMA range");
}
static void copy(ppc_ctx *c) {
    uint32_t dst = c->r[3], src = c->r[4], mode = c->r[6];
    uint64_t size = (uint64_t)c->r[5] * 4;
    check_range(c, dst, size); check_range(c, src, size);
    if (mode > 2) rt_fatal(c, "unsupported DMA endian mode %u", mode);
    memmove(g_mem + dst, g_mem + src, size);
    unsigned unit = mode == 1 ? 2 : mode == 2 ? 4 : 1;
    for (uint64_t i = 0; unit > 1 && i < size; i += unit)
        for (unsigned j = 0; j < unit / 2; j++) {
            uint8_t tmp = g_mem[dst + i + j];
            g_mem[dst + i + j] = g_mem[dst + i + unit - 1 - j];
            g_mem[dst + i + unit - 1 - j] = tmp;
        }
    retire(c);
}
static void fill(ppc_ctx *c) {
    uint32_t dst = c->r[3], value = c->r[4], count = c->r[5];
    check_range(c, dst, (uint64_t)count * 4);
    for (uint64_t i = 0; i < count; i++) st32(dst + i * 4, value);
    retire(c);
}
static void wait_done(ppc_ctx *c) { c->r[3] = 1; }
static void timestamp(ppc_ctx *c) { ret64(c, retired); }
hle_fn hle_dmae_lookup(const char *name) {
    if (!strcmp(name, "DMAECopyMem")) return copy;
    if (!strcmp(name, "DMAEFillMem")) return fill;
    if (!strcmp(name, "DMAEWaitDone")) return wait_done;
    if (!strcmp(name, "DMAEGetRetiredTimeStamp")) return timestamp;
    return NULL;
}
