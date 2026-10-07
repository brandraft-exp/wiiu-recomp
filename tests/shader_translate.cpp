/* Synthetic shader programs only: constant red fragment and constant position
 * vertex exports. No game binaries or generated shaders belong in the repo. */
#include "compat.h"
#include <fstream>
void render_red(const char* fragment);
int main(int argc, char **argv) {
    assert(argc == 2);
    LatteContextRegister regs{};
    uint32* r = reinterpret_cast<uint32*>(&regs);
    r[mmCB_SHADER_MASK] = r[Latte::REGADDR::CB_TARGET_MASK] = 15;
    uint32 program[] = {0, (0x28u << 23) | (1u << 21) | (1u << 31) | 5u | (4u << 3) | (4u << 6) | (5u << 9)};
    LatteDecompilerOptions options{};
    LatteShader_CreatePSInputTable(LatteSHRC_GetPSInputTable(), r);
    LatteFetchShader *fetch = LatteShaderRecompiler_createFetchShader(0, r, nullptr, 0);
    assert(fetch->bufferGroups.empty());
    for (int vertex = 0; vertex < 2; ++vertex) {
        LatteDecompilerOutput_t output{};
        if (vertex) {
            program[0] = 60 | (1u << 13);
            LatteDecompiler_DecompileVertexShader(0, r, reinterpret_cast<uint8*>(program), sizeof program, fetch, options, &output);
        } else {
            LatteDecompiler_DecompilePixelShader(0, r, reinterpret_cast<uint8*>(program), sizeof program, options, &output);
        }
        assert(output.shader && !output.shader->hasError && output.shader->strBuf_shaderSource);
        std::ofstream out(std::string(argv[1]) + (vertex ? "/constant.vert" : "/constant.frag"));
        out << output.shader->strBuf_shaderSource->c_str();
        if (!vertex) render_red(output.shader->strBuf_shaderSource->c_str());
        delete output.shader->strBuf_shaderSource;
        delete output.shader;
    }
    delete fetch;
}
