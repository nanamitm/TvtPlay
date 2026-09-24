# Builds the thumbnail generator test against the plugin's own Release objects
# (so the MP4 and MMTS readers, dantto4k and TSDuck come with it) and runs it on
# media files: .ts, .m2t, .m2ts, .mp4, .mmts or .mmtsedit.
# Usage: run-thumbnail-generator.ps1 <file> [<file>...]
param([Parameter(Mandatory, ValueFromRemainingArguments)][string[]]$Files)
$ErrorActionPreference = 'Stop'
$src = Join-Path $PSScriptRoot '..\src'

if (-not $env:VCINSTALLDIR) {
    $installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
    $vsPath = & (Join-Path $installer 'vswhere.exe') -latest -version '[17.0,18.0)' -products * -property installationPath
    $env:PATH = "$installer;$env:PATH"
    cmd /c "`"$vsPath\VC\Auxiliary\Build\vcvars64.bat`" >nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
    }
}

# The plugin build also builds FFmpeg and the TSDuck libraries when needed.
msbuild (Join-Path $src 'TvtPlay.sln') /m /nologo /v:m /p:Configuration=Release /p:Platform=x64
if ($LASTEXITCODE -ne 0) { throw 'Plugin build failed' }

$outputDir = Join-Path $src 'x64\ThumbnailTests'
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
$exe = Join-Path $outputDir 'thumbnail_generator_test.exe'
$ffmpeg = Join-Path $src 'thirdparty\ffmpeg'
$tsduck = Join-Path $src 'thirdparty\dantto4k\thirdparty\tsduck\bin\Release-x64'
# TvtPlay.obj holds the plugin's entry points; the test supplies g_hinstDLL itself.
$objects = Get-ChildItem (Join-Path $src 'TvtPlay\x64\Release\*.obj') |
    Where-Object Name -ne 'TvtPlay.obj' | ForEach-Object FullName
cl /nologo /std:c++17 /EHsc /MD /O2 /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DENABLE_MMT4K `
    "/Fo$outputDir\\" "/Fe$exe" `
    (Join-Path $PSScriptRoot 'thumbnail_generator_test.cpp') `
    /link /LTCG /NOLOGO @objects "/LIBPATH:$ffmpeg\lib" "/LIBPATH:$tsduck" `
    avcodec.lib swscale.lib avutil.lib tsducklib.lib tscorelib.lib `
    bcrypt.lib ole32.lib oleaut32.lib user32.lib gdi32.lib advapi32.lib shlwapi.lib ws2_32.lib comctl32.lib uxtheme.lib winscard.lib
if ($LASTEXITCODE -ne 0) { throw 'Thumbnail generator test build failed' }
& $exe $outputDir @Files
if ($LASTEXITCODE -ne 0) { throw 'Thumbnail generator test failed' }
