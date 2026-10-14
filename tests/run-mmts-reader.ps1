param([string]$Compiler = 'clang++')
$ErrorActionPreference = 'Stop'
$outputDir = Join-Path $PSScriptRoot '..\src\x64\ReaderTests'
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
$exe = Join-Path $outputDir 'mmts_reader_test.exe'
& $Compiler -std=c++17 -fno-access-control -DUNICODE -D_UNICODE -DENABLE_MMT4K `
    (Join-Path $PSScriptRoot 'mmts_reader_test.cpp') `
    (Join-Path $PSScriptRoot '..\src\ReadOnlyMmtsFile.cpp') `
    -lshlwapi -o $exe
if ($LASTEXITCODE -ne 0) { throw 'MMTS reader test build failed' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'MMTS reader tests failed' }

# Also exercise the production access-unit gate without media or CAS hardware.
$gateExe = Join-Path $outputDir 'mmts_edit_gate_test.exe'
& $Compiler -std=c++17 (Join-Path $PSScriptRoot 'mmts_edit_gate_test.cpp') -o $gateExe
if ($LASTEXITCODE -ne 0) { throw 'MMTS edit gate test build failed' }
& $gateExe
if ($LASTEXITCODE -ne 0) { throw 'MMTS edit gate tests failed' }
