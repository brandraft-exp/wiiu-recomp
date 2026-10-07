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
uint32_t hle_export_addr(const char *lib, const char *name);
int hle_call_thunk(ppc_ctx *c, uint32_t addr);
hle_fn hle_lookup(const char *lib, const char *name);

extern int g_trace_imports;
extern uint32_t g_loaded_data_end;
hle_fn hle_thread_lookup(const char *name);
void hle_thread_attach(ppc_ctx *c, uint32_t addr);
void hle_thread_poll(ppc_ctx *c);
void hle_attach_main(ppc_ctx *c);
hle_fn hle_fs_lookup(const char *name);
void hle_fs_configure(const char *base, const char *update, const char *save);
hle_fn hle_gx2_lookup(const char *name);
hle_fn hle_erreula_lookup(const char *name);
hle_fn hle_ipcbuf_lookup(const char *name);
hle_fn hle_swkbd_lookup(const char *name);
hle_fn hle_dmae_lookup(const char *name);
hle_fn hle_gx2_sampler_lookup(const char *name);
hle_fn hle_gx2_fetch_lookup(const char *name);
void hle_gpu_validate_configure(const char *path);
void hle_gpu_render_configure(const char *path);
void hle_gpu_memory_written(uint32_t address, uint32_t size);
hle_fn hle_gpu_validate_lookup(const char *name);

hle_fn hle_input_lookup(const char *name);
void hle_window_enable(void);
void hle_debug_overlay_enable(void);
void hle_window_present(const void *rgb, int width, int height);
void hle_input_replay(const char *path);
void hle_text_replay(const char *path);
void hle_text_input(const char *utf8);
void hle_text_begin(const uint16_t *initial, unsigned limit);
void hle_text_end(void);
void hle_text_allow_confirm(int enabled);
int hle_text_poll(uint16_t *out, unsigned *length);
hle_fn hle_save_lookup(const char *name);
hle_fn hle_account_lookup(const char *name);
uint32_t hle_sys_alloc(uint32_t size);
void hle_service_start(ppc_ctx *c, hle_fn fn, uint64_t period_ns);
hle_fn hle_audio_lookup(const char *name);
hle_fn hle_alarm_lookup(const char *name);
void hle_audio_enable(void);
void hle_audio_mix(int16_t *output, unsigned samples);
hle_fn hle_h264_lookup(const char *name);
void hle_thread_dump(void);
