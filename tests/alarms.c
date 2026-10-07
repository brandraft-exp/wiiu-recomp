#include "standalone/rt.h"
#include <assert.h>
#include <stdlib.h>
uint8_t *g_mem;
static uint64_t now=1000;
static hle_fn service;
static unsigned fired;
uint64_t ppc_timebase(ppc_ctx *c){(void)c;return now;}
void rt_fatal(ppc_ctx *c,const char *fmt,...){(void)c;(void)fmt;abort();}
void hle_service_start(ppc_ctx *c,hle_fn fn,uint64_t period){(void)c;assert(period==1000000);service=fn;}
static void current(ppc_ctx *c){c->r[3]=0x100;}
hle_fn hle_thread_lookup(const char *name){assert(!strcmp(name,"OSGetCurrentThread"));return current;}
void ppc_call_indirect(ppc_ctx *c,uint32_t target){assert(target==0x1234);assert(c->r[3]==0x200&&c->r[4]==0x100);fired++;}
static void call(ppc_ctx *c,const char *name){hle_fn fn=hle_alarm_lookup(name);assert(fn);fn(c);}
int main(void){
    g_mem=calloc(1,4096);ppc_ctx c={0};c.r[3]=0x200;call(&c,"OSCreateAlarm");assert(ld32(0x200)==0x614c724d);
    c.r[4]=0x87654321;call(&c,"OSSetAlarmUserData");call(&c,"OSGetAlarmUserData");assert(c.r[3]==0x87654321);
    c.r[3]=0x200;c.r[5]=0;c.r[6]=50;c.r[7]=0x1234;call(&c,"OSSetAlarm");
    assert(ld64(0x218)==1050);service(&c);assert(!fired);now=1050;service(&c);assert(fired==1);service(&c);assert(fired==1);
    c.r[3]=0x200;c.r[5]=0;c.r[6]=1000;c.r[7]=0;c.r[8]=100;c.r[9]=0x1234;call(&c,"OSSetPeriodicAlarm");
    assert(ld64(0x218)==1100);now=1350;service(&c);assert(fired==2&&ld64(0x218)==1400);
    c.r[3]=0x200;call(&c,"OSCancelAlarm");assert(c.r[3]==1);now=1500;service(&c);assert(fired==2);
    c.r[3]=0x200;call(&c,"OSCancelAlarm");assert(c.r[3]==0);free(g_mem);
}
