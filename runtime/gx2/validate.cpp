/* GPU submission frontend. PM4 encoding follows Cemu GX2_Shader.cpp,
 * GX2_shader_legacy.cpp, GX2_Draw.cpp and GX2_Texture.cpp. Diagnostic mode
 * exports translated shaders; render mode also executes them through OpenGL. */
#include "decompiler/compat.h"
#include "commands.h"
#include "render.h"
#include "profile.h"
#include <filesystem>
#include <fstream>
#include <unordered_set>
extern "C" {
#include "../standalone/rt.h"
}
namespace {
std::filesystem::path directory;
struct List { uint32 address{}, capacity{}, used{}; };
std::unordered_map<void*,List> lists;
std::unordered_set<std::string> seen;
ppc_ctx* cpu;
bool rendering;
uint64 submitted,retired,lastSwap;
uint32 swapCount,swapInterval=1;
uint32_t instances = 1, baseVertex, indexType;
void packet(uint8_t op, std::span<const uint32> p);
gpu::CommandProcessor processor([](uint32 a) { return ld32(a); }, packet);
void validate_draw(uint32 count, uint32 indices=0);
void packet(uint8_t op, std::span<const uint32> p) {
    if (op == 0x2f && p.size() == 1) { instances = p[0]; return; }
    if (op == 0x2a && p.size() == 1) {indexType=p[0];return;}
    if (op == 0x27 && p.size()==5 && !p[2]) {if(instances)validate_draw(p[3],p[1]);return;}
    if (op == 0x6f && p.size() == 2 && p[0]==0) {baseVertex=p[1];return;}
    // Private HLE packets snapshot descriptors while recording display lists.
    if (op == 0xf5 && p.size()==33) {float rgba[4];memcpy(rgba,p.data()+29,16);gpu::render_clear(p.data(),rgba);return;}
    if (op == 0xf6 && p.size()==32){float depth;memcpy(&depth,p.data()+29,4);gpu::render_clear_depth(p.data(),depth,p[30],p[31]);return;}
    if (op == 0xf2 && p.size()==2){gpu::render_invalidate(p[0],p[1]);return;}
    if (op == 0xee && p.size()==58) {gpu::render_copy(p.data(),p.data()+29);return;}
    if (op == 0xf3 && p.size()==30) {gpu::render_present(p.data(),p[29],directory);return;}
    if (op == 0x2d && p.size()==2) {if(instances)validate_draw(p[0]);return;}
    throw std::runtime_error("unsupported diagnostic GPU packet");
}
void execute(std::span<const uint32> words) {
    try { processor.execute(words); }
    catch (const std::exception& e) { rt_fatal(cpu, "GPU command: %s", e.what()); }
}
void emit(ppc_ctx* c, uint8_t op, std::vector<uint32> data) {
    cpu = c;
    std::vector<uint32> words{gpu::header(op, data.size())};
    words.insert(words.end(), data.begin(), data.end());
    List& list = lists[c->host];
    if (list.address) {
        if (words.size() * 4 > list.capacity - list.used) rt_fatal(c, "GX2 display list overflow");
        for (uint32 w : words) { st32(list.address + list.used, w); list.used += 4; }
    } else execute(words);
}
void regs(ppc_ctx* c, uint32 reg, std::vector<uint32> values) {
    uint32 base; uint8_t op;
    if (reg >= 0xf000) { base=0xf000;op=0x6e; }
    else if (reg >= 0xe000) { base=0xe000;op=0x6d; }
    else if (reg >= 0xc000) { base=0xc000;op=0x6a; }
    else if (reg >= 0xa000) { base=0xa000;op=0x69; }
    else { base=0x2000;op=0x68; }
    values.insert(values.begin(),reg-base);emit(c,op,std::move(values));
}
void reg_array(ppc_ctx* c, uint32 reg, uint32 addr, uint32 n) {
    if (!n) return;
    std::vector<uint32> v(n);for (uint32 i=0;i<n;i++) v[i]=ld32(addr+i*4);regs(c,reg,std::move(v));
}
void bind_program(ppc_ctx* c, uint32 reg, uint32 address, uint32 size, uint32 resource) {
    if (!address || !size || (uint64)address+size>(1ull<<32)) rt_fatal(c,"invalid GPU program range");
    regs(c,reg,{address>>8,size>>3,0x100000,0x100000,resource});
}
void fetch(ppc_ctx* c) {
    uint32 a=c->r[3];bind_program(c,mmSQ_PGM_START_FS,ld32(a+12),ld32(a+8),ld32(a+4));
    reg_array(c,Latte::REGADDR::VGT_INSTANCE_STEP_RATE_0,a+24,2);
}
void vertex(ppc_ctx* c) {
    uint32 a=c->r[3],mode=ld32(a+0xd8),ptr=ld32(a+0xd4),size=ld32(a+0xd0);
    if (!ptr) { ptr=ld32(a+0x130);size=ld32(a+0x128)*ld32(a+0x12c); }
    bind_program(c,mode==2?mmSQ_PGM_START_ES:mmSQ_PGM_START_VS,ptr,size,ld32(a));
    if (mode!=2) {
        regs(c,Latte::REGADDR::VGT_PRIMITIVEID_EN,{ld32(a+4)});
        regs(c,Latte::REGADDR::SPI_VS_OUT_CONFIG,{ld32(a+8)});
        regs(c,Latte::REGADDR::PA_CL_VS_OUT_CNTL,{ld32(a+0x38)});
        reg_array(c,mmSPI_VS_OUT_ID_0,a+16,std::min(ld32(a+12),10u));
    }
    uint32 n=std::min(ld32(a+0x40),32u);
    regs(c,Latte::REGADDR::SQ_VTX_SEMANTIC_CLEAR,{n==32?0u:0xffffffffu<<n});
    reg_array(c,Latte::REGADDR::SQ_VTX_SEMANTIC_0,a+0x44,n);
    regs(c,mmSQ_ESGS_RING_ITEMSIZE,{ld32(a+0x10c)});
}
void pixel(ppc_ctx* c) {
    uint32 a=c->r[3],ptr=ld32(a+0xa8),size=ld32(a+0xa4);
    if (!ptr) { ptr=ld32(a+0xe4);size=ld32(a+0xdc)*ld32(a+0xe0); }
    bind_program(c,mmSQ_PGM_START_PS,ptr,size,ld32(a));
    reg_array(c,mmSPI_PS_IN_CONTROL_0,a+8,2);reg_array(c,mmSPI_PS_INPUT_CNTL_0,a+20,std::min(ld32(a+16),32u));
    regs(c,mmCB_SHADER_MASK,{ld32(a+148)});regs(c,mmCB_SHADER_CONTROL,{ld32(a+152)});
    regs(c,mmDB_SHADER_CONTROL,{ld32(a+156)});regs(c,mmSPI_INPUT_Z,{ld32(a+160)});
}
void geometry(ppc_ctx* c) {
    uint32 a=c->r[3];
    bind_program(c,mmSQ_PGM_START_GS,ld32(a+0x50),ld32(a+0x4c),ld32(a));
    bind_program(c,mmSQ_PGM_START_VS,ld32(a+0x58),ld32(a+0x54),ld32(a+16));
    regs(c,mmVGT_GS_OUT_PRIM_TYPE,{ld32(a+4)});regs(c,Latte::REGADDR::VGT_GS_MODE,{ld32(a+8)});
    regs(c,mmPA_CL_VS_OUT_CNTL,{ld32(a+12)});regs(c,mmSQ_GS_VERT_ITEMSIZE,{ld32(a+20)});
    regs(c,mmSPI_VS_OUT_CONFIG,{ld32(a+24)});reg_array(c,mmSPI_VS_OUT_ID_0,a+32,std::min(ld32(a+28),10u));
    regs(c,mmSQ_GSVS_RING_ITEMSIZE,{ld32(a+0x88)&0x7fff});
}
void mode(ppc_ctx* c) {
    Latte::LATTE_SQ_CONFIG sq;
    sq.set_DX9_CONSTS(c->r[3]==0).set_ALU_INST_PREFER_VECTOR(true);
    regs(c,Latte::REGADDR::SQ_CONFIG,{sq.getRawValue()});
    if (c->r[3]!=2) regs(c,Latte::REGADDR::VGT_GS_MODE,{0});
}
template<uint32 base> void uniform(ppc_ctx* c) {
    if (c->r[3]>=16) rt_fatal(c,"invalid uniform buffer index");
    regs(c,base+c->r[3]*7,{c->r[5],c->r[4]-1,0,1,0,0,0xc0000000});
}
void attrib(ppc_ctx* c) {
    if (c->r[3]>=16) rt_fatal(c,"invalid vertex buffer index");
    regs(c,mmSQ_VTX_ATTRIBUTE_BLOCK_START+c->r[3]*7,{c->r[6],c->r[4]-1,(c->r[5]&65535)<<11,0,0,0,0xc0000000});
}
template<uint32 base> void texture(ppc_ctx* c) {
    uint32 a=c->r[3],unit=c->r[4];if(unit>=18)rt_fatal(c,"invalid texture unit");
    uint32 image=ld32(a+0x24),mip=ld32(a+0x2c),swizzle=ld32(a+0x34)&0xffff;
    if (ld32(a+0x30)>=4 && ld32(a+0x30)!=16) image^=swizzle;
    if(rendering){uint32 words[29];for(unsigned i=0;i<29;i++)words[i]=ld32(a+4*i);gpu::remember_surface(image,words);}
    regs(c,base+unit*7,{ld32(a+0x88),ld32(a+0x8c),image>>8,mip>>8,ld32(a+0x90),ld32(a+0x94),ld32(a+0x98)});
}
template<uint32 base> void sampler(ppc_ctx* c) {
    if(c->r[4]>=18)rt_fatal(c,"invalid sampler unit");reg_array(c,0xf000+(base+c->r[4])*3,c->r[3],3);
}
void begin(ppc_ctx* c) {
    List& l=lists[c->host]; if(l.address || !c->r[3] || (uint64)c->r[3]+c->r[4]>(1ull<<32))rt_fatal(c,"invalid display list begin");
    l={c->r[3],c->r[4]&~3u,0};
}
void end(ppc_ctx* c) {
    List& l=lists[c->host]; if(!l.address||c->r[3]!=l.address)rt_fatal(c,"mismatched display list end");
    uint32 padded=(l.used+31)&~31u;if(padded>l.capacity)rt_fatal(c,"display list padding overflow");
    while(l.used<padded){st32(l.address+l.used,0x80000000);l.used+=4;}
    c->r[3]=l.used;l={};
}
void call_list(ppc_ctx* c) { emit(c,0x32,{c->r[3],0,c->r[4]/4}); }
void draw(ppc_ctx* c) {
    regs(c,Latte::REGADDR::VGT_PRIMITIVE_TYPE,{c->r[3]});emit(c,0x6f,{0,c->r[5]});
    emit(c,0x2f,{c->r[6]});emit(c,0x2d,{c->r[4],0});
}
void draw_indexed(ppc_ctx* c) {
    regs(c,Latte::REGADDR::VGT_PRIMITIVE_TYPE,{c->r[3]});emit(c,0x6f,{0,c->r[7]});
    emit(c,0x2a,{c->r[5]});emit(c,0x2f,{c->r[8]});emit(c,0x27,{0xffffffff,c->r[6],0,c->r[4],0});
}
void blend(ppc_ctx* c) {
    if(c->r[3]>=8)rt_fatal(c,"invalid blend target");
    uint32 v=(c->r[4]&31)|((c->r[6]&7)<<5)|((c->r[5]&31)<<8)|((c->r[8]&31)<<16)|((c->r[10]&7)<<21)|((c->r[9]&31)<<24)|((c->r[7]!=0)<<29);
    regs(c,Latte::REGADDR::CB_BLEND0_CONTROL+c->r[3],{v});
}
float farg(ppc_ctx* c, unsigned n) { double v;memcpy(&v,&c->f[n],8);return (float)v; }
uint32 bits(float f) { uint32 v;memcpy(&v,&f,4);return v; }
void alpha(ppc_ctx* c) {
    Latte::LATTE_SX_ALPHA_TEST_CONTROL v;
    v.set_ALPHA_FUNC((Latte::LATTE_SX_ALPHA_TEST_CONTROL::E_ALPHA_FUNC)c->r[4]).set_ALPHA_TEST_ENABLE(c->r[3]!=0);
    regs(c,Latte::REGADDR::SX_ALPHA_TEST_CONTROL,{v.getRawValue()});regs(c,Latte::REGADDR::SX_ALPHA_REF,{bits(farg(c,1))});
}
void clip(ppc_ctx* c) {
    Latte::LATTE_PA_CL_CLIP_CNTL v;
    v.set_ZCLIP_NEAR_DISABLE(!c->r[4]).set_ZCLIP_FAR_DISABLE(!c->r[4]).set_DX_RASTERIZATION_KILL(!c->r[3]);
    v.set_DX_CLIP_SPACE_DEF(c->r[5]!=0).set_DX_LINEAR_ATTR_CLIP_ENA(true);
    regs(c,Latte::REGADDR::PA_CL_CLIP_CNTL,{v.getRawValue()});
}
void viewport(ppc_ctx* c) {
    float x=farg(c,1),y=farg(c,2),w=farg(c,3),h=farg(c,4),n=farg(c,5),f=farg(c,6);
    regs(c,Latte::REGADDR::PA_CL_VPORT_XSCALE,{bits(w*.5f),bits(x+w*.5f),bits(h*-.5f),bits(y+h*.5f),bits((f-n)*.5f),bits((n+f)*.5f)});
}
void scissor(ppc_ctx* c) { regs(c,Latte::REGADDR::PA_SC_GENERIC_SCISSOR_TL,
    {(c->r[3]&0x7fff)|((c->r[4]&0x7fff)<<16)|0x80000000u,((c->r[3]+c->r[5])&0x7fff)|(((c->r[4]+c->r[6])&0x7fff)<<16)}); }
void color_control(ppc_ctx* c) {
    Latte::LATTE_CB_COLOR_CONTROL v;
    v.set_MULTIWRITE_ENABLE(c->r[5]!=0).set_BLEND_MASK(c->r[4]).set_ROP((Latte::LATTE_CB_COLOR_CONTROL::E_LOGICOP)c->r[3]);
    v.set_SPECIAL_OP(c->r[6]?Latte::LATTE_CB_COLOR_CONTROL::E_SPECIALOP::NORMAL:Latte::LATTE_CB_COLOR_CONTROL::E_SPECIALOP::DISABLE);
    regs(c,Latte::REGADDR::CB_COLOR_CONTROL,{v.getRawValue()});
}
void masks(ppc_ctx* c) { uint32 v=0;for(unsigned i=0;i<8;i++)v|=(c->r[3+i]&15)<<(4*i);regs(c,Latte::REGADDR::CB_TARGET_MASK,{v}); }
void depth_control(ppc_ctx* c) {
    Latte::LATTE_DB_DEPTH_CONTROL v;
    v.set_Z_ENABLE(c->r[3]!=0).set_Z_WRITE_ENABLE(c->r[4]!=0).set_Z_FUNC((Latte::LATTE_DB_DEPTH_CONTROL::E_ZFUNC)c->r[5]);
    regs(c,Latte::REGADDR::DB_DEPTH_CONTROL,{v.getRawValue()});
}
// Cemu GX2_State.cpp::GX2SetDepthStencilControl / GX2SetStencilMask.
void depth_stencil_control(ppc_ctx* c) {
    auto arg=[&](unsigned i){return i<8?c->r[3+i]:ld32(c->r[1]+8+4*(i-8));};
    uint32 v=(arg(0)!=0?2:0)|(arg(1)!=0?4:0)|((arg(2)&7)<<4)
        |(arg(3)!=0?1:0)|(arg(4)!=0?128:0)
        |((arg(5)&7)<<8)|((arg(6)&7)<<14)|((arg(7)&7)<<17)|((arg(8)&7)<<11)
        |((arg(9)&7)<<20)|((arg(10)&7)<<26)|((arg(11)&7)<<29)|((arg(12)&7)<<23);
    regs(c,Latte::REGADDR::DB_DEPTH_CONTROL,{v});
}
void stencil_mask(ppc_ctx* c) {
    regs(c,Latte::REGADDR::DB_STENCILREFMASK,
        {(c->r[5]&255)|((c->r[3]&255)<<8)|((c->r[4]&255)<<16),
         (c->r[8]&255)|((c->r[6]&255)<<8)|((c->r[7]&255)<<16)});
}
void setup_context(ppc_ctx* c) {
    // Default registers consumed by shader translation, from Cemu GX2SetDefaultState.
    regs(c,Latte::REGADDR::PA_CL_VTE_CNTL,{0x43f});
    regs(c,Latte::REGADDR::DB_DEPTH_CONTROL,{0x16});
    Latte::LATTE_SQ_CONFIG sq; sq.set_DX9_CONSTS(true).set_ALU_INST_PREFER_VECTOR(true);
    regs(c,Latte::REGADDR::SQ_CONFIG,{sq.getRawValue()});
}
std::vector<uint32> surface_words(uint32 a) { std::vector<uint32> v(29);for(unsigned i=0;i<29;i++)v[i]=ld32(a+4*i);return v; }
void clear_depth(ppc_ctx* c){auto v=surface_words(c->r[3]);v.push_back(bits(farg(c,1)));v.push_back(c->r[4]);v.push_back(c->r[5]);emit(c,0xf6,std::move(v));}
void depth_buffer(ppc_ctx* c){auto v=surface_words(c->r[3]);if(ld32(c->r[3]+0x74)||ld32(c->r[3]+0x78))rt_fatal(c,"depth mip/slice unsupported");gpu::remember_surface(v[9],v.data());regs(c,mmDB_HTILE_DATA_BASE,{v[9]>>8});}
void cull(ppc_ctx* c){regs(c,Latte::REGADDR::PA_SU_SC_MODE_CNTL,{(c->r[4]&1)|((c->r[5]&1)<<1)|((c->r[3]&1)<<2)});}
void blend_color(ppc_ctx* c){regs(c,0xa105,{bits(farg(c,1)),bits(farg(c,2)),bits(farg(c,3)),bits(farg(c,4))});}
void clear_color(ppc_ctx* c) {auto v=surface_words(c->r[3]);for(unsigned i=1;i<=4;i++)v.push_back(bits(farg(c,i)));emit(c,0xf5,std::move(v));}
void flush(ppc_ctx* c){if(rendering)gpu::render_finish();submitted=std::max(submitted+1,ppc_timebase(c));retired=submitted;}
void draw_done(ppc_ctx* c){flush(c);c->r[3]=1;}
void timestamp(ppc_ctx* c){c->r[3]=submitted>>32;c->r[4]=submitted;}
void wait_timestamp(ppc_ctx* c){uint64 value=(uint64)c->r[3]<<32|c->r[4];c->r[3]=value<=retired;}
// Synchronous backend: every submitted flip has completed before returning.
// ABI follows Cemu GX2_Misc.cpp::gx2Export_GX2GetSwapStatus.
void swap_buffers(ppc_ctx* c){
    flush(c);ppc_ctx temp=*c;hle_lookup("coreinit","OSGetSystemTime")(&temp);
    lastSwap=(uint64)temp.r[3]<<32|temp.r[4];swapCount++;c->r[3]=0;
}
void swap_status(ppc_ctx* c){st32(c->r[3],swapCount);st32(c->r[4],swapCount);st64(c->r[5],lastSwap);st64(c->r[6],lastSwap);c->r[3]=0;}
void get_interval(ppc_ctx* c){c->r[3]=swapInterval;}
void set_interval(ppc_ctx* c){if(c->r[3]<20)swapInterval=c->r[3];c->r[3]=0;}
void gpu_timeout(ppc_ctx* c){c->r[3]=1000;}
void vsync(ppc_ctx* c){c->r[3]=0;c->r[4]=62156250/60;hle_thread_lookup("OSSleepTicks")(c);}
void invalidate(ppc_ctx* c){if(c->r[3]&0x42)emit(c,0xf2,{c->r[4],c->r[5]});}
void copy_surface(ppc_ctx* c) {if(c->r[4]||c->r[5]||c->r[7]||c->r[8])rt_fatal(c,"surface copy mip/slice unsupported");auto v=surface_words(c->r[3]),d=surface_words(c->r[6]);v.insert(v.end(),d.begin(),d.end());emit(c,0xee,std::move(v));}
void present(ppc_ctx* c) {auto v=surface_words(c->r[3]);v.push_back(c->r[4]);emit(c,0xf3,std::move(v));}
void color_buffer(ppc_ctx* c) {
    uint32 a=c->r[3],index=c->r[4];if(index>=8)rt_fatal(c,"invalid color target");
    auto v=surface_words(a);if(ld32(a+0x74)||ld32(a+0x78))rt_fatal(c,"color target mip/slice unsupported");
    uint32 addr=v[9];if(Latte::TM_IsMacroTiled((Latte::E_GX2TILEMODE)v[12])&&((v[13]>>16)&255))addr^=v[13]&65535;
    gpu::remember_surface(addr,v.data());regs(c,mmCB_COLOR0_BASE+index,{addr});
    regs(c,mmCB_COLOR0_SIZE+index,{ld32(a+0x88)});regs(c,mmCB_COLOR0_INFO+index,{ld32(a+0x8c)});regs(c,mmCB_COLOR0_VIEW+index,{ld32(a+0x90)});
}
void validate_draw(uint32 count,uint32 indices) {
    gpu::Profile profile(0);
    uint32* r=processor.registers.data();
    bool gs=(r[Latte::REGADDR::VGT_GS_MODE]&7)!=0;
    uint32 vsReg=gs?mmSQ_PGM_START_ES:mmSQ_PGM_START_VS;
    LatteShader_CreatePSInputTable(LatteSHRC_GetPSInputTable(),r);
    auto program=[&](uint32 reg) { uint32 addr=r[reg]<<8,size=r[reg+1]<<3;
        if(!addr||!size||size>1024*1024||(uint64)addr+size>(1ull<<32))rt_fatal(cpu,"invalid bound shader at reg %x",reg);
        return std::pair(g_mem+addr,size); };
    auto [fs,fsSize]=program(mmSQ_PGM_START_FS);
    LatteFetchShader* fetchShader=LatteShaderRecompiler_createFetchShader(0,r,(uint32*)fs,fsSize);
    LatteDecompilerOptions options{};options.usesGeometryShader=gs;options.strictMul=true;
    LatteDecompilerOutput_t output{};
    auto save=[&](const char* ext) {
        auto* s=output.shader;if(!s||s->hasError||!s->strBuf_shaderSource)rt_fatal(cpu,"shader translation failed (%s)",ext);
        std::string text=s->strBuf_shaderSource->c_str();
        if(seen.insert(text).second){std::ofstream f(directory/(std::to_string(seen.size())+ext));f<<text;
            if(!f)rt_fatal(cpu,"writing translated GPU shader");}
    };
    auto [vs,vsSize]=program(vsReg);
    LatteDecompiler_DecompileVertexShader(0,r,vs,vsSize,fetchShader,options,&output);save(".vert");
    uint32 ring=output.shader->ringParameterCount;auto vertexOutput=output;LatteDecompilerOutput_t geometryOutput{};
    if(gs){auto [code,size]=program(mmSQ_PGM_START_GS);auto [copy,copySize]=program(mmSQ_PGM_START_VS);output={};
        LatteDecompiler_DecompileGeometryShader(0,r,code,size,copy,copySize,ring,options,&output);save(".geom");
        geometryOutput=output;}
    auto [ps,psSize]=program(mmSQ_PGM_START_PS);output={};
    LatteDecompiler_DecompilePixelShader(0,r,ps,psSize,options,&output);save(".frag");
    profile.finish();
    if(rendering)gpu::render_draw(r,fetchShader,vertexOutput,gs?&geometryOutput:nullptr,output,count,baseVertex,instances,indices,indexType);
    for(auto* out:{&vertexOutput,&geometryOutput,&output})if(out->shader){delete out->shader->strBuf_shaderSource;delete out->shader;}
    delete fetchShader;
}
}
extern "C" void hle_gpu_validate_configure(const char* path) {
    auto p=std::filesystem::weakly_canonical(path);
    auto root=std::filesystem::weakly_canonical(std::filesystem::path(__FILE__).parent_path().parent_path().parent_path());
    auto rel=p.lexically_relative(root);
    if(rel.empty()||*rel.begin()!="..")throw std::runtime_error("GPU output must be outside the repository");
    std::filesystem::create_directories(p);directory=p;
}
extern "C" void hle_gpu_render_configure(const char* path) {hle_gpu_validate_configure(path);rendering=true;}
extern "C" void hle_gpu_memory_written(uint32_t address,uint32_t size) {gpu::render_invalidate(address,size);}
extern "C" hle_fn hle_gpu_validate_lookup(const char* name) {
    if(directory.empty())return nullptr;
#define E(n,f) if(!strcmp(name,n))return f
    if(rendering){E("GX2SetDepthBuffer",depth_buffer);E("GX2ClearDepthStencilEx",clear_depth);E("GX2Invalidate",invalidate);E("GX2CopySurface",copy_surface);E("GX2ClearColor",clear_color);E("GX2SetColorBuffer",color_buffer);E("GX2CopyColorBufferToScanBuffer",present);}
    E("GX2SwapScanBuffers",swap_buffers);E("GX2GetSwapStatus",swap_status);E("GX2GetSwapInterval",get_interval);E("GX2SetSwapInterval",set_interval);
    E("GX2GetGPUTimeout",gpu_timeout);E("GX2WaitForVsync",vsync);
    E("GX2Flush",flush);E("GX2DrawDone",draw_done);E("GX2GetLastSubmittedTimeStamp",timestamp);E("GX2GetRetiredTimeStamp",timestamp);E("GX2WaitTimeStamp",wait_timestamp);
    E("GX2SetupContextStateEx",setup_context);
    E("GX2SetCullOnlyControl",cull);E("GX2SetBlendConstantColor",blend_color);E("GX2SetBlendControl",blend);E("GX2DrawIndexedEx",draw_indexed);
    E("GX2SetAlphaTest",alpha);E("GX2SetRasterizerClipControlEx",clip);
    E("GX2SetViewport",viewport);E("GX2SetScissor",scissor);E("GX2SetColorControl",color_control);
    E("GX2SetTargetChannelMasks",masks);E("GX2SetDepthOnlyControl",depth_control);
    E("GX2SetDepthStencilControl",depth_stencil_control);E("GX2SetStencilMask",stencil_mask);
    E("GX2SetFetchShader",fetch);E("GX2SetVertexShader",vertex);E("GX2SetPixelShader",pixel);E("GX2SetGeometryShader",geometry);
    E("GX2SetShaderModeEx",mode);E("GX2SetAttribBuffer",attrib);
    E("GX2SetVertexUniformBlock",uniform<mmSQ_VTX_UNIFORM_BLOCK_START>);
    E("GX2SetPixelUniformBlock",uniform<mmSQ_PS_UNIFORM_BLOCK_START>);E("GX2SetGeometryUniformBlock",uniform<mmSQ_GS_UNIFORM_BLOCK_START>);
    E("GX2SetPixelTexture",texture<Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS>);
    E("GX2SetVertexTexture",texture<Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS>);
    E("GX2SetGeometryTexture",texture<Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS>);
    E("GX2SetPixelSampler",sampler<0>);E("GX2SetVertexSampler",sampler<18>);E("GX2SetGeometrySampler",sampler<36>);
    E("GX2BeginDisplayList",begin);E("GX2BeginDisplayListEx",begin);E("GX2EndDisplayList",end);E("GX2CallDisplayList",call_list);
    E("GX2DirectCallDisplayList",call_list);E("GX2DrawEx",draw);
    return nullptr;
#undef E
}
