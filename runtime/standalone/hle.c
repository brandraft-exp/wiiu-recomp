#define _GNU_SOURCE
/* High-level emulation of Wii U OS libraries (grows as the game needs it).
 * Semantics follow Cemu's implementation where behaviour isn't documented. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rt.h"

/* ---- guest memory map ------------------------------------------------------ */
#define MEM1_BASE 0xF4000000u
#define MEM1_SIZE 0x02000000u
#define MEM2_BASE g_mem2_base /* above every loaded RPX data section */
#define MEM2_END  0x4E000000u /* main stack lives above */
#define FG_BASE   0xE0000000u
#define FG_SIZE   0x02800000u
#define SYS_BASE  0x0F000000u /* runtime-owned objects (thread, system info, ...) */
#define THUNK_BASE 0xFFF00000u

#define TIMER_HZ 62156250ull /* bus clock / 4 */

static uint32_t g_sys_brk = SYS_BASE;
static uint32_t g_mem2_base;
uint32_t hle_sys_alloc(uint32_t size) {
    if(size>0x10000000u-g_sys_brk-63u)rt_fatal(NULL,"runtime system arena exhausted");
    uint32_t a = g_sys_brk;
    g_sys_brk = (g_sys_brk + size + 63) & ~63u;
    memset(g_mem + a, 0, size);
    return a;
}

static const char *gstr(uint32_t a) { return a ? (const char *)(g_mem + a) : "(null)"; }
#define ARG(n) (c->r[3 + (n)])
#define RET(v) (c->r[3] = (uint32_t)(v))

/* ---- heaps ---------------------------------------------------------------------
 * Heap metadata lives host-side, keyed by the heap handle (its guest start address). */
typedef struct block { uint32_t addr, size; int used; struct block *next; } block;
typedef struct heap {
    uint32_t handle, start, end;
    int is_frame;
    uint32_t head, tail; /* frame heap */
    block *blocks;       /* expanded heap, address-ordered */
    struct heap *next;
} heap;
static heap *g_heaps;
static uint32_t g_base_heap[9]; /* Cemu coreinit_Init: MEM1=0, MEM2=1, FG=8 */

static heap *heap_get(uint32_t handle) {
    for (heap *h = g_heaps; h; h = h->next)
        if (h->handle == handle) return h;
    return NULL;
}

static uint32_t exp_create(uint32_t start, uint32_t size) {
    heap *h = calloc(1, sizeof *h);
    h->handle = start;
    h->start = (start + 0x40 + 31) & ~31u; /* keep a little header space like the real heap */
    h->end = (start + size) & ~31u;
    h->blocks = calloc(1, sizeof(block));
    h->blocks->addr = h->start;
    h->blocks->size = h->end - h->start;
    h->next = g_heaps;
    g_heaps = h;
    return h->handle;
}

static uint32_t exp_alloc(heap *h, uint32_t size, int32_t align) {
    uint32_t al = (uint32_t)(align < 0 ? -align : align);
    if (al < 4) al = 4;
    size = (size + 3) & ~3u;
    if (!size) size = 4;
    block *pick = NULL;
    uint32_t pick_addr = 0;
    for (block *b = h->blocks; b; b = b->next) {
        if (b->used) continue;
        uint32_t a;
        if (align >= 0) {
            a = (b->addr + al - 1) & ~(al - 1);
            if (a + size > b->addr + b->size) continue;
            pick = b, pick_addr = a;
            break; /* first fit from the head */
        }
        a = (b->addr + b->size - size) & ~(al - 1);
        if (a < b->addr) continue;
        pick = b, pick_addr = a; /* last fit from the tail */
    }
    if (!pick) return 0;
    /* split: [pick->addr, pick_addr) free, [pick_addr, +size) used, rest free */
    block *b = pick;
    if (pick_addr > b->addr) {
        block *n = calloc(1, sizeof *n);
        n->addr = pick_addr;
        n->size = b->addr + b->size - pick_addr;
        n->next = b->next;
        b->size = pick_addr - b->addr;
        b->next = n;
        b = n;
    }
    if (b->size > size) {
        block *n = calloc(1, sizeof *n);
        n->addr = b->addr + size;
        n->size = b->size - size;
        n->next = b->next;
        b->next = n;
        b->size = size;
    }
    b->used = 1;
    return b->addr;
}

static void exp_free(heap *h, uint32_t addr) {
    for (block *b = h->blocks, *prev = NULL; b; prev = b, b = b->next) {
        if (b->addr != addr || !b->used) continue;
        b->used = 0;
        if (b->next && !b->next->used) {
            block *n = b->next;
            b->size += n->size;
            b->next = n->next;
            free(n);
        }
        if (prev && !prev->used) {
            prev->size += b->size;
            prev->next = b->next;
            free(b);
        }
        return;
    }
    fprintf(stderr, "warning: free of unknown block %08x in heap %08x\n", addr, h->handle);
}

static uint32_t exp_free_total(heap *h) {
    uint32_t t = 0;
    for (block *b = h->blocks; b; b = b->next)
        if (!b->used) t += b->size;
    return t;
}

static uint32_t exp_allocatable(heap *h, uint32_t al) {
    uint32_t best = 0;
    if (al < 4) al = 4;
    for (block *b = h->blocks; b; b = b->next) {
        if (b->used) continue;
        uint32_t a = (b->addr + al - 1) & ~(al - 1);
        if (a < b->addr + b->size && b->addr + b->size - a > best) best = b->addr + b->size - a;
    }
    return best;
}

static heap *heap_for(ppc_ctx *c, uint32_t handle) {
    heap *h = heap_get(handle);
    if (!h) rt_fatal(c, "unknown heap handle %08x", handle);
    return h;
}

static void MEMCreateExpHeapEx(ppc_ctx *c) { RET(exp_create(ARG(0), ARG(1))); }
static void MEMDestroyExpHeap(ppc_ctx *c) { RET(ARG(0)); /* metadata leaks; fine for now */ }
static void MEMAllocFromExpHeapEx(ppc_ctx *c) { RET(exp_alloc(heap_for(c, ARG(0)), ARG(1), (int32_t)ARG(2))); }
static void MEMFreeToExpHeap(ppc_ctx *c) { if (ARG(1)) exp_free(heap_for(c, ARG(0)), ARG(1)); }
static void MEMGetTotalFreeSizeForExpHeap(ppc_ctx *c) { RET(exp_free_total(heap_for(c, ARG(0)))); }
static void MEMGetAllocatableSizeForExpHeapEx(ppc_ctx *c) {
    int32_t al = (int32_t)ARG(1);
    RET(exp_allocatable(heap_for(c, ARG(0)), (uint32_t)(al < 0 ? -al : al)));
}
static void MEMGetSizeForMBlockExpHeap(ppc_ctx *c) {
    for (heap *h = g_heaps; h; h = h->next)
        for (block *b = h->blocks; b; b = b->next)
            if (b->used && b->addr == ARG(0)) { RET(b->size); return; }
    RET(0);
}

/* Frame heaps: bump allocation from both ends. */
static uint32_t frm_create(uint32_t start, uint32_t size) {
    heap *h = calloc(1, sizeof *h);
    h->handle = start;
    h->is_frame = 1;
    h->start = h->head = (start + 0x40 + 31) & ~31u;
    h->end = h->tail = (start + size) & ~31u;
    h->next = g_heaps;
    g_heaps = h;
    return h->handle;
}
static void MEMAllocFromFrmHeapEx(ppc_ctx *c) {
    heap *h = heap_for(c, ARG(0));
    uint32_t size = (ARG(1) + 3) & ~3u;
    int32_t align = (int32_t)ARG(2);
    uint32_t al = (uint32_t)(align < 0 ? -align : align);
    if (al < 4) al = 4;
    uint32_t a;
    if (align >= 0) {
        a = (h->head + al - 1) & ~(al - 1);
        if (a + size > h->tail) { RET(0); return; }
        h->head = a + size;
    } else {
        a = (h->tail - size) & ~(al - 1);
        if (a < h->head) { RET(0); return; }
        h->tail = a;
    }
    RET(a);
}
static void MEMFreeToFrmHeap(ppc_ctx *c) {
    heap *h = heap_for(c, ARG(0));
    if (ARG(1) & 1) h->head = h->start;
    if (ARG(1) & 2) h->tail = h->end;
}
static void MEMGetAllocatableSizeForFrmHeapEx(ppc_ctx *c) {
    heap *h = heap_for(c, ARG(0));
    int32_t align = (int32_t)ARG(1);
    uint32_t al = (uint32_t)(align < 0 ? -align : align);
    if (al < 4) al = 4;
    uint32_t a = (h->head + al - 1) & ~(al - 1);
    RET(a < h->tail ? h->tail - a : 0);
}

static void MEMGetBaseHeapHandle(ppc_ctx *c) { RET(ARG(0) < 9 ? g_base_heap[ARG(0)] : 0); }

/* MEMAllocator { funcs*, heap, align, 0 } */
static void MEMInitAllocatorForExpHeap(ppc_ctx *c) {
    st32(ARG(0) + 0, 0);
    st32(ARG(0) + 4, ARG(1));
    st32(ARG(0) + 8, ARG(2));
    st32(ARG(0) + 12, 0);
}
static void MEMAllocFromAllocator(ppc_ctx *c) {
    uint32_t hnd = ld32(ARG(0) + 4);
    RET(exp_alloc(heap_for(c, hnd), ARG(1), (int32_t)ld32(ARG(0) + 8)));
}
static void MEMFreeToAllocator(ppc_ctx *c) {
    if (ARG(1)) exp_free(heap_for(c, ld32(ARG(0) + 4)), ARG(1));
}

/* Default heap function-pointer targets (reached via thunks). */
static void DefaultAlloc(ppc_ctx *c) { RET(exp_alloc(heap_for(c, g_base_heap[1]), ARG(0), 0x40)); }
static void DefaultAllocEx(ppc_ctx *c) { RET(exp_alloc(heap_for(c, g_base_heap[1]), ARG(0), (int32_t)ARG(1))); }
static void DefaultFree(ppc_ctx *c) { if (ARG(0)) exp_free(heap_for(c, g_base_heap[1]), ARG(0)); }

static void OSGetMemBound(ppc_ctx *c) {
    uint32_t base = 0, size = 0;
    if (ARG(0) == 1) base = MEM1_BASE, size = MEM1_SIZE;
    else if (ARG(0) == 2) base = MEM2_BASE, size = MEM2_END - MEM2_BASE;
    if (ARG(1)) st32(ARG(1), base);
    if (ARG(2)) st32(ARG(2), size);
    RET(size ? 0 : (uint32_t)-1);
}
static void OSGetForegroundBucketFreeArea(ppc_ctx *c) {
    /* Cemu reserves the start of the bucket for the OS; the free area follows. */
    if (ARG(0)) st32(ARG(0), FG_BASE + 0x00800000);
    if (ARG(1)) st32(ARG(1), FG_SIZE - 0x00800000);
    RET(1);
}

/* ---- system / time ---------------------------------------------------------------- */
static uint32_t g_sysinfo, g_main_thread;
void hle_attach_main(ppc_ctx *c) { hle_thread_attach(c, g_main_thread); }
static uint64_t g_ticks_since_2000_at_boot;
static struct timespec g_boot;

static uint64_t ticks_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    uint64_t ns = (uint64_t)(t.tv_sec - g_boot.tv_sec) * 1000000000ull + (uint64_t)(t.tv_nsec - g_boot.tv_nsec);
    return (uint64_t)((__uint128_t)ns * TIMER_HZ / 1000000000ull);
}
uint64_t ppc_timebase(ppc_ctx *c) { (void)c; return g_ticks_since_2000_at_boot + ticks_now(); }

static void ret64(ppc_ctx *c, uint64_t v) { c->r[3] = (uint32_t)(v >> 32); c->r[4] = (uint32_t)v; }
static void OSGetTime(ppc_ctx *c) { ret64(c, g_ticks_since_2000_at_boot + ticks_now()); }
static void OSGetSystemTime(ppc_ctx *c) { ret64(c, ticks_now()); }
static void OSGetSystemInfo(ppc_ctx *c) { RET(g_sysinfo); }
static void OSGetMainCoreId(ppc_ctx *c) { RET(1); }
static void OSIsDebuggerInitialized(ppc_ctx *c) { RET(0); }
static void bspGetHardwareVersion(ppc_ctx *c) { RET(0x21201000); /* retail Cafe */ }

/* ---- simple return helpers --------------------------------------------------- */
static void Ret0(ppc_ctx *c) { RET(0); }
static void Ret1(ppc_ctx *c) { RET(1); }
static void Nop(ppc_ctx *c) { (void)c; }

/* ---- libc-ish ------------------------------------------------------------------------ */
static void hle_memcpy(ppc_ctx *c) { memcpy(g_mem + ARG(0), g_mem + ARG(1), ARG(2)); }
static void hle_memmove(ppc_ctx *c) { memmove(g_mem + ARG(0), g_mem + ARG(1), ARG(2)); }
static void hle_memset(ppc_ctx *c) { memset(g_mem + ARG(0), (int)(ARG(1) & 0xFF), ARG(2)); }
static void OSReport(ppc_ctx *c) { fprintf(stderr, "OSReport: %s", gstr(ARG(0))); }
static void OSConsoleWrite(ppc_ctx *c) { fwrite(g_mem + ARG(0), 1, ARG(1), stderr); }
static void OSFatal(ppc_ctx *c) { rt_fatal(c, "OSFatal: %s", gstr(ARG(0))); }
static void OSPanic(ppc_ctx *c) { rt_fatal(c, "OSPanic: %s:%u %s", gstr(ARG(0)), ARG(1), gstr(ARG(2))); }

/* ---- dynamic loading ---------------------------------------------------------------------
 * Module handles are small integers; exports resolve to HLE thunks by name. */
static const char *g_modules[32];
static int g_nmodules;
static void OSDynLoad_Acquire(ppc_ctx *c) {
    const char *name = gstr(ARG(0));
    int i;
    for (i = 0; i < g_nmodules; i++)
        if (!strcmp(g_modules[i], name)) break;
    if (i == g_nmodules && g_nmodules < 32) g_modules[g_nmodules++] = strdup(name);
    st32(ARG(1), 0x1000 + i);
    RET(0);
}
static void OSDynLoad_FindExport(ppc_ctx *c) {
    const char *name = gstr(ARG(2));
    uint32_t module = ARG(0) - 0x1000;
    const char *lib = module < (uint32_t)g_nmodules ? g_modules[module] : "?";
    uint32_t t = ARG(1) ? 0 : hle_export_addr(lib, name);
    fprintf(stderr, "OSDynLoad_FindExport(%s, data=%u, %s) -> %08x\n",
            (ARG(0) - 0x1000) < (uint32_t)g_nmodules ? g_modules[ARG(0) - 0x1000] : "?", ARG(1), name, t);
    st32(ARG(3), t);
    if(getenv("WIIU_TRACE_EXPORTS"))fprintf(stderr,"export caller=%08x output=%08x result=%08x\n",c->lr,ARG(3),t?0:0xFFFCFFE9u);
    RET(t ? 0 : 0xFFFCFFE9);
}

/* ---- registry ---------------------------------------------------------------------------- */
static const struct { const char *lib, *name; hle_fn fn; } k_hle[] = {
    {"coreinit", "MEMCreateExpHeapEx", MEMCreateExpHeapEx},
    {"coreinit", "MEMDestroyExpHeap", MEMDestroyExpHeap},
    {"coreinit", "MEMAllocFromExpHeapEx", MEMAllocFromExpHeapEx},
    {"coreinit", "MEMFreeToExpHeap", MEMFreeToExpHeap},
    {"coreinit", "MEMGetTotalFreeSizeForExpHeap", MEMGetTotalFreeSizeForExpHeap},
    {"coreinit", "MEMGetAllocatableSizeForExpHeapEx", MEMGetAllocatableSizeForExpHeapEx},
    {"coreinit", "MEMGetSizeForMBlockExpHeap", MEMGetSizeForMBlockExpHeap},
    {"coreinit", "MEMAllocFromFrmHeapEx", MEMAllocFromFrmHeapEx},
    {"coreinit", "MEMFreeToFrmHeap", MEMFreeToFrmHeap},
    {"coreinit", "MEMGetAllocatableSizeForFrmHeapEx", MEMGetAllocatableSizeForFrmHeapEx},
    {"coreinit", "MEMGetBaseHeapHandle", MEMGetBaseHeapHandle},
    {"coreinit", "MEMInitAllocatorForExpHeap", MEMInitAllocatorForExpHeap},
    {"coreinit", "MEMAllocFromAllocator", MEMAllocFromAllocator},
    {"coreinit", "MEMFreeToAllocator", MEMFreeToAllocator},
    {"coreinit", "MEMAllocFromDefaultHeap", DefaultAlloc},
    {"coreinit", "MEMAllocFromDefaultHeapEx", DefaultAllocEx},
    {"coreinit", "MEMFreeToDefaultHeap", DefaultFree},
    {"coreinit", "OSGetMemBound", OSGetMemBound},
    {"coreinit", "OSGetForegroundBucketFreeArea", OSGetForegroundBucketFreeArea},
    {"coreinit", "OSGetTime", OSGetTime},
    {"coreinit", "OSGetSystemTime", OSGetSystemTime},
    {"coreinit", "OSGetSystemInfo", OSGetSystemInfo},
    {"coreinit", "OSGetMainCoreId", OSGetMainCoreId},
    {"coreinit", "OSIsDebuggerInitialized", OSIsDebuggerInitialized},
    {"coreinit", "bspGetHardwareVersion", bspGetHardwareVersion},
    {"coreinit", "OSDisableInterrupts", Ret1},
    {"coreinit", "OSRestoreInterrupts", Nop},
    {"coreinit", "DCFlushRange", Nop},
    {"coreinit", "DCTouchRange", Nop},
    {"coreinit", "memcpy", hle_memcpy},
    {"coreinit", "memmove", hle_memmove},
    {"coreinit", "memset", hle_memset},
    {"coreinit", "OSReport", OSReport},
    {"coreinit", "OSConsoleWrite", OSConsoleWrite},
    {"coreinit", "OSFatal", OSFatal},
    {"coreinit", "OSPanic", OSPanic},
    {"coreinit", "OSDynLoad_Acquire", OSDynLoad_Acquire},
    {"coreinit", "OSDynLoad_FindExport", OSDynLoad_FindExport},
    {"coreinit", "OSDynLoad_Release", Nop},
    {"coreinit", "OSDynLoad_SetAllocator", Ret0},
};
#define N_HLE (sizeof k_hle / sizeof *k_hle)

hle_fn hle_lookup(const char *lib, const char *name) {
    char normalized[64];
    size_t len = strlen(lib);
    if (len > 4 && len < sizeof normalized && !strcmp(lib + len - 4, ".rpl")) {
        memcpy(normalized, lib, len - 4); normalized[len - 4] = 0; lib = normalized;
    }
    if (!strcmp(lib, "h264")) return hle_h264_lookup(name);
    if (!strcmp(lib, "snd_core")) return hle_audio_lookup(name);
    if (!strcmp(lib, "nn_save")) return hle_save_lookup(name);
    if (!strcmp(lib, "nn_act")) return hle_account_lookup(name);
    if (!strcmp(lib, "vpad")||!strcmp(lib,"padscore")) return hle_input_lookup(name);
    if (!strcmp(lib, "swkbd")) return hle_swkbd_lookup(name);
    if (!strcmp(lib, "erreula")) return hle_erreula_lookup(name);
    if (!strcmp(lib, "dmae")) return hle_dmae_lookup(name);
    if (!strcmp(lib, "gx2")) {
        hle_fn fn = hle_gpu_validate_lookup(name);
        if (fn) return fn;
        fn = hle_gx2_fetch_lookup(name);
        if (fn) return fn;
        fn = hle_gx2_sampler_lookup(name);
        return fn ? fn : hle_gx2_lookup(name);
    }
    if (!strcmp(lib, "coreinit")) {
        hle_fn fn = hle_alarm_lookup(name);
        if (fn) return fn;
        fn = hle_thread_lookup(name);
        if (fn) return fn;
        fn = hle_ipcbuf_lookup(name);
        if (fn) return fn;
        fn = hle_fs_lookup(name);
        if (fn) return fn;
    }
    for (size_t i = 0; i < N_HLE; i++)
        if (!strcmp(k_hle[i].lib, lib) && !strcmp(k_hle[i].name, name)) return k_hle[i].fn;
    return NULL;
}

/* Thunks: guest-callable addresses for HLE functions reached through pointers. */
static hle_fn g_thunks[4096];
static uint32_t g_nthunks;
uint32_t hle_thunk_addr(const char *name) {
    return hle_export_addr("coreinit", name);
}
uint32_t hle_export_addr(const char *lib, const char *name) {
    hle_fn fn = hle_lookup(lib, name);
    if (!fn) return 0;
    for (uint32_t i = 0; i < g_nthunks; i++)
        if (g_thunks[i] == fn) return THUNK_BASE + 4 * i;
    if (g_nthunks == 4096) rt_fatal(NULL, "HLE thunk table full");
    g_thunks[g_nthunks] = fn;
    return THUNK_BASE + 4 * g_nthunks++;
}
int hle_call_thunk(ppc_ctx *c, uint32_t addr) {
    if (addr < THUNK_BASE || addr >= THUNK_BASE + 4 * g_nthunks || (addr & 3)) return 0;
    hle_thread_poll(c);
    g_thunks[(addr - THUNK_BASE) / 4](c);
    return 1;
}

void hle_init_data_import(uint32_t addr, const char *name) {
    uint32_t t = hle_thunk_addr(name);
    if (t) { st32(addr, t); return; }
    if (!strcmp(name, "__gh_FOPEN_MAX")) { st32(addr, 20); return; }
    /* __atexit_cleanup, __cpp_exception_*_ptr, _iob: zero-initialised objects */
}

void hle_init(void) {
    g_mem2_base = (g_loaded_data_end + 0xFFFFu) & ~0xFFFFu;
    if (g_mem2_base < 0x10400000u) g_mem2_base = 0x10400000u;
    if (g_mem2_base >= MEM2_END) rt_fatal(NULL, "RPX data leaves no room for MEM2 heap");
    clock_gettime(CLOCK_MONOTONIC, &g_boot);
    g_ticks_since_2000_at_boot = (uint64_t)(time(NULL) - 946684800) * TIMER_HZ;

    g_sysinfo = hle_sys_alloc(0x20);
    st32(g_sysinfo + 0, 248625000);   /* bus clock */
    st32(g_sysinfo + 4, 1243125000);  /* core clock */
    st64(g_sysinfo + 8, g_ticks_since_2000_at_boot);
    st32(g_sysinfo + 0x10, 512 * 1024);
    st32(g_sysinfo + 0x14, 2048 * 1024);
    st32(g_sysinfo + 0x18, 512 * 1024);
    st32(g_sysinfo + 0x1C, 5);

    g_main_thread = hle_sys_alloc(0x6A0);
    st32(g_main_thread + 0x320, 0x74487244); /* 'tHrD' */

    g_base_heap[0] = frm_create(MEM1_BASE, MEM1_SIZE);
    g_base_heap[1] = exp_create(MEM2_BASE, MEM2_END - MEM2_BASE);
    g_base_heap[8] = frm_create(FG_BASE + 0x00800000, FG_SIZE - 0x00800000);
}
