/* Guest ABI regression: text buffers, receiver callback and dialog lifecycle. */
#include "standalone/rt.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
uint8_t *g_mem;
static uint16_t host_text[4096];
static unsigned host_length,host_limit,callbacks;
static int host_result,host_active,host_ok;
static ppc_ctx ctx;
uint32_t hle_sys_alloc(uint32_t size){assert(size==8192);return 0x8000;}
void hle_text_begin(const uint16_t *initial,unsigned limit){host_length=0;host_limit=limit;while(initial[host_length]){host_text[host_length]=initial[host_length];host_length++;}host_text[host_length]=0;host_result=0;host_active=host_ok=1;}
void hle_text_end(void){host_active=0;}
void hle_text_allow_confirm(int enabled){host_ok=enabled;}
int hle_text_poll(uint16_t *out,unsigned *length){memcpy(out,host_text,(host_length+1)*2);*length=host_length;return host_result;}
static void sleep_stub(ppc_ctx *c){(void)c;}
hle_fn hle_thread_lookup(const char *name){assert(!strcmp(name,"OSSleepTicks"));return sleep_stub;}
void ppc_call_indirect(ppc_ctx *c,uint32_t fn){
    assert(fn==0x1234&&c->r[3]==0x100&&ld32(c->r[4])==0&&ld32(c->r[4]+4)==0);
    assert(ld16(0x300)=='H'&&ld16(0x302)==0xe9&&ld16(0x304)==0);
    callbacks++;c->r[8]=0xdeadbeef;
}
static uint32_t call(const char *name,uint32_t arg){char symbol[128];snprintf(symbol,sizeof symbol,"%s__test",name);hle_fn fn=hle_swkbd_lookup(symbol);assert(fn);ctx.r[3]=arg;fn(&ctx);return ctx.r[3];}
int main(void){
    g_mem=calloc(1,65536);ctx.r[1]=0x7000;ctx.r[8]=42;
    call("SwkbdCreate",0);assert(call("SwkbdGetStateKeyboard",0)==0);
    st32(0x100,0x200);st32(0x20c,0x1234);
    st32(0x400,0x100);st32(0x404,0x300);st32(0x408,3);
    call("SwkbdSetReceiver",0x400);
    st32(0x500+0xc8,0x600);st16(0x600,'A');st32(0x500+0xd0,10);
    assert(call("SwkbdAppearInputForm",0x500)==1&&host_active&&host_limit==10&&host_text[0]=='A');
    assert(call("SwkbdGetStateInputForm",0)==2);
    call("SwkbdSetEnableOkButton",0);assert(!host_ok);call("SwkbdSetEnableOkButton",1);assert(host_ok);
    host_text[0]='H';host_text[1]=0xe9;host_text[2]='!';host_text[3]=0;host_length=3;host_result=1;
    call("SwkbdCalc",0);assert(callbacks==1&&ctx.r[8]==42&&ctx.r[1]==0x7000);
    uint32_t out=call("SwkbdGetInputFormString",0);assert(ld16(out+4)=='!'&&ld16(out+6)==0);
    assert(call("SwkbdIsDecideOkButton",0)==1&&call("SwkbdIsDecideCancelButton",0)==0);
    call("SwkbdCalc",0);assert(callbacks==1);
    call("SwkbdDisappearInputForm",0);assert(!host_active&&call("SwkbdGetStateKeyboard",0)==0);
    call("SwkbdAppearInputForm",0x500);host_result=2;call("SwkbdCalc",0);
    assert(call("SwkbdIsDecideCancelButton",0)==1&&call("SwkbdIsDecideOkButton",0)==0);
    memset(g_mem+0x1000,0xff,32);call("SwkbdGetKeyboardCondition",0x1000);assert(ld32(0x1000)==0&&ld32(0x1004)==0&&ld32(0x1008)==0xffffffff);
    call("SwkbdGetDrawStringInfo",0x1000);assert(ld32(0x1014)==0xffffffff&&ld8(0x1018)==0);
    call("SwkbdDestroy",0);assert(!host_active);free(g_mem);
}
