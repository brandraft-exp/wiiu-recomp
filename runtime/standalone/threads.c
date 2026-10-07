/* Serialized guest threads. A scheduler token and a host mutex ensure that
 * exactly one pthread executes guest code. Round-robin switches occur at HLE
 * safe points, not at arbitrary guest instructions (no preemption yet).
 * ABI/semantics reference: Cemu coreinit_Thread.cpp (__OSThreadInit,
 * __OSCreateThreadInternal2, OSResumeThread, OSJoinThread),
 * coreinit_Synchronization.cpp (OS*Mutex, OS*Event).
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include "rt.h"

#define A(n) c->r[3 + (n)]
#define RET(v) (c->r[3] = (uint32_t)(v))
#define HZ 62156250ull
typedef struct guest_thread {
    uint32_t addr, entry, suspend, result, attr;
    uint32_t wait_addr;
    int wait_kind, done, joined, timed_out;
    uint64_t deadline, service_period;
    hle_fn service;
    ppc_ctx ctx, *cpu;
    pthread_cond_t cv;
    pthread_t native;
    struct guest_thread *next;
} guest_thread;
enum { WAIT_EVENT = 1, WAIT_MUTEX, WAIT_JOIN, WAIT_SLEEP, WAIT_SEM, WAIT_SEND, WAIT_RECEIVE, WAIT_GHS };
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static guest_thread *threads, *current;
static unsigned quantum;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
static guest_thread *find(uint32_t addr) {
    for (guest_thread *t = threads; t; t = t->next) if (t->addr == addr) return t;
    return NULL;
}
static void state(guest_thread *t) {
    st8(t->addr + 0x324, t->done ? (t->attr & 8 ? 0 : 8) : t->wait_kind ? 4 : t == current ? 2 : 1);
    st32(t->addr + 0x328, t->suspend);
}
static void schedule(void) {
    guest_thread *old = current, *pick;
    for (;;) {
        uint64_t now = now_ns(), earliest = UINT64_MAX;
        for (guest_thread *t = threads; t; t = t->next) {
            if (t->deadline && t->deadline <= now) {
                t->wait_kind = 0; t->deadline = 0; t->timed_out = 1;
            }
            if (t->deadline && t->deadline < earliest) earliest = t->deadline;
        }
        pick = old->next ? old->next : threads;
        guest_thread *start = pick;
        do {
            if (!pick->done && !pick->suspend && !pick->wait_kind) break;
            pick = pick->next ? pick->next : threads;
        } while (pick != start);
        if (!pick->done && !pick->suspend && !pick->wait_kind) break;
        if (earliest == UINT64_MAX) {
            for (guest_thread *t = threads; t; t = t->next)
                fprintf(stderr, "thread %08x: done=%d suspend=%u wait=%d object=%08x\n",
                        t->addr, t->done, t->suspend, t->wait_kind, t->wait_addr);
            rt_fatal(old->cpu, "guest scheduler has no runnable threads or timers");
        }
        uint64_t delay = earliest > now ? earliest - now : 0;
        struct timespec ts = {delay / 1000000000ull, delay % 1000000000ull};
        while (nanosleep(&ts, &ts) && errno == EINTR) {}
    }
    current = pick;
    state(old); state(pick);
    if (pick != old) pthread_cond_signal(&pick->cv);
    if (old->done) { pthread_mutex_unlock(&lock); pthread_exit(NULL); }
    while (current != old) pthread_cond_wait(&old->cv, &lock);
}
void hle_thread_poll(ppc_ctx *c) {
    if (current && c->host && ++quantum % 128 == 0) schedule();
}
static void wait_on(int kind, uint32_t addr, uint64_t deadline) {
    current->wait_kind = kind; current->wait_addr = addr;
    current->deadline = deadline; current->timed_out = 0;
    schedule();
}
static int wake(int kind, uint32_t addr, int all) {
    int n = 0;
    for (guest_thread *t = threads; t; t = t->next) {
        if (t->wait_kind != kind || t->wait_addr != addr) continue;
        t->wait_kind = 0; t->deadline = 0; state(t); n++;
        if (!all) break;
    }
    return n;
}
static void finish(ppc_ctx *c) {
    current->result = A(0); current->done = 1;
    st32(current->addr + 0x334, A(0));
    wake(WAIT_JOIN, current->addr, 1);
    schedule();
    abort();
}
static void *thread_start(void *arg) {
    guest_thread *t = arg;
    pthread_mutex_lock(&lock);
    while (current != t) pthread_cond_wait(&t->cv, &lock);
    if(t->service){
        uint64_t next=now_ns();
        for(;;){
            t->service(t->cpu);
            // Keep periodic services on their clock, including time spent in
            // callbacks and waiting for the guest scheduler. Relative sleeps
            // after each callback made AX generate far less than 32 kHz.
            next+=t->service_period;
            wait_on(WAIT_SLEEP,0,next);
        }
    }
    ppc_call_indirect(t->cpu, t->entry);
    finish(t->cpu);
    return NULL;
}
void hle_thread_attach(ppc_ctx *c, uint32_t addr) {
    pthread_mutex_lock(&lock);
    guest_thread *t = calloc(1, sizeof *t);
    if (!t) rt_fatal(c, "allocating main thread");
    t->addr = addr; t->cpu = c; pthread_cond_init(&t->cv, NULL);
    threads = current = t; c->host = t; state(t);
}
static void OSGetCurrentThread(ppc_ctx *c) { RET(current->addr); }
static void OSCreateThread(ppc_ctx *c) {
    uint32_t addr = A(0), top = A(4), size = A(5), priority = A(6), attr = A(7);
    guest_thread *previous = find(addr);
    if (!addr || !A(1) || priority >= 32 || attr >= 32 || size < 16 || top < size ||
        (previous && (!previous->done || (!previous->joined && !(previous->attr & 8))))) {
        RET(0); return;
    }
    /* Cafe permits reuse after join or detached completion. Wait for the host
     * pthread to finish before releasing its context and condition variable. */
    if (previous) {
        pthread_join(previous->native, NULL);
        guest_thread **link = &threads;
        while (*link != previous) link = &(*link)->next;
        *link = previous->next;
        pthread_cond_destroy(&previous->cv); free(previous);
    }
    guest_thread *t = calloc(1, sizeof *t);
    if (!t) { RET(0); return; }
    t->addr = addr; t->entry = A(1); t->suspend = 1; t->attr = attr;
    t->cpu = &t->ctx; t->ctx.host = t;
    t->ctx.r[1] = (top & ~7u) - 8;
    t->ctx.r[2] = c->r[2]; t->ctx.r[13] = c->r[13];
    t->ctx.r[3] = A(2); t->ctx.r[4] = A(3);
    memset(g_mem + addr, 0, 0x6a0);
    st32(addr + 0x320, 0x74487244); st8(addr + 0x325, attr); st16(addr + 0x326, 0x8000);
    st32(addr + 0x32c, priority + 64); st32(addr + 0x330, priority + 64);
    st32(addr + 0x394, top); st32(addr + 0x398, top - size); st32(addr + 0x39c, t->entry);
    st32(addr + 0x5bc, 2); st32(top - size, 0xdeadbabe); st64(t->ctx.r[1], 0);
    pthread_cond_init(&t->cv, NULL);
    int error = pthread_create(&t->native, NULL, thread_start, t);
    if (error) { pthread_cond_destroy(&t->cv); free(t); RET(0); return; }
    guest_thread *tail = threads;
    while (tail->next) tail = tail->next;
    tail->next = t; state(t); RET(1);
}
void hle_service_start(ppc_ctx *c, hle_fn fn, uint64_t period_ns) {
    uint32_t addr=hle_sys_alloc(0x6a0),stack=hle_sys_alloc(0x10000);
    ppc_ctx call={0};call.r[2]=c->r[2];call.r[13]=c->r[13];
    call.r[3]=addr;call.r[4]=1;call.r[7]=stack+0x10000;call.r[8]=0x10000;call.r[9]=0;call.r[10]=8;
    OSCreateThread(&call);if(!call.r[3])rt_fatal(c,"creating runtime service thread");
    guest_thread *t=find(addr);t->service=fn;t->service_period=period_ns;t->suspend=0;state(t);
}
static void OSResumeThread(ppc_ctx *c) {
    guest_thread *t = find(A(0));
    if (!t) { RET(0); return; }
    uint32_t old = t->suspend;
    if (old) t->suspend--;
    state(t); RET(old);
    if (old == 1) schedule();
}
static void OSJoinThread(ppc_ctx *c) {
    guest_thread *t = find(A(0)); uint32_t out = A(1);
    if (!t || t == current || t->attr & 8 || t->joined) { RET(0); return; }
    while (!t->done) wait_on(WAIT_JOIN, t->addr, 0);
    if (out) st32(out, t->result);
    t->joined = 1; st8(t->addr + 0x324, 0); RET(1);
}
static void OSYieldThread(ppc_ctx *c) { (void)c; schedule(); }
static void OSSleepTicks(ppc_ctx *c) {
    uint64_t ticks = (uint64_t)A(0) << 32 | A(1);
    uint64_t delay = (uint64_t)((__uint128_t)ticks * 1000000000ull / HZ);
    uint64_t now = now_ns();
    wait_on(WAIT_SLEEP, 0, delay > UINT64_MAX - now ? UINT64_MAX : now + delay);
}
static void OSSetThreadName(ppc_ctx *c) { st32(A(0) + 0x5c0, A(1)); }
static void OSGetThreadName(ppc_ctx *c) { RET(ld32(A(0) + 0x5c0)); }
static void OSGetThreadPriority(ppc_ctx *c) { RET(ld32(A(0) + 0x330) - 64); }
static void OSGetThreadAffinity(ppc_ctx *c) { RET(ld8(A(0) + 0x325) & 7); }
static void OSSetThreadAffinity(ppc_ctx *c) {
    guest_thread *t = find(A(0));
    if (!t || !A(1) || A(1) > 7) { RET(0); return; }
    t->attr = (t->attr & ~7u) | A(1); st8(t->addr + 0x325, t->attr); RET(1);
}
static void OSSetThreadPriority(ppc_ctx *c) {
    if (A(1) >= 32) { RET(0); return; }
    st32(A(0) + 0x330, A(1) + 64); st32(A(0) + 0x32c, A(1) + 64); RET(1);
}
static void OSSetThreadSpecific(ppc_ctx *c) { if (A(0) < 16) st32(current->addr + 0x57c + 4 * A(0), A(1)); }
static void OSGetThreadSpecific(ppc_ctx *c) { RET(A(0) < 16 ? ld32(current->addr + 0x57c + 4 * A(0)) : 0); }
static void OSInitMutex(ppc_ctx *c) {
    memset(g_mem + A(0), 0, 0x2c); st32(A(0), 0x6d557458);
}
static void OSLockMutex(ppc_ctx *c) {
    uint32_t a = A(0);
    while (ld32(a + 0x1c) && ld32(a + 0x1c) != current->addr) wait_on(WAIT_MUTEX, a, 0);
    st32(a + 0x1c, current->addr); st32(a + 0x20, ld32(a + 0x20) + 1);
}
static void OSTryLockMutex(ppc_ctx *c) {
    if (ld32(A(0) + 0x1c) && ld32(A(0) + 0x1c) != current->addr) { RET(0); return; }
    OSLockMutex(c); RET(1);
}
static void OSUnlockMutex(ppc_ctx *c) {
    uint32_t a = A(0), count = ld32(a + 0x20);
    if (ld32(a + 0x1c) != current->addr || !count) rt_fatal(c, "unlock of unowned mutex %08x", a);
    st32(a + 0x20, --count);
    if (!count) { st32(a + 0x1c, 0); wake(WAIT_MUTEX, a, 1); }
}
static void OSInitEvent(ppc_ctx *c) {
    uint32_t a = A(0); memset(g_mem + a, 0, 0x24);
    st32(a, 0x65566e54); st32(a + 0xc, A(1)); st32(a + 0x20, A(2));
}
static void event_wait(ppc_ctx *c, int timed) {
    uint32_t a = A(0);
    /* PPC EABI aligns a 64-bit second argument to r5/r6; timeout is ns,
     * as in Cemu OSWaitEventWithTimeout, not timer ticks. */
    uint64_t delay = (uint64_t)c->r[5] << 32 | c->r[6];
    if (ld32(a + 0xc)) {
        if (ld32(a + 0x20)) st32(a + 0xc, 0);
        RET(1); return;
    }
    if (timed && !delay) { RET(0); return; }
    uint64_t now = now_ns();
    wait_on(WAIT_EVENT, a, timed ? (delay > UINT64_MAX - now ? UINT64_MAX : now + delay) : 0);
    RET(!current->timed_out);
}
static void OSWaitEvent(ppc_ctx *c) { event_wait(c, 0); }
static void OSWaitEventWithTimeout(ppc_ctx *c) { event_wait(c, 1); }
static void OSSignalEvent(ppc_ctx *c) {
    uint32_t a = A(0); int manual = !ld32(a + 0x20);
    if (ld32(a + 0xc)) return;
    int n = wake(WAIT_EVENT, a, manual);
    if (manual || !n) st32(a + 0xc, 1);
}
static void OSResetEvent(ppc_ctx *c) { st32(A(0) + 0xc, 0); }
/* Cemu coreinit_Synchronization.cpp: semaphore calls return the old count. */
static void OSInitSemaphore(ppc_ctx *c) {
    uint32_t a = A(0); memset(g_mem + a, 0, 0x20);
    st32(a, 0x73506852); st32(a + 0xc, A(1));
}
static void OSWaitSemaphore(ppc_ctx *c) {
    uint32_t a = A(0);
    while ((int32_t)ld32(a + 0xc) <= 0) wait_on(WAIT_SEM, a, 0);
    uint32_t old = ld32(a + 0xc); st32(a + 0xc, old - 1); RET(old);
}
static void OSTryWaitSemaphore(ppc_ctx *c) {
    uint32_t a = A(0), old = ld32(a + 0xc);
    if ((int32_t)old > 0) st32(a + 0xc, old - 1);
    RET(old);
}
static void OSSignalSemaphore(ppc_ctx *c) {
    uint32_t a = A(0), old = ld32(a + 0xc);
    st32(a + 0xc, old + 1); wake(WAIT_SEM, a, 1); RET(old);
}
static void OSGetSemaphoreCount(ppc_ctx *c) { RET(ld32(A(0) + 0xc)); }
/* Cemu coreinit_MessageQueue.cpp: 16-byte messages, blocking and urgent flags. */
static void OSInitMessageQueue(ppc_ctx *c) {
    uint32_t a = A(0); memset(g_mem + a, 0, 0x3c);
    st32(a, 0x6d536751); st32(a + 0x2c, A(1)); st32(a + 0x30, A(2));
}
static void OSSendMessage(ppc_ctx *c) {
    uint32_t a = A(0), msg = A(1), flags = A(2), cap = ld32(a + 0x30);
    if (!cap) { RET(0); return; }
    while (ld32(a + 0x38) >= cap) {
        if (!(flags & 1)) { RET(0); return; }
        wait_on(WAIT_SEND, a, 0);
    }
    uint32_t head = ld32(a + 0x34), used = ld32(a + 0x38), idx = (head + used) % cap;
    if (flags & 2) { idx = (head + cap - 1) % cap; st32(a + 0x34, idx); }
    memcpy(g_mem + ld32(a + 0x2c) + idx * 16, g_mem + msg, 16);
    st32(a + 0x38, used + 1); wake(WAIT_RECEIVE, a, 0); RET(1);
}
static void OSReceiveMessage(ppc_ctx *c) {
    uint32_t a = A(0), msg = A(1), flags = A(2);
    while (!ld32(a + 0x38)) {
        if (!(flags & 1)) { RET(0); return; }
        wait_on(WAIT_RECEIVE, a, 0);
    }
    uint32_t head = ld32(a + 0x34);
    memcpy(g_mem + msg, g_mem + ld32(a + 0x2c) + head * 16, 16);
    st32(a + 0x34, (head + 1) % ld32(a + 0x30));
    st32(a + 0x38, ld32(a + 0x38) - 1); wake(WAIT_SEND, a, 0); RET(1);
}
static void OSPeekMessage(ppc_ctx *c) {
    uint32_t a = A(0);
    if (!ld32(a + 0x38)) { RET(0); return; }
    if (A(1)) memcpy(g_mem + A(1), g_mem + ld32(a + 0x2c) + ld32(a + 0x34) * 16, 16);
    RET(1);
}
static uint32_t ghs_owner, ghs_depth;
static void ghs_lock(ppc_ctx *c) {
    (void)c;
    while (ghs_owner && ghs_owner != current->addr) wait_on(WAIT_GHS, 0, 0);
    ghs_owner = current->addr; ghs_depth++;
}
static void ghs_unlock(ppc_ctx *c) {
    if (ghs_owner != current->addr || !ghs_depth) rt_fatal(c, "unowned GHS lock");
    if (!--ghs_depth) { ghs_owner = 0; wake(WAIT_GHS, 0, 1); }
}
static void hle_exit(ppc_ctx *c) { exit((int)A(0)); }

hle_fn hle_thread_lookup(const char *name) {
#define EXPORT(n) if (!strcmp(name, #n)) return n
    EXPORT(OSGetCurrentThread); EXPORT(OSCreateThread); EXPORT(OSResumeThread);
    EXPORT(OSJoinThread); EXPORT(OSYieldThread); EXPORT(OSSleepTicks);
    EXPORT(OSSetThreadName); EXPORT(OSGetThreadName); EXPORT(OSGetThreadPriority); EXPORT(OSSetThreadPriority);
    EXPORT(OSGetThreadAffinity); EXPORT(OSSetThreadAffinity);
    EXPORT(OSSetThreadSpecific); EXPORT(OSGetThreadSpecific);
    EXPORT(OSInitMutex); EXPORT(OSLockMutex); EXPORT(OSTryLockMutex); EXPORT(OSUnlockMutex);
    EXPORT(OSInitEvent); EXPORT(OSWaitEvent); EXPORT(OSWaitEventWithTimeout);
    EXPORT(OSSignalEvent); EXPORT(OSResetEvent);
    EXPORT(OSInitSemaphore); EXPORT(OSWaitSemaphore); EXPORT(OSTryWaitSemaphore);
    EXPORT(OSSignalSemaphore); EXPORT(OSGetSemaphoreCount);
    EXPORT(OSInitMessageQueue); EXPORT(OSSendMessage); EXPORT(OSReceiveMessage); EXPORT(OSPeekMessage);
    if (!strcmp(name, "__ghsLock")) return ghs_lock;
    if (!strcmp(name, "__ghsUnlock")) return ghs_unlock;
    if (!strcmp(name, "OSExitThread")) return finish;
    if (!strcmp(name, "exit") || !strcmp(name, "_Exit")) return hle_exit;
    return NULL;
#undef EXPORT
}

void hle_thread_dump(void){
    for(guest_thread *t=threads;t;t=t->next)if(!t->done)fprintf(stderr,"thread %08x entry=%08x lr=%08x sp=%08x suspend=%u wait=%d object=%08x service=%d\n",t->addr,t->entry,t->cpu->lr,t->cpu->r[1],t->suspend,t->wait_kind,t->wait_addr,t->service!=NULL);
}
