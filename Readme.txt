# TvtPlay

TvtPlay is a file-playback plugin for [TVTest](https://github.com/DBCTRADO/TVTest).
It plays local transport-stream media through TVTest's `BonDriver_UDP` or
`BonDriver_Pipe` interface.

This fork is built for **64-bit TVTest only** and requires **TVTest 0.9.0 or
later**, because the seek bar is drawn as an item of TVTest's own status bar.
The current development branch is `work`.

## Features

- Playback of `.ts`, `.m2t`, `.m2ts`, and `.mp4` files.
- Playlist support for `.m3u` and `.tslist` files.
- Seek, repeat, playback-speed controls, and resume support, presented as a
  seek bar in TVTest's status bar.
- `BonDriver_Pipe.dll`, an optional named-pipe alternative to
  `BonDriver_UDP.dll`.
- Optional `TvtAudioStretchFilter.ax` for audio during speed-adjusted playback.

## Status bar integration

The seek bar, the transport buttons, and the position display are registered as
a single full-row item of TVTest's status bar instead of living in a window
TvtPlay creates and paints itself. This is the upstream `work-plus` arrangement,
merged into this branch.

Enable **TvtPlayステータスバー** in TVTest's status bar settings to show it. The
row follows TVTest's own colours, font, and DPI, so the colour keys in
`TvtPlay.ini` no longer have any effect; change the appearance from TVTest's
settings instead. Every other key in `TvtPlay.ini` still applies.

## Additions in this fork

### On-demand MMTS playback

Decrypted `.mmts` recordings are played through the bundled dantto4k MMT/TLV
demuxer and MPEG-2 TS remuxer. Converted TS data is generated only for the
requested range in memory; no full-size temporary TS file is written.

A dantto4k-generated `.mmtsmap` sidecar with the same base name is required
for duration detection and random access. Seeking restarts conversion at the
preceding RAP/IRAP point.

For recordings that still contain encrypted packets, configure a B-CAS/ACAS
reader or a CasProxy server in the `[MMTS]` section of `TvtPlay.ini`.

### Non-destructive `.mmtsedit` playback

Files created by `mmts-edit-gui` can be opened directly. A `.mmtsedit` file
contains an ordered EDL timeline of source ranges; TvtPlay reads the referenced
`.mmts` and `.mmtsmap` files on demand and exposes the concatenated timeline to
TVTest without exporting a new media file.

The EDL is applied **only when the `.mmtsedit` file itself is opened**. Opening
the source `.mmts` always plays the unedited recording, even if a sidecar EDL
exists. Source and map paths may be relative to the EDL file. The loader checks
the EDL version, source size, map, and timeline ranges before playback.

Like `mmts-dsfilter`, this playback path seeks to RAP boundaries and does not
re-encode partial GOPs. For frame-accurate non-RAP cuts, export the edited media
from `mmts-edit-gui` instead.

### Seek-bar thumbnails

Hovering over the seek bar shows a thumbnail of the video at that position
above the bar, with its time. Dragging with `SeekMode=1` updates it as well.
Thumbnails are decoded on a background thread from the file itself, so they
work for positions that have not been played yet, and recently shown ones are
kept in memory.

This first version covers **decrypted `.ts`, `.m2t` and `.m2ts` files with
MPEG-2 video** only. Nothing is shown for encrypted recordings, H.264/HEVC
video, `.mp4`, `.mmts` or `.mmtsedit`. The settings, in the `[Settings]`
section of `TvtPlay.ini`:

| Key | Default | Meaning |
| --- | --- | --- |
| `Thumbnail` | `1` | `0` turns the thumbnails off |
| `ThumbnailWidth` | `160` | Width in pixels at 96 DPI (64-640) |
| `ThumbnailCacheMax` | `128` | Number of thumbnails kept in memory |

Decoding uses FFmpeg (libavcodec, libswscale and libavutil), statically
linked and built LGPL-only with just the MPEG-2 video decoder. See
[FFmpeg](#ffmpeg) under Building.

### Recorded EIT handling

Recorded schedule EIT sections (`table_id` `0x50`-`0x5F`) are removed from the
TS output. Present/following EIT is retained. This prevents stale schedule data
in recordings from polluting TVTest's program guide.

## Installation and use

Copy `TvtPlay.tvtp` to TVTest's `Plugins` directory. Copy
`BonDriver_Pipe.dll` to the directory that contains `TVTest.exe` when using the
pipe BonDriver. Enable the plugin, then add the TvtPlay item to the status bar
from TVTest's settings.

For example:

```powershell
Start-Process "C:\Path\To\TVTest.exe" -ArgumentList `
  "/d BonDriver_Pipe.dll", "/tvtplay", "F:\recordings\program.mmtsedit"
```

Use a `.mmts` path instead to play the original recording. MMTS playback and
`.mmtsedit` playback require the 64-bit build.

## Building

Open `src/TvtPlay.sln` in Visual Studio and build either `Debug|x64` or
`Release|x64`. Win32 configurations are intentionally not provided. The build
uses the bundled dantto4k and TSDuck sources; the project pre-build step creates
the required TSDuck static libraries when needed.

### FFmpeg

The pre-build step also runs `src/thirdparty/build-ffmpeg.ps1`, which downloads
the pinned FFmpeg source release (checked by SHA-256), builds the minimal static
libraries with the MSVC tools, and installs them into `src/thirdparty/ffmpeg`.
It does nothing once they are up to date. The first build needs
[MSYS2](https://www.msys2.org/) with `make` and `diffutils`
(`pacman -S make diffutils`) at `C:\msys64`, or at the path in `MSYS2_ROOT`.

FFmpeg is licensed under the LGPL 2.1 or later. Release packages include its
licence as `FFmpeg_COPYING.LGPLv2.1.txt`; the FFmpeg source is available from
<https://ffmpeg.org/releases/>, and the version and configure options used are
in `build-ffmpeg.ps1`. Because TvtPlay's own source is public, it can be rebuilt
and relinked against a modified FFmpeg.

`tests/run-thumbnail-generator.ps1 <file.ts>` builds the thumbnail generator on
its own and writes thumbnails at several positions of a TS file as BMP files.

## Further documentation

The original detailed manual, including all plugin settings and legacy
features, is available in Japanese in [TvtPlay_Readme.txt](TvtPlay_Readme.txt).
The high-speed subtitle-viewing variant is documented in
[TvtPlay_hsw_Readme.txt](TvtPlay_hsw_Readme.txt).
