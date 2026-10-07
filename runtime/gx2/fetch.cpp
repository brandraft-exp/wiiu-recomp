/* Fetch program generation follows Cemu GX2_Shader.cpp::_writeFetchShaderCFCode,
 * _writeFetchShaderVTXCode, GX2InitFetchShaderEx. GPU code is little endian;
 * the surrounding Cafe structures are big endian. */
#include "decompiler/compat.h"
#include "decompiler/LatteInstructions.h"
extern "C" {
#include "../standalone/rt.h"
}
static uint32 fetch_size(ppc_ctx* c, uint32 count, uint32 type, uint32 tess) {
    if (count > 32 || type || tess) rt_fatal(c, "unsupported fetch shader parameters");
    return ((((count + 15) / 16 + 1) * 8 + 15) & ~15u) + count * 16;
}
static void calc_size(ppc_ctx* c) { c->r[3] = fetch_size(c, c->r[3], c->r[4], c->r[5]); }
static void init_fetch(ppc_ctx* c) {
    uint32 a = c->r[3], program = c->r[4], count = c->r[5], attributes = c->r[6];
    uint32 size = fetch_size(c, count, c->r[7], c->r[8]), cf_size = size - count * 16;
    if (!a || !program || (uint64)program + size > (1ull << 32)) rt_fatal(c, "invalid fetch shader buffer");
    memset(g_mem + a, 0, 32); memset(g_mem + program, 0, size);
    st32(a + 4, 2); st32(a + 8, size); st32(a + 12, program); st32(a + 16, count);
    uint32 index = 0;
    for (uint32 i = 0; i < count; i += 16) {
        LatteCFInstruction_DEFAULT cf;
        cf.setField_Opcode(LatteCFInstruction::INST_VTX_TC);
        cf.setField_COUNT(std::min(count - i, 16u)); cf.setField_ADDR(cf_size + i * 16);
        memcpy(g_mem + program + index++ * 8, &cf, 8);
    }
    LatteCFInstruction_DEFAULT ret;
    ret.setField_Opcode(LatteCFInstruction::INST_RETURN); ret.setField_BARRIER(true);
    memcpy(g_mem + program + index * 8, &ret, 8);
    static const uint8 formats[] = {1,2,5,6,7,13,14,15,16,22,26,25,29,30,31,32,47,48,34,35};
    static const uint8 endian_default[] = {0,0,1,1,0,2,2,1,1,2,0,2,2,2,1,1,2,2,2,2};
    for (uint32 i = 0; i < count; ++i) {
        uint32 p = attributes + i * 32, loc = ld32(p), buffer = ld32(p + 4), offset = ld32(p + 8);
        uint32 fmt = ld32(p + 12), type = ld32(p + 16), divisor = ld32(p + 20);
        uint32 sel = ld32(p + 24), endian = ld32(p + 28), src = 0;
        if ((fmt & 63) >= 20 || type > 1 || loc > 255 || buffer >= 16 || offset > 65535)
            rt_fatal(c, "invalid fetch attribute %u", i);
        if (endian == (uint32)LatteConst::VertexFetchEndianMode::SWAP_DEFAULT) endian = endian_default[fmt & 63];
        if (endian > 2) rt_fatal(c, "unsupported fetch endian mode %u", endian);
        if (type) {
            if (!divisor) rt_fatal(c, "zero fetch instance divisor");
            if (divisor == 1) src = 3;
            else {
                uint32 n = ld32(a + 20), slot = 0;
                while (slot < n && ld32(a + 24 + slot * 4) != divisor) ++slot;
                if (slot >= 2) rt_fatal(c, "too many fetch instance divisors");
                if (slot == n) { st32(a + 24 + slot * 4, divisor); st32(a + 20, n + 1); }
                src = slot + 1;
            }
        }
        LatteClauseInstruction_VTX vtx;
        using I = LatteClauseInstruction_VTX;
        vtx.setField_VTX_INST(I::VTX_INST::_VTX_INST_SEMANTIC);
        vtx.setFieldSEM_SEMANTIC_ID(loc); vtx.setField_BUFFER_ID(buffer + 0xa0);
        vtx.setField_FETCH_TYPE((LatteConst::VertexFetchType2)type);
        vtx.setField_SRC_SEL_X((I::SRC_SEL)src); vtx.setField_DATA_FORMAT((Latte::E_HWFMT)formats[fmt & 63]);
        vtx.setField_NUM_FORMAT_ALL((I::NUM_FORMAT_ALL)(fmt & 0x800 ? 2 : fmt & 0x100 ? 1 : 0));
        vtx.setField_OFFSET(offset);
        if (fmt & 0x200) vtx.setField_FORMAT_COMP_ALL(I::FORMAT_COMP::COMP_SIGNED);
        vtx.setField_ENDIAN_SWAP((LatteConst::VertexFetchEndianMode)endian);
        for (unsigned j = 0; j < 4; j++) vtx.setField_DST_SEL(j, (I::DST_SEL)((sel >> (24 - 8*j)) & 7));
        memcpy(g_mem + program + cf_size + i * 16, &vtx, 16);
    }
}
extern "C" hle_fn hle_gx2_fetch_lookup(const char* name) {
    if (!strcmp(name, "GX2CalcFetchShaderSizeEx")) return calc_size;
    if (!strcmp(name, "GX2InitFetchShaderEx")) return init_fetch;
    return nullptr;
}
