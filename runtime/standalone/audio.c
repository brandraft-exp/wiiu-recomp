/* Experimental AX voice renderer. ABI/constants follow Cemu ax.h,
 * ax_voice.cpp (voice setters), ax_mix.cpp (PCM/ADPCM decoding), and
 * ax_ist.cpp (3 ms frame callbacks). Stereo direct bus only; DSP effects and
 * hardware polyphase resampling remain unsupported. */
#include "rt.h"
#include <SDL2/SDL.h>
#include <math.h>
typedef struct {
    uint32_t addr,ratio,fraction;
    int used,volume,delta,mix[2],mix_delta[2],history[2],loop_history[2];
    int16_t coef[16];
    uint16_t scale,loop_scale,type;
    int16_t last;
} voice;
static voice voices[96];
static uint32_t frame_callback;
static int started,output_enabled,device_playing;
static SDL_AudioDeviceID device;
static unsigned trace_frames,trace_empty;
static uint64_t trace_start;
static int clamp16(int64_t x){return x<-32768?-32768:x>32767?32767:(int)x;}
void hle_audio_enable(void){output_enabled=1;}
static voice *get(ppc_ctx *c){
    for(unsigned i=0;i<96;i++)if(voices[i].used&&voices[i].addr==c->r[3])return &voices[i];
    rt_fatal(c,"invalid AX voice %08x",c->r[3]);
}
static int sample(voice *v){
    uint32_t a=v->addr,base=ld32(a+0x44),off=ld32(a+0x40),end=ld32(a+0x3c);uint16_t format=ld16(a+0x34);
    if(!ld32(a+4))return 0;
    int value;
    if(format==10)value=(int16_t)ld16(base+off*2);
    else if(format==25)value=(int8_t)ld8(base+off)*256;
    else if(format==0){
        if(!(off&15)){v->scale=ld8(base+off/2);off+=2;}
        int nibble=(ld8(base+off/2)>>(off&1?0:4))&15;if(nibble&8)nibble-=16;
        unsigned index=(v->scale>>4)&7;
        value=clamp16(((int64_t)nibble*(1<<(v->scale&15))*2048+v->history[0]*v->coef[index*2]+v->history[1]*v->coef[index*2+1]+1024)>>11);
        v->history[1]=v->history[0];v->history[0]=value;
    }else rt_fatal(NULL,"unsupported AX sample format %x",format);
    if(off>=end){
        if(ld16(a+0x36)){off=ld32(a+0x38);v->scale=v->loop_scale;if(!v->type){v->history[0]=v->loop_history[0];v->history[1]=v->loop_history[1];}}
        else {st32(a+4,0);}
    }else off++;
    st32(a+0x40,off);return value;
}
void hle_audio_mix(int16_t *out,unsigned count){
    for(unsigned i=0;i<count;i++){
        int64_t mix[2]={0,0};
        for(unsigned j=0;j<96;j++){
            voice *v=&voices[j];if(!v->used||!ld32(v->addr+4))continue;
            v->fraction+=v->ratio;while(v->fraction>=65536){v->last=sample(v);v->fraction-=65536;}
            for(unsigned ch=0;ch<2;ch++){mix[ch]+=((int64_t)v->last*v->volume*v->mix[ch])>>30;v->mix[ch]=(v->mix[ch]+v->mix_delta[ch])&65535;}
            v->volume=(v->volume+v->delta)&65535;
        }
        out[i*2]=clamp16(mix[0]);out[i*2+1]=clamp16(mix[1]);
    }
}
static void frame(ppc_ctx *c){
    if(frame_callback)ppc_call_indirect(c,frame_callback);
    int16_t output[192];hle_audio_mix(output,96);
    if(output_enabled){
        if(!device){
            if(SDL_InitSubSystem(SDL_INIT_AUDIO))rt_fatal(c,"SDL audio: %s",SDL_GetError());
            SDL_AudioSpec want={0};want.freq=32000;want.format=AUDIO_S16SYS;want.channels=2;want.samples=512;
            device=SDL_OpenAudioDevice(NULL,0,&want,NULL,0);if(!device)rt_fatal(c,"opening audio: %s",SDL_GetError());
        }
        if(getenv("WIIU_TRACE_AUDIO")){
            uint64_t now=SDL_GetTicks64();if(!trace_start)trace_start=now;
            if(!SDL_GetQueuedAudioSize(device))trace_empty++;
            if(++trace_frames%300==0)fprintf(stderr,"AX audio: %u frames (%u ms PCM), %llu ms elapsed, %u empty queues, %u bytes queued\n",
                trace_frames,trace_frames*3,(unsigned long long)(now-trace_start),trace_empty,SDL_GetQueuedAudioSize(device));
        }
        if(SDL_QueueAudio(device,output,sizeof output))rt_fatal(c,"queueing audio: %s",SDL_GetError());
        // Queue 48 ms before starting so the first host audio callback does
        // not consume a single 3 ms AX frame and immediately underrun.
        if(!device_playing&&SDL_GetQueuedAudioSize(device)>=32000*4*48/1000){SDL_PauseAudioDevice(device,0);device_playing=1;}
    }
}
static void AXInit(ppc_ctx *c){if(!started){started=1;hle_service_start(c,frame,3000000);}c->r[3]=0;}
static void AXRegisterFrameCallback(ppc_ctx *c){uint32_t old=frame_callback;frame_callback=c->r[3];c->r[3]=old;}
static void AXQuit(ppc_ctx *c){frame_callback=0;for(unsigned i=0;i<96;i++)if(voices[i].used)st32(voices[i].addr+4,0);c->r[3]=0;}
static void acquire(ppc_ctx *c){
    if(!c->r[3]||c->r[3]>=32){c->r[3]=0;return;}
    for(unsigned i=0;i<96;i++)if(!voices[i].used){
        voice *v=&voices[i];uint32_t a=v->addr?v->addr:hle_sys_alloc(0x58);memset(v,0,sizeof *v);v->addr=a;v->used=1;v->ratio=65536;
        memset(g_mem+a,0,0x58);st32(a,i);st32(a+0x1c,c->r[3]);st32(a+0x20,c->r[4]);st32(a+0x24,c->r[5]);c->r[3]=a;return;
    }
    c->r[3]=0; /* Priority-based stealing is not yet implemented. */
}
static void release(ppc_ctx *c){voice *v=get(c);v->used=0;st32(v->addr+4,0);st32(v->addr+0x1c,0);}
static void offsets(ppc_ctx *c){voice *v=get(c);memcpy(g_mem+v->addr+0x34,g_mem+c->r[4],20);}
static void get_offsets(ppc_ctx *c){voice *v=get(c);memcpy(g_mem+c->r[4],g_mem+v->addr+0x34,20);}
static void state(ppc_ctx *c){voice *v=get(c);st32(v->addr+4,c->r[4]);}
static void running(ppc_ctx *c){voice *v=get(c);c->r[3]=ld32(v->addr+4)==1;}
static void src(ppc_ctx *c){voice *v=get(c);v->ratio=ld32(c->r[4]);v->fraction=ld16(c->r[4]+4);}
static void ratio(ppc_ctx *c){voice *v=get(c);double d;memcpy(&d,&c->f[1],8);v->ratio=(uint32_t)(fmin(fmax(d,0),8)*65536);c->r[3]=0;}
static void envelope(ppc_ctx *c){voice *v=get(c);v->volume=ld16(c->r[4]);v->delta=(int16_t)ld16(c->r[4]+2);}
static void mix(ppc_ctx *c){voice *v=get(c);if(c->r[4]==0&&c->r[5]==0){for(unsigned i=0;i<2;i++){v->mix[i]=ld16(c->r[6]+i*16);v->mix_delta[i]=(int16_t)ld16(c->r[6]+i*16+2);}}c->r[3]=0;}
static void adpcm(ppc_ctx *c){voice *v=get(c);for(unsigned i=0;i<16;i++)v->coef[i]=(int16_t)ld16(c->r[4]+2*i);v->scale=ld16(c->r[4]+34);v->history[0]=(int16_t)ld16(c->r[4]+36);v->history[1]=(int16_t)ld16(c->r[4]+38);}
static void loop(ppc_ctx *c){voice *v=get(c);v->loop_scale=ld16(c->r[4]);v->loop_history[0]=(int16_t)ld16(c->r[4]+2);v->loop_history[1]=(int16_t)ld16(c->r[4]+4);}
static void type(ppc_ctx *c){get(c)->type=c->r[4];}
hle_fn hle_audio_lookup(const char *name){
#define E(n,f) if(!strcmp(name,n))return f
    E("AXInit",AXInit);E("AXRegisterFrameCallback",AXRegisterFrameCallback);E("AXQuit",AXQuit);
    E("AXAcquireVoice",acquire);E("AXFreeVoice",release);E("AXSetVoiceOffsets",offsets);E("AXGetVoiceOffsets",get_offsets);
    E("AXSetVoiceState",state);E("AXIsVoiceRunning",running);E("AXSetVoiceSrc",src);E("AXSetVoiceSrcRatio",ratio);
    E("AXSetVoiceVe",envelope);E("AXSetVoiceDeviceMix",mix);E("AXSetVoiceAdpcm",adpcm);E("AXSetVoiceAdpcmLoop",loop);E("AXSetVoiceType",type);
#undef E
    return NULL;
}
