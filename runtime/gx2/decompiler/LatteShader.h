/* SPDX-License-Identifier: MPL-2.0; Cemu LatteShader.h, see ../NOTICE. */
#pragma once
#define GPU7_PS_MAX_INPUTS	32

struct LatteShaderPSInputTable
{
	struct psImport_t
	{
		uint32 semanticId;
		bool isFlat;
		bool isNoPerspective;
	};
	psImport_t import[GPU7_PS_MAX_INPUTS]{}; // GPR is index
	sint32 count;
	uint64 key;
	uint8 paramGen;
	uint8 paramGenGPR;

	// returns semantic id of vertex shader output by index
	// the returned semanticId may not have a match in the pixel shader (use hasPSImportForSemanticId to determine matched exports)
	static sint32 getVertexShaderOutParamSemanticId(uint32* contextRegisters, sint32 index)
	{
		cemu_assert_debug(index >= 0 && index < 32);
		uint32 vsSemanticId = (contextRegisters[mmSPI_VS_OUT_ID_0 + (index / 4)] >> (8 * (index % 4))) & 0xFF;
		return (sint32)vsSemanticId;
	}

	bool hasPSImportForSemanticId(sint32 semanticId)
	{
		for (sint32 i = 0; i < this->count; i++)
		{
			if (this->import[i].semanticId == semanticId)
				return true;
		}
		return false;
	}

	psImport_t* getPSImportBySemanticId(sint32 semanticId)
	{
		if (semanticId > LATTE_ANALYZER_IMPORT_INDEX_PARAM_MAX)
			return nullptr;
		// get import based on semanticId
		sint32 psInputIndex = -1;
		for (sint32 f = 0; f < this->count; f++)
		{
			if (this->import[f].semanticId == semanticId)
				return this->import + f;
		}
		return nullptr;
	}

	sint32 getPSImportLocationBySemanticId(sint32 semanticId)
	{
		if (semanticId > LATTE_ANALYZER_IMPORT_INDEX_PARAM_MAX)
			return -1;
		// get import based on semanticId
		sint32 psInputIndex = -1;
		for (sint32 f = 0; f < this->count; f++)
		{
			if (this->import[f].semanticId == semanticId)
				return f;
		}
		return -1;
	}
};

void LatteShader_CreatePSInputTable(LatteShaderPSInputTable* psInputTable, uint32* contextRegisters);
void LatteShader_UpdatePSInputs(uint32* contextRegisters);
LatteShaderPSInputTable* LatteSHRC_GetPSInputTable();
