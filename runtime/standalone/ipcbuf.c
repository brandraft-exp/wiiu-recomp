/* Cafe IPC buffer pools. ABI and FIFO behavior follow Cemu
 * coreinit_IPCBuf.cpp::{IPCBufPoolCreate,IPCBufPoolAllocate,IPCBufPoolFree}.
 * Operations are atomic under the runtime's serialized guest scheduler token. */
#include "rt.h"
#define A(n) c->r[3 + (n)]
#define RET(v) (c->r[3] = (uint32_t)(v))
static void IPCBufPoolCreate(ppc_ctx *c) {
    uint32_t base = A(0), size = A(1), requested = A(2), output = A(3), flags = A(4);
    uint64_t pool64 = ((uint64_t)base + 3) & ~3ull, end = (uint64_t)base + size;
    uint64_t stride64 = ((uint64_t)requested + 63) & ~63ull;
    if (output) st32(output, 0);
    if (!base || end > (1ull << 32) || pool64 + 0x6c >= end || !stride64 || stride64 > UINT32_MAX) { RET(0); return; }
    uint32_t pool = pool64, data = pool + 0x6c, stride = stride64;
    uint32_t capacity = (end - data) / stride;
    if (capacity <= 1 || (uint64_t)capacity * 4 >= stride) { RET(0); return; }
    uint32_t count = capacity - 1, start = data + capacity * 4;
    memset(g_mem + base, 0, size);
    st32(pool, 0x0badf00d); st32(pool + 4, base); st32(pool + 8, size); st32(pool + 12, flags);
    st32(pool + 0x14, stride); st32(pool + 0x18, stride); st32(pool + 0x1c, count);
    st32(pool + 0x20, start); st32(pool + 0x24, capacity * 4);
    st32(pool + 0x28, 0); st32(pool + 0x2c, 0); st32(pool + 0x30, count);
    st32(pool + 0x34, count); st32(pool + 0x38, data);
    /* Initialize guest-visible mutex ABI using the existing HLE. */
    uint32_t saved[8]; memcpy(saved, &c->r[3], sizeof saved);
    A(0) = pool + 0x3c; hle_thread_lookup("OSInitMutex")(c);
    memcpy(&c->r[3], saved, sizeof saved);
    for (uint32_t i = 0; i < count; i++) st32(data + i * 4, start + i * stride);
    if (output) st32(output, count);
    RET(pool);
}
static void IPCBufPoolAllocate(ppc_ctx *c) {
    uint32_t p = A(0), size = A(1);
    if (!p || ld32(p) != 0x0badf00d || size > ld32(p + 0x14) || !ld32(p + 0x30)) { RET(0); return; }
    uint32_t read = ld32(p + 0x2c), count = ld32(p + 0x34), available = ld32(p + 0x30) - 1;
    uint32_t result = ld32(ld32(p + 0x38) + read * 4);
    st32(p + 0x30, available); st32(p + 0x2c, available ? (read + 1) % count : UINT32_MAX);
    RET(result);
}
static void IPCBufPoolFree(ppc_ctx *c) {
    uint32_t p = A(0), entry = A(1);
    if (!p || ld32(p) != 0x0badf00d) { RET(-4); return; }
    uint32_t start = ld32(p + 0x20), stride = ld32(p + 0x18), count = ld32(p + 0x1c);
    if (entry < start || !stride || (entry - start) % stride || (entry - start) / stride >= count) { RET(-4); return; }
    uint32_t available = ld32(p + 0x30), write = ld32(p + 0x28), array = ld32(p + 0x38);
    if (available == count) { RET(-8); return; }
    /* Reject a duplicate free instead of allowing two allocations to alias. */
    uint32_t read = ld32(p + 0x2c);
    for (uint32_t i = 0; i < available; i++) if (ld32(array + ((read + i) % count) * 4) == entry) { RET(-4); return; }
    st32(array + write * 4, entry);
    if (!available) st32(p + 0x2c, write);
    st32(p + 0x30, available + 1); st32(p + 0x28, (write + 1) % count);
    RET(0);
}
hle_fn hle_ipcbuf_lookup(const char *name) {
#define EXPORT(n) if (!strcmp(name, #n)) return n
    EXPORT(IPCBufPoolCreate); EXPORT(IPCBufPoolAllocate); EXPORT(IPCBufPoolFree);
    return NULL;
#undef EXPORT
}
