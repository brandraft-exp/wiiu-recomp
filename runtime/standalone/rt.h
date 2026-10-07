/* Shared declarations for the standalone runtime. */
#pragma once
#include "ppc.h"

#define RT_STACK_TOP 0x4F000000u

typedef void (*hle_fn)(ppc_ctx *c);

void ppc_import(ppc_ctx *c, int id);
int rt_import_by_addr(uint32_t addr);
const char *rt_import_name(int id);
uint32_t rt_entry_point(void);
void rt_fatal(ppc_ctx *c, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));

void hle_init(void);
void hle_init_data_import(uint32_t addr, const char *name);
uint32_t hle_thunk_addr(const char *name);
int hle_call_thunk(ppc_ctx *c, uint32_t addr);
hle_fn hle_lookup(const char *lib, const char *name);

extern int g_trace_imports;
