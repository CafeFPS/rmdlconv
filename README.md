# rmdlconv
copyright (c) 2022, rexx
---
### supported versions

**model format conversions:**
- Portal 2 (v49) -> Apex Legends Season 3 (v54 - rmdl v10)
- Titanfall 2 (v53) -> Apex Legends Season 3 (v54 - rmdl v10)
- Titanfall (v52) -> Titanfall 2 (v53) *(partial)*
- Apex Legends (v54 all subversions) -> Apex Legends Season 3 (v54 - rmdl v10):

| Version | Flag | Seasons |
|---------|------|---------|
| v8 | `-v8` | S0-1 |
| v12.1 | `-v121` | S7-8 |
| v12.5 | `-v122` | S11 |
| v13 | `-v13` | S12 |
| v14 | `-v14` | S13 |
| v14.1 | `-v141` | S15 |
| v15 | `-v15` | S16 |
| v16 | `-v16` | S15 |
| v17 | `-v17` | S17-18 |
| v18 | `-v18` | S22 |
| v19 | `-v19` | S26 |
| v19.1 | `-v191` | S27 |


### batch conversion (recommended for RMDL)
Convert entire folders of models with a single command:
```
rmdlconv.exe -v<version> <input_folder> [output_folder]
```

Examples:
```bash
rmdlconv.exe -v16 C:\models\season17          # Output to C:\models\season19_rmdlconv_out
rmdlconv.exe -v122 C:\s11_models C:\converted  # Custom output folder
```

### supported commands
- `-v<version>`: batch convert folder (see version table above)
- `-convertmodel <path> -sourceversion <ver>`: convert single model
- `-outputdir <path>`: custom output directory
- `-nopause`: automatically close console after running

### known issues
animation conversion is not currently supported and there may be various issues when using models in game
