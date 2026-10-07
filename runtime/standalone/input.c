/* VPADRead layout/defaults follow Cemu vpad.cpp::VPADRead and vpad.h.
 * SDL keyboard is a GamePad on channel zero; motion/touch remain neutral. */
#include "rt.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *screen;
static uint32_t previous, key_hold, key_pressed, key_released;
static uint8_t keys[SDL_NUM_SCANCODES];
static SDL_threadID window_thread;
static int enabled, screen_w, screen_h;
static FILE *replay;
static uint64_t sample, next_sample;
static uint32_t replay_buttons, next_buttons;
static float replay_x, replay_y, next_x, next_y;
static int has_next;
/* Native text editing is consumed by swkbd Calc on the guest thread. */
static uint16_t text_value[4096];
static unsigned text_length,text_limit;
static int text_active,text_result,text_started,text_allow_ok=1;
static TTF_Font *text_font;
static FILE *text_replay;
/* Presentation timing includes guest execution, rendering and pacing. It is
 * not GPU execution time. Guest HLE/presentation run under the scheduler token. */
static int debug_enabled;
static uint64_t debug_frames,debug_reads,debug_last_frame,debug_last_press;
static uint32_t debug_held,debug_pressed;
static float debug_x,debug_y;
static double debug_intervals[120];
static unsigned debug_count,debug_index;
static TTF_Font *debug_font;
static SDL_Texture *debug_texture;
static int debug_width,debug_height;
static uint64_t debug_refresh;
void hle_debug_overlay_enable(void){debug_enabled=1;debug_refresh=0;}
static TTF_Font *open_font(int size){
    if(!TTF_WasInit()&&TTF_Init())rt_fatal(NULL,"SDL_ttf: %s",TTF_GetError());
    const char *paths[]={"/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf"};
    for(unsigned i=0;i<sizeof paths/sizeof *paths;i++){
        TTF_Font *font=TTF_OpenFont(paths[i],size);if(font)return font;
    }
    rt_fatal(NULL,"text overlays need DejaVu Sans or Liberation Sans installed");
}
static void button_names(uint32_t bits,char *out,size_t size){
    const uint32_t masks[]={0x8000,0x4000,0x2000,0x1000,0x20,0x10,0x80,0x40,8,4,0x200,0x100,0x800,0x400};
    const char *names[]={"A","B","X","Y","L","R","ZL","ZR","+","-","Up","Down","Left","Right"};
    size_t used=0;out[0]=0;
    for(unsigned i=0;i<sizeof masks/sizeof *masks;i++)if(bits&masks[i]){
        int n=snprintf(out+used,size-used,"%s%s",used?" ":"",names[i]);
        if(n<0||(size_t)n>=size-used)break;used+=(size_t)n;
    }
    if(!used)snprintf(out,size,"none");
}
static void debug_draw(void){
    uint64_t now=SDL_GetPerformanceCounter(),frequency=SDL_GetPerformanceFrequency();
    if(debug_last_frame){
        debug_intervals[debug_index++%120]=(double)(now-debug_last_frame)*1000./frequency;
        if(debug_count<120)debug_count++;
    }
    debug_last_frame=now;debug_frames++;
    if(!debug_enabled)return;
    if(!debug_texture||!debug_refresh||now-debug_refresh>=frequency/4){
        double total=0,peak=0;
        for(unsigned i=0;i<debug_count;i++){double ms=debug_intervals[i];total+=ms;if(ms>peak)peak=ms;}
        double mean=debug_count?total/debug_count:0;
        char held[128],pressed[128],last[192],info[1200];
        button_names(debug_held,held,sizeof held);button_names(debug_pressed,pressed,sizeof pressed);
        if(debug_last_press)snprintf(last,sizeof last,"%s (%.1fs ago)",pressed,(double)(now-debug_last_press)/frequency);
        else snprintf(last,sizeof last,"none");
        snprintf(info,sizeof info,
            "DEBUG  |  F3 hide\nPresented: %.1f FPS  |  %.1f ms avg / %.1f ms max\n"
            "%dx%d  |  frames %llu  |  VPAD reads %llu\nFocus: %s  |  input: %s  |  text entry: %s\n"
            "GamePad held: %s  |  stick: %+.2f %+.2f\nLast game press: %s",
            mean>0?1000./mean:0,mean,peak,screen_w,screen_h,
            (unsigned long long)debug_frames,(unsigned long long)debug_reads,
            SDL_GetWindowFlags(window)&SDL_WINDOW_INPUT_FOCUS?"yes":"no",
            replay?"replay":"keyboard",text_active?"active":"off",held,debug_x,debug_y,last);
        if(!debug_font)debug_font=open_font(24);
        SDL_Surface *surface=TTF_RenderUTF8_Blended_Wrapped(debug_font,info,(SDL_Color){220,245,225,255},900);
        if(!surface)rt_fatal(NULL,"debug text: %s",TTF_GetError());
        if(debug_texture)SDL_DestroyTexture(debug_texture);
        debug_texture=SDL_CreateTextureFromSurface(renderer,surface);
        debug_width=surface->w;debug_height=surface->h;SDL_FreeSurface(surface);
        if(!debug_texture)rt_fatal(NULL,"debug texture: %s",SDL_GetError());
        debug_refresh=now;
    }
    float scale=(float)screen_h/720.;
    if((debug_width+24)*scale>screen_w)scale=(float)screen_w/(debug_width+24);
    SDL_Rect panel={0,0,(int)((debug_width+24)*scale),(int)((debug_height+20)*scale)};
    SDL_SetRenderDrawColor(renderer,16,22,28,255);SDL_RenderFillRect(renderer,&panel);
    SDL_Rect dest={(int)(12*scale),(int)(10*scale),(int)(debug_width*scale),(int)(debug_height*scale)};
    SDL_RenderCopy(renderer,debug_texture,NULL,&dest);
}
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
#define HOST_UTF16 "UTF-16LE"
#else
#define HOST_UTF16 "UTF-16BE"
#endif
static void text_append(const char *utf8) {
    uint16_t *u=(uint16_t*)SDL_iconv_string(HOST_UTF16,"UTF-8",utf8,strlen(utf8)+1);
    if(!u)return;
    for(unsigned i=0;u[i];){
        unsigned n=u[i]>=0xd800&&u[i]<=0xdbff&&u[i+1]?2:1;
        if(text_length+n>text_limit)break;
        for(unsigned j=0;j<n;j++)text_value[text_length++]=u[i++];
    }
    text_value[text_length]=0;SDL_free(u);
}
void hle_text_input(const char *utf8){if(text_active&&!text_result)text_append(utf8);}
void hle_text_replay(const char *path){text_replay=fopen(path,"r");if(!text_replay)rt_fatal(NULL,"opening keyboard replay %s",path);}
void hle_text_begin(const uint16_t *initial,unsigned limit){
    text_limit=limit>4095?4095:limit;text_length=0;text_result=0;text_active=1;text_allow_ok=1;
    while(initial&&initial[text_length]&&text_length<text_limit){text_value[text_length]=initial[text_length];text_length++;}
    text_value[text_length]=0;key_hold=key_pressed=key_released=previous=0;memset(keys,0,sizeof keys);
}
void hle_text_allow_confirm(int enabled){text_allow_ok=enabled;}
void hle_text_end(void){text_active=0;key_hold=key_pressed=key_released=previous=0;memset(keys,0,sizeof keys);}
int hle_text_poll(uint16_t *out,unsigned *length){
    if(text_active&&!text_result&&text_replay){
        char line[16384];if(!fgets(line,sizeof line,text_replay))rt_fatal(NULL,"keyboard replay exhausted");
        line[strcspn(line,"\r\n")]=0;text_length=0;text_append(line);text_result=1;
    }
    memcpy(out,text_value,(text_length+1)*sizeof *out);*length=text_length;return text_result;
}
static void text_draw(void){
    if(!text_active)return;
    if(!text_font)text_font=open_font(36);
    char *utf8=SDL_iconv_string("UTF-8",HOST_UTF16,(char*)text_value,(text_length+1)*2);
    SDL_Rect box={screen_w/8,screen_h/3,screen_w*3/4,screen_h/3};
    SDL_SetRenderDrawColor(renderer,20,20,24,255);SDL_RenderFillRect(renderer,&box);
    const char *lines[]={"Enter a name",utf8&&*utf8?utf8:"_","Enter: confirm   Esc: cancel   Backspace: delete"};
    for(unsigned i=0;i<3;i++){
        SDL_Surface *s=TTF_RenderUTF8_Blended(text_font,lines[i],(SDL_Color){255,255,255,255});
        if(!s)continue;SDL_Texture *t=SDL_CreateTextureFromSurface(renderer,s);
        float scale=(float)(box.w-40)/s->w;if(scale>1)scale=1;
        SDL_Rect dest={box.x+20,box.y+20+(int)i*(box.h-40)/3,(int)(s->w*scale),(int)(s->h*scale)};
        SDL_RenderCopy(renderer,t,NULL,&dest);SDL_DestroyTexture(t);SDL_FreeSurface(s);
    }
    SDL_free(utf8);
}
static void replay_next(void) {
    unsigned long long tick;unsigned buttons;char line[256],extra;
    if(!fgets(line,sizeof line,replay)){has_next=0;return;}
    float x=0,y=0;
    int n=sscanf(line,"%llu %x %f %f %c",&tick,&buttons,&x,&y,&extra);
    if((n!=2&&n!=4)||tick<sample||!isfinite(x)||!isfinite(y)||fabsf(x)>1||fabsf(y)>1)
        rt_fatal(NULL,"input replay requires ordered sample/hex-mask pairs with optional left-stick x/y in [-1,1]");
    next_sample=tick;next_buttons=buttons;next_x=x;next_y=y;has_next=1;
}
void hle_input_replay(const char *path) {
    replay=fopen(path,"r");if(!replay)rt_fatal(NULL,"opening input replay %s",path);
    replay_next();
}
void hle_window_enable(void) { enabled=1; }
static uint32_t button(SDL_Scancode key) {
    switch(key){
#define B(k,v) case SDL_SCANCODE_##k:return v
        B(Z,0x8000);B(X,0x4000);B(C,0x2000);B(V,0x1000);
        B(Q,0x20);B(E,0x10);B(1,0x80);B(3,0x40);B(RETURN,8);B(BACKSPACE,4);
        B(UP,0x200);B(DOWN,0x100);B(LEFT,0x800);B(RIGHT,0x400);
#undef B
        default:return 0;
    }
}
static void pump(void) {
    // SDL events belong to the thread that created the window. Guest VPAD
    // readers on other threads consume the snapshot collected at presentation.
    if (!window || SDL_ThreadID()!=window_thread) return;
    if(text_active&&!text_started){SDL_StartTextInput();text_started=1;}
    if(!text_active&&text_started){SDL_StopTextInput();text_started=0;}
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if(event.type==SDL_QUIT)exit(0);
        if((event.type==SDL_KEYDOWN||event.type==SDL_KEYUP)&&event.key.keysym.scancode==SDL_SCANCODE_F3){
            if(event.type==SDL_KEYDOWN&&!event.key.repeat){debug_enabled=!debug_enabled;debug_refresh=0;}
            continue;
        }
        if(text_active){
            if(event.type==SDL_TEXTINPUT)hle_text_input(event.text.text);
            if(event.type==SDL_KEYDOWN&&!text_result){
                SDL_Keycode k=event.key.keysym.sym;
                if(k==SDLK_RETURN&&text_allow_ok)text_result=1;
                else if(k==SDLK_ESCAPE)text_result=2;
                else if(k==SDLK_BACKSPACE&&text_length){
                    --text_length;if(text_value[text_length]>=0xdc00&&text_value[text_length]<=0xdfff&&text_length)--text_length;
                    text_value[text_length]=0;
                }
            }
            continue;
        }
        if(event.type==SDL_KEYDOWN||event.type==SDL_KEYUP){
            SDL_Scancode key=event.key.keysym.scancode;
            if(key<=SDL_SCANCODE_UNKNOWN||key>=SDL_NUM_SCANCODES)continue;
            uint32_t mask=button(key);int down=event.type==SDL_KEYDOWN;
            if(down&&!keys[key])key_pressed|=mask;
            if(!down&&keys[key])key_released|=mask;
            keys[key]=down;
            if(down)key_hold|=mask;else key_hold&=~mask;
        }else if(event.type==SDL_WINDOWEVENT&&(event.window.event==SDL_WINDOWEVENT_FOCUS_LOST||event.window.event==SDL_WINDOWEVENT_FOCUS_GAINED)){
            // Never carry an unconsumed press or stale held key across focus
            // changes. Fresh key events after refocusing start a new press.
            key_released|=key_hold;key_hold=key_pressed=0;memset(keys,0,sizeof keys);
            if(getenv("WIIU_TRACE_INPUT"))fprintf(stderr,"SDL focus %s\n",event.window.event==SDL_WINDOWEVENT_FOCUS_GAINED?"gained":"lost");
        }
    }
}
void hle_window_present(const void *rgb,int width,int height) {
    if(!enabled)return;
    if(!window) {
        if(SDL_InitSubSystem(SDL_INIT_VIDEO|SDL_INIT_EVENTS))rt_fatal(NULL,"SDL: %s",SDL_GetError());
        window_thread=SDL_ThreadID();
        window=SDL_CreateWindow("Wii U recompiler",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,960,540,SDL_WINDOW_RESIZABLE);
        if(!window)rt_fatal(NULL,"SDL window: %s",SDL_GetError());
        renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_ACCELERATED);
        if(!renderer)renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_SOFTWARE);
        if(!renderer)rt_fatal(NULL,"SDL renderer: %s",SDL_GetError());
    }
    pump();
    if(!screen||screen_w!=width||screen_h!=height){
        if(screen)SDL_DestroyTexture(screen);
        screen=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGB24,SDL_TEXTUREACCESS_STREAMING,width,height);
        if(!screen)rt_fatal(NULL,"SDL texture: %s",SDL_GetError());
        screen_w=width;screen_h=height;SDL_RenderSetLogicalSize(renderer,width,height);
    }
    SDL_UpdateTexture(screen,NULL,rgb,width*3);SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer,screen,NULL,NULL);text_draw();debug_draw();SDL_RenderPresent(renderer);
}
static void read_pad(ppc_ctx *c) {
    uint32_t channel=c->r[3],out=c->r[4],length=c->r[5],error=c->r[6];
    if(channel||!length||!out){if(error)st32(error,channel?(uint32_t)-2:(uint32_t)-1);c->r[3]=0;return;}
    pump();uint32_t hold=key_hold|key_pressed;
    memset(g_mem+out,0,0xac);
    float x=(float)keys[SDL_SCANCODE_D]-keys[SDL_SCANCODE_A],y=(float)keys[SDL_SCANCODE_W]-keys[SDL_SCANCODE_S];
    if(replay){while(has_next&&next_sample<=sample){replay_buttons=next_buttons;replay_x=next_x;replay_y=next_y;replay_next();}hold=replay_buttons;x=replay_x;y=replay_y;sample++;}
    uint32_t bits;memcpy(&bits,&x,4);st32(out+12,bits);memcpy(&bits,&y,4);st32(out+16,bits);
    if(text_active){hold=0;x=y=0;st32(out+12,0);st32(out+16,0);}
    uint32_t pressed=(hold&~previous)|(replay?0:key_pressed),released=(previous&~hold)|(replay?0:(key_released&~hold));
    debug_reads++;debug_held=hold;debug_x=x;debug_y=y;
    if(pressed){debug_pressed=pressed;debug_last_press=SDL_GetPerformanceCounter();debug_refresh=0;}
    st32(out,hold);st32(out+4,pressed);st32(out+8,released);previous=hold;
    key_pressed=key_released=0;
    if(getenv("WIIU_TRACE_INPUT")&&(pressed||released))fprintf(stderr,"VPAD held=%08x pressed=%08x released=%08x\n",hold,pressed,released);
    st16(out+0x58,3);st16(out+0x60,3);st16(out+0x68,3);
    st32(out+0x6c,0x3f800000);st32(out+0x7c,0x3f800000);st32(out+0x8c,0x3f800000);
    g_mem[out+0xa1]=0xc0;if(error)st32(error,0);c->r[3]=1;
}
/* Cemu padscore.cpp::_KPADRead and padscoreExport_WPADProbe. Only the
 * GamePad is connected; returning success for absent Wii controllers makes
 * games select a phantom controller instead of VPAD. */
static int kpad_initialized;
static void kpad_init(ppc_ctx *c){kpad_initialized=1;c->r[3]=0;}
static void kpad_shutdown(ppc_ctx *c){kpad_initialized=0;c->r[3]=0;}
static void kpad_read(ppc_ctx *c){if(c->r[6])st32(c->r[6],kpad_initialized?(uint32_t)-2:(uint32_t)-5);c->r[3]=0;}
static void wpad_probe(ppc_ctx *c){if(c->r[4])st32(c->r[4],253);c->r[3]=(uint32_t)-1;}
/* Cemu vpadExport_VPADGetTPCalibratedPoint / _tpRawToResolution. Preserve
 * touch and validity even for neutral samples; callers use stack outputs. */
static void calibrated_point(ppc_ctx *c){
    uint32_t out=c->r[4],raw=c->r[5];
    int x=(int16_t)ld16(raw)-92,y=4095-(int16_t)ld16(raw+2)-254;
    uint16_t touch=ld16(raw+4),valid=ld16(raw+6);
    if(x<0)x=0;if(y<0)y=0;
    st16(out,(uint16_t)((double)x/3883.*1280.));
    st16(out+2,(uint16_t)((double)y/3694.*720.));
    st16(out+4,touch);st16(out+6,valid);c->r[3]=0;
}
hle_fn hle_input_lookup(const char *name){
    if(!strcmp(name,"VPADRead"))return read_pad;
    if(!strcmp(name,"VPADGetTPCalibratedPoint"))return calibrated_point;
    if(!strcmp(name,"WPADProbe"))return wpad_probe;
    if(!strcmp(name,"KPADReadEx"))return kpad_read;
    if(!strcmp(name,"KPADInit")||!strcmp(name,"KPADInitEx"))return kpad_init;
    if(!strcmp(name,"KPADShutdown"))return kpad_shutdown;
    return NULL;
}
