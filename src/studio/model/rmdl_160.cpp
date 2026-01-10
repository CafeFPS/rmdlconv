// Copyright (c) 2024, rexx
// See LICENSE.txt for licensing information (GPL v3)

#include <pch.h>
#include <studio/studio.h>
#include <studio/versions.h>
#include <studio/common.h>
#include <studio/optimize.h>

/*
	Type:    RMDL
	Version: 16
	Game:    Apex Legends Season 17

	Files: .rmdl, .vg
*/

#define FILEBUFSIZE (32 * 1024 * 1024)

#define V10_BONE_USED_BY_BONE_MERGE 0x00040000

static int TranslateBoneFlags_160(int flags)
{
	return flags & ~V10_BONE_USED_BY_BONE_MERGE;
}

#define VERTEX_HAS_UV2_FLAG 0x200000000ULL

static uint64_t ConvertMeshFlags_160(uint64_t flags)
{
	return flags & ~VERTEX_HAS_UV2_FLAG;
}

static uint32_t CalculateVertexSize_160(uint64_t flags)
{
	uint32_t size = 0;

	if (flags & 0x1) size += 12;
	if (flags & 0x2) size += 8;
	if (flags & 0x10) size += 4;
	if (flags & 0x200) size += 4;
	if (flags & 0x1000) size += 4;
	if (flags & 0x2000) size += 8;
	if (flags & 0x4000) size += 4;
	if (flags & 0x2000000) size += 8;

	return size;
}

static uint32_t GetBoneOffset_160(uint64_t flags)
{
	uint32_t offset = 0;

	if (flags & 0x1) offset += 12;
	else if (flags & 0x2) offset += 8;

	if (flags & 0x2000) offset += 8;
	if (flags & 0x4000) offset += 4;

	return offset;
}

static const uint8_t* FindBoneStateData_160(const char* rmdlData, size_t rmdlSize,
                                             uint16_t boneStateCount, uint16_t totalBones)
{
	if (boneStateCount == 0 || totalBones == 0 || rmdlSize < boneStateCount)
		return nullptr;

	// Search BACKWARDS from end of file - boneState is typically near the end
	// in the embedded VG data section. Searching backwards avoids false positives
	// from bone name tables and other data earlier in the file.
	const size_t searchStart = 0x1000;  // Don't search in header area

	for (size_t off = rmdlSize - boneStateCount; off >= searchStart; off--)
	{
		const uint8_t* p = reinterpret_cast<const uint8_t*>(rmdlData + off);

		// Check if all values are valid bone indices AND all unique
		std::set<uint8_t> uniqueValues;
		bool valid = true;

		for (uint16_t i = 0; i < boneStateCount; i++)
		{
			if (p[i] >= totalBones)
			{
				valid = false;
				break;
			}
			uniqueValues.insert(p[i]);
		}

		// Must have ALL unique values (each hardware bone maps to different model bone)
		if (valid && uniqueValues.size() == boneStateCount)
		{
			// Additional validation: the 16 bytes before boneState should look like
			// a small header structure, not part of the data. Check for:
			// - header[0] = LOD count (1-8, non-zero)
			// - header[2-3] = flags (typically 01 01)
			// - header[4-15] = zeros (padding)
			// - header[15] must be 0 (not part of bone data)
			if (off >= 16)
			{
				const uint8_t* header = p - 16;
				bool looksLikeHeader = (header[0] >= 1 && header[0] <= 8  // Valid LOD count
					&& header[4] == 0 && header[8] == 0 && header[12] == 0  // Padding zeros
					&& header[15] == 0);  // Last byte before data must be 0
				if (looksLikeHeader)
				{
					return p;
				}
			}
		}
	}

	// Fallback: search forward without header check (less reliable)
	for (size_t off = searchStart; off < rmdlSize - boneStateCount; off++)
	{
		const uint8_t* p = reinterpret_cast<const uint8_t*>(rmdlData + off);

		std::set<uint8_t> uniqueValues;
		bool valid = true;

		for (uint16_t i = 0; i < boneStateCount; i++)
		{
			if (p[i] >= totalBones)
			{
				valid = false;
				break;
			}
			uniqueValues.insert(p[i]);
		}

		if (valid && uniqueValues.size() == boneStateCount)
		{
			return p;
		}
	}

	return nullptr;
}

// Rev1 layout order (from working v10 VG analysis):
// 1. Header (224 bytes)
// 2. BoneStateChange data (1 byte per unique bone used) - FROM RMDL boneState table!
// 3. Mesh headers (72 bytes each)
// 4. Index data
// 5. Vertex data
// 6. ExtraBoneWeight data
// 7. Unknown data (0x30 bytes each)
// 8. LOD headers (8 bytes each)
// 9. LegacyWeight data (16 bytes per vertex)
// 10. Strip data (0x23 bytes each)
//
static void ConvertVGData_160(const char* vgInputBuf, uintmax_t vgInputSize, const std::string& vgOutPath,
	const r5::v160::studiohdr_t* pRmdlHdr = nullptr, const char* rmdlData = nullptr, size_t rmdlSize = 0)
{
	printf("Converting VG data (rev4 -> rev1)...\n");

	const vg::rev4::VertexGroupHeader_t* pGroupHdr = reinterpret_cast<const vg::rev4::VertexGroupHeader_t*>(vgInputBuf);

	if (pGroupHdr->lodCount == 0)
	{
		printf("WARNING: VG has 0 LODs, skipping conversion\n");
		return;
	}

	// Calculate total sizes needed for output
	size_t totalMeshCount = 0;
	size_t totalVertexCount = 0;
	size_t totalVertexBufSize = 0;
	size_t totalIndexBufSize = 0;
	size_t totalExtraWeightSize = 0;
	size_t totalStripCount = 0;

	// Collect all unique bone indices used by vertices (for boneStateChange)
	uint8_t maxBoneIndex = 0;
	std::vector<uint8_t> allBoneIndices;  // Will collect unique bones

	// First pass: count all meshes and data sizes, collect bone indices
	for (int lodIdx = 0; lodIdx < pGroupHdr->lodCount; lodIdx++)
	{
		const vg::rev4::ModelLODHeader_t* pLodHdr = pGroupHdr->pLod(lodIdx);
		if (!pLodHdr) continue;

		for (int meshIdx = 0; meshIdx < pLodHdr->meshCount; meshIdx++)
		{
			const vg::rev4::MeshHeader_t* pMesh = pLodHdr->pMesh(meshIdx);
			if (!pMesh) continue;

			totalMeshCount++;
			totalVertexCount += pMesh->vertCount;

			uint64_t v10Flags = ConvertMeshFlags_160(pMesh->flags);
			uint32_t v10VertCacheSize = CalculateVertexSize_160(v10Flags);
			totalVertexBufSize += v10VertCacheSize * pMesh->vertCount;

			totalIndexBufSize += pMesh->indexCount * sizeof(uint16_t);
			totalExtraWeightSize += pMesh->extraBoneWeightSize;

			if (pMesh->flags != 0 && pMesh->vertCount > 0)
				totalStripCount++;

			// Scan vertices for max bone index
			const char* pVerts = pMesh->pVertices();
			if (pVerts && pMesh->vertCount > 0 && (pMesh->flags & 0x1000))
			{
				for (uint32_t v = 0; v < pMesh->vertCount; v++)
				{
					const uint8_t* pBones = reinterpret_cast<const uint8_t*>(pVerts + v * pMesh->vertCacheSize + 12);
					for (int b = 0; b < 4; b++)
					{
						if (pBones[b] > maxBoneIndex)
							maxBoneIndex = pBones[b];
					}
				}
			}
		}
	}

	// Get boneStateChange from RMDL by pattern search
	// v16 header's pBoneStates() offset often points to garbage, so search for
	// a sequence of unique bytes that are valid bone indices instead.
	const uint8_t* pBoneStateData = nullptr;
	size_t boneStateChangeCount = 0;
	std::vector<uint8_t> boneStates;

	if (pRmdlHdr && pRmdlHdr->boneStateCount > 0 && rmdlData && rmdlSize > 0)
	{
		// Try pattern search first - this is more reliable than header offset
		pBoneStateData = FindBoneStateData_160(rmdlData, rmdlSize,
			pRmdlHdr->boneStateCount, pRmdlHdr->boneCount);

		if (pBoneStateData)
		{
			boneStateChangeCount = pRmdlHdr->boneStateCount;
			boneStates.assign(pBoneStateData, pBoneStateData + boneStateChangeCount);
		}
		else
		{
			// Fallback: try header offset
			pBoneStateData = pRmdlHdr->pBoneStates();
			boneStateChangeCount = pRmdlHdr->boneStateCount;

			// Validate the data
			bool validData = true;
			std::set<uint8_t> uniqueCheck;
			for (size_t i = 0; i < boneStateChangeCount && validData; i++)
			{
				if (pBoneStateData[i] >= pRmdlHdr->boneCount)
					validData = false;
				uniqueCheck.insert(pBoneStateData[i]);
			}
			validData = validData && (uniqueCheck.size() == boneStateChangeCount);

			if (validData)
				boneStates.assign(pBoneStateData, pBoneStateData + boneStateChangeCount);
			else
				pBoneStateData = nullptr;
		}
	}

	// Final fallback: sequential indices (will produce wrong animations!)
	if (boneStates.empty())
	{
		printf("  WARNING: Using sequential bone indices - animations may be broken!\n");
		boneStateChangeCount = maxBoneIndex + 1;
		for (size_t i = 0; i < boneStateChangeCount; i++)
		{
			boneStates.push_back(static_cast<uint8_t>(i));
		}
	}

	// v16 vertices already have local bone indices that map through boneStateChange

	size_t unknownCount = (pGroupHdr->lodCount > 0) ? (totalMeshCount / pGroupHdr->lodCount) : 0;
	size_t legacyWeightSize = totalVertexCount * 16;

	// Rev1 layout order (based on working v10 VG analysis):
	// 1. Header (224 bytes)
	// 2. BoneStateChange (boneStateChangeCount bytes)
	// 3. Mesh headers (72 bytes each)
	// 4. Index data (aligned)
	// 5. Vertex data (aligned)
	// 6. ExtraBoneWeight data
	// 7. Unknown data (0x30 bytes each)
	// 8. LOD headers (8 bytes each)
	// 9. LegacyWeight (16 bytes per vertex)
	// 10. Strip data (0x23 bytes each)

	// Allocate output buffer
	size_t outputBufSize = sizeof(vg::rev1::VertexGroupHeader_t)
		+ boneStateChangeCount  // boneStateChange
		+ (totalMeshCount * sizeof(vg::rev1::MeshHeader_t))
		+ totalIndexBufSize + 16  // indices + alignment
		+ totalVertexBufSize + 16 // vertices + alignment
		+ totalExtraWeightSize
		+ (unknownCount * sizeof(vg::rev1::UnkVgData_t))
		+ (pGroupHdr->lodCount * sizeof(vg::rev1::ModelLODHeader_t))
		+ legacyWeightSize  // legacyWeight
		+ (totalStripCount * sizeof(OptimizedModel::StripHeader_t))
		+ 4096; // Extra padding

	std::unique_ptr<char[]> outputBuf(new char[outputBufSize]);
	memset(outputBuf.get(), 0, outputBufSize);

	char* pWrite = outputBuf.get();

	// Write rev1 header
	vg::rev1::VertexGroupHeader_t* pOutHdr = reinterpret_cast<vg::rev1::VertexGroupHeader_t*>(pWrite);
	memset(pOutHdr, 0, sizeof(vg::rev1::VertexGroupHeader_t));
	pOutHdr->id = 'GVt0'; // '0tVG' magic
	pOutHdr->version = 1;
	pOutHdr->unk = 0;
	pOutHdr->lodCount = pGroupHdr->lodCount;
	pOutHdr->meshCount = totalMeshCount;
	pWrite += sizeof(vg::rev1::VertexGroupHeader_t);

	// BoneStateChange data (right after header!)
	char* pBoneStateChange = pWrite;
	pOutHdr->boneStateChangeOffset = pBoneStateChange - outputBuf.get();
	pOutHdr->boneStateChangeCount = boneStateChangeCount;

	memcpy(pWrite, boneStates.data(), boneStateChangeCount);
	pWrite += boneStateChangeCount;

	// Mesh headers
	char* pMeshStart = pWrite;
	pOutHdr->meshOffset = pMeshStart - outputBuf.get();
	pWrite = pMeshStart + (totalMeshCount * sizeof(vg::rev1::MeshHeader_t));

	// Align for index data
	pWrite = reinterpret_cast<char*>((reinterpret_cast<uintptr_t>(pWrite) + 15) & ~15);

	// Index data
	char* pIndexData = pWrite;
	pOutHdr->indexOffset = pIndexData - outputBuf.get();

	// Copy index data
	for (int lodIdx = 0; lodIdx < pGroupHdr->lodCount; lodIdx++)
	{
		const vg::rev4::ModelLODHeader_t* pLodHdr = pGroupHdr->pLod(lodIdx);
		if (!pLodHdr) continue;

		for (int meshIdx = 0; meshIdx < pLodHdr->meshCount; meshIdx++)
		{
			const vg::rev4::MeshHeader_t* pMesh = pLodHdr->pMesh(meshIdx);
			if (!pMesh) continue;

			const uint16_t* pSrcIndices = pMesh->pIndices();
			if (pSrcIndices && pMesh->indexCount > 0)
			{
				size_t indexSize = pMesh->indexCount * sizeof(uint16_t);
				memcpy(pWrite, pSrcIndices, indexSize);
				pWrite += indexSize;
			}
		}
	}

	// Align for vertex data
	pWrite = reinterpret_cast<char*>((reinterpret_cast<uintptr_t>(pWrite) + 15) & ~15);

	// Vertex data
	char* pVertexData = pWrite;
	pOutHdr->vertOffset = pVertexData - outputBuf.get();

	// Copy vertex data (stripping UV2 if present)
	for (int lodIdx = 0; lodIdx < pGroupHdr->lodCount; lodIdx++)
	{
		const vg::rev4::ModelLODHeader_t* pLodHdr = pGroupHdr->pLod(lodIdx);
		if (!pLodHdr) continue;

		for (int meshIdx = 0; meshIdx < pLodHdr->meshCount; meshIdx++)
		{
			const vg::rev4::MeshHeader_t* pMesh = pLodHdr->pMesh(meshIdx);
			if (!pMesh) continue;

			const char* pSrcVerts = pMesh->pVertices();
			if (pSrcVerts && pMesh->vertBufferSize > 0)
			{
				uint64_t v16Flags = pMesh->flags;
				uint64_t v10Flags = ConvertMeshFlags_160(v16Flags);
				uint32_t v16VertSize = pMesh->vertCacheSize;
				uint32_t v10VertSize = CalculateVertexSize_160(v10Flags);

				bool hasWeights = (v16Flags & 0x1000);  // VERT_BLENDINDICES
				uint32_t boneOffset = hasWeights ? GetBoneOffset_160(v16Flags) : 0;

				// If v16 has UV2 and we're stripping it, need to remove 8 bytes per vertex
				if ((v16Flags & VERTEX_HAS_UV2_FLAG) && v16VertSize > v10VertSize)
				{
					// UV2 at end of vertex - copy without it
					for (uint32_t v = 0; v < pMesh->vertCount; v++)
					{
						const char* pSrcVert = pSrcVerts + v * v16VertSize;
						memcpy(pWrite, pSrcVert, v10VertSize);
						pWrite += v10VertSize;
					}
				}
				else
				{
					// No UV2 to strip, copy vertices as-is
					for (uint32_t v = 0; v < pMesh->vertCount; v++)
					{
						const char* pSrcVert = pSrcVerts + v * v16VertSize;
						memcpy(pWrite, pSrcVert, min(v16VertSize, v10VertSize));
						pWrite += v10VertSize;
					}
				}
			}
		}
	}

	// Extra bone weight data - copy as-is (bone IDs are already local indices)
	char* pWeightData = pWrite;
	pOutHdr->extraBoneWeightOffset = pWeightData - outputBuf.get();

	for (int lodIdx = 0; lodIdx < pGroupHdr->lodCount; lodIdx++)
	{
		const vg::rev4::ModelLODHeader_t* pLodHdr = pGroupHdr->pLod(lodIdx);
		if (!pLodHdr) continue;

		for (int meshIdx = 0; meshIdx < pLodHdr->meshCount; meshIdx++)
		{
			const vg::rev4::MeshHeader_t* pMesh = pLodHdr->pMesh(meshIdx);
			if (!pMesh) continue;

			const vvw::mstudioboneweightextra_t* pSrcWeights =
				reinterpret_cast<const vvw::mstudioboneweightextra_t*>(pMesh->pBoneWeights());

			if (pSrcWeights && pMesh->extraBoneWeightSize > 0)
			{
				// Copy extra bone weights as-is (no remapping needed)
				memcpy(pWrite, pSrcWeights, pMesh->extraBoneWeightSize);
				pWrite += pMesh->extraBoneWeightSize;
			}
		}
	}

	// Unknown data
	char* pUnknownData = pWrite;
	pOutHdr->unknownOffset = pUnknownData - outputBuf.get();
	pOutHdr->unknownCount = unknownCount;
	if (unknownCount > 0)
	{
		memset(pWrite, 0, unknownCount * sizeof(vg::rev1::UnkVgData_t));
		pWrite += unknownCount * sizeof(vg::rev1::UnkVgData_t);
	}

	// LOD headers
	char* pLodStart = pWrite;
	pOutHdr->lodOffset = pLodStart - outputBuf.get();
	pWrite += pGroupHdr->lodCount * sizeof(vg::rev1::ModelLODHeader_t);

	// LegacyWeight data (16 bytes per vertex)
	char* pLegacyWeight = pWrite;
	pOutHdr->legacyWeightOffset = pLegacyWeight - outputBuf.get();
	pOutHdr->legacyWeightCount = totalVertexCount;

	// Generate default legacy weights
	for (size_t v = 0; v < totalVertexCount; v++)
	{
		float* pWeights = reinterpret_cast<float*>(pWrite);
		pWeights[0] = 1.0f;  // First bone gets full weight
		pWeights[1] = 0.0f;
		pWeights[2] = 0.0f;
		pWeights[3] = 0.0f;
		pWrite += 16;
	}

	// Strip data
	char* pStripData = pWrite;
	pOutHdr->stripOffset = pStripData - outputBuf.get();
	pOutHdr->stripCount = totalStripCount;

	size_t stripIndex = 0;
	for (int lodIdx = 0; lodIdx < pGroupHdr->lodCount; lodIdx++)
	{
		const vg::rev4::ModelLODHeader_t* pLodHdr = pGroupHdr->pLod(lodIdx);
		if (!pLodHdr) continue;

		for (int meshIdx = 0; meshIdx < pLodHdr->meshCount; meshIdx++)
		{
			const vg::rev4::MeshHeader_t* pMesh = pLodHdr->pMesh(meshIdx);
			if (!pMesh) continue;

			if (pMesh->flags != 0 && pMesh->vertCount > 0)
			{
				OptimizedModel::StripHeader_t* pStrip = reinterpret_cast<OptimizedModel::StripHeader_t*>(pWrite);
				memset(pStrip, 0, sizeof(OptimizedModel::StripHeader_t));

				pStrip->numIndices = pMesh->indexCount;
				pStrip->indexOffset = 0;
				pStrip->numVerts = pMesh->vertCount;
				pStrip->vertOffset = 0;
				pStrip->numBones = pMesh->vertBoneCount;  // Copy bone count from v16 mesh header
				pStrip->flags = OptimizedModel::STRIP_IS_TRILIST;

				pWrite += sizeof(OptimizedModel::StripHeader_t);
				stripIndex++;
			}
		}
	}

	// Now fill in LOD and mesh headers with correct offsets
	short meshStartIdx = 0;
	size_t indexOffset = 0;
	size_t vertexOffset = 0;
	size_t weightOffset = 0;
	size_t legacyWeightIdx = 0;  // Running index into legacyWeight buffer
	stripIndex = 0;

	for (int lodIdx = 0; lodIdx < pGroupHdr->lodCount; lodIdx++)
	{
		const vg::rev4::ModelLODHeader_t* pLodHdr = pGroupHdr->pLod(lodIdx);
		if (!pLodHdr) continue;

		vg::rev1::ModelLODHeader_t* pOutLod = reinterpret_cast<vg::rev1::ModelLODHeader_t*>(pLodStart) + lodIdx;
		pOutLod->meshOffset = meshStartIdx;
		pOutLod->meshCount = pLodHdr->meshCount;
		pOutLod->switchPoint = 0.0f;

		for (int meshIdx = 0; meshIdx < pLodHdr->meshCount; meshIdx++)
		{
			const vg::rev4::MeshHeader_t* pMesh = pLodHdr->pMesh(meshIdx);
			if (!pMesh) continue;

			vg::rev1::MeshHeader_t* pOutMesh = reinterpret_cast<vg::rev1::MeshHeader_t*>(pMeshStart) + meshStartIdx;
			memset(pOutMesh, 0, sizeof(vg::rev1::MeshHeader_t));

			// Convert flags (strip UV2)
			uint64_t v10Flags = ConvertMeshFlags_160(pMesh->flags);
			uint32_t v10VertCacheSize = CalculateVertexSize_160(v10Flags);

			pOutMesh->flags = v10Flags;
			pOutMesh->vertOffset = static_cast<uint32_t>(vertexOffset);
			pOutMesh->vertCacheSize = v10VertCacheSize;
			pOutMesh->vertCount = static_cast<uint32_t>(pMesh->vertCount);
			pOutMesh->indexOffset = static_cast<int>(indexOffset / sizeof(uint16_t));
			pOutMesh->indexCount = static_cast<int>(pMesh->indexCount);
			pOutMesh->extraBoneWeightOffset = static_cast<int>(weightOffset);
			pOutMesh->extraBoneWeightSize = static_cast<int>(pMesh->extraBoneWeightSize);

			// Set legacyWeight offsets for this mesh
			pOutMesh->legacyWeightOffset = static_cast<int>(legacyWeightIdx);
			pOutMesh->legacyWeightCount = static_cast<int>(pMesh->vertCount);

			// Set strip data
			if (pMesh->flags != 0 && pMesh->vertCount > 0)
			{
				pOutMesh->stripOffset = static_cast<int>(stripIndex);
				pOutMesh->stripCount = 1;
				stripIndex++;
			}

			// Update running offsets using v10 sizes
			indexOffset += pMesh->indexCount * sizeof(uint16_t);
			vertexOffset += v10VertCacheSize * pMesh->vertCount;
			weightOffset += pMesh->extraBoneWeightSize;
			legacyWeightIdx += pMesh->vertCount;

			// Update header totals
			pOutHdr->indexCount += pMesh->indexCount;
			pOutHdr->vertBufferSize += v10VertCacheSize * pMesh->vertCount;
			pOutHdr->extraBoneWeightSize += pMesh->extraBoneWeightSize;

			meshStartIdx++;
		}
	}

	pOutHdr->dataSize = static_cast<int>(pWrite - outputBuf.get());

	std::ofstream vgOut(vgOutPath, std::ios::out | std::ios::binary);
	vgOut.write(outputBuf.get(), pOutHdr->dataSize);
	vgOut.close();

	printf("VG: %d LODs, %zu meshes, %zu strips, %d bytes\n",
		pGroupHdr->lodCount, totalMeshCount, totalStripCount, pOutHdr->dataSize);
}

//
// ConvertStudioHdr_160
static void ConvertStudioHdr_160(r5::v8::studiohdr_t* out, const r5::v160::studiohdr_t* hdr, const char* pInputData)
{
	// Zero-initialize the entire header first to ensure all offsets default to 0
	memset(out, 0, sizeof(r5::v8::studiohdr_t));

	out->id = 'TSDI';
	out->version = 54;

	out->checksum = hdr->checksum;

	// Name will be handled separately with string table
	memset(out->name, 0, sizeof(out->name));

	out->length = 0xbadf00d; // needs to be written later

	// These vectors need to be read from packed format
	out->eyeposition = Vector(0, 0, 0); // Will compute from bones if available
	out->illumposition = hdr->illumposition;
	out->hull_min = hdr->hull_min;
	out->hull_max = hdr->hull_max;

	out->mins = hdr->hull_min;
	out->maxs = hdr->hull_max;

	out->view_bbmin = hdr->view_bbmin;
	out->view_bbmax = hdr->view_bbmax;

	// Copy flags with filtering for v10 compatibility
	// Clear flags that v16 sets but v10 originals don't have:
	// - STUDIOHDR_FLAGS_USES_UV2 (0x2000000) - we strip UV2 data
	// - STUDIOHDR_FLAGS_AMBIENT_BOOST (0x10000) - not in v10 models
	// - STUDIOHDR_FLAGS_SUBDIVISION_SURFACE (0x80000) - not in v10 models
	int headerFlags = hdr->flags;
	headerFlags &= ~0x2000000;  // Clear USES_UV2
	headerFlags &= ~0x10000;    // Clear AMBIENT_BOOST
	headerFlags &= ~0x80000;    // Clear SUBDIVISION_SURFACE
	out->flags = headerFlags;

	// Count vars
	out->numbones = hdr->boneCount;
	out->numbonecontrollers = 0; // Not used in v16
	out->numhitboxsets = hdr->numhitboxsets;
	out->numlocalanim = 0; // deprecated
	out->numlocalseq = hdr->numlocalseq;
	out->activitylistversion = hdr->activitylistversion;

	out->numtextures = hdr->numtextures;
	out->numcdtextures = 1; // We'll generate a single empty cdtexture
	out->numskinref = hdr->numskinref;
	out->numskinfamilies = hdr->numskinfamilies;
	out->numbodyparts = hdr->numbodyparts;
	out->numlocalattachments = hdr->numlocalattachments;

	out->numlocalnodes = hdr->numlocalnodes;
	out->numikchains = hdr->numikchains;
	out->numlocalposeparameters = hdr->numlocalposeparameters;
	out->numsrcbonetransform = hdr->numsrcbonetransform;

	out->numincludemodels = -1; // No include models

	// Misc vars
	out->mass = hdr->mass;
	out->contents = hdr->contents;

	out->defaultFadeDist = hdr->fadeDistance;
	out->flVertAnimFixedPointScale = 1.0f; // Default scale

	// Explicitly set sourceFilenameOffset to 0 (no maya strings)
	out->sourceFilenameOffset = 0;

	// VG/physics file info - will need to be computed
	out->phyOffset = -123456;  // Sentinel for external .phy file
	out->vtxOffset = 0;
	out->vvdOffset = 0;
	out->vvcOffset = 0;
	out->vvwOffset = 0;
	out->vtxSize = 0;
	out->vvdSize = 0;
	out->vvcSize = 0;
	out->vvwSize = 0;
	out->phySize = 0;  // Will be set later if .phy file exists
}

//
// GenerateRigHdr_160
static void GenerateRigHdr_160(r5::v8::studiohdr_t* out, const r5::v160::studiohdr_t* hdr)
{
	// Zero-initialize the entire header first
	memset(out, 0, sizeof(r5::v8::studiohdr_t));

	out->id = 'TSDI';
	out->version = 54;

	out->numbones = hdr->boneCount;
	out->numbonecontrollers = 0;
	out->numhitboxsets = hdr->numhitboxsets;
	out->numlocalattachments = hdr->numlocalattachments;
	out->numlocalnodes = hdr->numlocalnodes;
	out->numikchains = hdr->numikchains;
	out->numlocalposeparameters = hdr->numlocalposeparameters;

	out->mass = hdr->mass;
	out->contents = hdr->contents;

	out->defaultFadeDist = hdr->fadeDistance;
}

static void ConvertBones_160(const r5::v160::studiohdr_t* pOldHdr, const char* pOldData, int numBones, bool isRig)
{
	printf("converting %i bones...\n", numBones);

	std::vector<r5::v8::mstudiobone_t*> proceduralBones;

	char* pBoneStart = g_model.pData;
	g_model.hdrV54()->boneindex = g_model.pData - g_model.pBase;

	const r5::v160::mstudiolinearbone_t* pLinearBone = r5::v160::GetLinearBone(pOldHdr);

	// Validate linear bone data
	if (pLinearBone && pLinearBone->numbones != numBones)
		pLinearBone = nullptr;

	for (int i = 0; i < numBones; ++i)
	{
		const r5::v160::mstudiobonehdr_t* oldBoneHdr = r5::v160::GetBoneHdr(pOldHdr, i);
		const r5::v160::mstudiobonedata_t* oldBoneData = r5::v160::GetBoneData(pOldHdr, i);

		r5::v8::mstudiobone_t* newBone = reinterpret_cast<r5::v8::mstudiobone_t*>(g_model.pData) + i;

		AddToStringTable((char*)newBone, &newBone->sznameindex, oldBoneHdr->pszName());

		const char* surfaceProp = (char*)oldBoneHdr + FIX_OFFSET(oldBoneHdr->surfacepropidx);
		AddToStringTable((char*)newBone, &newBone->surfacepropidx, surfaceProp);

		newBone->parent = oldBoneData->parent;
		newBone->flags = TranslateBoneFlags_160(oldBoneData->flags);
		newBone->proctype = oldBoneData->proctype;
		newBone->procindex = oldBoneData->procindex;
		newBone->contents = oldBoneHdr->contents;
		newBone->surfacepropLookup = oldBoneHdr->surfacepropLookup;
		newBone->physicsbone = oldBoneHdr->physicsbone;

		// Convert collision index (0xFF in v16 means -1)
		newBone->collisionIndex = oldBoneData->collisionIndex == 0xFF ? -1 : oldBoneData->collisionIndex;

		// Bone controllers (not used in newer formats)
		memset(&newBone->bonecontroller, -1, sizeof(newBone->bonecontroller));

		// Pose data from linear bone arrays
		if (pLinearBone && pLinearBone->numbones > 0)
		{
			newBone->pos = *pLinearBone->pPos(i);
			newBone->quat = *pLinearBone->pQuat(i);
			newBone->rot = *pLinearBone->pRot(i);
			newBone->poseToBone = *pLinearBone->pPoseToBone(i);

			// v16 linear bone doesn't have qalignment/scale - use inline bonedata
			newBone->qAlignment = oldBoneData->qAlignment;
			newBone->scale = oldBoneData->scale;
		}
		else
		{
			// Fallback - use inline bone data transforms
			newBone->pos = oldBoneData->pos;
			newBone->quat = oldBoneData->quat;
			newBone->rot = oldBoneData->rot;
			newBone->scale = oldBoneData->scale;
			newBone->poseToBone = oldBoneData->poseToBone;
			newBone->qAlignment = oldBoneData->qAlignment;  // Read from source
		}

		// Only process JIGGLE bones (proctype == 5)
		// Other proctypes (AXISINTERP=1, QUATINTERP=2, AIMATBONE=3, AIMATATTACH=4, TWIST_MASTER=6, TWIST_SLAVE=7)
		// are not supported in v10 format
		const int STUDIO_PROC_JIGGLE = 5;
		if (oldBoneData->proctype == STUDIO_PROC_JIGGLE)
			proceduralBones.push_back(newBone);
		else if (oldBoneData->proctype > 0)
		{
			// Clear proctype for unsupported proc bone types
			newBone->proctype = 0;
			newBone->procindex = 0;
		}
	}

	g_model.pData += numBones * sizeof(r5::v8::mstudiobone_t);
	ALIGN4(g_model.pData);

	// Rigs do not have proc bones
	if (proceduralBones.empty())
		return;

	printf("copying %lld jiggle bones...\n", proceduralBones.size());

	std::map<uint8_t, uint8_t> linearprocbones;

	for (auto bone : proceduralBones)
	{
		int boneid = ((char*)bone - pBoneStart) / sizeof(r5::v8::mstudiobone_t);
		const r5::v160::mstudiobonedata_t* oldBoneData = r5::v160::GetBoneData(pOldHdr, boneid);

		void* oldJBone = (char*)oldBoneData + FIX_OFFSET(oldBoneData->procindex);

		r5::v8::mstudiojigglebone_t* jBone = reinterpret_cast<r5::v8::mstudiojigglebone_t*>(g_model.pData);

		bone->procindex = (char*)jBone - (char*)bone;

		memcpy_s(jBone, sizeof(r5::v8::mstudiojigglebone_t), oldJBone, sizeof(r5::v8::mstudiojigglebone_t));

		linearprocbones.emplace(jBone->bone, static_cast<uint8_t>(linearprocbones.size()));

		g_model.pData += sizeof(r5::v8::mstudiojigglebone_t);
	}

	ALIGN4(g_model.pData);

	if (linearprocbones.empty())
		return;

	g_model.hdrV54()->procBoneCount = static_cast<int>(linearprocbones.size());
	g_model.hdrV54()->procBoneTableOffset = static_cast<int>(g_model.pData - g_model.pBase);

	for (auto& it : linearprocbones)
	{
		*g_model.pData = it.first;
		g_model.pData += sizeof(uint8_t);
	}

	g_model.hdrV54()->linearProcBoneOffset = static_cast<int>(g_model.pData - g_model.pBase);

	for (int i = 0; i < numBones; i++)
	{
		*g_model.pData = linearprocbones.count(i) ? linearprocbones.find(i)->second : 0xff;
		g_model.pData += sizeof(uint8_t);
	}

	ALIGN4(g_model.pData);
}

static void ConvertHitboxes_160(const r5::v160::studiohdr_t* pOldHdr, const char* pOldData, int numHitboxSets)
{
	printf("converting %i hitboxsets...\n", numHitboxSets);

	g_model.hdrV54()->hitboxsetindex = static_cast<int>(g_model.pData - g_model.pBase);

	const r5::v160::mstudiohitboxset_t* pOldHitboxSets = reinterpret_cast<const r5::v160::mstudiohitboxset_t*>(
		(const char*)pOldHdr + FIX_OFFSET(pOldHdr->hitboxsetindex));

	mstudiohitboxset_t* hboxsetStart = reinterpret_cast<mstudiohitboxset_t*>(g_model.pData);

	// Write hitbox set headers
	for (int i = 0; i < numHitboxSets; ++i)
	{
		const r5::v160::mstudiohitboxset_t* oldhboxset = &pOldHitboxSets[i];
		mstudiohitboxset_t* newhboxset = reinterpret_cast<mstudiohitboxset_t*>(g_model.pData);

		AddToStringTable((char*)newhboxset, &newhboxset->sznameindex, oldhboxset->pszName());
		newhboxset->numhitboxes = oldhboxset->numhitboxes;
		newhboxset->hitboxindex = 0; // Will be set later

		g_model.pData += sizeof(mstudiohitboxset_t);
	}

	// Write hitboxes for each set
	for (int i = 0; i < numHitboxSets; ++i)
	{
		const r5::v160::mstudiohitboxset_t* oldhboxset = &pOldHitboxSets[i];
		mstudiohitboxset_t* newhboxset = hboxsetStart + i;

		newhboxset->hitboxindex = static_cast<int>(g_model.pData - (char*)newhboxset);

		for (int j = 0; j < oldhboxset->numhitboxes; ++j)
		{
			const r5::v160::mstudiobbox_t* oldHitbox = oldhboxset->pHitbox(j);
			r5::v8::mstudiobbox_t* newHitbox = reinterpret_cast<r5::v8::mstudiobbox_t*>(g_model.pData);

			newHitbox->bone = oldHitbox->bone;
			newHitbox->group = oldHitbox->group;
			newHitbox->bbmin = oldHitbox->bbmin;
			newHitbox->bbmax = oldHitbox->bbmax;

			AddToStringTable((char*)newHitbox, &newHitbox->szhitboxnameindex, oldHitbox->pszHitboxName());

			// Hit data group
			const char* hitDataGroup = (char*)oldHitbox + FIX_OFFSET(oldHitbox->hitdataGroupOffset);
			AddToStringTable((char*)newHitbox, &newHitbox->hitdataGroupOffset, hitDataGroup);

			g_model.pData += sizeof(r5::v8::mstudiobbox_t);
		}
	}

	ALIGN4(g_model.pData);
}

//
// ConvertAttachments_160
static int ConvertAttachments_160(const r5::v160::studiohdr_t* pOldHdr, const char* pOldData, int numAttachments)
{
	printf("converting %i attachments...\n", numAttachments);

	int index = static_cast<int>(g_model.pData - g_model.pBase);

	const r5::v160::mstudioattachment_t* pOldAttachments = reinterpret_cast<const r5::v160::mstudioattachment_t*>(
		(const char*)pOldHdr + FIX_OFFSET(pOldHdr->localattachmentindex));

	for (int i = 0; i < numAttachments; ++i)
	{
		const r5::v160::mstudioattachment_t* oldAttach = &pOldAttachments[i];
		r5::v8::mstudioattachment_t* newAttach = reinterpret_cast<r5::v8::mstudioattachment_t*>(g_model.pData) + i;

		AddToStringTable((char*)newAttach, &newAttach->sznameindex, oldAttach->pszName());
		newAttach->flags = oldAttach->flags;
		newAttach->localbone = oldAttach->localbone;
		memcpy(&newAttach->localmatrix, &oldAttach->local, sizeof(oldAttach->local));
	}

	g_model.pData += numAttachments * sizeof(r5::v8::mstudioattachment_t);
	ALIGN4(g_model.pData);

	return index;
}

//
// ConvertBodyParts_160
static void ConvertBodyParts_160(const r5::v160::studiohdr_t* pOldHdr, const char* pOldData, int numBodyParts)
{
	printf("converting %i bodyparts...\n", numBodyParts);

	g_model.hdrV54()->bodypartindex = static_cast<int>(g_model.pData - g_model.pBase);

	mstudiobodyparts_t* bodypartStart = reinterpret_cast<mstudiobodyparts_t*>(g_model.pData);

	// Write bodypart headers
	for (int i = 0; i < numBodyParts; ++i)
	{
		const r5::v160::mstudiobodyparts_t* oldbodypart = pOldHdr->pBodypart(i);
		mstudiobodyparts_t* newbodypart = reinterpret_cast<mstudiobodyparts_t*>(g_model.pData);

		AddToStringTable((char*)newbodypart, &newbodypart->sznameindex, oldbodypart->pszName());
		newbodypart->nummodels = oldbodypart->nummodels;
		newbodypart->base = oldbodypart->base;

		printf("  bodypart: %s\n", oldbodypart->pszName());

		g_model.pData += sizeof(mstudiobodyparts_t);
	}

	// Write models and meshes for each bodypart
	for (int i = 0; i < numBodyParts; ++i)
	{
		const r5::v160::mstudiobodyparts_t* oldbodypart = pOldHdr->pBodypart(i);
		mstudiobodyparts_t* newbodypart = bodypartStart + i;

		newbodypart->modelindex = static_cast<int>(g_model.pData - (char*)newbodypart);

		r5::v8::mstudiomodel_t* newModels = reinterpret_cast<r5::v8::mstudiomodel_t*>(g_model.pData);

		// Write model headers
		for (int j = 0; j < oldbodypart->nummodels; ++j)
		{
			const r5::v160::mstudiomodel_t* oldModel = oldbodypart->pModel(j);
			r5::v8::mstudiomodel_t* newModel = newModels + j;

			// v16 model uses unkStringOffset for name
			memset(newModel->name, 0, sizeof(newModel->name));
			const char* modelName = oldModel->pszString();
			if (modelName && *modelName)
			{
				strncpy_s(newModel->name, sizeof(newModel->name), modelName, _TRUNCATE);
			}

			newModel->type = 0; // Standard model type
			newModel->boundingradius = 0.0f; // Not stored in v16
			newModel->nummeshes = oldModel->meshCountTotal;
			newModel->meshindex = 0; // Will be set later

			// Vertex data indices will reference VG file
			newModel->numvertices = 0;
			newModel->vertexindex = 0;
			newModel->tangentsindex = 0;
			newModel->numattachments = 0;
			newModel->attachmentindex = 0;
			newModel->deprecated_numeyeballs = 0;
			newModel->deprecated_eyeballindex = 0;
			newModel->colorindex = 0;
			newModel->uv2index = 0;
		}

		g_model.pData += oldbodypart->nummodels * sizeof(r5::v8::mstudiomodel_t);

		// Write meshes for each model
		for (int j = 0; j < oldbodypart->nummodels; ++j)
		{
			const r5::v160::mstudiomodel_t* oldModel = oldbodypart->pModel(j);
			r5::v8::mstudiomodel_t* newModel = newModels + j;

			newModel->meshindex = static_cast<int>(g_model.pData - (char*)newModel);

			r5::v8::mstudiomesh_t* newMeshes = reinterpret_cast<r5::v8::mstudiomesh_t*>(g_model.pData);

			for (int k = 0; k < oldModel->meshCountTotal; ++k)
			{
				const r5::v160::mstudiomesh_t* oldMesh = oldModel->pMesh(k);
				r5::v8::mstudiomesh_t* newMesh = newMeshes + k;

				newMesh->material = oldMesh->material;
				newMesh->meshid = oldMesh->meshid;
				newMesh->center = oldMesh->center;

				// Vertex data will be filled from VG conversion
				newMesh->numvertices = 0;
				newMesh->vertexoffset = 0;
				newMesh->vertexloddata.numLODVertexes[0] = 0;

				newMesh->modelindex = static_cast<int>((char*)newModel - (char*)newMesh);
			}

			g_model.pData += oldModel->meshCountTotal * sizeof(r5::v8::mstudiomesh_t);
		}
	}

	ALIGN4(g_model.pData);
}

//
// ConvertTextures_160
static void ConvertTextures_160(const r5::v160::studiohdr_t* pOldHdr, const char* pOldData, int numTextures)
{
	printf("converting %i textures...\n", numTextures);

	g_model.hdrV54()->textureindex = static_cast<int>(g_model.pData - g_model.pBase);

	// Read original material GUIDs from v16 data
	// textureindex is absolute offset from header start (like bvhOffset)
	int textureDataOffset = FIX_OFFSET(pOldHdr->textureindex);
	const uint64_t* pOldTextureGuids = reinterpret_cast<const uint64_t*>(
		(const char*)pOldHdr + textureDataOffset);

	for (int i = 0; i < numTextures; ++i)
	{
		uint64_t materialGuid = pOldTextureGuids[i];
		r5::v8::mstudiotexture_t* newTexture = reinterpret_cast<r5::v8::mstudiotexture_t*>(g_model.pData);

		// Use default empty material name
		// snprintf(textureName, 32, "material_%016llX", materialGuid);
		char* textureName = new char[32];
		snprintf(textureName, 32, "dev/empty");
		AddToStringTable((char*)newTexture, &newTexture->sznameindex, textureName);

		// Keep original GUID - v10 can use GUID lookup
		newTexture->textureGuid = materialGuid;

		printf("  texture %d: GUID=0x%016llX\n", i, materialGuid);

		g_model.pData += sizeof(r5::v8::mstudiotexture_t);
	}

	// Material shader types - use RGDP for static props
	ALIGN4(g_model.pData);

	g_model.hdrV54()->materialtypesindex = static_cast<int>(g_model.pData - g_model.pBase);
	memset(g_model.pData, RGDP, numTextures);
	g_model.pData += numTextures;

	ALIGN4(g_model.pData);

	// Write static cdtexture data
	g_model.hdrV54()->cdtextureindex = static_cast<int>(g_model.pData - g_model.pBase);
	AddToStringTable(g_model.pBase, (int*)g_model.pData, "");
	g_model.pData += sizeof(int);
}

//
// ConvertSkins_160
// V16 uses uint16_t offsets with no alignment; V10 uses int offsets with ALIGN4
//
static void ConvertSkins_160(const r5::v160::studiohdr_t* pOldHdr, const char* pOldData, int numSkinRef, int numSkinFamilies)
{
	printf("converting %i skins (%i skinrefs)...\n", numSkinFamilies, numSkinRef);

	g_model.hdrV54()->skinindex = static_cast<int>(g_model.pData - g_model.pBase);

	const char* pOldSkinData = (const char*)pOldHdr + FIX_OFFSET(pOldHdr->skinindex);

	const int skinIndexDataSize = sizeof(short) * numSkinRef * numSkinFamilies;
	memcpy(g_model.pData, pOldSkinData, skinIndexDataSize);
	g_model.pData += skinIndexDataSize;

	ALIGN4(g_model.pData);

	// V16 stores skin name offsets as uint16_t immediately after skin data (no alignment)
	const uint16_t* pOldSkinNameOffsets = reinterpret_cast<const uint16_t*>(pOldSkinData + skinIndexDataSize);

	for (int i = 0; i < numSkinFamilies - 1; ++i)
	{
		const uint16_t nameOffset = pOldSkinNameOffsets[i];
		const char* skinName = (const char*)pOldHdr + FIX_OFFSET(nameOffset);

		if (nameOffset > 0 && skinName[0] != '\0' && strlen(skinName) < 256)
		{
			AddToStringTable(g_model.pBase, (int*)g_model.pData, skinName);
		}
		else
		{
			char skinNameBuf[32];
			sprintf_s(skinNameBuf, 32, "skin%d", i + 1);
			AddToStringTable(g_model.pBase, (int*)g_model.pData, skinNameBuf);
		}
		g_model.pData += sizeof(int);
	}

	ALIGN4(g_model.pData);
}

//
// ConvertIkChains_160
static void ConvertIkChains_160(const r5::v160::studiohdr_t* pOldHdr, const char* pOldData, int numIkChains, bool isRig)
{
	g_model.hdrV54()->ikchainindex = static_cast<int>(g_model.pData - g_model.pBase);

	if (numIkChains == 0)
		return;

	printf("converting %i ikchains...\n", numIkChains);

	const r5::v160::mstudioikchain_t* pOldChains = reinterpret_cast<const r5::v160::mstudioikchain_t*>(
		(const char*)pOldHdr + FIX_OFFSET(pOldHdr->ikchainindex));

	int currentLinkCount = 0;

	// Write chain headers
	for (int i = 0; i < numIkChains; i++)
	{
		const r5::v160::mstudioikchain_t* oldChain = &pOldChains[i];
		r5::v8::mstudioikchain_t* newChain = reinterpret_cast<r5::v8::mstudioikchain_t*>(g_model.pData);

		AddToStringTable((char*)newChain, &newChain->sznameindex, oldChain->pszName());

		newChain->linktype = oldChain->linktype;
		newChain->numlinks = oldChain->numlinks;
		newChain->linkindex = static_cast<int>((sizeof(r5::v8::mstudioiklink_t) * currentLinkCount) + (sizeof(r5::v8::mstudioikchain_t) * (numIkChains - i)));
		newChain->unk = oldChain->unk_10;

		g_model.pData += sizeof(r5::v8::mstudioikchain_t);

		currentLinkCount += oldChain->numlinks;
	}

	// Write chain links
	for (int i = 0; i < numIkChains; i++)
	{
		const r5::v160::mstudioikchain_t* oldChain = &pOldChains[i];

		for (int linkIdx = 0; linkIdx < oldChain->numlinks; linkIdx++)
		{
			const r5::v160::mstudioiklink_t* oldLink = oldChain->pLink(linkIdx);
			r5::v8::mstudioiklink_t* newLink = reinterpret_cast<r5::v8::mstudioiklink_t*>(g_model.pData);

			newLink->bone = oldLink->bone;
			newLink->kneeDir = oldLink->kneeDir;

			g_model.pData += sizeof(r5::v8::mstudioiklink_t);
		}
	}

	ALIGN4(g_model.pData);
}

//
// ConvertPoseParams_160
static int ConvertPoseParams_160(const r5::v160::studiohdr_t* pOldHdr, const char* pOldData, int numPoseParams, bool isRig)
{
	int index = static_cast<int>(g_model.pData - g_model.pBase);

	if (numPoseParams == 0)
		return index;

	printf("converting %i pose parameters...\n", numPoseParams);

	const r5::v160::mstudioposeparamdesc_t* pOldParams = reinterpret_cast<const r5::v160::mstudioposeparamdesc_t*>(
		(const char*)pOldHdr + FIX_OFFSET(pOldHdr->localposeparamindex));

	for (int i = 0; i < numPoseParams; i++)
	{
		const r5::v160::mstudioposeparamdesc_t* oldParam = &pOldParams[i];
		mstudioposeparamdesc_t* newParam = reinterpret_cast<mstudioposeparamdesc_t*>(g_model.pData);

		AddToStringTable((char*)newParam, &newParam->sznameindex, oldParam->pszName());
		newParam->flags = oldParam->flags;
		newParam->start = oldParam->start;
		newParam->end = oldParam->end;
		newParam->loop = oldParam->loop;

		g_model.pData += sizeof(mstudioposeparamdesc_t);
	}

	ALIGN4(g_model.pData);

	return index;
}

static void ConvertSequences_160(const r5::v160::studiohdr_t* pOldHdr, const char* pOldData, int numSeqs, int subversion)
{
	g_model.hdrV54()->localseqindex = static_cast<int>(g_model.pData - g_model.pBase);
	g_model.hdrV54()->numlocalseq = numSeqs;

	if (numSeqs == 0)
		return;

	// Determine sequence stride based on subversion
	// v16/v17: rseq v11 = 112 bytes, v18/v19: rseq v12 = 116 bytes
	const size_t seqStride = (subversion >= 18) ? sizeof(r5::v180::mstudioseqdesc_t) : sizeof(r5::v160::mstudioseqdesc_t);
	const char* rseqVersion = (subversion >= 18) ? "rseq v12" : "rseq v11";

	printf("converting %i sequences from v%d (%s, stride=%zu bytes)...\n", numSeqs, subversion, rseqVersion, seqStride);

	// Get sequence array base pointer
	int seqOffset = FIX_OFFSET(pOldHdr->localseqindex);
	const char* pOldSeqBase = (const char*)pOldHdr + seqOffset;

	r5::v8::mstudioseqdesc_t* newSeqBase = reinterpret_cast<r5::v8::mstudioseqdesc_t*>(g_model.pData);

	// Write sequence descriptors
	for (int i = 0; i < numSeqs; i++)
	{
		// Use manual pointer arithmetic with correct stride for the subversion
		const r5::v160::mstudioseqdesc_t* oldSeq =
			reinterpret_cast<const r5::v160::mstudioseqdesc_t*>(pOldSeqBase + (i * seqStride));
		r5::v8::mstudioseqdesc_t* newSeq = &newSeqBase[i];

		// Initialize with zeros
		memset(newSeq, 0, sizeof(r5::v8::mstudioseqdesc_t));

		// Copy label from v16
		const char* label = oldSeq->pszLabel();
		AddToStringTable((char*)newSeq, &newSeq->szlabelindex, label);
		printf("  seq %d: label='%s'\n", i, label);

		// Copy activity name if present (non-zero index)
		if (oldSeq->szactivitynameindex > 0)
		{
			const char* activityName = oldSeq->pszActivity();
			AddToStringTable((char*)newSeq, &newSeq->szactivitynameindex, activityName);
		}
		else
		{
			newSeq->szactivitynameindex = 0;
		}

		// Copy flags - preserve STUDIO_ANIM_UNK2 (0x80000) as it exists in v10/v54 format
		// The original v10 files have this flag set on "ref" sequences
		newSeq->flags = oldSeq->flags;

		// Copy activity (promote uint16_t to int, 65535 means no activity -> -1)
		newSeq->activity = (oldSeq->activity == 65535) ? -1 : static_cast<int>(oldSeq->activity);
		newSeq->actweight = static_cast<int>(oldSeq->actweight);

		// Events - v16 event data would need separate handling
		newSeq->numevents = 0;
		newSeq->eventindex = 0;

		// Copy bounding box from v16
		newSeq->bbmin = oldSeq->bbmin;
		newSeq->bbmax = oldSeq->bbmax;

		// Copy blend info
		newSeq->numblends = static_cast<int>(oldSeq->numblends);
		newSeq->groupsize[0] = oldSeq->groupsize[0];
		newSeq->groupsize[1] = oldSeq->groupsize[1];

		// Copy parameter indices and values
		newSeq->paramindex[0] = static_cast<int>(oldSeq->paramindex[0]);
		newSeq->paramindex[1] = static_cast<int>(oldSeq->paramindex[1]);
		newSeq->paramstart[0] = oldSeq->paramstart[0];
		newSeq->paramstart[1] = oldSeq->paramstart[1];
		newSeq->paramend[0] = oldSeq->paramend[0];
		newSeq->paramend[1] = oldSeq->paramend[1];
		newSeq->paramparent = 0; // v16 doesn't have this field

		// Copy fade times
		newSeq->fadeintime = oldSeq->fadeintime;
		newSeq->fadeouttime = oldSeq->fadeouttime;

		// Copy node info
		newSeq->localentrynode = static_cast<int>(oldSeq->localentrynode);
		newSeq->localexitnode = static_cast<int>(oldSeq->localexitnode);
		newSeq->nodeflags = 0; // v16 doesn't have this field

		// Default phase values (not in v16)
		newSeq->entryphase = 0.0f;
		newSeq->exitphase = 0.0f;
		newSeq->lastframe = 0.0f;
		newSeq->nextseq = 0;
		newSeq->pose = 0;

		// Copy IK/autolayer counts
		newSeq->numikrules = static_cast<int>(oldSeq->numikrules);
		newSeq->numautolayers = static_cast<int>(oldSeq->numautolayers);
		newSeq->numiklocks = static_cast<int>(oldSeq->numiklocks);

		// Copy activity modifiers
		newSeq->numactivitymodifiers = static_cast<int>(oldSeq->numactivitymodifiers);
		newSeq->ikResetMask = oldSeq->ikResetMask;

		// cycleposeindex - promote to int
		newSeq->cycleposeindex = static_cast<int>(oldSeq->cycleposeindex);

		// These will be set later when writing the actual data
		newSeq->animindexindex = 0;
		newSeq->movementindex = 0;
		newSeq->autolayerindex = 0;
		newSeq->weightlistindex = 0;
		newSeq->posekeyindex = 0;
		newSeq->iklockindex = 0;
		newSeq->activitymodifierindex = 0;
		newSeq->keyvalueindex = 0;
		newSeq->keyvaluesize = 0;
	}

	g_model.pData += numSeqs * sizeof(r5::v8::mstudioseqdesc_t);

	// Write animation data for each sequence
	for (int i = 0; i < numSeqs; i++)
	{
		// Use manual pointer arithmetic with correct stride for the subversion
		const r5::v160::mstudioseqdesc_t* oldSeq =
			reinterpret_cast<const r5::v160::mstudioseqdesc_t*>(pOldSeqBase + (i * seqStride));
		r5::v8::mstudioseqdesc_t* newSeq = &newSeqBase[i];

		// Get number of animations in blend grid
		int numAnims = oldSeq->groupsize[0] * oldSeq->groupsize[1];
		if (numAnims <= 0)
			numAnims = 1; // At least one animation

		// Write animation index array (offsets to animation descriptors)
		ALIGN4(g_model.pData);
		newSeq->animindexindex = static_cast<int>(g_model.pData - (char*)newSeq);
		int* newAnimIndices = reinterpret_cast<int*>(g_model.pData);
		g_model.pData += numAnims * sizeof(int);

		// Get v16 animation indices
		const uint16_t* v16AnimIndices = nullptr;
		if (oldSeq->animindexindex > 0)
		{
			v16AnimIndices = reinterpret_cast<const uint16_t*>((char*)oldSeq + FIX_OFFSET(oldSeq->animindexindex));
		}

		// Process each animation in the blend grid
		for (int animIdx = 0; animIdx < numAnims; animIdx++)
		{
			ALIGN4(g_model.pData);
			r5::v8::mstudioanimdesc_t* newAnim = reinterpret_cast<r5::v8::mstudioanimdesc_t*>(g_model.pData);
			newAnimIndices[animIdx] = static_cast<int>((char*)newAnim - (char*)newSeq);
			memset(newAnim, 0, sizeof(r5::v8::mstudioanimdesc_t));
			g_model.pData += sizeof(r5::v8::mstudioanimdesc_t);

			// Get v16 animation descriptor
			const r5::v160::mstudioanimdesc_t* oldAnimDesc = nullptr;
			if (v16AnimIndices && v16AnimIndices[animIdx] > 0)
			{
				oldAnimDesc = reinterpret_cast<const r5::v160::mstudioanimdesc_t*>(
					(char*)oldSeq + FIX_OFFSET(v16AnimIndices[animIdx]));
			}

			if (oldAnimDesc)
			{
				// Copy animation name
				if (oldAnimDesc->sznameindex > 0)
				{
					const char* animName = oldAnimDesc->pszName();
					AddToStringTable((char*)newAnim, &newAnim->sznameindex, animName);
				}
				else
				{
					// Use sequence label as fallback
					const char* seqLabel = oldSeq->pszLabel();
					AddToStringTable((char*)newAnim, &newAnim->sznameindex, seqLabel);
				}

				// Copy animation metadata from v16
				newAnim->fps = oldAnimDesc->fps;
				newAnim->flags = oldAnimDesc->flags;  // NOT STUDIO_ALLZEROS!
				newAnim->numframes = oldAnimDesc->numframes;
				newAnim->nummovements = 0; // Deprecated

				// Frame movement - v16 structure is incompatible, skip

				// Copy IK rules if present
				if (oldAnimDesc->numikrules > 0 && oldAnimDesc->ikruleindex > 0)
				{
					ALIGN4(g_model.pData);
					newAnim->ikruleindex = static_cast<int>(g_model.pData - (char*)newAnim);
					newAnim->numikrules = oldAnimDesc->numikrules;

					const r5::v160::mstudioikrule_t* oldIKRules =
						reinterpret_cast<const r5::v160::mstudioikrule_t*>(
							(char*)oldAnimDesc + FIX_OFFSET(oldAnimDesc->ikruleindex));

					for (int ikIdx = 0; ikIdx < oldAnimDesc->numikrules; ikIdx++)
					{
						const r5::v160::mstudioikrule_t* oldIK = &oldIKRules[ikIdx];
						r5::v8::mstudioikrule_t* newIK = reinterpret_cast<r5::v8::mstudioikrule_t*>(g_model.pData);

						// v8 IK rule structure is different - map fields
						newIK->index = ikIdx;
						newIK->type = oldIK->type;
						newIK->chain = oldIK->chain;
						newIK->bone = oldIK->bone;
						newIK->slot = oldIK->slot;
						newIK->height = oldIK->height;
						newIK->radius = oldIK->radius;
						newIK->floor = oldIK->floor;
						newIK->pos = oldIK->pos;
						newIK->q = oldIK->q;

						// Copy compressed IK error from v16
						newIK->compressedIkError.sectionFrames = oldIK->compressedikerror.sectionframes;
						for (int s = 0; s < 6; s++)
							newIK->compressedIkError.scale[s] = oldIK->compressedikerror.scale[s];

						// Expand offsets from uint16_t to int
						newIK->compressedikerrorindex = static_cast<int>(oldIK->compressedikerrorindex);
						newIK->iStart = oldIK->iStart;
						newIK->ikerrorindex = static_cast<int>(oldIK->ikerrorindex);

						// Copy timing data
						newIK->start = oldIK->start;
						newIK->peak = oldIK->peak;
						newIK->tail = oldIK->tail;
						newIK->end = oldIK->end;
						newIK->contact = oldIK->contact;
						newIK->drop = oldIK->drop;
						newIK->top = oldIK->top;
						newIK->endHeight = oldIK->endHeight;

						// Copy attachment name if present
						if (oldIK->szattachmentindex > 0)
						{
							const char* attachName = (char*)oldIK + FIX_OFFSET(oldIK->szattachmentindex);
							AddToStringTable((char*)newIK, &newIK->szattachmentindex, attachName);
						}

						g_model.pData += sizeof(r5::v8::mstudioikrule_t);
					}
				}

				// ★★★ CRITICAL: Copy RLE animation data if present ★★★
				// v16 has animindex field (int) that points to embedded animation data
				// v16 animation format (like v10) is:
				//   1. Bone flags array: 4 bits (nibble) per bone, aligned to 2 bytes
				//   2. RLE data: Only for bones with non-zero flags
				if (oldAnimDesc->animindex > 0)
				{
					ALIGN4(g_model.pData);
					newAnim->animindex = static_cast<int>(g_model.pData - (char*)newAnim);

					const char* pV16AnimData = (char*)oldAnimDesc + oldAnimDesc->animindex;
					int numBones = g_model.hdrV54()->numbones;

					// Calculate bone flags array size: 4 bits per bone, aligned to 2 bytes
					// Formula: ((4 * numBones + 7) / 8 + 1) & ~1
					int flagSize = ((4 * numBones + 7) / 8 + 1) & 0xFFFFFFFE;

					// Copy bone flags array first (this is what v10 runtime expects!)
					memcpy(g_model.pData, pV16AnimData, flagSize);
					const uint8_t* boneFlagArray = reinterpret_cast<const uint8_t*>(g_model.pData);
					g_model.pData += flagSize;

					// Now copy RLE data - ONLY for bones with non-zero flags
					// Each bone has 4 bits (nibble) of flags at boneFlagArray[boneIdx/2]
					// Lower nibble for even bones, upper nibble for odd bones
					const char* pRead = pV16AnimData + flagSize;
					char* pWriteStart = g_model.pData;

					for (int bone = 0; bone < numBones; bone++)
					{
						// Extract 4-bit flag for this bone
						uint8_t boneFlags = (boneFlagArray[bone / 2] >> (4 * (bone % 2))) & 0xF;

						// Only process bones with animation data (flags 0x1=POS, 0x2=ROT, 0x4=SCALE)
						if (boneFlags & 0x7)
						{
							const r5::mstudio_rle_anim_t* pRLE = reinterpret_cast<const r5::mstudio_rle_anim_t*>(pRead);
							int boneSize = pRLE->size;

							if (boneSize > 0 && boneSize < 4096) // Safety check
							{
								memcpy(g_model.pData, pRead, boneSize);
								pRead += boneSize;
								g_model.pData += boneSize;
							}
							else
							{
								printf("    WARNING: Invalid RLE size %d for bone %d (flags 0x%X)\n",
									boneSize, bone, boneFlags);
							}
						}
						// Note: bones without flags have NO RLE data - don't write anything
					}

					printf("    Copied %d bytes bone flags + %zd bytes RLE animation data for %d bones\n",
						flagSize, g_model.pData - pWriteStart, numBones);
				}
				else
				{
					// No embedded animation data - create minimal placeholder
					ALIGN4(g_model.pData);
					newAnim->animindex = static_cast<int>(g_model.pData - (char*)newAnim);

					// Write minimal bone flags array for STUDIO_ALLZEROS
					int numBones = g_model.hdrV54()->numbones;
					if (numBones > 0)
					{
						int flagSize = ((4 * numBones + 7) / 8 + 1) & 0xFFFFFFFE;
						memset(g_model.pData, 0, flagSize);
						g_model.pData += flagSize;
					}
				}

				// Copy animation sections if present
				if (oldAnimDesc->sectionindex > 0)
				{
					ALIGN2(g_model.pData);
					newAnim->sectionindex = static_cast<int>(g_model.pData - (char*)newAnim);
					newAnim->sectionframes = oldAnimDesc->sectionframes;

					// Calculate number of sections
					int numSections = 1;
					if (oldAnimDesc->sectionframes > 0)
					{
						int stall = oldAnimDesc->sectionstallframes;
						numSections = ((oldAnimDesc->numframes - stall - 1) / oldAnimDesc->sectionframes) + 2;
					}

					const r5::v160::mstudioanimsections_t* oldSections =
						reinterpret_cast<const r5::v160::mstudioanimsections_t*>(
							(char*)oldAnimDesc + FIX_OFFSET(oldAnimDesc->sectionindex));

					for (int s = 0; s < numSections; s++)
					{
						r5::v8::mstudioanimsections_t* newSection =
							reinterpret_cast<r5::v8::mstudioanimsections_t*>(g_model.pData);
						newSection->animindex = oldSections[s].animindex;
						g_model.pData += sizeof(r5::v8::mstudioanimsections_t);
					}
				}
			}
			else
			{
				// No v16 animation descriptor - create minimal placeholder
				const char* seqLabel = oldSeq->pszLabel();
				AddToStringTable((char*)newAnim, &newAnim->sznameindex, seqLabel);

				newAnim->fps = 30.0f;
				newAnim->flags = STUDIO_ALLZEROS;
				newAnim->numframes = 1;

				ALIGN4(g_model.pData);
				newAnim->animindex = static_cast<int>(g_model.pData - (char*)newAnim);

				int numBones = g_model.hdrV54()->numbones;
				if (numBones > 0)
				{
					int flagSize = ((4 * numBones + 7) / 8 + 1) & 0xFFFFFFFE;
					memset(g_model.pData, 0, flagSize);
					g_model.pData += flagSize;
				}
			}

			ALIGN2(g_model.pData);
		}

		// Write autolayer data if present
		if (oldSeq->numautolayers > 0 && oldSeq->autolayerindex > 0)
		{
			ALIGN4(g_model.pData);
			newSeq->autolayerindex = static_cast<int>(g_model.pData - (char*)newSeq);

			// v16 autolayer has 8-byte assetSequence prefix, v8 doesn't
			// v16: assetSequence(8) + iSequence(2) + iPose(2) + flags(4) + start(4) + peak(4) + tail(4) + end(4) = 32 bytes
			// v8:  iSequence(2) + iPose(2) + flags(4) + start(4) + peak(4) + tail(4) + end(4) = 24 bytes
			const char* pOldAutolayers = reinterpret_cast<const char*>(oldSeq) + FIX_OFFSET(oldSeq->autolayerindex);

			for (int a = 0; a < oldSeq->numautolayers; a++)
			{
				// Read v16 autolayer (32 bytes)
				const char* pOldAL = pOldAutolayers + a * 32;

				// Write v8 autolayer (24 bytes) - skip first 8 bytes (assetSequence)
				r5::v8::mstudioautolayer_t* pNewAL = reinterpret_cast<r5::v8::mstudioautolayer_t*>(g_model.pData);
				memset(pNewAL, 0, sizeof(r5::v8::mstudioautolayer_t));

				// Copy fields (skip assetSequence at offset 0)
				pNewAL->iSequence = *reinterpret_cast<const short*>(pOldAL + 8);
				pNewAL->iPose = *reinterpret_cast<const short*>(pOldAL + 10);
				pNewAL->flags = *reinterpret_cast<const int*>(pOldAL + 12);
				pNewAL->start = *reinterpret_cast<const float*>(pOldAL + 16);
				pNewAL->peak = *reinterpret_cast<const float*>(pOldAL + 20);
				pNewAL->tail = *reinterpret_cast<const float*>(pOldAL + 24);
				pNewAL->end = *reinterpret_cast<const float*>(pOldAL + 28);

				g_model.pData += sizeof(r5::v8::mstudioautolayer_t);
			}
		}

		// Write event data if present
		// v16 mstudioevent_t: 20 bytes (float cycle, int event, int type, int unk_C, uint16_t optionsindex, uint16_t szeventindex)
		// v10 mstudioevent_t: 80 bytes (float cycle, int event, int type, char options[64], int szeventindex)
		if (oldSeq->numevents > 0 && oldSeq->eventindex > 0)
		{
			ALIGN4(g_model.pData);
			newSeq->eventindex = static_cast<int>(g_model.pData - (char*)newSeq);
			newSeq->numevents = oldSeq->numevents;

			const char* pOldEvents = reinterpret_cast<const char*>(oldSeq) + FIX_OFFSET(oldSeq->eventindex);

			printf("    Converting %d events from v16 to v10...\n", oldSeq->numevents);

			for (int e = 0; e < oldSeq->numevents; e++)
			{
				// Read v16 event (20 bytes)
				const r5::v160::mstudioevent_t* pOldEvent = reinterpret_cast<const r5::v160::mstudioevent_t*>(pOldEvents + e * sizeof(r5::v160::mstudioevent_t));

				// Write v10 event (80 bytes)
				r5::v8::mstudioevent_t* pNewEvent = reinterpret_cast<r5::v8::mstudioevent_t*>(g_model.pData);
				memset(pNewEvent, 0, sizeof(r5::v8::mstudioevent_t));

				pNewEvent->cycle = pOldEvent->cycle;
				pNewEvent->event = pOldEvent->event;
				pNewEvent->type = pOldEvent->type;

				// Copy options string from v16 string table to v10 inline array
				if (pOldEvent->optionsindex > 0)
				{
					const char* optionsStr = reinterpret_cast<const char*>(pOldEvent) + FIX_OFFSET(pOldEvent->optionsindex);
					strncpy_s(pNewEvent->options, sizeof(pNewEvent->options), optionsStr, _TRUNCATE);
				}

				// Copy event name string reference
				if (pOldEvent->szeventindex > 0)
				{
					const char* eventName = reinterpret_cast<const char*>(pOldEvent) + FIX_OFFSET(pOldEvent->szeventindex);
					AddToStringTable(reinterpret_cast<char*>(pNewEvent), &pNewEvent->szeventindex, eventName);
				}

				g_model.pData += sizeof(r5::v8::mstudioevent_t);
			}
		}

		// Write weightlist data if present
		// Weightlist is an array of floats (one per bone) defining bone influence for the sequence
		if (oldSeq->weightlistindex > 0)
		{
			ALIGN4(g_model.pData);
			const size_t copyCount = g_model.hdrV54()->numbones * sizeof(float);
			const char* pOldWeightlist = reinterpret_cast<const char*>(oldSeq) + FIX_OFFSET(oldSeq->weightlistindex);

			memcpy(g_model.pData, pOldWeightlist, copyCount);
			newSeq->weightlistindex = static_cast<int>(g_model.pData - reinterpret_cast<const char*>(newSeq));
			g_model.pData += copyCount;
		}

		// Write posekey data if present
		// Posekey is an array of floats for pose parameter blending
		// Size = (groupsize[0] + groupsize[1]) * sizeof(float) - uses ADDITION not multiplication!
		// This is because posekeys define blend positions along each axis, not total animations
		if (oldSeq->posekeyindex > 0)
		{
			ALIGN4(g_model.pData);
			const int numPoseKeys = oldSeq->groupsize[0] + oldSeq->groupsize[1];
			const size_t copyCount = numPoseKeys * sizeof(float);
			const char* pOldPosekey = reinterpret_cast<const char*>(oldSeq) + FIX_OFFSET(oldSeq->posekeyindex);

			memcpy(g_model.pData, pOldPosekey, copyCount);
			newSeq->posekeyindex = static_cast<int>(g_model.pData - reinterpret_cast<const char*>(newSeq));
			g_model.pData += copyCount;
		}

		// Write iklock data if present
		// v16 mstudioiklock_t: 12 bytes (uint16_t chain, uint16_t flags, float posWeight, float localQWeight)
		// v10 mstudioiklock_t: 32 bytes (int chain, float posWeight, float localQWeight, int flags, int unused[4])
		if (oldSeq->numiklocks > 0 && oldSeq->iklockindex > 0)
		{
			ALIGN4(g_model.pData);
			newSeq->iklockindex = static_cast<int>(g_model.pData - reinterpret_cast<const char*>(newSeq));

			const char* pOldIkLocks = reinterpret_cast<const char*>(oldSeq) + FIX_OFFSET(oldSeq->iklockindex);

			for (int ik = 0; ik < oldSeq->numiklocks; ik++)
			{
				// Read v16 iklock (12 bytes)
				const r5::v160::mstudioiklock_t* pOldIkLock = reinterpret_cast<const r5::v160::mstudioiklock_t*>(pOldIkLocks + ik * sizeof(r5::v160::mstudioiklock_t));

				// Write v10 iklock (32 bytes)
				r1::mstudioiklock_t* pNewIkLock = reinterpret_cast<r1::mstudioiklock_t*>(g_model.pData);
				memset(pNewIkLock, 0, sizeof(r1::mstudioiklock_t));

				pNewIkLock->chain = static_cast<int>(pOldIkLock->chain);
				pNewIkLock->flPosWeight = pOldIkLock->flPosWeight;
				pNewIkLock->flLocalQWeight = pOldIkLock->flLocalQWeight;
				pNewIkLock->flags = static_cast<int>(pOldIkLock->flags);

				g_model.pData += sizeof(r1::mstudioiklock_t);
			}
		}

		// Write activity modifier data if present
		// v16 mstudioactivitymodifier_t: 3 bytes (uint16_t sznameindex, bool negate)
		// v10 mstudioactivitymodifier_t: 8 bytes (int sznameindex, bool negate + padding)
		if (oldSeq->numactivitymodifiers > 0 && oldSeq->activitymodifierindex > 0)
		{
			ALIGN4(g_model.pData);
			newSeq->activitymodifierindex = static_cast<int>(g_model.pData - reinterpret_cast<const char*>(newSeq));

			const char* pOldActMods = reinterpret_cast<const char*>(oldSeq) + FIX_OFFSET(oldSeq->activitymodifierindex);

			for (int am = 0; am < oldSeq->numactivitymodifiers; am++)
			{
				// Read v16 activitymodifier (3 bytes)
				const r5::v160::mstudioactivitymodifier_t* pOldActMod = reinterpret_cast<const r5::v160::mstudioactivitymodifier_t*>(pOldActMods + am * sizeof(r5::v160::mstudioactivitymodifier_t));

				// Write v10 activitymodifier (8 bytes)
				r1::mstudioactivitymodifier_t* pNewActMod = reinterpret_cast<r1::mstudioactivitymodifier_t*>(g_model.pData);
				memset(pNewActMod, 0, sizeof(r1::mstudioactivitymodifier_t));

				// Copy the name via string table
				const char* actModName = reinterpret_cast<const char*>(pOldActMod) + FIX_OFFSET(pOldActMod->sznameindex);
				AddToStringTable(reinterpret_cast<char*>(pNewActMod), &pNewActMod->sznameindex, actModName);
				pNewActMod->negate = pOldActMod->negate;

				g_model.pData += sizeof(r1::mstudioactivitymodifier_t);
				ALIGN4(g_model.pData);
			}
		}
	}

	ALIGN4(g_model.pData);
}

static void ConvertLinearBoneTable_160(const r5::v160::studiohdr_t* pOldHdr)
{
	if (pOldHdr->linearboneindex == 0 || pOldHdr->boneCount <= 1)
		return;

	const r5::v160::mstudiolinearbone_t* pOldLinear = r5::v160::GetLinearBone(pOldHdr);

	g_model.hdrV54()->linearboneindex = static_cast<int>(g_model.pData - g_model.pBase);

	// Write v10 linear bone structure
	r5::v8::mstudiolinearbone_t* pNewLinear = reinterpret_cast<r5::v8::mstudiolinearbone_t*>(g_model.pData);

	const int numBones = pOldHdr->boneCount;
	pNewLinear->numbones = numBones;

	char* pDataStart = g_model.pData;
	g_model.pData += sizeof(r5::v8::mstudiolinearbone_t);

	// Flags (translate v16 flags to v10 format)
	ALIGN4(g_model.pData);
	pNewLinear->flagsindex = static_cast<int>(g_model.pData - pDataStart);
	for (int i = 0; i < numBones; i++)
	{
		*reinterpret_cast<int*>(g_model.pData) = TranslateBoneFlags_160(pOldLinear->flags(i));
		g_model.pData += sizeof(int);
	}

	// Parent
	ALIGN4(g_model.pData);
	pNewLinear->parentindex = static_cast<int>(g_model.pData - pDataStart);
	for (int i = 0; i < numBones; i++)
	{
		*reinterpret_cast<int*>(g_model.pData) = *pOldLinear->pParent(i);
		g_model.pData += sizeof(int);
	}

	// Pos
	ALIGN4(g_model.pData);
	pNewLinear->posindex = static_cast<int>(g_model.pData - pDataStart);
	for (int i = 0; i < numBones; i++)
	{
		*reinterpret_cast<Vector*>(g_model.pData) = *pOldLinear->pPos(i);
		g_model.pData += sizeof(Vector);
	}

	// Quat - with special handling for "delta" bones
	ALIGN4(g_model.pData);
	pNewLinear->quatindex = static_cast<int>(g_model.pData - pDataStart);
	for (int i = 0; i < numBones; i++)
	{
		const r5::v160::mstudiobonehdr_t* boneHdr = r5::v160::GetBoneHdr(pOldHdr, i);
		const char* boneName = boneHdr->pszName();

		// Delta bones require special quaternion (0.5, 0.5, 0.5, 0.5)
		if (i == 0 && strstr(boneName, "delta") != nullptr)
		{
			Quaternion deltaQuat(0.5f, 0.5f, 0.5f, 0.5f);
			*reinterpret_cast<Quaternion*>(g_model.pData) = deltaQuat;
		}
		else
		{
			*reinterpret_cast<Quaternion*>(g_model.pData) = *pOldLinear->pQuat(i);
		}
		g_model.pData += sizeof(Quaternion);
	}

	// Rot
	ALIGN4(g_model.pData);
	pNewLinear->rotindex = static_cast<int>(g_model.pData - pDataStart);
	for (int i = 0; i < numBones; i++)
	{
		*reinterpret_cast<RadianEuler*>(g_model.pData) = *pOldLinear->pRot(i);
		g_model.pData += sizeof(RadianEuler);
	}

	// PoseToBone
	ALIGN4(g_model.pData);
	pNewLinear->posetoboneindex = static_cast<int>(g_model.pData - pDataStart);
	for (int i = 0; i < numBones; i++)
	{
		*reinterpret_cast<matrix3x4_t*>(g_model.pData) = *pOldLinear->pPoseToBone(i);
		g_model.pData += sizeof(matrix3x4_t);
	}

	ALIGN4(g_model.pData);
}

static void ConvertUIPanelMeshes_160(const r5::v160::studiohdr_t* const oldHeader)
{
	if (oldHeader->uiPanelCount == 0)
		return;

	printf("Converting %d UI panel meshes...\n", oldHeader->uiPanelCount);

	// Set the output header count
	g_model.hdrV54()->uiPanelCount = oldHeader->uiPanelCount;

	// Calculate source data pointer - v16 uses direct byte offsets (FIX_OFFSET is identity)
	const char* pOldBase = reinterpret_cast<const char*>(oldHeader);
	const char* pOldUIPanelData = pOldBase + oldHeader->uiPanelOffset;

	// Copy all RUI headers
	const size_t totalHeaderBufSize = oldHeader->uiPanelCount * sizeof(r5::v8::mstudiorruiheader_t);
	memcpy(g_model.pData, pOldUIPanelData, totalHeaderBufSize);

	// Set the output offset
	g_model.hdrV54()->uiPanelOffset = static_cast<int>(g_model.pData - g_model.pBase);

	r5::v8::mstudiorruiheader_t* const ruiHeaders = reinterpret_cast<r5::v8::mstudiorruiheader_t*>(g_model.pData);
	g_model.pData += totalHeaderBufSize;

	// The RUI mesh data itself must be aligned to 16 bytes
	ALIGN16(g_model.pData);

	for (int i = 0; i < oldHeader->uiPanelCount; i++)
	{
		r5::v8::mstudiorruiheader_t* ruiHeader = &ruiHeaders[i];

		// Calculate source mesh offset - relative to this header's position in the old data
		const size_t oldHeaderOffset = oldHeader->uiPanelOffset + (i * sizeof(r5::v8::mstudiorruiheader_t));
		const char* pOldMesh = pOldBase + oldHeaderOffset + ruiHeader->ruimeshindex;

		// Copy the mesh header
		const r5::v8::mstudioruimesh_t* pOldMeshHdr = reinterpret_cast<const r5::v8::mstudioruimesh_t*>(pOldMesh);
		memcpy(g_model.pData, pOldMesh, sizeof(r5::v8::mstudioruimesh_t));

		// Update the mesh index as it can be different due to header alignment
		ruiHeader->ruimeshindex = static_cast<int>(g_model.pData - reinterpret_cast<const char*>(ruiHeader));

		const r5::v8::mstudioruimesh_t* const header = reinterpret_cast<r5::v8::mstudioruimesh_t*>(g_model.pData);
		g_model.pData += sizeof(r5::v8::mstudioruimesh_t);

		// Read UI mesh name string and padding - this is within the space between
		// our cursor and the parent index
		const char* pOldMeshName = pOldMesh + sizeof(r5::v8::mstudioruimesh_t);
		memcpy(g_model.pData, pOldMeshName, header->parentindex);
		g_model.pData += header->parentindex;

		// Parents
		const size_t parentBytes = header->numparents * sizeof(short);
		const char* pOldParents = pOldMesh + sizeof(r5::v8::mstudioruimesh_t) + pOldMeshHdr->parentindex;
		memcpy(g_model.pData, pOldParents, parentBytes);
		g_model.pData += parentBytes;

		// Vertex maps
		const size_t vertMapBytes = header->numfaces * sizeof(r5::v8::mstudioruivertmap_t);
		const char* pOldVertMaps = pOldMesh + sizeof(r5::v8::mstudioruimesh_t) + pOldMeshHdr->vertmapindex;
		memcpy(g_model.pData, pOldVertMaps, vertMapBytes);
		g_model.pData += vertMapBytes;

		// Fourth vertices (unk data)
		const size_t fourthVertBytes = header->numfaces * sizeof(r5::v8::mstudioruifourthvert_t);
		const char* pOldFourthVerts = pOldMesh + sizeof(r5::v8::mstudioruimesh_t) + pOldMeshHdr->unkindex;
		memcpy(g_model.pData, pOldFourthVerts, fourthVertBytes);
		g_model.pData += fourthVertBytes;

		// Vertices
		const size_t vertBytes = header->numvertices * sizeof(r5::v8::mstudioruivert_t);
		const char* pOldVerts = pOldMesh + sizeof(r5::v8::mstudioruimesh_t) + pOldMeshHdr->vertexindex;
		memcpy(g_model.pData, pOldVerts, vertBytes);
		g_model.pData += vertBytes;

		// Faces (bounds/UV data)
		const size_t faceBytes = header->numfaces * sizeof(r5::v8::mstudioruimeshface_t);
		const char* pOldFaces = pOldMesh + sizeof(r5::v8::mstudioruimesh_t) + pOldMeshHdr->facedataindex;
		memcpy(g_model.pData, pOldFaces, faceBytes);
		g_model.pData += faceBytes;

		printf("  UI Panel %d: %d parents, %d verts, %d faces\n",
			i, header->numparents, header->numvertices, header->numfaces);
	}

	ALIGN4(g_model.pData);
	printf("UI panel conversion complete.\n");
}

static void ConvertCollisionData_V160(const r5::v160::studiohdr_t* const oldStudioHdr, const char* const pOldBVHData, const size_t fileSize)
{
	printf("Converting V16 collision to V10 format...\n");

	g_model.hdrV54()->bvhOffset = static_cast<int>(g_model.pData - g_model.pBase);

	const r5::v8::mstudiocollmodel_t* const pOldCollModel = reinterpret_cast<const r5::v8::mstudiocollmodel_t*>(pOldBVHData);
	r5::v8::mstudiocollmodel_t* const pNewCollModel = reinterpret_cast<r5::v8::mstudiocollmodel_t*>(g_model.pData);

	const int headerCount = pOldCollModel->headerCount;
	pNewCollModel->headerCount = headerCount;

	printf("  V16 collision: %d headers\n", headerCount);

	g_model.pData += sizeof(r5::v8::mstudiocollmodel_t);

	// Cast v160 headers to v120 since they're binary compatible
	const r5::v120::mstudiocollheader_t* const pOldCollHeaders = reinterpret_cast<const r5::v120::mstudiocollheader_t*>(pOldBVHData + sizeof(r5::v8::mstudiocollmodel_t));
	r5::v8::mstudiocollheader_t* const pNewCollHeaders = reinterpret_cast<r5::v8::mstudiocollheader_t*>(g_model.pData);

	g_model.pData += headerCount * sizeof(r5::v8::mstudiocollheader_t);

	// Copy collision buffers: surface props, content masks, and surface names
	{
		const char* const oldBase = (char*)pOldCollModel;
		const char* const newBase = (char*)pNewCollModel;

		const int surfacePropsSize = pOldCollModel->contentMasksIndex - pOldCollModel->surfacePropsIndex;
		const int contentMasksSize = pOldCollModel->surfaceNamesIndex - pOldCollModel->contentMasksIndex;
		const int surfaceNamesSize = pOldCollHeaders[0].surfacePropDataIndex - pOldCollModel->surfaceNamesIndex;

		pNewCollModel->surfacePropsIndex = static_cast<int>(g_model.pData - newBase);
		memcpy(g_model.pData, oldBase + pOldCollModel->surfacePropsIndex, surfacePropsSize);
		g_model.pData += surfacePropsSize;

		pNewCollModel->contentMasksIndex = static_cast<int>(g_model.pData - newBase);
		memcpy(g_model.pData, oldBase + pOldCollModel->contentMasksIndex, contentMasksSize);
		g_model.pData += contentMasksSize;

		pNewCollModel->surfaceNamesIndex = static_cast<int>(g_model.pData - newBase);
		memcpy(g_model.pData, oldBase + pOldCollModel->surfaceNamesIndex, surfaceNamesSize);
		g_model.pData += surfaceNamesSize;
	}

	// Convert surface properties (v160/v120 has extra indirection via dsurfacepropertydata_t)
	{
		const r5::v120::mstudiocollheader_t& oldHeader = pOldCollHeaders[0];
		const r5::v120::dsurfacepropertydata_t* const oldSurfPropDatas =
			reinterpret_cast<const r5::v120::dsurfacepropertydata_t*>(&pOldBVHData[oldHeader.surfacePropDataIndex]);

		const r5::v8::dsurfaceproperty_t* const pOldSurfProps =
			reinterpret_cast<const r5::v8::dsurfaceproperty_t*>(&pOldBVHData[pOldCollModel->surfacePropsIndex]);
		r5::v8::dsurfaceproperty_t* const pNewSurfProps =
			reinterpret_cast<r5::v8::dsurfaceproperty_t*>(reinterpret_cast<char*>(pNewCollModel) + pNewCollModel->surfacePropsIndex);

		// Calculate the actual number of dsurfaceproperty_t entries from the array size.
		// This is different from oldHeader.surfacePropCount which is the count of entries
		// in the dsurfacepropertydata_t array. We must iterate over ALL dsurfaceproperty_t
		// entries to properly convert their surfacePropId from an index into dsurfacepropertydata_t
		// to the actual surface property ID. Failing to convert all entries causes climbing/grappling
		// surface properties to break on converted models.
		const int actualSurfacePropCount = (pOldCollModel->contentMasksIndex - pOldCollModel->surfacePropsIndex) / sizeof(r5::v8::dsurfaceproperty_t);

		for (int i = 0; i < actualSurfacePropCount; i++)
		{
			const r5::v8::dsurfaceproperty_t& oldSurfProp = pOldSurfProps[i];
			const r5::v120::dsurfacepropertydata_t& oldSurfPropData =
				oldSurfPropDatas[oldHeader.surfacePropArrayCount * oldSurfProp.surfacePropId];

			r5::v8::dsurfaceproperty_t& newSurfProp = pNewSurfProps[i];
			newSurfProp.surfacePropId = oldSurfPropData.surfacePropId1;
		}
	}

	// Convert each collision header and copy its data
	for (int i = 0; i < headerCount; ++i)
	{
		const r5::v120::mstudiocollheader_t* const oldHeader = &pOldCollHeaders[i];
		r5::v8::mstudiocollheader_t* const newHeader = &pNewCollHeaders[i];

		newHeader->unk = oldHeader->unk;
		memcpy_s(newHeader->origin, sizeof(newHeader->origin), oldHeader->origin, sizeof(oldHeader->origin));
		newHeader->scale = oldHeader->scale;

		// Copy vertex data
		const __int64 vertSize = oldHeader->bvhLeafIndex - oldHeader->vertIndex;
		const void* const vertData = reinterpret_cast<const char*>(pOldCollModel) + oldHeader->vertIndex;

		ALIGN64(g_model.pData);
		newHeader->vertIndex = static_cast<int>(g_model.pData - reinterpret_cast<char*>(pNewCollModel));
		memcpy_s(g_model.pData, vertSize, vertData, vertSize);
		g_model.pData += vertSize;

		// Copy leaf data
		__int64 leafSize;
		if (i != headerCount - 1)
			leafSize = pOldCollHeaders[i + 1].vertIndex - oldHeader->bvhLeafIndex;
		else
			leafSize = pOldCollHeaders[0].bvhNodeIndex - oldHeader->bvhLeafIndex;

		const void* const leafData = reinterpret_cast<const char*>(pOldCollModel) + oldHeader->bvhLeafIndex;

		ALIGN64(g_model.pData);
		newHeader->bvhLeafIndex = static_cast<int>(g_model.pData - reinterpret_cast<char*>(pNewCollModel));
		memcpy_s(g_model.pData, leafSize, leafData, leafSize);
		g_model.pData += leafSize;
	}

	// Second pass: copy node data for each header
	// Nodes are stored contiguously after all vertices and leaves
	for (int i = 0; i < headerCount; ++i)
	{
		const r5::v120::mstudiocollheader_t* const oldHeader = &pOldCollHeaders[i];
		r5::v8::mstudiocollheader_t* const newHeader = &pNewCollHeaders[i];

		__int64 nodeSize;
		if (i != headerCount - 1)
		{
			nodeSize = pOldCollHeaders[i + 1].bvhNodeIndex - oldHeader->bvhNodeIndex;
		}
		else
		{
			// For last header, calculate size using collision data boundary
			// The node data ends where the collision data ends
			size_t collisionOffset = reinterpret_cast<const char*>(pOldBVHData) - reinterpret_cast<const char*>(oldStudioHdr);
			size_t maxNodeEnd = fileSize - collisionOffset - oldHeader->bvhNodeIndex;

			// Try to find the VG data offset to get exact node size
			// For now, use a safe estimate based on available space
			// The nodes for the last header end at the VG data or file end
			nodeSize = maxNodeEnd;

			// Clamp to reasonable size if needed
			if (nodeSize > 1024 * 1024)
				nodeSize = 1024 * 1024;
		}

		const void* nodeData = reinterpret_cast<const char*>(pOldCollModel) + oldHeader->bvhNodeIndex;
		ALIGN64(g_model.pData);
		newHeader->bvhNodeIndex = static_cast<int>(g_model.pData - reinterpret_cast<char*>(pNewCollModel));
		memcpy_s(g_model.pData, nodeSize, nodeData, nodeSize);
		g_model.pData += nodeSize;
	}

	size_t totalCollSize = g_model.pData - reinterpret_cast<char*>(pNewCollModel);
	printf("  Collision converted: V16 -> V10, %zu bytes written at offset 0x%X\n",
		totalCollSize, g_model.hdrV54()->bvhOffset);
}

//
// ConvertRMDL160To10
void ConvertRMDL160To10(char* pMDL, const size_t fileSize, const std::string& pathIn, const std::string& pathOut, int subversion)
{
	std::string rawModelName = std::filesystem::path(pathIn).filename().u8string();

	printf("Converting model '%s' from version 54 (subversion %d) to version 54 (subversion 10)...\n", rawModelName.c_str(), subversion);
	printf("Input file size: %zu bytes\n", fileSize);

	TIME_SCOPE(__FUNCTION__);

	const r5::v160::studiohdr_t* oldHeader = reinterpret_cast<const r5::v160::studiohdr_t*>(pMDL);

	// Debug: Print first few bytes to verify format
	printf("First 16 bytes: ");
	for (int i = 0; i < 16 && i < (int)fileSize; i++)
		printf("%02X ", (unsigned char)pMDL[i]);
	printf("\n");

	// Debug: Print header info
	printf("Header info:\n");
	printf("  flags: 0x%08X\n", oldHeader->flags);
	printf("  checksum: 0x%08X\n", oldHeader->checksum);
	printf("  boneCount: %d\n", oldHeader->boneCount);
	printf("  numhitboxsets: %d\n", oldHeader->numhitboxsets);
	printf("  numlocalseq: %d\n", oldHeader->numlocalseq);
	printf("  numbodyparts: %d\n", oldHeader->numbodyparts);
	printf("  numtextures: %d\n", oldHeader->numtextures);

	std::filesystem::path inputPath(pathIn);
	std::filesystem::path outputDir;
	std::string baseOutputPath;
	std::string rmdlPath;

	// If pathOut is different from pathIn, use it directly (batch mode)
	// Otherwise, create rmdlconv_out subfolder (legacy single-file mode)
	if (pathOut != pathIn && !pathOut.empty())
	{
		rmdlPath = pathOut;
		outputDir = std::filesystem::path(pathOut).parent_path();
		// Remove .rmdl extension for baseOutputPath
		baseOutputPath = rmdlPath.substr(0, rmdlPath.length() - 5);
		std::filesystem::create_directories(outputDir);
	}
	else
	{
		outputDir = inputPath.parent_path() / "rmdlconv_out";
		std::filesystem::create_directories(outputDir);
		baseOutputPath = (outputDir / inputPath.stem().string()).string();
		rmdlPath = baseOutputPath + ".rmdl";
	}

	printf("Output: %s\n", rmdlPath.c_str());
	std::ofstream out(rmdlPath, std::ios::out | std::ios::binary);

	// Allocate temp file buffer
	g_model.pBase = new char[FILEBUFSIZE]{};
	g_model.pData = g_model.pBase;

	// Convert mdl header
	r5::v8::studiohdr_t* pHdr = reinterpret_cast<r5::v8::studiohdr_t*>(g_model.pData);
	ConvertStudioHdr_160(pHdr, oldHeader, pMDL);
	g_model.pHdr = pHdr;
	g_model.pData += sizeof(r5::v8::studiohdr_t);

	// Init string table
	BeginStringTable();

	// v16 stores a truncated name in the inline name[33] field (max 32 chars + null)
	// Use the input filename to get the full model name since inline name is often truncated
	std::string inlineName = oldHeader->name;
	std::string originalModelName = rawModelName;

	// Remove .rmdl extension if present
	if (originalModelName.length() > 5 && originalModelName.substr(originalModelName.length() - 5) == ".rmdl")
		originalModelName = originalModelName.substr(0, originalModelName.length() - 5);

	// Debug: print what we got
	printf("Model name from filename: '%s' (inline: '%s')\n", originalModelName.c_str(), inlineName.c_str());

	std::string modelName = originalModelName;
	if (modelName.rfind("mdl/", 0) != 0)
		modelName = "mdl/" + modelName;
	if (EndsWith(modelName, ".mdl"))
	{
		modelName = modelName.substr(0, modelName.length() - 4);
		modelName += ".rmdl";
	}

	memcpy_s(&pHdr->name, 64, modelName.c_str(), min(modelName.length(), 64));
	AddToStringTable((char*)pHdr, &pHdr->sznameindex, modelName.c_str());

	// Surface prop
	const char* surfaceProp = (const char*)oldHeader + FIX_OFFSET(oldHeader->surfacepropindex);
	AddToStringTable((char*)pHdr, &pHdr->surfacepropindex, surfaceProp);
	AddToStringTable((char*)pHdr, &pHdr->unkStringOffset, "");

	// Convert bones
	ConvertBones_160(oldHeader, pMDL, oldHeader->boneCount, false);

	// Convert attachments
	g_model.hdrV54()->localattachmentindex = ConvertAttachments_160(oldHeader, pMDL, oldHeader->numlocalattachments);

	// Convert hitboxsets and hitboxes
	ConvertHitboxes_160(oldHeader, pMDL, oldHeader->numhitboxsets);

	// Copy bonebyname table
	if (oldHeader->bonetablebynameindex > 0)
	{
		const char* pOldBoneTable = (const char*)oldHeader + FIX_OFFSET(oldHeader->bonetablebynameindex);
		memcpy(g_model.pData, pOldBoneTable, oldHeader->boneCount);
		g_model.hdrV54()->bonetablebynameindex = static_cast<int>(g_model.pData - g_model.pBase);
		g_model.pData += oldHeader->boneCount;
		ALIGN4(g_model.pData);
	}

	// Convert sequences and animations
	ConvertSequences_160(oldHeader, pMDL, oldHeader->numlocalseq, subversion);

	// Convert bodyparts, models, and meshes
	ConvertBodyParts_160(oldHeader, pMDL, oldHeader->numbodyparts);

	// Convert pose parameters
	g_model.hdrV54()->localposeparamindex = ConvertPoseParams_160(oldHeader, pMDL, oldHeader->numlocalposeparameters, false);

	// Convert IK chains
	ConvertIkChains_160(oldHeader, pMDL, oldHeader->numikchains, false);

	// Convert textures
	ConvertTextures_160(oldHeader, pMDL, oldHeader->numtextures);

	// Convert skins
	ConvertSkins_160(oldHeader, pMDL, oldHeader->numskinref, oldHeader->numskinfamilies);

	// Convert UI panel meshes (RUI)
	ConvertUIPanelMeshes_160(oldHeader);

	// Write keyvalues
	std::string keyValues = "mdlkeyvalue{prop_data{base \"\"}}\n";
	strcpy_s(g_model.pData, keyValues.length() + 1, keyValues.c_str());
	pHdr->keyvalueindex = static_cast<int>(g_model.pData - g_model.pBase);
	pHdr->keyvaluesize = IALIGN2(static_cast<int>(keyValues.length() + 1));
	g_model.pData += keyValues.length() + 1;
	ALIGN4(g_model.pData);

	// Convert linear bone table
	ConvertLinearBoneTable_160(oldHeader);

	// Write string table
	g_model.pData = WriteStringTable(g_model.pData);
	ALIGN64(g_model.pData);

	// Collision conversion - v16 bvhOffset is absolute from header start
	if (oldHeader->bvhOffset > 0)
	{
		// v16 uses absolute offset for collision data
		const char* pOldCollision = reinterpret_cast<const char*>(oldHeader) +
			FIX_OFFSET(oldHeader->bvhOffset);

		printf("Converting V16 collision data...\n");
		printf("  bvhOffset: 0x%04X (absolute)\n", oldHeader->bvhOffset);

		// Validate collision data
		const r5::v8::mstudiocollmodel_t* pCollModel = reinterpret_cast<const r5::v8::mstudiocollmodel_t*>(pOldCollision);
		printf("  headerCount: %d\n", pCollModel->headerCount);

		if (pCollModel->headerCount > 0 && pCollModel->headerCount < 100)
		{
			// Valid collision data - convert it
			ConvertCollisionData_V160(oldHeader, pOldCollision, fileSize);
		}
		else
		{
			printf("  WARNING: Invalid collision headerCount (%d), skipping collision\n", pCollModel->headerCount);
			pHdr->bvhOffset = 0;
		}
	}
	else
	{
		pHdr->bvhOffset = 0;
	}

	pHdr->length = static_cast<int>(g_model.pData - g_model.pBase);

	out.write(g_model.pBase, pHdr->length);

	delete[] g_model.pBase;

	// RRIG generation disabled - not needed for converted models
	// The game loads animation data from external .rseq files via RPak
#if 0
	///////////////
	// ANIM RIGS //
	///////////////

	std::string rigName = originalModelName;
	if (rigName.rfind("animrig/", 0) != 0)
		rigName = "animrig/" + rigName;
	if (EndsWith(rigName, ".mdl"))
	{
		rigName = rigName.substr(0, rigName.length() - 4);
		rigName += ".rrig";
	}

	printf("Creating rig from model...\n");

	std::string rrigPath = baseOutputPath + ".rrig";
	std::ofstream rigOut(rrigPath, std::ios::out | std::ios::binary);

	g_model.pBase = new char[FILEBUFSIZE]{};
	g_model.pData = g_model.pBase;

	// Generate rig header
	pHdr = reinterpret_cast<r5::v8::studiohdr_t*>(g_model.pData);
	GenerateRigHdr_160(pHdr, oldHeader);
	g_model.pHdr = pHdr;
	g_model.pData += sizeof(r5::v8::studiohdr_t);

	// Reset string table for rig
	BeginStringTable();

	memcpy_s(&pHdr->name, 64, rigName.c_str(), min(rigName.length(), 64));
	AddToStringTable((char*)pHdr, &pHdr->sznameindex, rigName.c_str());
	AddToStringTable((char*)pHdr, &pHdr->surfacepropindex, surfaceProp);
	AddToStringTable((char*)pHdr, &pHdr->unkStringOffset, "");

	// Convert bones for rig
	ConvertBones_160(oldHeader, pMDL, oldHeader->boneCount, true);

	// Convert attachments for rig
	g_model.hdrV54()->localattachmentindex = ConvertAttachments_160(oldHeader, pMDL, oldHeader->numlocalattachments);

	// Convert hitboxsets for rig
	ConvertHitboxes_160(oldHeader, pMDL, oldHeader->numhitboxsets);

	// Copy bonebyname table
	if (oldHeader->bonetablebynameindex > 0)
	{
		const char* pOldBoneTable = (const char*)oldHeader + FIX_OFFSET(oldHeader->bonetablebynameindex);
		memcpy(g_model.pData, pOldBoneTable, oldHeader->boneCount);
		g_model.hdrV54()->bonetablebynameindex = static_cast<int>(g_model.pData - g_model.pBase);
		g_model.pData += oldHeader->boneCount;
		ALIGN4(g_model.pData);
	}

	// Convert pose parameters for rig
	g_model.hdrV54()->localposeparamindex = ConvertPoseParams_160(oldHeader, pMDL, oldHeader->numlocalposeparameters, true);

	// Convert IK chains for rig
	ConvertIkChains_160(oldHeader, pMDL, oldHeader->numikchains, true);
	ALIGN4(g_model.pData);

	g_model.pData = WriteStringTable(g_model.pData);

	pHdr->length = static_cast<int>(g_model.pData - g_model.pBase);

	rigOut.write(g_model.pBase, pHdr->length);

	delete[] g_model.pBase;

	g_model.stringTable.clear();
#endif

	///////////////
	// VG FILE   //
	///////////////

	// Check for VG file alongside the RMDL
	std::string vgFilePath = ChangeExtension(pathIn, "vg");
	std::string vgOutPath = baseOutputPath + ".vg";

	if (FILE_EXISTS(vgFilePath))
	{
		printf("Found VG file, attempting conversion...\n");

		uintmax_t vgInputSize = GetFileSize(vgFilePath);
		char* vgInputBuf = new char[vgInputSize];

		std::ifstream vgIfs(vgFilePath, std::ios::in | std::ios::binary);
		vgIfs.read(vgInputBuf, vgInputSize);
		vgIfs.close();

		int vgMagic = *(int*)vgInputBuf;

		if (vgMagic == 'GVt0')
		{
			// v12.1+ VG format - use existing converter
			printf("VG file is v12.1+ format (0tVG magic), converting...\n");
			ConvertVGData_12_1(vgInputBuf, vgFilePath, vgOutPath);
		}
		else if (vgMagic == '0GVt' || vgMagic == 0x47567430)
		{
			// Already v8/v9 format - copy as-is
			printf("VG file appears to be v8/v9 format, copying as-is...\n");
			std::ofstream vgOut(vgOutPath, std::ios::out | std::ios::binary);
			vgOut.write(vgInputBuf, vgInputSize);
			vgOut.close();
			delete[] vgInputBuf;
		}
		else
		{
			// Check if this is v16 rev4 format (no magic, starts with small values for lodIndex, lodCount, etc.)
			// rev4 format: first 4 bytes are lodIndex(1), lodCount(1), groupIndex(1), lodMap(1)
			const vg::rev4::VertexGroupHeader_t* pTestHdr = reinterpret_cast<const vg::rev4::VertexGroupHeader_t*>(vgInputBuf);

			// Heuristic: if lodCount is reasonable (1-8) and lodMap is non-zero, assume rev4 format
			if (pTestHdr->lodCount > 0 && pTestHdr->lodCount <= 8 && pTestHdr->lodMap != 0)
			{
				printf("VG file appears to be v16 rev4 format (no magic, detected via header structure)\n");
				ConvertVGData_160(vgInputBuf, vgInputSize, vgOutPath, oldHeader, pMDL, fileSize);
				delete[] vgInputBuf;
			}
			else
			{
				// Unknown format - try to copy anyway
				printf("WARNING: VG file has unknown magic 0x%08X, copying as-is...\n", vgMagic);
				std::ofstream vgOut(vgOutPath, std::ios::out | std::ios::binary);
				vgOut.write(vgInputBuf, vgInputSize);
				vgOut.close();
				delete[] vgInputBuf;
			}
		}
	}
	else
	{
		printf("WARNING: No VG file found at '%s'\n", vgFilePath.c_str());
		printf("         v16 VG data is typically stored in RPak files.\n");
		printf("         You may need to extract the VG data separately using Legion or similar tools.\n");
	}

	///////////////
	// PHY FILE  //
	///////////////

	// Check for PHY file alongside the RMDL and convert to v10 format
	std::string phyFilePath = ChangeExtension(pathIn, "phy");
	std::string phyOutPath = baseOutputPath + ".phy";

	if (FILE_EXISTS(phyFilePath))
	{
		printf("Found PHY file, converting to v10 format...\n");

		uintmax_t phyInputSize = GetFileSize(phyFilePath);
		char* phyInputBuf = new char[phyInputSize];

		std::ifstream phyIfs(phyFilePath, std::ios::in | std::ios::binary);
		phyIfs.read(phyInputBuf, phyInputSize);
		phyIfs.close();

		// V16 PHY format has a compact 4-byte header (same as v19.1):
		//   [0-1]: version (uint16) = 1
		//   [2-3]: keyValuesOffset (uint16) - offset to text data
		//
		// V10 PHY format has a 20-byte IVPS header:
		//   [0-3]: size (int32) = 20
		//   [4-7]: id (int32) = 1
		//   [8-11]: solidCount (int32) = 1
		//   [12-15]: checkSum (int32) = model checksum
		//   [16-19]: keyValuesOffset (int32) = v16_offset + 16
		//
		// Data after header is identical, just offset by 16 bytes

		// Read v16 header
		uint16_t v16Version = *reinterpret_cast<uint16_t*>(phyInputBuf);
		uint16_t v16KeyValuesOffset = *reinterpret_cast<uint16_t*>(phyInputBuf + 2);

		printf("  V16 PHY: version=%d, keyValuesOffset=%d\n", v16Version, v16KeyValuesOffset);

		// Create v10 IVPS header
		struct IVPSHeader {
			int32_t size;           // 20
			int32_t id;             // 1
			int32_t solidCount;     // 1
			int32_t checkSum;       // model checksum
			int32_t keyValuesOffset; // v16_offset + 16
		};

		IVPSHeader v10Header;
		v10Header.size = 20;
		v10Header.id = 1;
		v10Header.solidCount = 1;
		v10Header.checkSum = oldHeader->checksum;  // Use model's checksum
		v10Header.keyValuesOffset = v16KeyValuesOffset + 16;  // Adjust for header size difference

		printf("  V10 PHY: size=%d, id=%d, solidCount=%d, checkSum=0x%08X, keyValuesOffset=%d\n",
			v10Header.size, v10Header.id, v10Header.solidCount, v10Header.checkSum, v10Header.keyValuesOffset);

		// Calculate output size: 20-byte header + (v16 data - 4-byte v16 header)
		size_t v10PhySize = sizeof(IVPSHeader) + (phyInputSize - 4);

		std::ofstream phyOut(phyOutPath, std::ios::out | std::ios::binary);

		// Write v10 IVPS header
		phyOut.write(reinterpret_cast<char*>(&v10Header), sizeof(IVPSHeader));

		// Copy v16 data (skip the 4-byte v16 header)
		phyOut.write(phyInputBuf + 4, phyInputSize - 4);

		phyOut.close();
		delete[] phyInputBuf;

		// Update phySize in the rmdl header
		std::fstream rmdlUpdate(rmdlPath, std::ios::in | std::ios::out | std::ios::binary);
		if (rmdlUpdate.is_open())
		{
			// Seek to phySize offset in studiohdr_t and write the size
			size_t phySizeOffset = offsetof(r5::v8::studiohdr_t, phySize);
			int phySizeValue = static_cast<int>(v10PhySize);
			rmdlUpdate.seekp(phySizeOffset);
			rmdlUpdate.write(reinterpret_cast<char*>(&phySizeValue), sizeof(int));
			rmdlUpdate.close();
			printf("  PHY converted successfully (v16: %llu bytes -> v10: %zu bytes)\n",
				phyInputSize, v10PhySize);
		}
		else
		{
			printf("  PHY converted but could not update phySize in header.\n");
		}
	}

	printf("Finished converting model '%s', proceeding...\n\n", rawModelName.c_str());
}
