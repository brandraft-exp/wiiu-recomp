#pragma once
#include "decompiler/compat.h"
#include <filesystem>
namespace gpu {
void render_finish();
void render_invalidate(uint32 address,uint32 size);
void remember_surface(uint32 key, const uint32* words);
void render_clear_depth(const uint32* words,float depth,uint32 stencil,uint32 flags);
void render_clear(const uint32* words, const float* rgba);
void render_copy(const uint32* src, const uint32* dst);
void render_present(const uint32* words, uint32 target, const std::filesystem::path& directory);
void render_draw(uint32* registers, LatteFetchShader* fetch,
                 LatteDecompilerOutput_t& vertex, LatteDecompilerOutput_t* geometry,
                 LatteDecompilerOutput_t& pixel, uint32 count, uint32 base, uint32 instances, uint32 indices, uint32 indexType);
}
