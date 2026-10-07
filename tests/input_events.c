/* Real SDL event queue, including taps completed between guest reads. */
#include "standalone/rt.h"
#include <SDL2/SDL.h>
#include <assert.h>
#include <stdlib.h>
#include <pthread.h>
uint8_t *g_mem;
void rt_fatal(ppc_ctx *c,const char *fmt,...){(void)c;(void)fmt;abort();}
static ppc_ctx ctx;
static void* read_worker(void* unused){(void)unused;ctx.r[3]=0;ctx.r[4]=256;ctx.r[5]=1;ctx.r[6]=512;hle_input_lookup("VPADRead")(&ctx);return NULL;}
static void read_pad(void){pthread_t t;assert(!pthread_create(&t,NULL,read_worker,NULL));assert(!pthread_join(t,NULL));assert(ctx.r[3]==1);}
static void key(Uint32 type,SDL_Scancode code){SDL_Event event={0};event.type=type;event.key.keysym.scancode=code;event.key.keysym.sym=SDL_GetKeyFromScancode(code);assert(SDL_PushEvent(&event)==1);}
/* SDL2-compat's synthetic TEXTINPUT path dereferences missing dummy focus.
 * Feed UTF-8 through the same host-input entry used by real TEXTINPUT events. */
static void text_event(const char *text){hle_text_input(text);}
static void check_overlay(int visible){
    SDL_Window *window=NULL;for(unsigned id=1;id<64&&!window;id++)window=SDL_GetWindowFromID(id);assert(window);
    SDL_Renderer *renderer=SDL_GetRenderer(window);assert(renderer);
    uint8_t pixel[4];SDL_Rect corner={0,0,1,1};
    assert(!SDL_RenderReadPixels(renderer,&corner,SDL_PIXELFORMAT_RGBA32,pixel,4));
    assert(pixel[0]==(visible?16:0)&&pixel[1]==(visible?22:0)&&pixel[2]==(visible?28:0));
    const char *capture=getenv("WIIU_TEST_OVERLAY_CAPTURE");
    if(visible&&capture){
        int w,h;assert(!SDL_GetRendererOutputSize(renderer,&w,&h));
        SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormat(0,w,h,32,SDL_PIXELFORMAT_RGBA32);assert(surface);
        assert(!SDL_RenderReadPixels(renderer,NULL,surface->format->format,surface->pixels,surface->pitch));
        assert(!SDL_SaveBMP(surface,capture));SDL_FreeSurface(surface);
    }
}
int main(void){
    g_mem=calloc(1,4096);uint8_t pixels[12]={0};hle_window_enable();hle_window_present(pixels,2,2);
    key(SDL_KEYDOWN,SDL_SCANCODE_RETURN);key(SDL_KEYUP,SDL_SCANCODE_RETURN);hle_window_present(pixels,2,2);read_pad();
    assert(ld32(256)==8&&ld32(260)==8&&ld32(264)==0);
    read_pad();assert(ld32(256)==0&&ld32(260)==0&&ld32(264)==8);
    read_pad();assert(ld32(260)==0&&ld32(264)==0);
    key(SDL_KEYDOWN,SDL_SCANCODE_Z);key(SDL_KEYDOWN,SDL_SCANCODE_W);hle_window_present(pixels,2,2);read_pad();
    assert(ld32(256)==0x8000&&ld32(260)==0x8000&&ld32(272)==0x3f800000);
    key(SDL_KEYDOWN,SDL_SCANCODE_Z);hle_window_present(pixels,2,2);read_pad();assert(ld32(260)==0);
    SDL_Event focus={0};focus.type=SDL_WINDOWEVENT;focus.window.event=SDL_WINDOWEVENT_FOCUS_LOST;assert(SDL_PushEvent(&focus)==1);
    hle_window_present(pixels,2,2);read_pad();assert(ld32(256)==0&&ld32(264)==0x8000&&ld32(272)==0);
    focus.window.event=SDL_WINDOWEVENT_FOCUS_GAINED;assert(SDL_PushEvent(&focus)==1);
    key(SDL_KEYDOWN,SDL_SCANCODE_Z);hle_window_present(pixels,2,2);read_pad();assert(ld32(256)==0x8000&&ld32(260)==0x8000);
    // Losing focus before a guest read must discard the pending button press.
    key(SDL_KEYDOWN,SDL_SCANCODE_RETURN);focus.window.event=SDL_WINDOWEVENT_FOCUS_LOST;assert(SDL_PushEvent(&focus)==1);
    hle_window_present(pixels,2,2);read_pad();assert(ld32(256)==0&&ld32(260)==0);
    focus.window.event=SDL_WINDOWEVENT_FOCUS_GAINED;assert(SDL_PushEvent(&focus)==1);
    key(SDL_KEYDOWN,SDL_SCANCODE_DOWN);key(SDL_KEYUP,SDL_SCANCODE_DOWN);hle_window_present(pixels,2,2);read_pad();assert(ld32(256)==0x100&&ld32(260)==0x100);
    uint8_t *large=calloc(640*360,3);uint16_t initial[]={'A',0},value[4096];unsigned length;
    hle_text_begin(initial,4);hle_window_present(large,640,360);check_overlay(0);
    key(SDL_KEYDOWN,SDL_SCANCODE_F3);hle_window_present(large,640,360);check_overlay(1);
    SDL_Event repeat={0};repeat.type=SDL_KEYDOWN;repeat.key.keysym.scancode=SDL_SCANCODE_F3;repeat.key.repeat=1;
    assert(SDL_PushEvent(&repeat)==1);key(SDL_KEYUP,SDL_SCANCODE_F3);
    hle_window_present(large,640,360);check_overlay(1);read_pad();assert(ld32(256)==0&&ld32(260)==0);
    key(SDL_KEYDOWN,SDL_SCANCODE_F3);hle_window_present(large,640,360);check_overlay(0);
    key(SDL_KEYDOWN,SDL_SCANCODE_Z);text_event("z\xc3\xa9\xf0\x9f\x98\x80");
    hle_window_present(large,640,360);read_pad();assert(ld32(256)==0&&ld32(260)==0);
    assert(hle_text_poll(value,&length)==0&&length==3&&value[2]==0xe9);
    key(SDL_KEYDOWN,SDL_SCANCODE_BACKSPACE);hle_window_present(large,640,360);text_event("\xf0\x9f\x98\x80");
    hle_window_present(large,640,360);assert(hle_text_poll(value,&length)==0&&length==4&&value[2]==0xd83d&&value[3]==0xde00);
    hle_text_allow_confirm(0);key(SDL_KEYDOWN,SDL_SCANCODE_RETURN);
    hle_window_present(large,640,360);assert(hle_text_poll(value,&length)==0);
    hle_text_allow_confirm(1);
    key(SDL_KEYDOWN,SDL_SCANCODE_BACKSPACE);key(SDL_KEYDOWN,SDL_SCANCODE_RETURN);
    hle_window_present(large,640,360);assert(hle_text_poll(value,&length)==1&&length==2);
    hle_text_end();hle_window_present(large,640,360);read_pad();assert(ld32(256)==0);
    key(SDL_KEYDOWN,SDL_SCANCODE_Z);hle_window_present(large,640,360);read_pad();assert(ld32(260)==0x8000);
    hle_text_begin(initial,4);hle_window_present(large,640,360);
    key(SDL_KEYDOWN,SDL_SCANCODE_ESCAPE);hle_window_present(large,640,360);assert(hle_text_poll(value,&length)==2);
    hle_text_end();hle_debug_overlay_enable();hle_window_present(large,640,360);check_overlay(1);free(large);free(g_mem);SDL_Quit();
}
