/* Synthetic guest threads: argument delivery, event handoff, recursive mutex
 * contention, join results, and timeout. No generated/game code needed. */
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include "standalone/rt.h"
uint8_t *g_mem;
static int progress;
uint32_t hle_sys_alloc(uint32_t size){static uint32_t next=0x80000;uint32_t a=next;next=(next+size+63)&~63u;assert(next<0x100000);return a;}
static int service_calls;
static void service(ppc_ctx *c){assert(c->r[2]==0x1111&&c->r[13]==0x2222);service_calls++;}
static void call(ppc_ctx *c, const char *name, uint32_t a, uint32_t b, uint32_t d) {
    c->r[3] = a; c->r[4] = b; c->r[5] = d;
    hle_fn fn = hle_thread_lookup(name); assert(fn); fn(c);
}
void rt_fatal(ppc_ctx *c, const char *fmt, ...) {
    (void)c; va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); abort();
}
void ppc_call_indirect(ppc_ctx *c, uint32_t entry) {
    assert(c->r[3] == 123 && c->r[4] == 456);
    assert(c->r[2] == 0x1111 && c->r[13] == 0x2222);
    call(c, "OSGetCurrentThread", 0, 0, 0);
    assert(c->r[3] == 0x10000 + entry * 0x1000);
    progress++;
    if (entry == 1) call(c, "OSWaitEvent", 0x20000, 0, 0);
    else if (entry == 2) {
        call(c, "OSTryLockMutex", 0x21000, 0, 0); assert(c->r[3] == 0);
        call(c, "OSLockMutex", 0x21000, 0, 0);
        call(c, "OSUnlockMutex", 0x21000, 0, 0);
    } else if (entry == 3) {
        call(c, "OSWaitSemaphore", 0x23000, 0, 0); assert(c->r[3] == 1);
    } else {
        call(c, "OSReceiveMessage", 0x24000, 0x26000, 1);
        assert(c->r[3] == 1 && ld32(0x26000) == 99 && ld32(0x2600c) == 100);
    }
    progress++;
    c->r[3] = 42;
}
static void create(ppc_ctx *c, uint32_t addr, uint32_t entry) {
    c->r[3] = addr; c->r[4] = entry; c->r[5] = 123; c->r[6] = 456;
    c->r[7] = 0x90000; c->r[8] = 0x10000; c->r[9] = 16; c->r[10] = 2;
    hle_thread_lookup("OSCreateThread")(c); assert(c->r[3] == 1);
    assert(ld32(addr + 0x328) == 1);
}
int main(void) {
    g_mem = calloc(1, 0x100000); assert(g_mem);
    ppc_ctx c = {0}; c.r[2] = 0x1111; c.r[13] = 0x2222;
    hle_thread_attach(&c, 0x10000);
    call(&c, "OSInitEvent", 0x20000, 0, 1);
    create(&c, 0x11000, 1);
    call(&c, "OSResumeThread", 0x11000, 0, 0); assert(c.r[3] == 1 && progress == 1);
    call(&c, "OSSignalEvent", 0x20000, 0, 0);
    call(&c, "OSJoinThread", 0x11000, 0x22000, 0);
    assert(c.r[3] == 1 && progress == 2 && ld32(0x22000) == 42);
    assert(ld32(0x2000c) == 0); /* auto-reset consumed by waiter */
    call(&c, "OSInitMutex", 0x21000, 0, 0);
    call(&c, "OSLockMutex", 0x21000, 0, 0);
    call(&c, "OSLockMutex", 0x21000, 0, 0);
    create(&c, 0x12000, 2);
    call(&c, "OSResumeThread", 0x12000, 0, 0); assert(progress == 3);
    call(&c, "OSUnlockMutex", 0x21000, 0, 0);
    call(&c, "OSYieldThread", 0, 0, 0); assert(progress == 3);
    call(&c, "OSUnlockMutex", 0x21000, 0, 0);
    call(&c, "OSJoinThread", 0x12000, 0x22000, 0); assert(progress == 4);
    c.r[6] = 1000000;
    call(&c, "OSWaitEventWithTimeout", 0x20000, 0, 0); assert(c.r[3] == 0);
    call(&c, "OSInitSemaphore", 0x23000, 0, 0);
    create(&c, 0x13000, 3);
    call(&c, "OSResumeThread", 0x13000, 0, 0); assert(progress == 5);
    call(&c, "OSSignalSemaphore", 0x23000, 0, 0); assert(c.r[3] == 0);
    call(&c, "OSJoinThread", 0x13000, 0, 0); assert(progress == 6);
    call(&c, "OSTryWaitSemaphore", 0x23000, 0, 0); assert(c.r[3] == 0);
    call(&c, "OSInitMessageQueue", 0x24000, 0x25000, 2);
    create(&c, 0x14000, 4);
    call(&c, "OSResumeThread", 0x14000, 0, 0); assert(progress == 7);
    st32(0x27000, 99); st32(0x2700c, 100);
    call(&c, "OSSendMessage", 0x24000, 0x27000, 0); assert(c.r[3] == 1);
    call(&c, "OSJoinThread", 0x14000, 0, 0); assert(progress == 8);
    call(&c, "OSSendMessage", 0x24000, 0x27000, 0);
    st32(0x27000, 77);
    call(&c, "OSSendMessage", 0x24000, 0x27000, 2); assert(c.r[3] == 1);
    call(&c, "OSSendMessage", 0x24000, 0x27000, 0); assert(c.r[3] == 0);
    call(&c, "OSPeekMessage", 0x24000, 0x26000, 0); assert(ld32(0x26000) == 77);
    call(&c, "OSReceiveMessage", 0x24000, 0x26000, 0); assert(ld32(0x26000) == 77);
    call(&c, "OSReceiveMessage", 0x24000, 0x26000, 0); assert(ld32(0x26000) == 99);
    call(&c, "OSReceiveMessage", 0x24000, 0x26000, 0); assert(c.r[3] == 0);
    /* Recreate a joined worker repeatedly, exactly as streaming decompression
     * does. Every generation must execute with fresh arguments and context. */
    for (unsigned i = 0; i < 20; i++) {
        call(&c, "OSSignalEvent", 0x20000, 0, 0);
        create(&c, 0x11000, 1);
        call(&c, "OSResumeThread", 0x11000, 0, 0);
        call(&c, "OSJoinThread", 0x11000, 0x22000, 0);
        assert(c.r[3] == 1 && ld32(0x22000) == 42);
        assert(progress == 10 + 2 * (int)i);
    }
    hle_service_start(&c,service,1000000);
    call(&c,"OSSleepTicks",0,621562,0);
    assert(service_calls>=2);
    puts("thread/event/mutex/timeout/semaphore/message tests passed");
    return 0;
}
