/* Exercise the actual SDL presenter alongside the offscreen EGL renderer. */
#include "../runtime/gx2/render.cpp"
#include <SDL2/SDL.h>
extern "C" {
uint8_t* g_mem;
void rt_fatal(ppc_ctx*,const char* message,...){fprintf(stderr,"%s\n",message);abort();}
}
int main(int argc,char** argv){
    assert(argc==2);std::vector<uint8_t> memory(65536);g_mem=memory.data();
    uint32_t surface[29]={1,32,32,1,1,26,0,3};surface[12]=16;assert(gx2_surface_layout(surface));surface[9]=4096;
    hle_window_enable();hle_debug_overlay_enable();
    for(unsigned frame=0;frame<4;frame++){
        float color[4]={float(frame&1),float(!(frame&1)),0,1};
        gpu::render_clear(surface,color);gpu::render_present(surface,1,argv[1]);
        SDL_Window* window=nullptr;for(unsigned id=1;id<64&&!window;id++)window=SDL_GetWindowFromID(id);assert(window);
        SDL_Renderer* renderer=SDL_GetRenderer(window);assert(renderer);
        SDL_RendererInfo info;assert(SDL_GetRendererInfo(renderer,&info)==0);
        assert(info.flags&SDL_RENDERER_ACCELERATED);
        auto context=eglGetCurrentContext();assert(context!=EGL_NO_CONTEXT);
        {gpu::Current current;uint8_t pixel[4];gpu::bind_target(gpu::texture(surface[9]));glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
         assert(pixel[0]==(frame&1?255:0)&&pixel[1]==(frame&1?0:255));assert(glGetError()==GL_NO_ERROR);}
        assert(eglGetCurrentContext()==context);
    }
    SDL_Quit();
}
