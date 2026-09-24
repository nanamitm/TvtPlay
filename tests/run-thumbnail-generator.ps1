# Builds the thumbnail generator against the static FFmpeg and runs it on TS files.
# Usage: run-thumbnail-generator.ps1 <file.ts> [<file.ts>...]
param([Parameter(Mandatory, ValueFromRemainingArguments)][string[]]$TsFiles)
$ErrorActionPreference = 'Stop'
$src = Join-Path $PSScriptRoot '..\src'
& (Join-Path $src 'thirdparty\build-ffmpeg.ps1')

if (-not $env:VCINSTALLDIR) {
    $installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
    $vsPath = & (Join-Path $installer 'vswhere.exe') -latest -version '[17.0,18.0)' -products * -property installationPath
    $env:PATH = "$installer;$env:PATH"
    cmd /c "`"$vsPath\VC\Auxiliary\Build\vcvars64.bat`" >nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
    }
}

$outputDir = Join-Path $src 'x64\ThumbnailTests'
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
$exe = Join-Path $outputDir 'thumbnail_generator_test.exe'
$ffmpeg = Join-Path $src 'thirdparty\ffmpeg'
cl /nologo /std:c++17 /EHsc /MD /O2 /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN "/I$ffmpeg\include" `
    "/Fo$outputDir\\" "/Fe$exe" `
    (Join-Path $PSScriptRoot 'thumbnail_generator_test.cpp') `
    (Join-Path $src 'ThumbnailGenerator.cpp') (Join-Path $src 'ReadOnlyFile.cpp') (Join-Path $src 'Util.cpp') `
    (Join-Path $src 'ReadOnlyMpeg4File.cpp') (Join-Path $src 'PsiArchiveReader.cpp') (Join-Path $src 'B24CaptionUtil.cpp') `
    /link "/LIBPATH:$ffmpeg\lib" avcodec.lib swscale.lib avutil.lib bcrypt.lib ole32.lib user32.lib gdi32.lib advapi32.lib shlwapi.lib
if ($LASTEXITCODE -ne 0) { throw 'Thumbnail generator test build failed' }
& $exe $outputDir @TsFiles
if ($LASTEXITCODE -ne 0) { throw 'Thumbnail generator test failed' }
