#pragma once
#include "../formats.h"
#include <array>
#include <span>
#define LATTE_GLSL_DYNAMIC_UNIFORM_BLOCK_SIZE 4096
#include <bitset>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <unordered_map>
#include <limits>
#include <type_traits>
#include <fmt/format.h>
using sint8 = int8_t;
using float32 = float;
#define cemu_assert_unimplemented() assert(false)
#define cemu_assert_suspicious() assert(false)
#define debugBreakpoint() assert(false)
#define assert_dbg() assert(false)
#define cemuLog_log(type, msg, ...) std::fprintf(stderr, "%s\n", msg)
#define cemuLog_logDebug(type, msg, ...) std::fprintf(stderr, "%s\n", msg)
#define debug_printf(...) std::fprintf(stderr, __VA_ARGS__)
inline uint32 _swapEndianU32(uint32 x) { return __builtin_bswap32(x); }
template<class T> struct reverse_range { T& v; auto begin() { return v.rbegin(); } auto end() { return v.rend(); } };
template<class T> reverse_range<T> reverse_itr(T& v) { return {v}; }
#include "registers.h"
#include "RegDefines.h"
#include "LatteConst.h"
#include "LatteShaderAssembly.h"
class RendererShader;
enum class RendererAPI { OpenGL, Vulkan, Metal };
struct StandaloneRenderer { RendererAPI GetType() const { return RendererAPI::OpenGL; } };
inline StandaloneRenderer renderer_instance;
inline StandaloneRenderer *g_renderer = &renderer_instance;
inline struct { int glVendor = 0; } LatteGPUState;
namespace ActiveSettings {
inline bool DumpShadersEnabled() { return false; }
inline bool ForceSamplerRoundToPrecision() { return false; }
inline bool ShaderPreventInfiniteLoopsEnabled() { return true; }
}
#include "StringBuf.h"
#include "LatteDecompiler.h"
#include "FetchShader.h"
#include "LatteShader.h"
#include "LatteDecompilerInternal.h"
#include "LatteDecompilerInstructions.h"
