/* SPDX-License-Identifier: MPL-2.0
 * Derived from Cemu (cemu-project/Cemu), revision
 * fced55a9dee09292587d509c30904494d4f6e8f2. See NOTICE and LICENSE-MPL-2.0.
 * Adapted to standalone host types/includes; address calculations unchanged.
 */
#include "addr.h"
#include "surface.h"
#define GX2_RESFLAG_USAGE_COLOR_BUFFER 2
#define GX2_RESFLAG_USAGE_DEPTH_BUFFER 4
#define GX2_RESFLAG_USAGE_SCAN_BUFFER 8
struct GX2Surface {
    Latte::E_DIM dim;
    uint32 width, height, depth, numLevels;
    Latte::E_GX2SURFFMT format;
    uint32 aa, resFlag, imageSize, imagePtr, mipSize, mipPtr;
    Latte::E_GX2TILEMODE tileMode;
    uint32 swizzle, alignment, pitch, mipOffset[13];
};
static_assert(sizeof(GX2Surface) == 0x74);
namespace GX2 {
	void GX2CalculateSurfaceInfo(GX2Surface* surfacePtr, uint32 level, LatteAddrLib::AddrSurfaceInfo_OUT* pSurfOut)
	{
		bool optimizeForDepthBuffer = (surfacePtr->resFlag & GX2_RESFLAG_USAGE_DEPTH_BUFFER) != 0;
		bool optimizeForScanBuffer = (surfacePtr->resFlag & GX2_RESFLAG_USAGE_SCAN_BUFFER) != 0;
		LatteAddrLib::GX2CalculateSurfaceInfo(surfacePtr->format, surfacePtr->width, surfacePtr->height, surfacePtr->depth, surfacePtr->dim, surfacePtr->tileMode, surfacePtr->aa, level, pSurfOut, optimizeForDepthBuffer, optimizeForScanBuffer);
	}

	uint32 _CalculateLevels(uint32 resolution)
	{
		uint32 x = 0x80000000;
		uint32 v = resolution;
		uint32 n = 0;
		while (!(v & x))
		{
			n++;
			if (n == 32)
				break;
			x >>= 1;
		}
		return 32 - n;
	}

	uint32 _GX2AdjustLevelCount(GX2Surface* surfacePtr)
	{
		if (surfacePtr->numLevels <= 1)
			return 1;
		uint32 levels = std::max(_CalculateLevels(surfacePtr->width), _CalculateLevels(surfacePtr->height));
		if (surfacePtr->dim == Latte::E_DIM::DIM_3D)
			levels = std::max(levels, _CalculateLevels(surfacePtr->depth));
		return levels;
	}

	void GX2CalcSurfaceSizeAndAlignment(GX2Surface* surface)
	{
		LatteAddrLib::AddrSurfaceInfo_OUT surfOut = { 0 };
		uint32 firstMipOffset = 0;
		bool changeTilemode = false;
		Latte::E_GX2TILEMODE lastTilemode = surface->tileMode;
		bool hasTileMode32 = surface->tileMode == Latte::E_GX2TILEMODE::TM_32_SPECIAL;
		if (surface->tileMode == Latte::E_GX2TILEMODE::TM_LINEAR_GENERAL || hasTileMode32)
		{
			if (surface->dim != Latte::E_DIM::DIM_1D || (surface->resFlag & GX2_RESFLAG_USAGE_DEPTH_BUFFER) != 0 || surface->aa)
			{
				if (surface->dim != Latte::E_DIM::DIM_3D || (surface->resFlag & GX2_RESFLAG_USAGE_COLOR_BUFFER) != 0)
					surface->tileMode = Latte::E_GX2TILEMODE::TM_2D_TILED_THIN1;
				else
					surface->tileMode = Latte::E_GX2TILEMODE::TM_2D_TILED_THICK;
				changeTilemode = true;
			}
			else
			{
				surface->tileMode = Latte::E_GX2TILEMODE::TM_LINEAR_ALIGNED;
			}
			lastTilemode = surface->tileMode;
		}

		if (surface->numLevels == 0)
			surface->numLevels = 1;
		surface->numLevels = std::min<uint32>(surface->numLevels, _GX2AdjustLevelCount(surface));
		surface->mipOffset[0] = 0;
		if (Latte::TM_IsMacroTiled(surface->tileMode))
			surface->swizzle = (surface->swizzle & 0xFF00FFFF) | 0xD0000;
		else
			surface->swizzle = surface->swizzle & 0xFF00FFFF;
		// FIX 32
		uint32 fix32Mode;
		if (hasTileMode32)
		{
			if (Latte::IsCompressedFormat(surface->format))
				fix32Mode = 2;
			else
				fix32Mode = 0;
		}
		else
		{
			fix32Mode = 0;
		}
		// setup levels
		uint32 prevSize = 0;
		for (uint32 level = 0; level < surface->numLevels; ++level)
		{
			GX2CalculateSurfaceInfo(surface, level, &surfOut);
			if (level)
			{
				uint32 pad = 0;
				if (Latte::TM_IsMacroTiled(lastTilemode) && !Latte::TM_IsMacroTiled(surfOut.hwTileMode))
				{
					surface->swizzle = (surface->swizzle & 0xFF00FFFF) | (level << 16);
					lastTilemode = (Latte::E_GX2TILEMODE)surfOut.hwTileMode;
					if (level > 1)
						pad = surface->swizzle & 0xFFFF;
				}
				pad += (surfOut.baseAlign - prevSize % surfOut.baseAlign) % surfOut.baseAlign;
				if (level == 1)
				{
					firstMipOffset = pad + prevSize;
				}
				else if (level > 1)
				{
					surface->mipOffset[level - 1] = pad + prevSize + surface->mipOffset[level - 2];
				}
			}
			else
			{
				if (changeTilemode)
				{
					if (surface->tileMode != (Latte::E_GX2TILEMODE)surfOut.hwTileMode)
					{
						surface->tileMode = (Latte::E_GX2TILEMODE)surfOut.hwTileMode;
						GX2CalculateSurfaceInfo(surface, 0, &surfOut);
						if (!Latte::TM_IsMacroTiled(surface->tileMode))
							surface->swizzle = surface->swizzle & 0xFF00FFFF;
						lastTilemode = surface->tileMode;
					}
					if (surface->width < (surfOut.pitchAlign << fix32Mode)
						&& surface->height < (surfOut.heightAlign << fix32Mode))
					{
						if (surface->tileMode == Latte::E_GX2TILEMODE::TM_2D_TILED_THICK)
							surface->tileMode = Latte::E_GX2TILEMODE::TM_1D_TILED_THICK;
						else
							surface->tileMode = Latte::E_GX2TILEMODE::TM_1D_TILED_THIN1;
						GX2CalculateSurfaceInfo(surface, 0, &surfOut);
						surface->swizzle = surface->swizzle & 0xFF00FFFF;
						lastTilemode = surface->tileMode;
					}
				}

				surface->imageSize = (uint32)(surfOut.surfSize);
				surface->alignment = surfOut.baseAlign;
				surface->pitch = surfOut.pitch;
			}
			prevSize = (uint32)(surfOut.surfSize);
		}
		if (surface->numLevels > 1)
			surface->mipSize = prevSize + surface->mipOffset[surface->numLevels - 2];
		else
			surface->mipSize = 0;
		surface->mipOffset[0] = firstMipOffset;
		if (surface->format == Latte::E_GX2SURFFMT::NV12_UNORM)
		{
			uint32 padding = (surface->alignment - surface->imageSize % surface->alignment) % surface->alignment;
			surface->mipOffset[0] = padding + surface->imageSize;
			surface->imageSize = surface->mipOffset[0] + ((uint32)surface->imageSize >> 1);
		}
	}

}
extern "C" int gx2_surface_layout(uint32_t words[29]) {
    GX2Surface surface;
    std::memcpy(&surface, words, sizeof surface);
    if ((uint32)surface.dim > 7 || !surface.width || !surface.height || !surface.depth ||
        surface.width > 8192 || surface.height > 8192 || surface.depth > 8192 ||
        surface.numLevels > 14 || surface.aa > 3 ||
        ((uint32)surface.tileMode > 16 && (uint32)surface.tileMode != 32) ||
        !Latte::GetFormatBits(surface.format)) return 0;
    // Cemu's linear-special path has no MSAA dimension case and produces a
    // zero-sized surface. Reject that unsupported combination at our boundary.
    if ((uint32)surface.tileMode == 16 && (uint32)surface.dim >= 6) return 0;
    GX2::GX2CalcSurfaceSizeAndAlignment(&surface);
    std::memcpy(words, &surface, sizeof surface);
    return 1;
}
extern "C" uint32_t gx2_format_bits(uint32_t format) {
    uint32 bits = Latte::GetFormatBits((Latte::E_GX2SURFFMT)format);
    return Latte::IsCompressedFormat((Latte::E_GX2SURFFMT)format) ? bits / 16 : bits;
}
extern "C" int gx2_mip_layout(const uint32_t words[29], uint32_t level, uint32_t out[8]) {
    uint32_t validated[29];
    std::memcpy(validated, words, sizeof validated);
    if (level > 13 || !gx2_surface_layout(validated)) return 0;
    GX2Surface s;
    std::memcpy(&s, words, sizeof s);
    LatteAddrLib::AddrSurfaceInfo_OUT info{};
    GX2::GX2CalculateSurfaceInfo(&s, level, &info);
    out[0] = info.pitch; out[1] = info.height; out[2] = info.depth;
    out[3] = (uint32)info.hwTileMode; out[4] = info.bpp; out[5] = info.baseAlign;
    out[6] = (uint32)info.surfSize; out[7] = info.sliceSize;
    return info.surfSize <= UINT32_MAX;
}

/* Cemu GX2_TilingAperture.cpp::retileTexture, using scalar AddrLib addressing.
 * Preserve its special 8-bit aperture coordinate permutation. */
static int surface_copy(const uint32_t words[29], uint32_t level, uint32_t slice,
                                 uint8_t *linear, uint32_t linear_size, uint8_t *tiled,
                                 uint32_t tiled_size, int write_back, bool aperture) {
    uint32_t info[8];
    if (!gx2_mip_layout(words, level, info) || words[6] || slice >= info[2]) return 0;
    unsigned bytes = info[4] / 8;
    if (!bytes || bytes > 16) return 0;
    uint32 width = std::max(words[1] >> level, 1u), height = std::max(words[2] >> level, 1u);
    if (Latte::IsCompressedFormat((Latte::E_GX2SURFFMT)words[5])) {
        width = (width + 3) / 4; height = (height + 3) / 4;
    }
    auto tile = (Latte::E_HWTILEMODE)info[3];
    // Linear movie planes already have row-major storage. Copy rows instead
    // of computing an address for each individual byte/texel. Keep the special
    // 8-bit CPU aperture permutation on the scalar path.
    if(info[3]<=1&&!(aperture&&bytes==1)){
        uint64 stride=(uint64)info[0]*bytes, row=(uint64)width*bytes;
        uint64 base=(uint64)slice*info[1]*stride;
        for(uint32 y=0;y<height;y++){
            uint64 lin=(uint64)y*stride,off=base+lin;
            if(lin+row>linear_size||off+row>tiled_size)return 0;
            if(write_back)std::memcpy(tiled+off,linear+lin,row);
            else std::memcpy(linear+lin,tiled+off,row);
        }
        return 1;
    }
    for (uint32 y = 0; y < height; ++y) for (uint32 x = 0; x < width; ++x) {
        uint64 lin = ((uint64)(aperture && bytes == 1 ? y ^ 2 : y) * info[0] + (aperture && bytes == 1 ? x ^ 8 : x)) * bytes;
        uint32 off;
        if (info[3] <= 1)
            off = LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(x,y,slice,0,info[4],info[0],info[1],info[2]);
        else if (info[3] <= 3)
            off = LatteAddrLib::ComputeSurfaceAddrFromCoordMicroTiled(x,y,slice,info[4],info[0],info[1],tile,false);
        else
            off = LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiled(x,y,slice,0,info[4],info[0],info[1],1,tile,false,
                                                                    (words[13] >> 8) & 1, (words[13] >> 9) & 3);
        if (lin + bytes > linear_size || (uint64)off + bytes > tiled_size) return 0;
        if (write_back) std::memcpy(tiled + off, linear + lin, bytes);
        else std::memcpy(linear + lin, tiled + off, bytes);
    }
    return 1;
}

extern "C" int gx2_aperture_copy(const uint32_t words[29], uint32_t level, uint32_t slice,
 uint8_t* linear, uint32_t linear_size, uint8_t* tiled, uint32_t tiled_size, int write_back) {
 return surface_copy(words, level, slice, linear, linear_size, tiled, tiled_size, write_back, true);
}
extern "C" int gx2_surface_read(const uint32_t words[29], uint32_t level, uint32_t slice,
 uint8_t* linear, uint32_t linear_size, const uint8_t* tiled, uint32_t tiled_size) {
 return surface_copy(words, level, slice, linear, linear_size, const_cast<uint8_t*>(tiled), tiled_size, 0, false);
}
