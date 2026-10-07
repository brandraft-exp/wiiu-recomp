/* Initial offscreen OpenGL backend. Guest execution is serialized; release the
 * EGL context between submissions so different guest pthreads may submit. */
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include "render.h"
#include "surface.h"
#include "profile.h"
#include <fstream>
extern "C" {
#include "../standalone/rt.h"
}
namespace gpu {
namespace {
EGLDisplay display=EGL_NO_DISPLAY;
EGLContext context=EGL_NO_CONTEXT;
EGLSurface pbuffer=EGL_NO_SURFACE;
GLuint framebuffer,vao;
PFNGLCLIPCONTROLPROC clipControl;
struct Texture { std::array<uint32,29> words{}; GLuint object{}; GLenum target{GL_TEXTURE_2D}; bool rendered{},dirty{}; };
std::unordered_map<uint32,Texture> textures;
std::unordered_map<uint32,uint32> aliases;
std::unordered_map<std::string,GLuint> programs;
unsigned frame;
void fail(const char* msg) { throw std::runtime_error(msg); }
struct Current {
    EGLDisplay previousDisplay;
    EGLContext previousContext;
    EGLSurface previousDraw,previousRead;
    Current() {
        previousDisplay=eglGetCurrentDisplay();previousContext=eglGetCurrentContext();
        previousDraw=eglGetCurrentSurface(EGL_DRAW);previousRead=eglGetCurrentSurface(EGL_READ);
        if(display==EGL_NO_DISPLAY) {
            auto platform=(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
            if(!platform)fail("EGL platform extension unavailable");
            display=platform(EGL_PLATFORM_SURFACELESS_MESA,EGL_DEFAULT_DISPLAY,nullptr);
            EGLint major,minor;if(!eglInitialize(display,&major,&minor)||!eglBindAPI(EGL_OPENGL_API))fail("initializing offscreen OpenGL");
            const EGLint attrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
            EGLConfig config;EGLint n;if(!eglChooseConfig(display,attrs,&config,1,&n)||!n)fail("no EGL framebuffer configuration");
            const EGLint ctx[]={EGL_CONTEXT_MAJOR_VERSION,4,EGL_CONTEXT_MINOR_VERSION,3,EGL_CONTEXT_OPENGL_PROFILE_MASK,EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,EGL_NONE};
            context=eglCreateContext(display,config,EGL_NO_CONTEXT,ctx);
            const EGLint size[]={EGL_WIDTH,1,EGL_HEIGHT,1,EGL_NONE};pbuffer=eglCreatePbufferSurface(display,config,size);
            if(context==EGL_NO_CONTEXT||pbuffer==EGL_NO_SURFACE)fail("creating OpenGL 4.3 context");
        }
        if(!eglMakeCurrent(display,pbuffer,pbuffer,context))fail("binding OpenGL context");
        if(!framebuffer){
            GLint major,minor,n;glGetIntegerv(GL_MAJOR_VERSION,&major);glGetIntegerv(GL_MINOR_VERSION,&minor);
            bool supported=major>4||(major==4&&minor>=5);
            glGetIntegerv(GL_NUM_EXTENSIONS,&n);
            for(GLint i=0;i<n&&!supported;i++)supported=!strcmp((const char*)glGetStringi(GL_EXTENSIONS,i),"GL_ARB_clip_control");
            clipControl=(PFNGLCLIPCONTROLPROC)eglGetProcAddress("glClipControl");
            if(!supported||!clipControl)fail("OpenGL 4.5 or ARB_clip_control is required for guest render-target coordinates");
            glGenFramebuffers(1,&framebuffer);glGenVertexArrays(1,&vao);
        }
    }
    ~Current(){
        if(previousContext!=EGL_NO_CONTEXT)eglMakeCurrent(previousDisplay,previousDraw,previousRead,previousContext);
        else eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    }
};
float as_float(uint32 v){return std::bit_cast<float>(v);}
Texture& texture(uint32 address, bool target=false) {
    if(auto it=aliases.find(address);it!=aliases.end())address=it->second;
    auto it=textures.find(address);if(it==textures.end())fail("missing surface descriptor for GPU texture");
    Texture& t=it->second;auto& s=t.words;
    if(t.object&&t.dirty){glDeleteTextures(1,&t.object);t.object=0;t.dirty=false;}
    if(t.object)return t;
    t.dirty=false;
    Profile profile(2);
    if(s[0]>1||s[3]!=1||s[6]||(target&&s[0]!=1))throw std::runtime_error(fmt::format("unsupported texture surface: address={:08x} dim={} {}x{} depth={} AA={} format={:x}",address,s[0],s[1],s[2],s[3],s[6],s[5]));
    t.target=s[0]==0?GL_TEXTURE_1D:GL_TEXTURE_2D;
    glGenTextures(1,&t.object);glBindTexture(t.target,t.object);
    glTexParameteri(t.target,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(t.target,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(t.target,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(t.target,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    uint32 hw=s[5]&63;
    if(target){glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,s[1],s[2],0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);return t;}
    uint32 levels=std::max(s[4],1u);glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    for(uint32 level=0;level<levels;level++) {
        uint32 info[8];if(!gx2_mip_layout(s.data(),level,info))fail("invalid texture mip layout");
        uint32 width=std::max(s[1]>>level,1u),height=std::max(s[2]>>level,1u);
        uint32 ptr=level?s[11]:s[9],off=level>1?s[16+level-1]:0,size=level?s[10]:s[8];
        if(off>size||!ptr||(uint64)ptr+size>(1ull<<32))fail("invalid texture memory range");
        std::vector<uint8> linear((size_t)info[0]*info[1]*(info[4]/8));
        if(!gx2_surface_read(s.data(),level,0,linear.data(),linear.size(),g_mem+ptr+off,size-off))fail("texture detiling failed");
        bool bc=hw>=49&&hw<=53;uint32 row=(bc?(width+3)/4:width)*(info[4]/8),rows=bc?(height+3)/4:height;
        std::vector<uint8> packed((size_t)row*rows);
        for(uint32 y=0;y<rows;y++)memcpy(packed.data()+y*row,linear.data()+(size_t)y*info[0]*(info[4]/8),row);
        if(bc){const GLenum formats[]={GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,GL_COMPRESSED_RGBA_S3TC_DXT3_EXT,GL_COMPRESSED_RGBA_S3TC_DXT5_EXT,GL_COMPRESSED_RED_RGTC1,GL_COMPRESSED_RG_RGTC2};
            glCompressedTexImage2D(GL_TEXTURE_2D,level,formats[hw-49],width,height,0,packed.size(),packed.data());}
        else {GLenum fmt,internal,type=GL_UNSIGNED_BYTE;
            if(hw==26){fmt=GL_RGBA;internal=s[5]&0x400?GL_SRGB8_ALPHA8:GL_RGBA8;}
            else if(hw==1){fmt=GL_RED;internal=GL_R8;}
            else if(hw==7){fmt=GL_RG;internal=GL_RG8;}
            else if(s[5]==0x80e){fmt=GL_RED;internal=GL_R32F;type=GL_FLOAT;}
            else throw std::runtime_error(fmt::format("unsupported OpenGL texture format {:x}",s[5]));
            if(t.target==GL_TEXTURE_1D)glTexImage1D(t.target,level,internal,width,0,fmt,type,packed.data());
            else glTexImage2D(t.target,level,internal,width,height,0,fmt,type,packed.data());}
    }
    glTexParameteri(t.target,GL_TEXTURE_MAX_LEVEL,levels-1);
    return t;
}
void bind_target(Texture& t,const char* operation="bind") {
    glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,0,0);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_STENCIL_ATTACHMENT,GL_TEXTURE_2D,0,0);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,t.object,0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);glReadBuffer(GL_COLOR_ATTACHMENT0);
    GLenum status=glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if(status!=GL_FRAMEBUFFER_COMPLETE)throw std::runtime_error(fmt::format(
        "incomplete render target ({}): status={:x} address={:08x} object={} format={:x} dim={} {}x{} depth={} samples={} GLerror={:x}",
        operation,status,t.words[9],t.object,t.words[5],t.words[0],t.words[1],t.words[2],t.words[3],t.words[6],glGetError()));
}
Texture& depth_texture(uint32 address){
    auto it=textures.find(address);if(it==textures.end())fail("missing depth surface descriptor");auto& t=it->second;
    if(t.object&&t.dirty){glDeleteTextures(1,&t.object);t.object=0;t.dirty=false;}
    if(t.words[6]||t.words[3]!=1)fail("unsupported depth surface samples/layers");
    if(!t.object){
        t.dirty=false;
        glGenTextures(1,&t.object);glBindTexture(GL_TEXTURE_2D,t.object);
        uint32 fmt=t.words[5];GLenum internal,format,type;
        if(fmt==5){internal=GL_DEPTH_COMPONENT16;format=GL_DEPTH_COMPONENT;type=GL_UNSIGNED_SHORT;}
        else if(fmt==17){internal=GL_DEPTH24_STENCIL8;format=GL_DEPTH_STENCIL;type=GL_UNSIGNED_INT_24_8;}
        else if(fmt==0x80e){internal=GL_DEPTH_COMPONENT32F;format=GL_DEPTH_COMPONENT;type=GL_FLOAT;}
        else if(fmt==0x81c){internal=GL_DEPTH32F_STENCIL8;format=GL_DEPTH_STENCIL;type=GL_FLOAT_32_UNSIGNED_INT_24_8_REV;}
        else fail("unsupported depth format");
        glTexImage2D(GL_TEXTURE_2D,0,internal,t.words[1],t.words[2],0,format,type,nullptr);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    }return t;
}
void attach_depth(Texture& t){
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,t.object,0);
    if(t.words[5]==17||t.words[5]==0x81c)glFramebufferTexture2D(GL_FRAMEBUFFER,GL_STENCIL_ATTACHMENT,GL_TEXTURE_2D,t.object,0);
}
GLuint compile(GLenum type,const std::string& code) {
    GLuint s=glCreateShader(type);const char* p=code.c_str();glShaderSource(s,1,&p,nullptr);glCompileShader(s);
    GLint ok;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);if(!ok){char log[8192];glGetShaderInfoLog(s,sizeof log,nullptr,log);throw std::runtime_error(log);}return s;
}
GLuint program(LatteDecompilerOutput_t& vs,LatteDecompilerOutput_t* gs,LatteDecompilerOutput_t& ps) {
    std::string v=vs.shader->strBuf_shaderSource->c_str(),p=ps.shader->strBuf_shaderSource->c_str(),g=gs?gs->shader->strBuf_shaderSource->c_str():"";
    std::string key=v+g+p;if(auto it=programs.find(key);it!=programs.end())return it->second;
    GLuint result=glCreateProgram();std::vector<GLuint> shaders{compile(GL_VERTEX_SHADER,v),compile(GL_FRAGMENT_SHADER,p)};
    if(gs)shaders.push_back(compile(GL_GEOMETRY_SHADER,g));for(auto s:shaders)glAttachShader(result,s);glLinkProgram(result);
    GLint ok;glGetProgramiv(result,GL_LINK_STATUS,&ok);if(!ok){char log[8192];glGetProgramInfoLog(result,sizeof log,nullptr,log);throw std::runtime_error(log);}
    for(auto s:shaders)glDeleteShader(s);programs.emplace(key,result);return result;
}
uint32 gpu_word(uint32 address){uint32 v;memcpy(&v,g_mem+address,4);return v;}
void uniforms(GLuint prog,uint32* r,LatteDecompilerOutput_t& out,const char* suffix,uint32 blockBase,uint32 texBase,uint32 samplerBase,std::vector<GLuint>& buffers) {
    auto* s=out.shader;std::vector<GLint> values(s->list_remappedUniformEntries.size()*4);
    if(s->uniformMode==LATTE_DECOMPILER_UNIFORM_MODE_FULL_CFILE){
        uint32 count=out.uniformOffsetsGL.count_uniformRegister;
        if(count>256)fail("uniform register file exceeds hardware size");
        glUniform4iv(glGetUniformLocation(prog,(std::string("uf_uniformRegister")+suffix).c_str()),count,
                     reinterpret_cast<GLint*>(r+(s->shaderType==LatteConst::ShaderType::Pixel?0xc000:0xc400)));
    }
    if(s->uniformMode==LATTE_DECOMPILER_UNIFORM_MODE_FULL_CBANK){
        // Cemu LatteBufferCache: dynamic indexing requires complete GPU-order
        // banks, rather than the compact entries used by static accesses.
        for(unsigned bank=0;bank<LATTE_NUM_MAX_UNIFORM_BUFFERS;bank++){
            int binding=out.resourceMappingGL.uniformBuffersBindingPoint[bank];if(binding<0)continue;
            auto name=std::string("uniformBlock")+suffix+std::to_string(bank);
            GLuint index=glGetUniformBlockIndex(prog,name.c_str());if(index==GL_INVALID_INDEX)continue;
            GLint required=0;glGetActiveUniformBlockiv(prog,index,GL_UNIFORM_BLOCK_DATA_SIZE,&required);
            uint32 address=r[blockBase+bank*7];uint64 size=(uint64)r[blockBase+bank*7+1]+1;
            if(!address||(uint64)address+size>(1ull<<32)||required<=0)fail("invalid uniform bank");
            std::vector<uint8> data(required);memcpy(data.data(),g_mem+address,std::min<uint64>(size,required));
            GLuint buffer;glGenBuffers(1,&buffer);buffers.push_back(buffer);glBindBuffer(GL_UNIFORM_BUFFER,buffer);
            glBufferData(GL_UNIFORM_BUFFER,required,data.data(),GL_STREAM_DRAW);glBindBufferBase(GL_UNIFORM_BUFFER,binding,buffer);
        }
    }
    for(auto& e:s->list_remappedUniformEntries)for(unsigned j=0;j<4;j++) {
        uint32 v=e.isRegister?r[(s->shaderType==LatteConst::ShaderType::Pixel?0xc000:0xc400)+e.index*4+j]:gpu_word(r[blockBase+e.kcacheBankId*7]+e.index*16+j*4);
        values[e.mappedIndex*4+j]=v;
    }
    if(!values.empty())glUniform4iv(glGetUniformLocation(prog,(std::string("uf_remapped")+suffix).c_str()),values.size()/4,values.data());
    for(unsigned i=0;i<s->textureUnitListCount;i++) {
        unsigned unit=s->textureUnitList[i],reg=texBase+unit*7;
        int binding=out.resourceMappingGL.textureUnitToBindingPoint[unit];if(binding<0)fail("invalid texture binding");
        glActiveTexture(GL_TEXTURE0+binding);auto& t=texture(r[reg+2]<<8);glBindTexture(t.target,t.object);
        uint32 sel=r[reg+4];const GLint swizzles[]={GL_RED,GL_GREEN,GL_BLUE,GL_ALPHA,GL_ZERO,GL_ONE,GL_ZERO,GL_ZERO};
        GLint sw[4];for(unsigned j=0;j<4;j++)sw[j]=swizzles[(sel>>(16+3*j))&7];glTexParameteriv(t.target,GL_TEXTURE_SWIZZLE_RGBA,sw);
        uint32 sample=s->textureUnitSamplerAssignment[unit],w=r[0xf000+(samplerBase+sample)*3];
        const GLint wraps[]={GL_REPEAT,GL_MIRRORED_REPEAT,GL_CLAMP_TO_EDGE,GL_MIRROR_CLAMP_TO_EDGE,GL_CLAMP_TO_BORDER,GL_MIRROR_CLAMP_TO_EDGE,GL_CLAMP_TO_BORDER,GL_MIRROR_CLAMP_TO_EDGE};
        glTexParameteri(t.target,GL_TEXTURE_WRAP_S,wraps[w&7]);glTexParameteri(t.target,GL_TEXTURE_WRAP_T,wraps[(w>>3)&7]);
        glTexParameteri(t.target,GL_TEXTURE_MAG_FILTER,((w>>9)&3)?GL_LINEAR:GL_NEAREST);
        glTexParameteri(t.target,GL_TEXTURE_MIN_FILTER,((w>>12)&3)?GL_LINEAR:GL_NEAREST);
    }
}
}
void render_finish(){Current current;glFinish();}
void render_invalidate(uint32 address,uint32 size){
    uint64 end=std::min<uint64>((uint64)address+size,1ull<<32);
    for(auto& [ptr,t]:textures)if(!t.rendered&&((ptr<end&&(uint64)ptr+t.words[8]>address)||(t.words[11]<end&&(uint64)t.words[11]+t.words[10]>address)))t.dirty=true;
}
void remember_surface(uint32 key,const uint32* words){
    auto& t=textures[words[9]];
    // An address can be recycled for a different image between render passes.
    // Delete lazily: descriptor registration may occur without a GL context.
    if(!std::equal(words,words+7,t.words.begin())||words[11]!=t.words[11]||
       words[12]!=t.words[12]||words[13]!=t.words[13]||words[15]!=t.words[15]){
        t.dirty=true;t.rendered=false;
    }
    std::copy(words,words+29,t.words.begin());aliases[key]=words[9];
}
// Cemu LatteMRT::GetColorAttachmentTexture: display lists may patch the
// target address without another GX2SetColorBuffer call. Registers describe
// the active color view, not the last surface associated with that address.
Texture& color_target(uint32* r){
    uint32 key=r[mmCB_COLOR0_BASE],address=key;
    if(auto it=aliases.find(key);it!=aliases.end())address=it->second;
    uint32 info=r[mmCB_COLOR0_INFO],size=r[mmCB_COLOR0_SIZE];
    const uint32 types[]={0,0x200,0,0,0x100,0x300,0x400,0x800};
    uint32 format=((info>>2)&63)|types[(info>>12)&7];
    uint32 pitch=((size&1023)+1)*8,height=(((size>>10)&0xfffff)+1)*64/pitch;
    auto it=textures.find(address);uint32 layout[8];
    bool matches=it!=textures.end()&&it->second.words[5]==format&&
        gx2_mip_layout(it->second.words.data(),0,layout)&&layout[0]==pitch&&layout[1]==height;
    if(!matches){
        uint32 scissor=r[Latte::REGADDR::PA_SC_GENERIC_SCISSOR_BR];
        uint32 width=pitch,sh=(scissor>>16)&0x7fff,sw=scissor&0x7fff;
        if(sw&&((sw+7)&~7u)==pitch)width=sw;
        if(sh&&sh<=height&&height-sh<32)height=sh;
        std::array<uint32,29> words{1,width,height,1,1,format,0,2};
        words[12]=(info>>8)&15;
        if(!gx2_surface_layout(words.data()))fail("invalid register-defined color surface");
        words[9]=address;remember_surface(key,words.data());
    }
    return texture(key,true);
}
void render_clear_depth(const uint32* words,float depth,uint32 stencil,uint32 flags){
    Current current;remember_surface(words[9],words);auto& t=depth_texture(words[9]);GLuint f;glGenFramebuffers(1,&f);glBindFramebuffer(GL_FRAMEBUFFER,f);
    attach_depth(t);glDrawBuffer(GL_NONE);glReadBuffer(GL_NONE);glDisable(GL_SCISSOR_TEST);glDepthMask(GL_TRUE);glStencilMask(255);
    if(flags&1)glClearBufferfv(GL_DEPTH,0,&depth);
    if(flags&2){GLint value=stencil;glClearBufferiv(GL_STENCIL,0,&value);}
    glDeleteFramebuffers(1,&f);t.rendered=true;
}
void render_clear(const uint32* words,const float* rgba){Current current;remember_surface(words[9],words);auto& t=texture(words[9],true);bind_target(t,"clear");
    glDisable(GL_SCISSOR_TEST);glColorMask(1,1,1,1);glClearBufferfv(GL_COLOR,0,rgba);t.rendered=true;}
void render_copy(const uint32* src,const uint32* dst){Current current;remember_surface(src[9],src);remember_surface(dst[9],dst);auto& a=texture(src[9]);auto& b=texture(dst[9],true);
    bind_target(b,"copy destination");GLuint read;glGenFramebuffers(1,&read);glBindFramebuffer(GL_READ_FRAMEBUFFER,read);glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,a.object,0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,framebuffer);glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,b.object,0);glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0,0,src[1],src[2],0,0,dst[1],dst[2],GL_COLOR_BUFFER_BIT,GL_NEAREST);glDeleteFramebuffers(1,&read);b.rendered=true;}
void render_present(const uint32* words,uint32 target,const std::filesystem::path& directory){if(target!=1)return;++frame;
    Profile profile(3);Profile::report(frame);
    uint32 w=words[1],h=words[2];std::vector<uint8> pixels((size_t)w*h*3);
    {
        Current current;remember_surface(words[9],words);auto& t=texture(words[9]);bind_target(t,"present");
        glPixelStorei(GL_PACK_ALIGNMENT,1);glReadPixels(0,0,w,h,GL_RGB,GL_UNSIGNED_BYTE,pixels.data());
    }
    // SDL can create an EGL context even for a software renderer's window
    // surface. Release our offscreen context before entering that backend.
    hle_window_present(pixels.data(),w,h);
    if(frame>10&&frame%100)return;
    std::ofstream f(directory/("frame-"+(frame>1000?std::string("latest"):std::to_string(frame))+".ppm"),std::ios::binary);f<<"P6\n"<<w<<" "<<h<<"\n255\n";
    f.write((char*)pixels.data(),pixels.size());if(!f)fail("writing frame capture");
}
void render_draw(uint32* r,LatteFetchShader* fetch,LatteDecompilerOutput_t& vs,LatteDecompilerOutput_t* gs,LatteDecompilerOutput_t& ps,uint32 count,uint32 base,uint32 instances,uint32 indices,uint32 indexType){
    Profile profile(1);
    Current current;
    uint32 control=r[Latte::REGADDR::CB_COLOR_CONTROL];
    // Cemu LatteMRT::GetActiveColorBufferMask intersects shader exports,
    // channel writes and SPECIAL_OP before resolving color attachments.
    uint32 mask=(((control>>4)&7)==1?0:r[Latte::REGADDR::CB_TARGET_MASK])&15;
    if(!(ps.shader->pixelColorOutputMask&1))mask=0;
    uint32 z=r[Latte::REGADDR::DB_DEPTH_CONTROL];
    Texture *color=nullptr,*depth=nullptr;
    if(mask){color=&color_target(r);bind_target(*color,"draw color");}
    else {
        // Depth/stencil-only passes may leave an obsolete color address bound.
        // Do not interpret that address as a color surface or attach its image.
        if(!(z&3))return;
        glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,0,0);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,0,0);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_STENCIL_ATTACHMENT,GL_TEXTURE_2D,0,0);
        glDrawBuffer(GL_NONE);glReadBuffer(GL_NONE);
    }
    if(z&3){depth=&depth_texture(r[mmDB_HTILE_DATA_BASE]<<8);attach_depth(*depth);}
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)fail("incomplete draw framebuffer");
    GLuint prog=program(vs,gs,ps);glUseProgram(prog);glBindVertexArray(vao);
    float xs=as_float(r[Latte::REGADDR::PA_CL_VPORT_XSCALE]),ys=as_float(r[Latte::REGADDR::PA_CL_VPORT_YSCALE]);
    float xo=as_float(r[Latte::REGADDR::PA_CL_VPORT_XOFFSET]),yo=as_float(r[Latte::REGADDR::PA_CL_VPORT_YOFFSET]);
    // Cemu OpenGLRenderer::renderTarget_setViewport: render targets and uploaded
    // textures must share guest row order, including when a target is sampled.
    uint32 clip=r[Latte::REGADDR::PA_CL_CLIP_CNTL];
    clipControl(ys>0?GL_LOWER_LEFT:GL_UPPER_LEFT,(clip&(1<<19))?GL_ZERO_TO_ONE:GL_NEGATIVE_ONE_TO_ONE);
    glViewport(xo-std::abs(xs),yo-std::abs(ys),2*std::abs(xs),2*std::abs(ys));
    glUniform2f(glGetUniformLocation(prog,"uf_windowSpaceToClipSpaceTransform"),1/std::abs(xs),1/std::abs(ys));
    glUniform1f(glGetUniformLocation(prog,"uf_alphaTestRef"),as_float(r[Latte::REGADDR::SX_ALPHA_REF]));
    glUniform2f(glGetUniformLocation(prog,"uf_fragCoordScale"),1,1);
    std::vector<GLuint> buffers;
    uniforms(prog,r,vs,"VS",mmSQ_VTX_UNIFORM_BLOCK_START,Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS,18,buffers);
    if(gs)uniforms(prog,r,*gs,"GS",mmSQ_GS_UNIFORM_BLOCK_START,Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS,36,buffers);
    uniforms(prog,r,ps,"PS",mmSQ_PS_UNIFORM_BLOCK_START,Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS,0,buffers);
    if(z&2){glEnable(GL_DEPTH_TEST);glDepthMask((z>>2)&1);glDepthFunc(GL_NEVER+((z>>4)&7));
        float scale=as_float(r[Latte::REGADDR::PA_CL_VPORT_ZSCALE]),offset=as_float(r[Latte::REGADDR::PA_CL_VPORT_ZOFFSET]);glDepthRange((clip&(1<<19))?offset:offset-scale,offset+scale);
    }else glDisable(GL_DEPTH_TEST);
    // Cemu OpenGLRenderer::renderstate_update: stencil-only draws still need
    // the depth/stencil attachment, and disabled back state uses front state.
    if(z&1){
        glEnable(GL_STENCIL_TEST);
        const GLenum ops[]={GL_KEEP,GL_ZERO,GL_REPLACE,GL_INCR,GL_DECR,GL_INVERT,GL_INCR_WRAP,GL_DECR_WRAP};
        for(unsigned face=0;face<2;face++){
            bool back=face&&(z&128);unsigned shift=back?20:8;
            uint32 ref=r[back?Latte::REGADDR::DB_STENCILREFMASK_BF:Latte::REGADDR::DB_STENCILREFMASK];
            GLenum side=face?GL_BACK:GL_FRONT;
            glStencilFuncSeparate(side,GL_NEVER+((z>>shift)&7),ref&255,(ref>>8)&255);
            glStencilMaskSeparate(side,(ref>>16)&255);
            glStencilOpSeparate(side,ops[(z>>(shift+3))&7],ops[(z>>(shift+9))&7],ops[(z>>(shift+6))&7]);
        }
    }else glDisable(GL_STENCIL_TEST);
    uint32 polygon=r[Latte::REGADDR::PA_SU_SC_MODE_CNTL];
    glFrontFace(polygon&4?GL_CW:GL_CCW);
    if(polygon&3){glEnable(GL_CULL_FACE);glCullFace((polygon&3)==3?GL_FRONT_AND_BACK:polygon&1?GL_FRONT:GL_BACK);}else glDisable(GL_CULL_FACE);
    glColorMask(mask&1,mask&2,mask&4,mask&8);
    uint32 tl=r[Latte::REGADDR::PA_SC_GENERIC_SCISSOR_TL],br=r[Latte::REGADDR::PA_SC_GENERIC_SCISSOR_BR];
    int left=tl&0x7fff,top=(tl>>16)&0x7fff,right=br&0x7fff,bottom=(br>>16)&0x7fff;
    glEnable(GL_SCISSOR_TEST);glScissor(left,top,std::max(right-left,0),std::max(bottom-top,0));
    if(control&(1<<8)){
        glEnable(GL_BLEND);uint32 b=r[Latte::REGADDR::CB_BLEND0_CONTROL];
        const GLenum factors[]={GL_ZERO,GL_ONE,GL_SRC_COLOR,GL_ONE_MINUS_SRC_COLOR,GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_DST_ALPHA,GL_ONE_MINUS_DST_ALPHA,GL_DST_COLOR,GL_ONE_MINUS_DST_COLOR,GL_SRC_ALPHA_SATURATE,GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_CONSTANT_COLOR,GL_ONE_MINUS_CONSTANT_COLOR,GL_SRC1_COLOR,GL_ONE_MINUS_SRC1_COLOR,GL_SRC1_ALPHA,GL_ONE_MINUS_SRC1_ALPHA,GL_CONSTANT_ALPHA,GL_ONE_MINUS_CONSTANT_ALPHA};
        const GLenum equations[]={GL_FUNC_ADD,GL_FUNC_SUBTRACT,GL_MIN,GL_MAX,GL_FUNC_REVERSE_SUBTRACT};
        uint32 src=b&31,dst=(b>>8)&31,eq=(b>>5)&7,as=(b>>16)&31,ad=(b>>24)&31,ae=(b>>21)&7;
        if(!(b&(1<<29))){as=src;ad=dst;ae=eq;}
        if(src>=21||dst>=21||as>=21||ad>=21||eq>=5||ae>=5)fail("invalid blend state");
        glBlendColor(as_float(r[0xa105]),as_float(r[0xa106]),as_float(r[0xa107]),as_float(r[0xa108]));
        glBlendFuncSeparate(factors[src],factors[dst],factors[as],factors[ad]);glBlendEquationSeparate(equations[eq],equations[ae]);
    }else glDisable(GL_BLEND);
    GLint maxAttributes;glGetIntegerv(GL_MAX_VERTEX_ATTRIBS,&maxAttributes);for(int i=0;i<maxAttributes;i++)glDisableVertexAttribArray(i);
    for(auto& group:fetch->bufferGroups){uint32 reg=mmSQ_VTX_ATTRIBUTE_BLOCK_START+group.attributeBufferIndex*7,addr=r[reg],size=r[reg+1]+1,stride=(r[reg+2]>>11)&65535;
        if(!addr||!size||(uint64)addr+size>(1ull<<32))fail("invalid attribute buffer");GLuint b;glGenBuffers(1,&b);buffers.push_back(b);glBindBuffer(GL_ARRAY_BUFFER,b);glBufferData(GL_ARRAY_BUFFER,size,g_mem+addr,GL_STREAM_DRAW);
        for(int i=0;i<group.attribCount;i++){auto& a=group.attrib[i];int loc=vs.resourceMappingGL.attributeMapping[a.semanticId];if(loc<0)continue;
            uint32 bits=Latte::GetFormatBits(a.format);unsigned components=(bits+31)/32;GLenum type=GL_UNSIGNED_INT;
            uint32 fmt=(uint32)a.format;
            if(fmt==1||fmt==7||fmt==26||fmt==44){type=GL_UNSIGNED_BYTE;components=bits/8;}
            else if(fmt==5||fmt==6||fmt==15||fmt==16||fmt==31||fmt==32||fmt==45||fmt==46){type=GL_UNSIGNED_SHORT;components=bits/16;}
            glEnableVertexAttribArray(loc);glVertexAttribIPointer(loc,components,type,stride,(void*)(uintptr_t)a.offset);
            glVertexAttribDivisor(loc,a.fetchType==LatteConst::INSTANCE_DATA?std::max(a.aluDivisor,1):0);}
    }
    GLenum primitive;switch(r[Latte::REGADDR::VGT_PRIMITIVE_TYPE]){case 1:primitive=GL_POINTS;break;case 2:primitive=GL_LINES;break;case 3:primitive=GL_LINE_STRIP;break;case 4:primitive=GL_TRIANGLES;break;case 5:primitive=GL_TRIANGLE_FAN;break;case 6:primitive=GL_TRIANGLE_STRIP;break;default:fail("unsupported primitive");}
    if(indices){
        if(indexType!=0&&indexType!=1&&indexType!=4&&indexType!=9)fail("invalid GPU index type");
        uint32 bytes=(indexType&1)?4:2;if((uint64)indices+(uint64)count*bytes>(1ull<<32))fail("index buffer outside guest memory");
        std::vector<uint32> values(count);for(uint32 i=0;i<count;i++){
            uint32 addr=indices+i*bytes;
            if(indexType==4)values[i]=ld16(addr);else if(indexType==9)values[i]=ld32(addr);
            else if(bytes==4)memcpy(&values[i],g_mem+addr,4);else {uint16 v;memcpy(&v,g_mem+addr,2);values[i]=v;}
        }
        GLuint b;glGenBuffers(1,&b);buffers.push_back(b);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,b);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,values.size()*4,values.data(),GL_STREAM_DRAW);
        glDrawElementsInstancedBaseVertex(primitive,count,GL_UNSIGNED_INT,nullptr,instances,(GLint)base);
    }else glDrawArraysInstanced(primitive,base,count,instances);glDeleteBuffers(buffers.size(),buffers.data());
    if(color)color->rendered=true;if(depth)depth->rendered=true;
    GLenum err=glGetError();if(err!=GL_NO_ERROR)throw std::runtime_error("OpenGL draw error "+std::to_string(err));
}
}
