/* Synthetic integration checks for the runtime backend, without game data. */
#include "../runtime/gx2/render.cpp"
static unsigned presented;
static EGLContext windowContext=EGL_NO_CONTEXT;
extern "C" { uint8_t* g_mem; void hle_window_present(const void* rgb,int width,int height) {
    assert(eglGetCurrentContext()==windowContext);
    assert(width==16&&height==16);auto pixels=static_cast<const uint8_t*>(rgb);
    assert(pixels[0]==191&&pixels[1]==255&&pixels[2]==0);++presented;
    if(windowContext==EGL_NO_CONTEXT){
        EGLint attrs[]={EGL_CONTEXT_MAJOR_VERSION,4,EGL_CONTEXT_MINOR_VERSION,3,EGL_NONE};
        windowContext=eglCreateContext(gpu::display,nullptr,EGL_NO_CONTEXT,attrs);assert(windowContext!=EGL_NO_CONTEXT);
    }
    assert(eglMakeCurrent(gpu::display,EGL_NO_SURFACE,EGL_NO_SURFACE,windowContext));
} }
static std::array<uint32,29> surface(uint32 address,uint32 format=26,uint32 dim=1,uint32 extent=16) {
    std::array<uint32,29> s{dim,extent,dim?extent:1u,1,1,format,0,3};s[12]=16;
    assert(gx2_surface_layout(s.data()));s[9]=address;return s;
}
int main(int argc,char** argv) {
    assert(argc==2);
    std::vector<uint8> memory(1<<20);g_mem=memory.data();
    auto color=surface(0x1000),depth=surface(0x2000,17,1,32),lookup=surface(0x3000,0x80e,0),copy=surface(0x4000);
    for(unsigned i=0;i<16;i++){float value=.25f;memcpy(g_mem+0x3000+4*i,&value,4);}
    const float black[]{0,0,0,1};gpu::render_clear(color.data(),black);gpu::render_clear_depth(depth.data(),1,0,3);
    gpu::remember_surface(lookup[9],lookup.data());
    std::vector<uint32> regs(0x10009);auto r=regs.data();r[mmCB_COLOR0_BASE]=color[9];r[mmDB_HTILE_DATA_BASE]=depth[9]>>8;
    r[mmCB_COLOR0_INFO]=26<<2;r[mmCB_COLOR0_SIZE]=1|(3<<10);
    r[Latte::REGADDR::PA_CL_VPORT_XSCALE]=r[Latte::REGADDR::PA_CL_VPORT_YSCALE]=std::bit_cast<uint32>(8.f);
    r[Latte::REGADDR::PA_CL_VPORT_XOFFSET]=r[Latte::REGADDR::PA_CL_VPORT_YOFFSET]=std::bit_cast<uint32>(8.f);
    r[Latte::REGADDR::PA_CL_VPORT_ZSCALE]=r[Latte::REGADDR::PA_CL_VPORT_ZOFFSET]=std::bit_cast<uint32>(.5f);
    r[Latte::REGADDR::CB_TARGET_MASK]=15;r[Latte::REGADDR::PA_SC_GENERIC_SCISSOR_BR]=16|(16<<16);r[Latte::REGADDR::VGT_PRIMITIVE_TYPE]=4;
    r[Latte::REGADDR::DB_DEPTH_CONTROL]=2|4|(1<<4); // depth test/write, LESS
    r[Latte::REGADDR::PA_SU_SC_MODE_CNTL]=2; // cull back, CCW front
    r[mmSQ_VTX_UNIFORM_BLOCK_START]=0x5000;r[mmSQ_VTX_UNIFORM_BLOCK_START+1]=15;
    const float transform[]{0,0,0,1};memcpy(g_mem+0x5000,transform,sizeof transform);
    r[Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS+2]=lookup[9]>>8;
    r[Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS+4]=(0|(1<<3)|(2<<6)|(3<<9))<<16;
    StringBuf vertex(1024),fragment(1024);
    vertex.add("#version 430\nlayout(std140,binding=0) uniform uniformBlockVS0 {vec4 transform;}; void main(){vec2 p[3]=vec2[](vec2(-1,-1),vec2(3,-1),vec2(-1,3));gl_Position=vec4(p[gl_VertexID],transform.z,transform.w);}");
    fragment.add("#version 430\nlayout(binding=0) uniform sampler1D lookup;layout(location=0) out vec4 color;void main(){color=vec4(texture(lookup,.5).r,1,0,1);}");
    LatteDecompilerShader v(LatteConst::ShaderType::Vertex),p(LatteConst::ShaderType::Pixel);
    v.strBuf_shaderSource=&vertex;v.uniformMode=LATTE_DECOMPILER_UNIFORM_MODE_FULL_CBANK;
    p.strBuf_shaderSource=&fragment;p.pixelColorOutputMask=1;p.textureUnitListCount=1;p.textureUnitList[0]=0;
    LatteDecompilerOutput_t vo{},po{};vo.shader=&v;po.shader=&p;vo.resourceMappingGL.uniformBuffersBindingPoint[0]=0;po.resourceMappingGL.textureUnitToBindingPoint[0]=0;
    LatteFetchShader fetch{};
    gpu::render_draw(r,&fetch,vo,nullptr,po,3,0,1,0,0);
    { gpu::Current current;uint8 pixel[4];glReadPixels(8,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);assert(pixel[0]==64&&pixel[1]==255&&pixel[2]==0); }
    // A farther fragment with a changed lookup must fail depth testing.
    float z=.8f,value=.75f;memcpy(g_mem+0x5008,&z,4);for(unsigned i=0;i<16;i++)memcpy(g_mem+0x3000+4*i,&value,4);
    gpu::render_invalidate(0x3000,64);gpu::render_draw(r,&fetch,vo,nullptr,po,3,0,1,0,0);
    { gpu::Current current;uint8 pixel[4];glReadPixels(8,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);assert(pixel[0]==64); }
    // Clear depth, redraw, then copy with a previously attached depth target.
    gpu::render_clear_depth(depth.data(),1,0,1);gpu::render_draw(r,&fetch,vo,nullptr,po,3,0,1,0,0);
    gpu::render_copy(color.data(),copy.data());
    { gpu::Current current;gpu::bind_target(gpu::texture(copy[9]));uint8 pixel[4];glReadPixels(8,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);assert(pixel[0]==191&&pixel[1]==255&&pixel[2]==0);assert(glGetError()==GL_NO_ERROR); }
    gpu::render_present(copy.data(),1,argv[1]);assert(presented==1);
    // Rendering must reacquire its context after window presentation.
    gpu::render_clear(color.data(),black);
    assert(eglGetCurrentContext()==windowContext);
    { gpu::Current current;uint8 pixel[4];glReadPixels(8,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);assert(pixel[0]==0&&pixel[1]==0&&pixel[2]==0);assert(glGetError()==GL_NO_ERROR); }
    gpu::render_present(copy.data(),1,argv[1]);assert(presented==2);
    // No pixel color exports also makes this a depth-only draw, even with
    // channel writes enabled and a stale color address aliasing the depth map.
    gpu::render_clear_depth(depth.data(),1,0,1);p.pixelColorOutputMask=0;
    r[mmCB_COLOR0_BASE]=depth[9];
    gpu::render_draw(r,&fetch,vo,nullptr,po,3,0,1,0,0);
    { gpu::Current current;float value=0;glReadPixels(8,8,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,&value);
      assert(value>.89f&&value<.91f);assert(glGetError()==GL_NO_ERROR); }
    p.pixelColorOutputMask=1;r[mmCB_COLOR0_BASE]=color[9];
    // A stencil-only mask pass must not write color. The subsequent effect
    // is visible only inside that mask, even when depth testing is disabled.
    gpu::render_clear(color.data(),black);gpu::render_clear_depth(depth.data(),1,0,3);
    r[Latte::REGADDR::PA_SU_SC_MODE_CNTL]=0;
    r[Latte::REGADDR::CB_COLOR_CONTROL]=1<<4;
    r[Latte::REGADDR::DB_DEPTH_CONTROL]=1|(7<<8)|(2<<14); // ALWAYS, REPLACE
    r[Latte::REGADDR::DB_STENCILREFMASK]=1|(255<<8)|(255<<16);
    r[Latte::REGADDR::PA_SC_GENERIC_SCISSOR_BR]=8|(16<<16);
    // The color register may still alias a depth image when color writes are
    // disabled, as in the village shadow pass. It must not be attached as color.
    r[mmCB_COLOR0_BASE]=depth[9];
    gpu::render_draw(r,&fetch,vo,nullptr,po,3,0,1,0,0);
    { gpu::Current current;uint8 pixel[4],stencil;
      glReadPixels(4,8,1,1,GL_STENCIL_INDEX,GL_UNSIGNED_BYTE,&stencil);assert(stencil==1);
      gpu::bind_target(gpu::texture(color[9]));
      glReadPixels(4,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);assert(pixel[0]==0&&pixel[1]==0); }
    r[mmCB_COLOR0_BASE]=color[9];
    r[Latte::REGADDR::CB_COLOR_CONTROL]=0;
    r[Latte::REGADDR::DB_DEPTH_CONTROL]=1|(2<<8); // EQUAL, KEEP
    r[Latte::REGADDR::PA_SC_GENERIC_SCISSOR_BR]=16|(16<<16);
    gpu::render_draw(r,&fetch,vo,nullptr,po,3,0,1,0,0);
    { gpu::Current current;uint8 pixel[4];
      glReadPixels(4,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);assert(pixel[0]==191&&pixel[1]==255);
      glReadPixels(12,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);assert(pixel[0]==0&&pixel[1]==0); }
    // Independent back-face comparison can reject the same geometry.
    gpu::render_clear(color.data(),black);
    r[Latte::REGADDR::PA_SU_SC_MODE_CNTL]=4; // CW front; triangle is back-facing
    r[Latte::REGADDR::DB_DEPTH_CONTROL]=1|128|(7<<8); // front ALWAYS, back NEVER
    gpu::render_draw(r,&fetch,vo,nullptr,po,3,0,1,0,0);
    { gpu::Current current;uint8 pixel[4];glReadPixels(4,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);assert(pixel[1]==0); }
    r[Latte::REGADDR::DB_DEPTH_CONTROL]=1|(2<<8); // back inherits front again
    gpu::render_draw(r,&fetch,vo,nullptr,po,3,0,1,0,0);
    { gpu::Current current;uint8 pixel[4];glReadPixels(4,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);assert(pixel[1]==255);assert(glGetError()==GL_NO_ERROR); }
    // Render-to-texture must preserve guest top/bottom ordering on reuse.
    r[Latte::REGADDR::DB_DEPTH_CONTROL]=0;r[Latte::REGADDR::PA_SU_SC_MODE_CNTL]=0;
    r[Latte::REGADDR::PA_CL_VPORT_YSCALE]=std::bit_cast<uint32>(-8.f);
    StringBuf orientedVertex(1024),bands(1024),sampled(1024);
    orientedVertex.add("#version 430\nout vec2 uv;void main(){vec2 p[3]=vec2[](vec2(-1,-1),vec2(3,-1),vec2(-1,3));vec2 q=p[gl_VertexID];gl_Position=vec4(q,0,1);uv=vec2(q.x+1,1-q.y)*.5;}");
    bands.add("#version 430\nin vec2 uv;layout(location=0) out vec4 color;void main(){color=uv.y<.5?vec4(1,0,0,1):vec4(0,1,0,1);}");
    sampled.add("#version 430\nin vec2 uv;layout(binding=0) uniform sampler2D source;layout(location=0) out vec4 color;void main(){color=texture(source,uv);}");
    v.strBuf_shaderSource=&orientedVertex;p.strBuf_shaderSource=&bands;p.textureUnitListCount=0;
    gpu::render_draw(r,&fetch,vo,nullptr,po,3,0,1,0,0);
    auto checkBands=[](){gpu::Current current;uint8 pixel[4];
        glReadPixels(8,2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);assert(pixel[0]==255&&pixel[1]==0);
        glReadPixels(8,13,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);assert(pixel[0]==0&&pixel[1]==255);};
    checkBands();
    r[mmCB_COLOR0_BASE]=copy[9];r[Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS+2]=color[9]>>8;
    p.strBuf_shaderSource=&sampled;p.textureUnitListCount=1;
    gpu::render_draw(r,&fetch,vo,nullptr,po,3,0,1,0,0);checkBands();
    // Recorded command buffers can replace only CB_COLOR0_BASE, pointing at
    // memory previously used by a depth image. The color registers define the
    // new view even without a host GX2SetColorBuffer descriptor registration.
    r[mmCB_COLOR0_BASE]=depth[9];
    gpu::render_draw(r,&fetch,vo,nullptr,po,3,0,1,0,0);checkBands();
    { gpu::Current current;auto& target=gpu::texture(depth[9]);
      assert(target.words[5]==26&&target.words[1]==16&&target.words[2]==16); }
    // Switching the allocation back to depth must discard the color GL image.
    gpu::render_clear_depth(depth.data(),.3f,0,1);
    { gpu::Current current;GLuint f;glGenFramebuffers(1,&f);glBindFramebuffer(GL_FRAMEBUFFER,f);
      gpu::attach_depth(gpu::depth_texture(depth[9]));glDrawBuffer(GL_NONE);glReadBuffer(GL_NONE);
      assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
      float value=0;glReadPixels(28,28,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,&value);
      assert(value>.29f&&value<.31f);assert(glGetError()==GL_NO_ERROR);glDeleteFramebuffers(1,&f); }
    assert(eglMakeCurrent(gpu::display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT));eglDestroyContext(gpu::display,windowContext);
}
