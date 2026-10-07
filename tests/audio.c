#include "standalone/rt.h"
#include <assert.h>
#include <stdlib.h>
uint8_t *g_mem;
void rt_fatal(ppc_ctx *c,const char *fmt,...){(void)c;(void)fmt;abort();}
void hle_service_start(ppc_ctx *c,hle_fn fn,uint64_t p){(void)c;(void)fn;(void)p;}
void ppc_call_indirect(ppc_ctx *c,uint32_t entry){(void)c;(void)entry;abort();}
uint32_t hle_sys_alloc(uint32_t size){static uint32_t next=0x1000;uint32_t a=next;next+=size;return a;}
static void call(ppc_ctx *c,const char *name,uint32_t a,uint32_t b){c->r[3]=a;c->r[4]=b;hle_fn fn=hle_audio_lookup(name);assert(fn);fn(c);}
int main(void){
    g_mem=calloc(1,65536);ppc_ctx c={0};call(&c,"AXAcquireVoice",31,0);uint32_t voice=c.r[3];assert(voice);
    st16(0x200,32768);call(&c,"AXSetVoiceVe",voice,0x200);
    st16(0x300,32768);st16(0x310,32768);c.r[5]=0;c.r[6]=0x300;call(&c,"AXSetVoiceDeviceMix",voice,0);
    st16(0x400,10);st32(0x408,1);st32(0x410,0x500);st16(0x500,1000);st16(0x502,(uint16_t)-1000);
    call(&c,"AXSetVoiceOffsets",voice,0x400);call(&c,"AXSetVoiceState",voice,1);
    int16_t out[8];hle_audio_mix(out,4);
    assert(out[0]==1000&&out[1]==1000&&out[2]==-1000&&out[3]==-1000&&out[4]==0);
    call(&c,"AXIsVoiceRunning",voice,0);assert(!c.r[3]);
    st16(0x402,1);call(&c,"AXSetVoiceOffsets",voice,0x400);call(&c,"AXSetVoiceState",voice,1);hle_audio_mix(out,4);
    assert(out[0]==1000&&out[2]==-1000&&out[4]==1000&&out[6]==-1000);
    /* DSP ADPCM scale zero, zero coefficients: signed nibble samples. */
    st16(0x400,0);st16(0x402,0);st32(0x408,5);st32(0x40c,0);st8(0x500,0);st8(0x501,0x1f);st8(0x502,0x78);
    call(&c,"AXSetVoiceOffsets",voice,0x400);call(&c,"AXSetVoiceAdpcm",voice,0x600);call(&c,"AXSetVoiceState",voice,1);hle_audio_mix(out,4);
    assert(out[0]==1&&out[2]==-1&&out[4]==7&&out[6]==-8);
    call(&c,"AXFreeVoice",voice,0);call(&c,"AXAcquireVoice",31,0);assert(c.r[3]==voice);
    free(g_mem);
}
