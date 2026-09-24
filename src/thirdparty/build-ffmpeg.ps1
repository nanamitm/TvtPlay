# Builds the minimal static FFmpeg that TvtPlay links for seek-bar thumbnails.
#
# Only libavcodec (with the MPEG-2, H.264 and HEVC video decoders), libswscale
# and libavutil are built, LGPL-only, with MSVC and the /MD runtime. The result
# is installed into thirdparty\ffmpeg\{include,lib}; a stamp file there records
# the version and options so the build is skipped while they are unchanged.
#
# Requirements: Visual Studio with the x64 C++ tools, and MSYS2 (make, diffutils,
# tar, xz, curl) at C:\msys64 or at $env:MSYS2_ROOT.

param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$Version = '8.1.3'
$Sha256 = '7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3'
$ConfigureOptions = @(
    '--toolchain=msvc'
    '--arch=x86_64'
    '--target-os=win64'
    '--enable-static'
    '--disable-shared'
    '--disable-programs'
    '--disable-doc'
    '--disable-debug'
    '--disable-network'
    '--disable-autodetect'
    '--disable-x86asm'
    '--disable-everything'
    '--disable-avformat'
    '--disable-avdevice'
    '--disable-avfilter'
    '--disable-swresample'
    '--enable-decoder=mpeg2video,h264,hevc'
    '--enable-parser=mpegvideo,h264,hevc'
    '--enable-swscale'
    '--extra-cflags=-MD'
)

$root = $PSScriptRoot
$prefix = Join-Path $root 'ffmpeg'
$work = Join-Path $root 'ffmpeg-build'
$stamp = Join-Path $prefix 'build.stamp'
$stampText = "$Version`n$($ConfigureOptions -join ' ')"

if (-not $Force -and (Test-Path -LiteralPath $stamp) -and
    (Get-Content -LiteralPath $stamp -Raw) -eq $stampText) {
    Write-Host "FFmpeg $Version is up to date."
    exit 0
}

$msys = if ($env:MSYS2_ROOT) { $env:MSYS2_ROOT } else { 'C:\msys64' }
$bash = Join-Path $msys 'usr\bin\bash.exe'
if (-not (Test-Path -LiteralPath $bash)) {
    throw "MSYS2 was not found at $msys. Install it or set MSYS2_ROOT."
}

# Import the x64 MSVC environment unless this is already a developer prompt.
if (-not $env:VCINSTALLDIR -or $env:VSCMD_ARG_TGT_ARCH -ne 'x64') {
    $installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
    $vswhere = Join-Path $installer 'vswhere.exe'
    $vsArgs = @('-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath')
    # Prefer Visual Studio 2022, whose v143 tools the release build uses, so
    # an older linker never meets objects from a newer compiler.
    $vsPath = & $vswhere -latest -version '[17.0,18.0)' @vsArgs
    if (-not $vsPath) { $vsPath = & $vswhere -latest @vsArgs }
    if (-not $vsPath) { throw 'Visual Studio with the x64 C++ tools was not found.' }
    # vcvars64.bat runs vswhere itself and expects to find it on PATH.
    $env:PATH = "$installer;$env:PATH"
    $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
    cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
    }
}

New-Item -ItemType Directory -Path $work -Force | Out-Null
$archive = Join-Path $work "ffmpeg-$Version.tar.xz"
if (-not (Test-Path -LiteralPath $archive) -or
    (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $Sha256) {
    Write-Host "Downloading FFmpeg $Version..."
    Invoke-WebRequest -Uri "https://ffmpeg.org/releases/ffmpeg-$Version.tar.xz" -OutFile $archive
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $Sha256) {
        Remove-Item -LiteralPath $archive
        throw "The SHA-256 of ffmpeg-$Version.tar.xz does not match."
    }
}

function ConvertTo-MsysPath([string]$path) {
    $full = [System.IO.Path]::GetFullPath($path)
    '/' + $full.Substring(0, 1).ToLowerInvariant() + $full.Substring(2).Replace('\', '/')
}

$workMsys = ConvertTo-MsysPath $work
$prefixMsys = ConvertTo-MsysPath $prefix
$options = ($ConfigureOptions | ForEach-Object { "'$_'" }) -join ' '
$script = @"
set -e
cd '$workMsys'
rm -rf 'ffmpeg-$Version' '$prefixMsys'
tar -xJf 'ffmpeg-$Version.tar.xz'
cd 'ffmpeg-$Version'
./configure --prefix='$prefixMsys' $options
make -j`$(nproc)
make install
"@

# Keep the MSVC tools on PATH inside the MSYS2 shell.
$env:MSYS2_PATH_TYPE = 'inherit'
$env:CHERE_INVOKING = '1'
& $bash -lc $script
if ($LASTEXITCODE -ne 0) { throw "The FFmpeg build failed ($LASTEXITCODE)." }

Copy-Item -LiteralPath (Join-Path $work "ffmpeg-$Version\COPYING.LGPLv2.1") -Destination $prefix
Set-Content -LiteralPath $stamp -Value $stampText -NoNewline
Write-Host "FFmpeg $Version was installed into $prefix."
