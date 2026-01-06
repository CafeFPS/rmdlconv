// Copyright (c) 2022, rexx
// See LICENSE.txt for licensing information (GPL v3)

#include <pch.h>
#include <algorithm>
#include <core/CommandLine.h>
#include <studio/versions.h>
#include <core/utils.h>

const char* pszVersionHelpString = {
	"Please input the version of your model:\n"
	"-- OLD --\n"
	"8:    s0,1\n"
	"9:    s2\n"
	"10:   s3,4\n"
	"11:   s5\n"
	"12:   s6\n"
	"-- NEW --\n"
	"12.1: s7,8\n"
	"12.2: s9,10,11\n"
	"13:   s12\n"
	"14:   s13.1\n"
	"14.1: s14\n"
	"15:   s15\n"
	"16:   s16,17\n"
	"17:   s18\n"
	"18:   s18.1\n"
	"19:   s19\n"
	"19.1: s19.1+ (Season 19+)\n"
	"> "
};

const char* pszRSeqVersionHelpString = {
	"Please input the version of your sequence : \n"
	"7:    s0,1,3,4,5,6\n"
	"7.1:  s7,8\n"
	"10:   s9,10,11,12,13,14\n"
	"11:   s15\n"
	"> "
};

// Forward declaration
static std::unique_ptr<char[]> ReadFileToBuffer(const std::string& path, uintmax_t& outSize);

// Legacy handling for MDL and rseq files (non-RMDL)
void LegacyConversionHandling(CommandLine& cmdline)
{
	if (cmdline.argc > 2)
		return;

	if (!FILE_EXISTS(cmdline.argv[1]))
		Error("couldn't find input file\n");

	std::string filePath(cmdline.argv[1]);

	BinaryIO fileIn;
	fileIn.open(filePath, BinaryIOMode::Read);

	int magic = fileIn.read<int>();

	// Handle MDL files with 'IDST' magic
	if (magic == 'TSDI')
	{
		int mdlVersion = fileIn.read<int>();
		uintmax_t fileSize = 0;
		auto buf = ReadFileToBuffer(filePath, fileSize);
		if (!buf)
			Error("Failed to read file\n");

		switch (mdlVersion)
		{
		case MdlVersion::GARRYSMOD:
			ConvertMDL48To54(buf.get(), filePath, filePath);
			break;
		case MdlVersion::PORTAL2:
			ConvertMDL49To54(buf.get(), filePath, filePath);
			break;
		case MdlVersion::TITANFALL:
			ConvertMDL52To53(buf.get(), filePath, filePath);
			break;
		case MdlVersion::TITANFALL2:
			ConvertMDL53To54(buf.get(), filePath, filePath);
			break;
		case MdlVersion::APEXLEGENDS:
			Error("Use -v<version> flag for RMDL conversion (e.g., -v191 for Season 19+)\n");
			break;
		default:
			Error("MDL version %i is currently unsupported\n", mdlVersion);
			break;
		}
		return;
	}

	// Handle rseq files
	std::string ext = std::filesystem::path(filePath).extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

	if (ext == ".rseq")
	{
		std::string version = "7.1";
		if (cmdline.HasParam("-version"))
			version = cmdline.GetParamValue("-version", "7.1");
		else
		{
			std::cout << pszRSeqVersionHelpString;
			std::cin >> version;
		}

		uintmax_t fileSize = 0;
		auto seqBuf = ReadFileToBuffer(filePath, fileSize);
		if (!seqBuf)
			Error("Failed to read file\n");

		// Load external data if exists
		std::string rseqExtPath = ChangeExtension(filePath, "rseq_ext");
		std::unique_ptr<char[]> seqExternalBuf;
		if (FILE_EXISTS(rseqExtPath))
		{
			uintmax_t extSize = 0;
			seqExternalBuf = ReadFileToBuffer(rseqExtPath, extSize);
		}

		if (version == "7.1")
			ConvertRSEQFrom71To7(seqBuf.get(), seqExternalBuf.get(), filePath);
		else if (version == "10")
			ConvertRSEQFrom10To7(seqBuf.get(), seqExternalBuf.get(), filePath);
		else
			Error("Unsupported rseq version: %s\n", version.c_str());

		return;
	}

	Error("Invalid input file. Use -v<version> flag for RMDL conversion.\n");
}

// Help text for batch mode
const char* pszBatchHelpString = {
	"Batch conversion mode:\n"
	"  rmdlconv.exe -v<version> <input_folder> [output_folder]\n"
	"\n"
	"Version flags:\n"
	"  -v8     Model v8\n"
	"  -v121   Model v12.1\n"
	"  -v122   Model v12.2\n"
	"  -v123   Model v12.3\n"
	"  -v124   Model v12.4\n"
	"  -v125   Model v12.5\n"
	"  -v13    Model v13\n"
	"  -v131   Model v13.1\n"
	"  -v14    Model v14\n"
	"  -v141   Model v14.1\n"
	"  -v15    Model v15\n"
	"  -v16    Model v16\n"
	"  -v17    Model v17\n"
	"  -v18    Model v18\n"
	"  -v19    Model v19\n"
	"  -v191   Model v19.1\n"
	"\n"
	"If output_folder is not specified, uses '<input_folder>_rmdlconv_out'\n"
	"Internal folder structure is preserved.\n"
	"\n"
	"Example:\n"
	"  rmdlconv.exe -v122 C:\\models\\input C:\\models\\converted\n"
	"  rmdlconv.exe -v191 C:\\models\\input\n"
};

// Converter IDs
enum ConverterID
{
	CONV_V8 = 0,
	CONV_V121,
	CONV_V122,
	CONV_V124,
	CONV_V125,
	CONV_V140,
	CONV_V150,
	CONV_V160,
	CONV_V191
};

struct VersionMapping
{
	const char* version;      // Version string (e.g., "12.1")
	const char* batchFlag;    // Batch flag (e.g., "-v121") or nullptr if alias
	int converterID;          // Which converter to use
	int subversion;           // Subversion for v16-19
	bool hasVG;               // Whether to convert VG files
};

static const VersionMapping s_versionMappings[] = {
	// Version 8
	{ "8",     "-v8",   CONV_V8,   0,  false },

	// Version 12.x (all use VG rev2)
	{ "12.1",  "-v121", CONV_V121, 0,  true },
	{ "121",   nullptr, CONV_V121, 0,  true },  // Alias
	{ "12.2",  "-v122", CONV_V122, 0,  true },
	{ "122",   nullptr, CONV_V122, 0,  true },
	{ "12.3",  "-v123", CONV_V122, 0,  true },  // Same converter as v12.2
	{ "123",   nullptr, CONV_V122, 0,  true },
	{ "12.4",  "-v124", CONV_V124, 0,  true },
	{ "124",   nullptr, CONV_V124, 0,  true },
	{ "12.5",  "-v125", CONV_V125, 0,  true },
	{ "125",   nullptr, CONV_V125, 0,  true },

	// Version 13.x (use VG rev2)
	{ "13",    "-v13",  CONV_V125, 0,  true },
	{ "13.1",  "-v131", CONV_V125, 0,  true },
	{ "131",   nullptr, CONV_V125, 0,  true },

	// Version 14.x
	{ "14",    "-v14",  CONV_V140, 0,  false },
	{ "14.1",  "-v141", CONV_V140, 0,  false },
	{ "141",   nullptr, CONV_V140, 0,  false },

	// Version 15
	{ "15",    "-v15",  CONV_V150, 0,  false },

	// Version 16-19 (use subversion parameter)
	{ "16",    "-v16",  CONV_V160, 16, false },
	{ "17",    "-v17",  CONV_V160, 17, false },
	{ "18",    "-v18",  CONV_V160, 18, false },
	{ "19",    "-v19",  CONV_V160, 19, false },

	// Version 19.1+
	{ "19.1",  "-v191", CONV_V191, 0,  false },
	{ "191",   nullptr, CONV_V191, 0,  false },

	{ nullptr, nullptr, 0,         0,  false }  // Terminator
};

static const VersionMapping* FindVersionMapping(const std::string& version)
{
	for (const VersionMapping* m = s_versionMappings; m->version != nullptr; m++)
	{
		if (version == m->version)
			return m;
	}
	return nullptr;
}

static const VersionMapping* FindVersionMappingByFlag(const char* flag)
{
	for (const VersionMapping* m = s_versionMappings; m->version != nullptr; m++)
	{
		if (m->batchFlag && strcmp(m->batchFlag, flag) == 0)
			return m;
	}
	return nullptr;
}

static std::unique_ptr<char[]> ReadFileToBuffer(const std::string& path, uintmax_t& outSize)
{
	if (!std::filesystem::exists(path))
		return nullptr;

	outSize = std::filesystem::file_size(path);
	std::unique_ptr<char[]> buf(new char[outSize]);

	std::ifstream ifs(path, std::ios::in | std::ios::binary);
	if (!ifs.is_open())
		return nullptr;

	ifs.read(buf.get(), outSize);
	ifs.close();

	return buf;
}

static bool ConvertModel(const VersionMapping* mapping, char* pMDL, size_t fileSize,
                         const std::string& inputFile, const std::string& outputFile)
{
	switch (mapping->converterID)
	{
	case CONV_V8:
		ConvertRMDL8To10(pMDL, inputFile, outputFile);
		break;
	case CONV_V121:
		ConvertRMDL121To10(pMDL, inputFile, outputFile);
		break;
	case CONV_V122:
		ConvertRMDL122To10(pMDL, inputFile, outputFile);
		break;
	case CONV_V124:
		ConvertRMDL124To10(pMDL, inputFile, outputFile);
		break;
	case CONV_V125:
		ConvertRMDL125To10(pMDL, inputFile, outputFile);
		break;
	case CONV_V140:
		ConvertRMDL140To10(pMDL, inputFile, outputFile);
		break;
	case CONV_V150:
		ConvertRMDL150To10(pMDL, inputFile, outputFile);
		break;
	case CONV_V160:
		ConvertRMDL160To10(pMDL, fileSize, inputFile, outputFile, mapping->subversion);
		break;
	case CONV_V191:
		ConvertRMDL191To10(pMDL, fileSize, inputFile, outputFile);
		break;
	default:
		return false;
	}
	return true;
}

static bool ConvertVGFile(const std::string& inputFile, const std::string& outputFile)
{
	std::filesystem::path vgInputPath(inputFile);
	vgInputPath.replace_extension(".vg");

	if (!std::filesystem::exists(vgInputPath))
		return false;

	uintmax_t vgSize = std::filesystem::file_size(vgInputPath);
	std::unique_ptr<char[]> vgBuf(new char[vgSize]);

	std::ifstream vgIfs(vgInputPath, std::ios::in | std::ios::binary);
	if (!vgIfs.is_open())
		return false;

	vgIfs.read(vgBuf.get(), vgSize);
	vgIfs.close();

	if (*(int*)vgBuf.get() != 'GVt0')
		return false;

	std::filesystem::path vgOutputPath(outputFile);
	vgOutputPath.replace_extension(".vg");

	ConvertVGData_12_1(vgBuf.get(), vgInputPath.string(), vgOutputPath.string());
	printf("  VG converted: %s\n", vgOutputPath.filename().string().c_str());
	return true;
}

static bool ConvertSingleModel(const std::string& inputPath, const std::string& outputPath, const std::string& version)
{
	const VersionMapping* mapping = FindVersionMapping(version);
	if (!mapping)
	{
		printf("ERROR: Unknown version '%s'\n", version.c_str());
		return false;
	}

	uintmax_t fileSize = 0;
	auto pMDL = ReadFileToBuffer(inputPath, fileSize);
	if (!pMDL)
	{
		printf("ERROR: Could not read file '%s'\n", inputPath.c_str());
		return false;
	}

	// Create output directory if needed
	std::filesystem::path outPath(outputPath);
	if (outPath.has_parent_path())
		std::filesystem::create_directories(outPath.parent_path());

	printf("Converting: %s (v%s)\n", inputPath.c_str(), version.c_str());

	if (!ConvertModel(mapping, pMDL.get(), fileSize, inputPath, outputPath))
	{
		printf("ERROR: Conversion failed\n");
		return false;
	}

	if (mapping->hasVG)
		ConvertVGFile(inputPath, outputPath);

	return true;
}

void BatchConvertModels(const std::string& sourceVersion, const std::string& inputFolder, const std::string& outputFolder)
{
	std::filesystem::path inputPath(inputFolder);
	std::filesystem::path outputPath(outputFolder);

	if (!std::filesystem::exists(inputPath))
		Error("Input folder does not exist: %s\n", inputFolder.c_str());

	if (!std::filesystem::is_directory(inputPath))
		Error("Input path is not a folder: %s\n", inputFolder.c_str());

	const VersionMapping* mapping = FindVersionMapping(sourceVersion);
	if (!mapping)
		Error("Unknown source version: %s\n", sourceVersion.c_str());

	std::filesystem::create_directories(outputPath);

	printf("Batch converting from: %s\n", inputFolder.c_str());
	printf("Output folder: %s\n", outputFolder.c_str());
	printf("Source version: %s\n", sourceVersion.c_str());
	printf("\n");

	int successCount = 0;
	int failCount = 0;
	int totalCount = 0;

	for (const auto& entry : std::filesystem::recursive_directory_iterator(inputPath))
	{
		if (!entry.is_regular_file())
			continue;

		std::string ext = entry.path().extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

		if (ext != ".rmdl")
			continue;

		totalCount++;

		std::filesystem::path relativePath = std::filesystem::relative(entry.path(), inputPath);
		std::filesystem::path outputFilePath = outputPath / relativePath;

		std::filesystem::create_directories(outputFilePath.parent_path());

		std::string inputFile = entry.path().string();
		std::string outputFile = outputFilePath.string();

		printf("[%d] Converting: %s\n", totalCount, relativePath.string().c_str());

		try
		{
			uintmax_t fileSize = std::filesystem::file_size(entry.path());
			std::unique_ptr<char[]> pMDL(new char[fileSize]);

			std::ifstream ifs(inputFile, std::ios::in | std::ios::binary);
			if (!ifs.is_open())
			{
				printf("  ERROR: Could not open file\n");
				failCount++;
				continue;
			}
			ifs.read(pMDL.get(), fileSize);
			ifs.close();

			if (!ConvertModel(mapping, pMDL.get(), fileSize, inputFile, outputFile))
			{
				printf("  ERROR: Conversion failed\n");
				failCount++;
				continue;
			}

			if (mapping->hasVG)
				ConvertVGFile(inputFile, outputFile);

			successCount++;
		}
		catch (const std::exception& e)
		{
			printf("  ERROR: %s\n", e.what());
			failCount++;
		}
	}

	printf("\n");
	printf("========================================\n");
	printf("Batch conversion complete!\n");
	printf("  Total:   %d\n", totalCount);
	printf("  Success: %d\n", successCount);
	printf("  Failed:  %d\n", failCount);
	printf("========================================\n");
}

int main(int argc, char** argv)
{
	printf("rmdlconv - Copyright (c) %s, rexx\n", &__DATE__[7]);

	CommandLine cmdline(argc, argv);

	if (argc < 2)
	{
		printf("%s", pszBatchHelpString);
		return 1;
	}

	// Check for help flag
	if (cmdline.HasParam("-help") || cmdline.HasParam("--help") || cmdline.HasParam("-h") || cmdline.HasParam("-?"))
	{
		printf("%s", pszBatchHelpString);
		return 0;
	}

	// Check for batch conversion flags (uses version mapping table)
	for (const VersionMapping* m = s_versionMappings; m->version != nullptr; m++)
	{
		if (!m->batchFlag)
			continue;  // Skip aliases

		if (cmdline.HasParam(m->batchFlag))
		{
			int flagIdx = cmdline.FindParam((char*)m->batchFlag);

			if (flagIdx < 0 || flagIdx + 1 >= argc)
			{
				printf("%s", pszBatchHelpString);
				Error("Missing input folder for batch conversion\n");
			}

			std::string inputFolder = argv[flagIdx + 1];
			std::string outputFolder;

			if (flagIdx + 2 < argc && argv[flagIdx + 2][0] != '-')
				outputFolder = argv[flagIdx + 2];
			else
				outputFolder = inputFolder + "_rmdlconv_out";

			BatchConvertModels(m->version, inputFolder, outputFolder);

			if (!cmdline.HasParam("-nopause"))
				std::system("pause");

			return 0;
		}
	}

	// Single model conversion with -convertmodel -sourceversion
	if (cmdline.HasParam("-convertmodel"))
	{
		std::string modelPath = cmdline.GetParamValue("-convertmodel");

		if (!cmdline.HasParam("-sourceversion"))
			Error("Missing '-sourceversion' parameter for RMDL conversion\n");

		const char* sourceVersion = cmdline.GetParamValue("-sourceversion");

		std::string outputPath = modelPath;  // Default: overwrite in place
		if (cmdline.HasParam("-outputdir"))
		{
			const char* customDir = cmdline.GetParamValue("-outputdir");
			outputPath = std::string(customDir) + "/" + std::filesystem::path(modelPath).filename().string();
		}

		if (!ConvertSingleModel(modelPath, outputPath, sourceVersion))
		{
			if (!cmdline.HasParam("-nopause"))
				std::system("pause");
			return 1;
		}

		if (!cmdline.HasParam("-nopause"))
			std::system("pause");

		return 0;
	}

	// Legacy: handle drag-and-drop or single file argument
	if (argc == 2 && std::filesystem::exists(argv[1]))
	{
		std::string filePath = argv[1];
		std::string ext = std::filesystem::path(filePath).extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

		if (ext == ".rmdl")
		{
			// Prompt for version
			std::string version;
			std::cout << pszVersionHelpString;
			std::cin >> version;

			std::filesystem::path inputPath(filePath);
			std::filesystem::path outputDir = inputPath.parent_path() / "rmdlconv_out";
			std::filesystem::create_directories(outputDir);

			std::string outputPath = (outputDir / inputPath.filename()).string();
			ConvertSingleModel(filePath, outputPath, version);

			if (!cmdline.HasParam("-nopause"))
				std::system("pause");

			return 0;
		}
	}

	// Fallback to legacy handling for MDL files
	LegacyConversionHandling(cmdline);

	if (!cmdline.HasParam("-nopause"))
		std::system("pause");

	return 0;
}
