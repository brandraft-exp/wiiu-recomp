/* Minimal standalone runtime: load an RPX at its link addresses and run the
 * recompiled entry point. OS imports are logged and stubbed (r3 = 0) unless an
 * HLE implementation is registered in hle.c. */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <zlib.h>

#include "ppc.h"
#include "rt.h"

uint8_t *g_mem;
uint32_t g_loaded_data_end;

extern const struct { uint32_t addr; ppc_fn fn; } g_fn_table[];
extern const size_t g_fn_count;
extern const uint32_t g_text_lo, g_text_hi;
extern const struct { uint32_t addr; const char *lib, *name; int is_data; } g_imports[];
extern const size_t g_import_count;
extern const struct { uint32_t addr; int id; } g_tramps[];
extern const size_t g_tramp_count;
extern const struct { uint32_t addr; const char *name; } g_data_aliases[];
extern const size_t g_data_alias_count;

static ppc_fn *g_fn_index; /* (addr - g_text_lo) / 4 -> fn */
static unsigned long g_import_calls;
int g_trace_imports = 1;
static const char *g_stop_before;
static int g_strict_imports;
static unsigned long g_max_imports;

void rt_fatal(ppc_ctx *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "\n*** FATAL: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (c)
        fprintf(stderr, "\n    lr=%08x r1=%08x r3=%08x r4=%08x r5=%08x r6=%08x\n", c->lr, c->r[1], c->r[3],
                c->r[4], c->r[5], c->r[6]);
    fprintf(stderr, "    after %lu import calls\n", g_import_calls);
    hle_thread_dump();
    exit(2);
}

/* ---- ppc.h hooks ---------------------------------------------------------- */

void ppc_call_indirect(ppc_ctx *c, uint32_t t) {
    if (t >= g_text_lo && t < g_text_hi && g_fn_index[(t - g_text_lo) >> 2]) {
        g_fn_index[(t - g_text_lo) >> 2](c);
        return;
    }
    if (hle_call_thunk(c, t)) return;
    int id = rt_import_by_addr(t);
    if (id >= 0) {
        ppc_import(c, id);
        return;
    }
    rt_fatal(c, "indirect call to unknown target %08x", t);
}
void ppc_branch_mid(ppc_ctx *c, uint32_t t, uint32_t from) {
    rt_fatal(c, "branch into the middle of a function: %08x -> %08x", from, t);
}
void ppc_syscall(ppc_ctx *c, uint32_t a) { rt_fatal(c, "syscall at %08x", a); }
void ppc_trap(ppc_ctx *c, uint32_t a) { rt_fatal(c, "trap at %08x", a); }

/* ---- imports -------------------------------------------------------------- */

int rt_import_by_addr(uint32_t addr) {
    for (size_t lo = 0, hi = g_tramp_count; lo < hi;) {
        size_t m = (lo + hi) / 2;
        if (g_tramps[m].addr == addr) return g_tramps[m].id;
        if (g_tramps[m].addr < addr) lo = m + 1;
        else hi = m;
    }
    size_t lo = 0, hi = g_import_count;
    while (lo < hi) {
        size_t m = (lo + hi) / 2;
        if (g_imports[m].addr < addr) lo = m + 1;
        else hi = m;
    }
    return (lo < g_import_count && g_imports[lo].addr == addr) ? (int)lo : -1;
}

const char *rt_import_name(int id) { return g_imports[id].name; }

void ppc_import(ppc_ctx *c, int id) {
    hle_thread_poll(c);
    g_import_calls++;
    if (g_max_imports && g_import_calls > g_max_imports)
        rt_fatal(c, "diagnostic import limit reached (%lu)", g_max_imports);
    if (g_stop_before) {
        char name[256];
        snprintf(name, sizeof name, "%s.%s", g_imports[id].lib, g_imports[id].name);
        if (!strcmp(name, g_stop_before)) {
            fprintf(stderr, "BOOT CHECKPOINT: %s after %lu import calls\n", name, g_import_calls);
            exit(0);
        }
    }
    hle_fn h = hle_lookup(g_imports[id].lib, g_imports[id].name);
    if (g_trace_imports)
        fprintf(stderr, "[%6lu] %s%s.%s(%08x, %08x, %08x, %08x) lr=%08x\n", g_import_calls, h ? "" : "STUB ",
                g_imports[id].lib, g_imports[id].name, c->r[3], c->r[4], c->r[5], c->r[6], c->lr);
    if (h) h(c);
    else {
        if (g_strict_imports)
            rt_fatal(c, "unimplemented import %s.%s", g_imports[id].lib, g_imports[id].name);
        c->r[3] = 0;
    }
}

/* ---- loader ----------------------------------------------------------------- */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

uint32_t g_sda_base, g_sda2_base, g_stack_size, g_entry;

uint32_t rt_entry_point(void) { return g_entry; }

static void load_rpx(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *raw = malloc(n);
    if (fread(raw, 1, n, f) != (size_t)n) { perror("read"); exit(1); }
    fclose(f);
    if (memcmp(raw, "\x7f" "ELF", 4) || raw[7] != 0xCA || raw[8] != 0xFE) { fprintf(stderr, "not an RPX\n"); exit(1); }
    g_entry = be32(raw + 0x18);
    uint32_t shoff = be32(raw + 0x20);
    uint16_t shentsize = be16(raw + 0x2E), shnum = be16(raw + 0x30);
    for (int i = 0; i < shnum; i++) {
        const uint8_t *sh = raw + shoff + i * shentsize;
        uint32_t type = be32(sh + 4), flags = be32(sh + 8), addr = be32(sh + 12), off = be32(sh + 16),
                 size = be32(sh + 20);
        if (type == 0x80000004) { /* RPL_FILEINFO */
            g_sda_base = be32(raw + off + 0x24);
            g_sda2_base = be32(raw + off + 0x28);
            g_stack_size = be32(raw + off + 0x2C);
            continue;
        }
        if (!addr || !size || addr >= 0xC0000000u) continue; /* metadata / import placeholders */
        uint32_t mem_size = (flags & 0x08000000) && type == 1 ? be32(raw + off) : size;
        if ((type == 1 || type == 8) && addr >= 0x10000000u && addr < 0x80000000u) {
            if (mem_size > 0x80000000u - addr) { fprintf(stderr, "invalid data section size\n"); exit(1); }
            if (addr + mem_size > g_loaded_data_end) g_loaded_data_end = addr + mem_size;
        }
        if (type == 8) { memset(g_mem + addr, 0, size); continue; } /* NOBITS */
        if (type != 1) continue;
        if (flags & 0x08000000) { /* RPL_ZLIB */
            uLongf out = be32(raw + off);
            if (uncompress(g_mem + addr, &out, raw + off + 4, size - 4) != Z_OK) { fprintf(stderr, "zlib\n"); exit(1); }
        } else {
            memcpy(g_mem + addr, raw + off, size);
        }
    }
    free(raw);
}

static void map_guest(void) {
    g_mem = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (g_mem == MAP_FAILED) { perror("mmap 4G"); exit(1); }
    mprotect(g_mem, 0x10000, PROT_NONE); /* catch null-pointer accesses */
}

/* Report guest-relative fault addresses and the recompiled call stack. */
static void on_segv(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    uintptr_t a = (uintptr_t)si->si_addr, base = (uintptr_t)g_mem;
    char buf[160];
    int n = (a >= base && a < base + (1ull << 32))
                ? snprintf(buf, sizeof buf, "\n*** SIGNAL %d at guest address %08lx\n", sig, (unsigned long)(a - base))
                : snprintf(buf, sizeof buf, "\n*** SIGNAL %d at host address %p\n", sig, si->si_addr);
    write(2, buf, n);
    void *frames[48];
    int nf = backtrace(frames, 48);
    backtrace_symbols_fd(frames, nf, 2);
    _exit(3);
}

static void init_data_imports(void) {
    for (size_t i = 0; i < g_import_count; i++)
        if (g_imports[i].is_data) hle_init_data_import(g_imports[i].addr, g_imports[i].name);
    for (size_t i = 0; i < g_data_alias_count; i++)
        hle_init_data_import(g_data_aliases[i].addr, g_data_aliases[i].name);
}

static void build_fn_index(void) {
    g_fn_index = calloc((g_text_hi - g_text_lo) / 4 + 1, sizeof(ppc_fn));
    for (size_t i = 0; i < g_fn_count; i++)
        g_fn_index[(g_fn_table[i].addr - g_text_lo) >> 2] = g_fn_table[i].fn;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.rpx> [entry-hex] [--quiet] [--strict-imports] [--stop-before library.function]\n", argv[0]);
        fprintf(stderr, "       [--content-base dir] [--content-update dir] [--save-dir existing-dir]\n");
        fprintf(stderr, "       [--debug-overlay] [--max-imports count] [--input-replay FILE] [--keyboard-replay FILE]\n");
        return 1;
    }
    uint32_t entry_override = 0;
    const char *content_base = NULL, *content_update = NULL, *save_dir = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--quiet")) g_trace_imports = 0;
        else if (!strcmp(argv[i], "--strict-imports")) g_strict_imports = 1;
        else if (!strcmp(argv[i], "--stop-before") && i + 1 < argc) g_stop_before = argv[++i];
        else if (!strcmp(argv[i], "--content-base") && i + 1 < argc) content_base = argv[++i];
        else if (!strcmp(argv[i], "--content-update") && i + 1 < argc) content_update = argv[++i];
        else if (!strcmp(argv[i], "--save-dir") && i + 1 < argc) save_dir = argv[++i];
        else if (!strcmp(argv[i], "--input-replay") && i+1<argc) hle_input_replay(argv[++i]);
        else if (!strcmp(argv[i], "--keyboard-replay") && i+1<argc) hle_text_replay(argv[++i]);
        else if (!strcmp(argv[i], "--audio")) hle_audio_enable();
        else if (!strcmp(argv[i], "--debug-overlay")) hle_debug_overlay_enable();
        else if (!strcmp(argv[i], "--window")) hle_window_enable();
        else if (!strcmp(argv[i], "--gpu-render") && i + 1 < argc) hle_gpu_render_configure(argv[++i]);
        else if (!strcmp(argv[i], "--gpu-validate") && i + 1 < argc) hle_gpu_validate_configure(argv[++i]);
        else if (!strcmp(argv[i], "--max-imports") && i + 1 < argc) {
            char *end;
            const char *value = argv[++i];
            g_max_imports = strtoul(value, &end, 10);
            if (!*value || *end || *value == '-' || !g_max_imports) {
                fprintf(stderr, "invalid import limit: %s\n", value); return 1;
            }
        }
        else {
            char *end;
            unsigned long entry = strtoul(argv[i], &end, 16);
            if (!*argv[i] || *end || entry > UINT32_MAX || !entry || entry_override) {
                fprintf(stderr, "invalid argument: %s\n", argv[i]); return 1;
            }
            entry_override = (uint32_t)entry;
        }
    }
    hle_fs_configure(content_base, content_update, save_dir);
    map_guest();
    load_rpx(argv[1]);
    build_fn_index();
    hle_init();
    init_data_imports();
    struct sigaction sa = {0};
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    static uint8_t altstack[65536];
    stack_t ss = {.ss_sp = altstack, .ss_size = sizeof altstack};
    sigaltstack(&ss, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    ppc_ctx *c = calloc(1, sizeof(ppc_ctx));
    hle_attach_main(c);
    uint32_t stack_top = RT_STACK_TOP;
    c->r[1] = stack_top - 0x20;
    c->r[2] = g_sda2_base;
    c->r[13] = g_sda_base;
    c->r[3] = 0; /* argc */
    c->r[4] = 0; /* argv */
    c->lr = 0;
    uint32_t entry = entry_override ? entry_override : rt_entry_point();
    fprintf(stderr, "entry %08x  r1=%08x r2=%08x r13=%08x  (%zu functions)\n", entry, c->r[1], c->r[2], c->r[13],
            g_fn_count);
    ppc_call_indirect(c, entry);
    fprintf(stderr, "entry returned r3=%08x after %lu import calls\n", c->r[3], g_import_calls);
    return 0;
}
