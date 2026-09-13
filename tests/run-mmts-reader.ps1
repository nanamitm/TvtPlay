param([string]$Compiler = 'clang++')
$ErrorActionPreference = 'Stop'
$outputDir = Join-Path $PSScriptRoot '..\src\x64\ReaderTests'
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
$exe = Join-Path $outputDir 'mmts_reader_test.exe'
& $Compiler -std=c++17 -DUNICODE -D_UNICODE -DENABLE_MMT4K `
    (Join-Path $PSScriptRoot 'mmts_reader_test.cpp') `
    (Join-Path $PSScriptRoot '..\src\ReadOnlyMmtsFile.cpp') `
    -lshlwapi -o $exe
if ($LASTEXITCODE -ne 0) { throw 'MMTS reader test build failed' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'MMTS reader tests failed' }
