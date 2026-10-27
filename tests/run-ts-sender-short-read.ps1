# Build the production plugin and test short reads at the sender boundary.
param([string]$MediaPath)
$ErrorActionPreference = 'Stop'
$src = Join-Path $PSScriptRoot '..\src'

if (-not $env:VCINSTALLDIR) {
    $installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
    # The plugin and this test are built by the same Visual Studio, so any
    # version whose toolset the project knows will do (VS 2022 and 2026).
    $vsPath = & (Join-Path $installer 'vswhere.exe') -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsPath) { throw 'Visual Studio with the C++ x64 tools is required' }
    $env:PATH = "$installer;$env:PATH"
    cmd /c "`"$vsPath\VC\Auxiliary\Build\vcvars64.bat`" >nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
    }
}

# The plugin build also builds FFmpeg and the TSDuck libraries when needed.
msbuild (Join-Path $src 'TvtPlay.sln') /m /nologo /v:m /p:Configuration=Release /p:Platform=x64
if ($LASTEXITCODE -ne 0) { throw 'Plugin build failed' }

$outputDir = Join-Path $src 'x64\SenderTests'
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
$exe = Join-Path $outputDir 'ts_sender_short_read_test.exe'
$ffmpeg = Join-Path $src 'thirdparty\ffmpeg'
$tsduck = Join-Path $src 'thirdparty\dantto4k\thirdparty\tsduck\bin\Release-x64'
# The test supplies g_hinstDLL and compiles TsSender.cpp with test access.
$objects = Get-ChildItem (Join-Path $src 'TvtPlay\x64\Release\*.obj') |
    Where-Object Name -notin 'TvtPlay.obj', 'TsSender.obj' | ForEach-Object FullName
cl /nologo /std:c++20 /EHsc /MD /O2 /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DENABLE_MMT4K `
    "/Fo$outputDir\\" "/Fe$exe" `
    (Join-Path $PSScriptRoot 'ts_sender_short_read_test.cpp') `
    /link /LTCG /NOLOGO @objects "/LIBPATH:$ffmpeg\lib" "/LIBPATH:$tsduck" `
    avcodec.lib swscale.lib avutil.lib tsducklib.lib tscorelib.lib `
    bcrypt.lib ole32.lib oleaut32.lib user32.lib gdi32.lib advapi32.lib shlwapi.lib ws2_32.lib comctl32.lib comdlg32.lib uxtheme.lib winscard.lib
if ($LASTEXITCODE -ne 0) { throw 'TS sender test build failed' }
# Avoid migrating settings on the optional real-MMTS test path.
@("[MMTS]", "Version=3", "Enabled=1", "UseSmartCard=0") |
    Set-Content (Join-Path $outputDir 'ts_sender_short_read_test.ini') -Encoding ascii
if ($MediaPath) { & $exe $MediaPath } else { & $exe }
if ($LASTEXITCODE -ne 0) { throw 'TS sender test failed' }

