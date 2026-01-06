// Copyright (c) 2023, rexx
// See LICENSE.txt for licensing information (GPL v3)

#pragma once

enum MdlVersion : int
{
	GARRYSMOD   = 48,
	PORTAL2     = 49,
	TITANFALL   = 52,
	TITANFALL2  = 53,
	APEXLEGENDS = 54
};

enum class eRMdlSubVersion : char
{
	VERSION_UNK = -1,
	VERSION_8,
	VERSION_9,
	VERSION_10,
	VERSION_11,
	VERSION_12,
	VERSION_12_1,
	VERSION_12_2,
	VERSION_13,
	VERSION_14,
	VERSION_15,
	VERSION_16,
	VERSION_17,
	VERSION_18,
	VERSION_19,
	VERSION_19_1
};

// conversion to rmdl v10 (studio version 54)
void ConvertMDL48To54(char* pMDL, const std::string& pathIn, const std::string& pathOut);
void ConvertMDL49To54(char* pMDL, const std::string& pathIn, const std::string& pathOut);
void ConvertMDL53To54(char* pMDL, const std::string& pathIn, const std::string& pathOut);
void ConvertRMDL8To10(char* pMDL, const std::string& pathIn, const std::string& pathOut);

void ConvertRMDL120To10(char* pMDL, const size_t fileSize, const std::string& pathIn, const std::string& pathOut);
void ConvertRMDL121To10(char* pMDL, const std::string& pathIn, const std::string& pathOut);
void ConvertRMDL122To10(char* pMDL, const std::string& pathIn, const std::string& pathOut);
// v12.3 uses same structure as v12.2 (only animation format changed) - use ConvertRMDL122To10
void ConvertRMDL124To10(char* pMDL, const std::string& pathIn, const std::string& pathOut);
void ConvertRMDL125To10(char* pMDL, const std::string& pathIn, const std::string& pathOut);
void ConvertRMDL160To10(char* pMDL, const size_t fileSize, const std::string& pathIn, const std::string& pathOut, int subversion = 16);
void ConvertRMDL191To10(char* pMDL, const size_t fileSize, const std::string& pathIn, const std::string& pathOut);

// conversion to mdl v53
void ConvertMDL52To53(char* pMDL, const std::string& pathIn, const std::string& pathOut);

// VG
void ConvertVGData_12_1(char* inputBuf, const std::string& filePath, const std::string& pathOut);
void ConvertVGData_Rev3(char* inputBuf, const std::string& filePath, const std::string& pathOut);

// RMDL v14/v15 conversion
void ConvertRMDL140To10(char* pMDL, const std::string& pathIn, const std::string& pathOut);
void ConvertRMDL150To10(char* pMDL, const std::string& pathIn, const std::string& pathOut);

// deprecated
//void CreateVGFile_v8(const std::string& filePath);


// rseq conversion
void ConvertRSEQFrom71To7(char* inputBuf, char* inputExternalBuf, const std::string& filePath);
void ConvertRSEQFrom10To7(char* inputBuf, char* inputExternalBuf, const std::string& filePath);

// model conversion handlers
void UpgradeStudioModelTo53(std::string& modelPath, const char* outputDir);
void UpgradeStudioModelTo54(std::string& modelPath, const char* outputDir);
void UpgradeStudioModel(std::string& modelPath, int targetVersion, const char* outputDir);