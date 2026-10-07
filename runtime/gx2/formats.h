/* SPDX-License-Identifier: MPL-2.0
 * Derived from Cemu (cemu-project/Cemu), revision
 * fced55a9dee09292587d509c30904494d4f6e8f2. See NOTICE and LICENSE-MPL-2.0.
 * Adapted to standalone host types/includes; address calculations unchanged.
 */
#pragma once
#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>
using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
using sint32 = int32_t;
#define cemu_assert(x) assert(x)
#define cemu_assert_debug(x) assert(x)
#define cemu_assert_suspicious() assert(false)
#pragma once

namespace Latte
{
	// common enums
	enum class E_DIM : uint32 // shared between Latte backend and GX2 code
	{
		DIM_1D = 0,
		DIM_2D = 1,
		DIM_3D = 2,
		DIM_CUBEMAP = 3,
		DIM_1D_ARRAY = 4,
		DIM_2D_ARRAY = 5,
		DIM_2D_MSAA = 6,
		DIM_2D_ARRAY_MSAA = 7
	};

	enum class E_AAMODE : uint32 // shared between Latte backend and GX2 code
	{
		AA_1X = 0,
		AA_2X = 1,
		AA_4X = 2,
		AA_8X = 3,
	};

	enum class E_HWTILEMODE
	{
		TM_LINEAR_GENERAL = 0,  // linear (pitch must be aligned to 8?)
		TM_LINEAR_ALIGNED = 1,  // pitch must be multiple of 64 pixels?

		TM_1D_TILED_THIN1 = 2,  // no macro tiling, 8x8 micro tiles
		TM_1D_TILED_THICK = 3,  // no macro tiling, 8x8x4 micro tiles

		TM_2D_TILED_THIN1 = 4,  // 4x2
		TM_2D_TILED_THIN2 = 5,  // 2x4
		TM_2D_TILED_THIN4 = 6,  // 1x8
		TM_2D_TILED_THICK = 7,  // 4x2x1

		TM_2B_TILED_THIN1 = 8,  // 4x2
		TM_2B_TILED_THIN2 = 9,  // 2x4
		TM_2B_TILED_THIN4 = 10, // 1x8
		TM_2B_TILED_THICK = 11, // 4x2x1

		TM_3D_TILED_THIN1 = 12, // 4x2
		TM_3D_TILED_THICK = 13, // 4x2x1

		TM_3B_TILED_THIN1 = 14, // 4x2
		TM_3B_TILED_THICK = 15, // 4x2x1
	};

	enum class E_GX2TILEMODE : uint32
	{
		// same as E_TILEMODE but contains additional options with special meaning
		TM_LINEAR_GENERAL = 0,
		TM_LINEAR_ALIGNED = 1,

		// micro-tiled
		TM_1D_TILED_THIN1 = 2,
		TM_1D_TILED_THICK = 3,

		// macro-tiled
		TM_2D_TILED_THIN1 = 4,
		TM_2D_TILED_THIN2 = 5,
		TM_2D_TILED_THIN4 = 6,
		TM_2D_TILED_THICK = 7,

		TM_2B_TILED_THIN1 = 8,
		TM_2B_TILED_THIN2 = 9,
		TM_2B_TILED_THIN4 = 10,
		TM_2B_TILED_THICK = 11,

		TM_3D_TILED_THIN1 = 12,
		TM_3D_TILED_THICK = 13,

		TM_3B_TILED_THIN1 = 14,
		TM_3B_TILED_THICK = 15,

		// special
		TM_LINEAR_SPECIAL = 16,
		TM_32_SPECIAL	  = 32,
	};

	inline E_HWTILEMODE MakeHWTileMode(const E_GX2TILEMODE gx2Tilemode)
	{
		return (E_HWTILEMODE)((uint32)gx2Tilemode & 0xF);
	}

	inline E_GX2TILEMODE MakeGX2TileMode(const E_HWTILEMODE hwTilemode)
	{
		return (E_GX2TILEMODE)hwTilemode;
	}

	inline bool TM_IsMacroTiled(const E_HWTILEMODE tm)
	{
		return (uint32)tm >= 4;
	}

	inline bool TM_IsMacroTiled(const E_GX2TILEMODE tm)
	{
		return (uint32)tm >= 4 && (uint32)tm != 16;
	}

	inline bool TM_IsBankSwapped(const E_HWTILEMODE tileMode)
	{
		return
			tileMode == E_HWTILEMODE::TM_2B_TILED_THIN1 ||
			tileMode == E_HWTILEMODE::TM_2B_TILED_THIN2 ||
			tileMode == E_HWTILEMODE::TM_2B_TILED_THIN4 ||
			tileMode == E_HWTILEMODE::TM_2B_TILED_THICK ||
			tileMode == E_HWTILEMODE::TM_3B_TILED_THIN1 ||
			tileMode == E_HWTILEMODE::TM_3B_TILED_THICK;
	}

	enum class E_HWFMT : uint32 // used by both texture and memory fetch instructions
	{
		INVALID_FORMAT = 0,
		// hardware formats only
		HWFMT_8 = 0x1,
		HWFMT_4_4 = 0x2,
		HWFMT_3_3_2 = 0x3,
		HWFMT_16 = 0x5,
		HWFMT_16_FLOAT = 0x6,
		HWFMT_8_8 = 0x7,
		HWFMT_5_6_5 = 0x8,
		HWFMT_6_5_5 = 0x9,
		HWFMT_1_5_5_5 = 0xA,
		HWFMT_4_4_4_4 = 0xB,
		HWFMT_5_5_5_1 = 0xC,
		HWFMT_32 = 0xD,
		HWFMT_32_FLOAT = 0xE,
		HWFMT_16_16 = 0xF,
		HWFMT_16_16_FLOAT = 0x10,
		HWFMT_8_24 = 0x11,
		HWFMT_8_24_FLOAT = 0x12,
		HWFMT_24_8 = 0x13,
		HWFMT_24_8_FLOAT = 0x14,
		HWFMT_10_11_11 = 0x15,
		HWFMT_10_11_11_FLOAT = 0x16,
		HWFMT_11_11_10 = 0x17,
		HWFMT_11_11_10_FLOAT = 0x18,
		HWFMT_2_10_10_10 = 0x19,
		HWFMT_8_8_8_8 = 0x1A,
		HWFMT_10_10_10_2 = 0x1B,
		HWFMT_X24_8_32_FLOAT = 0x1C,
		HWFMT_32_32 = 0x1D,
		HWFMT_32_32_FLOAT = 0x1E,
		HWFMT_16_16_16_16 = 0x1F,
		HWFMT_16_16_16_16_FLOAT = 0x20,
		HWFMT_32_32_32_32 = 0x22,
		HWFMT_32_32_32_32_FLOAT = 0x23,
		HWFMT_BC1 = 0x31,
		HWFMT_BC2 = 0x32,
		HWFMT_BC3 = 0x33,
		HWFMT_BC4 = 0x34,
		HWFMT_BC5 = 0x35,

		// formats 0x36 to 0x3E return only zero on Latte
		// format 0x3F seems to be an alias for BC1?

		// formats which are not a power of two in size are vertex fetch only?
		HWFMT_8_8_8 = 0x2C,
		HWFMT_16_16_16 = 0x2D,
		HWFMT_16_16_16_FLOAT = 0x2E,
		HWFMT_32_32_32 = 0x2F,
		HWFMT_32_32_32_FLOAT = 0x30,
	};

	enum class E_GX2SURFFMT // GX2 surface format
	{
		INVALID_FORMAT = 0,
		// base hardware formats (shared with E_HWFMT)
		HWFMT_8 = 0x1,
		HWFMT_4_4 = 0x2,
		HWFMT_3_3_2 = 0x3,
		HWFMT_16 = 0x5,
		HWFMT_16_FLOAT = 0x6,
		HWFMT_8_8 = 0x7,
		HWFMT_5_6_5 = 0x8,
		HWFMT_6_5_5 = 0x9,
		HWFMT_1_5_5_5 = 0xA,
		HWFMT_4_4_4_4 = 0xB,
		HWFMT_5_5_5_1 = 0xC,
		HWFMT_32 = 0xD,
		HWFMT_32_FLOAT = 0xE,
		HWFMT_16_16 = 0xF,
		HWFMT_16_16_FLOAT = 0x10,
		HWFMT_8_24 = 0x11,
		HWFMT_8_24_FLOAT = 0x12,
		HWFMT_24_8 = 0x13,
		HWFMT_24_8_FLOAT = 0x14,
		HWFMT_10_11_11 = 0x15,
		HWFMT_10_11_11_FLOAT = 0x16,
		HWFMT_11_11_10 = 0x17,
		HWFMT_11_11_10_FLOAT = 0x18,
		HWFMT_2_10_10_10 = 0x19,
		HWFMT_8_8_8_8 = 0x1A,
		HWFMT_10_10_10_2 = 0x1B,
		HWFMT_X24_8_32_FLOAT = 0x1C,
		HWFMT_32_32 = 0x1D,
		HWFMT_32_32_FLOAT = 0x1E,
		HWFMT_16_16_16_16 = 0x1F,
		HWFMT_16_16_16_16_FLOAT = 0x20,
		HWFMT_32_32_32_32 = 0x22,
		HWFMT_32_32_32_32_FLOAT = 0x23,
		HWFMT_BC1 = 0x31,
		HWFMT_BC2 = 0x32,
		HWFMT_BC3 = 0x33,
		HWFMT_BC4 = 0x34,
		HWFMT_BC5 = 0x35,

		// GX2 extra format bits
		FMT_BIT_INT = 0x100,
		FMT_BIT_SIGNED = 0x200,
		FMT_BIT_SRGB = 0x400,
		FMT_BIT_FLOAT = 0x800,

		// GX2 formats
		R4_G4_UNORM = HWFMT_4_4,

		R5_G6_B5_UNORM = HWFMT_5_6_5,
		R5_G5_B5_A1_UNORM = HWFMT_1_5_5_5,
		R4_G4_B4_A4_UNORM = HWFMT_4_4_4_4,
		A1_B5_G5_R5_UNORM = HWFMT_5_5_5_1,

		R8_UNORM = HWFMT_8,
		R8_SNORM = (HWFMT_8 | FMT_BIT_SIGNED),
		R8_UINT = (HWFMT_8 | FMT_BIT_INT),
		R8_SINT = (HWFMT_8 | FMT_BIT_INT | FMT_BIT_SIGNED),

		R8_G8_UNORM = HWFMT_8_8,
		R8_G8_SNORM = (HWFMT_8_8 | FMT_BIT_SIGNED),
		R8_G8_UINT = (HWFMT_8_8 | FMT_BIT_INT),
		R8_G8_SINT = (HWFMT_8_8 | FMT_BIT_INT | FMT_BIT_SIGNED),

		R8_G8_B8_A8_UNORM = HWFMT_8_8_8_8,
		R8_G8_B8_A8_SNORM = (HWFMT_8_8_8_8 | FMT_BIT_SIGNED),
		R8_G8_B8_A8_UINT = (HWFMT_8_8_8_8 | FMT_BIT_INT),
		R8_G8_B8_A8_SINT = (HWFMT_8_8_8_8 | FMT_BIT_INT | FMT_BIT_SIGNED),
		R8_G8_B8_A8_SRGB = (HWFMT_8_8_8_8 | FMT_BIT_SRGB),

		R10_G10_B10_A2_UNORM = HWFMT_2_10_10_10,
		R10_G10_B10_A2_SNORM = (HWFMT_2_10_10_10 | FMT_BIT_SIGNED),
		R10_G10_B10_A2_UINT = (HWFMT_2_10_10_10 | FMT_BIT_INT),
		R10_G10_B10_A2_SINT = (HWFMT_2_10_10_10 | FMT_BIT_INT | FMT_BIT_SIGNED),
		R10_G10_B10_A2_SRGB = (HWFMT_2_10_10_10 | FMT_BIT_SRGB),

		A2_B10_G10_R10_UNORM = HWFMT_10_10_10_2,
		A2_B10_G10_R10_UINT = (HWFMT_10_10_10_2 | FMT_BIT_INT),

		R16_UNORM = HWFMT_16,
		R16_SNORM = (HWFMT_16 | FMT_BIT_SIGNED),
		R16_UINT = (HWFMT_16 | FMT_BIT_INT),
		R16_SINT = (HWFMT_16 | FMT_BIT_INT | FMT_BIT_SIGNED),
		R16_FLOAT = (HWFMT_16_FLOAT | FMT_BIT_FLOAT),

		R16_G16_UNORM = HWFMT_16_16,
		R16_G16_SNORM = (HWFMT_16_16 | FMT_BIT_SIGNED),
		R16_G16_UINT = (HWFMT_16_16 | FMT_BIT_INT),
		R16_G16_SINT = (HWFMT_16_16 | FMT_BIT_INT | FMT_BIT_SIGNED),
		R16_G16_FLOAT = (HWFMT_16_16_FLOAT | FMT_BIT_FLOAT),

		R16_G16_B16_A16_UNORM = HWFMT_16_16_16_16,
		R16_G16_B16_A16_SNORM = (HWFMT_16_16_16_16 | FMT_BIT_SIGNED),
		R16_G16_B16_A16_UINT = (HWFMT_16_16_16_16 | FMT_BIT_INT),
		R16_G16_B16_A16_SINT = (HWFMT_16_16_16_16 | FMT_BIT_INT | FMT_BIT_SIGNED),
		R16_G16_B16_A16_FLOAT = (HWFMT_16_16_16_16_FLOAT | FMT_BIT_FLOAT),


		R24_X8_UNORM = (HWFMT_8_24),
		R24_X8_FLOAT = (HWFMT_8_24 | FMT_BIT_FLOAT),
		X24_G8_UINT = (HWFMT_8_24 | FMT_BIT_INT),
		R32_X8_FLOAT = (HWFMT_X24_8_32_FLOAT | FMT_BIT_FLOAT), // R32_X8_FLOAT
		X32_G8_UINT_X24 = (HWFMT_X24_8_32_FLOAT | FMT_BIT_INT), // X32_G8_UINT

		R11_G11_B10_FLOAT = (HWFMT_10_11_11_FLOAT | FMT_BIT_FLOAT),

		// 32bit component formats do not support SNORM/UNORM (at least GX2 doesnt expose it)
		R32_UINT = (HWFMT_32 | FMT_BIT_INT),
		R32_SINT = (HWFMT_32 | FMT_BIT_INT | FMT_BIT_SIGNED),
		R32_FLOAT = (HWFMT_32_FLOAT | FMT_BIT_FLOAT),

		R32_G32_UINT = (HWFMT_32_32 | FMT_BIT_INT),
		R32_G32_SINT = (HWFMT_32_32 | FMT_BIT_INT | FMT_BIT_SIGNED),
		R32_G32_FLOAT = (HWFMT_32_32_FLOAT | FMT_BIT_FLOAT),

		R32_G32_B32_A32_UINT = (HWFMT_32_32_32_32 | FMT_BIT_INT),
		R32_G32_B32_A32_SINT = (HWFMT_32_32_32_32 | FMT_BIT_INT | FMT_BIT_SIGNED),
		R32_G32_B32_A32_FLOAT = (HWFMT_32_32_32_32_FLOAT | FMT_BIT_FLOAT),

		// depth
		D24_S8_UNORM = (HWFMT_8_24),
		D24_S8_FLOAT = (HWFMT_8_24 | FMT_BIT_FLOAT),
		D32_S8_FLOAT = (HWFMT_X24_8_32_FLOAT | FMT_BIT_FLOAT),
		D16_UNORM = HWFMT_16,
		D32_FLOAT = (HWFMT_32_FLOAT | FMT_BIT_FLOAT),

		// compressed formats
		BC1_UNORM = (HWFMT_BC1),
		BC1_SRGB = (HWFMT_BC1 | FMT_BIT_SRGB),
		BC2_UNORM = (HWFMT_BC2),
		BC2_SRGB = (HWFMT_BC2 | FMT_BIT_SRGB),
		BC3_UNORM = (HWFMT_BC3),
		BC3_SRGB = (HWFMT_BC3 | FMT_BIT_SRGB),
		BC4_UNORM = (HWFMT_BC4),
		BC4_SNORM = (HWFMT_BC4 | FMT_BIT_SIGNED),
		BC5_UNORM = (HWFMT_BC5),
		BC5_SNORM = (HWFMT_BC5 | FMT_BIT_SIGNED),

		// special
		NV12_UNORM = 0x81,
	};


	inline uint32 GetFormatBits(const Latte::E_HWFMT hwFmt)
	{
		const uint8 sBitsTable[0x40] = {
		0x00,0x08,0x08,0x00,0x00,0x10,0x10,0x10,
		0x10,0x10,0x10,0x10,0x10,0x20,0x20,0x20,
		0x20,0x20,0x00,0x20,0x00,0x00,0x20,0x00,
		0x00,0x20,0x20,0x20,0x40,0x40,0x40,0x40,
		0x40,0x00,0x80,0x80,0x00,0x00,0x00,0x10,
		0x10,0x20,0x20,0x20,0x00,0x00,0x00,0x60,
		0x60,0x40,0x80,0x80,0x40,0x80,0x00,0x00,
		0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
		};
		cemu_assert((uint32)hwFmt < 0x40);
		return sBitsTable[(uint32)hwFmt];
	}

	inline uint32 GetFormatBits(const Latte::E_GX2SURFFMT gx2Fmt)
	{
		return GetFormatBits((Latte::E_HWFMT)((uint32)gx2Fmt & 0x3F));
	}

	inline E_HWFMT GetHWFormat(E_GX2SURFFMT format)
	{
		return (E_HWFMT)((uint32)format & 0x3F);
	}

	inline bool IsCompressedFormat(Latte::E_HWFMT format)
	{
		return (uint32)format >= 0x31 && (uint32)format <= 0x35;
	}

	inline bool IsCompressedFormat(Latte::E_GX2SURFFMT format)
	{
		return IsCompressedFormat((Latte::E_HWFMT)((uint32)format & 0x3F));
	}

	inline bool IsMSAA(Latte::E_DIM dim)
	{
		return dim == E_DIM::DIM_2D_MSAA || dim == E_DIM::DIM_2D_ARRAY_MSAA;
	}

}
