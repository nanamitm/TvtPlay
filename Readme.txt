# TvtPlay

TvtPlay is a file-playback plugin for [TVTest](https://github.com/DBCTRADO/TVTest).
It plays local transport-stream media through TVTest's `BonDriver_UDP` or
`BonDriver_Pipe` interface.

This fork is built for **64-bit TVTest only**. The current development branch
is `work`.

## Features

- Playback of `.ts`, `.m2t`, `.m2ts`, and `.mp4` files.
- Playlist support for `.m3u` and `.tslist` files.
- Seek, repeat, playback-speed controls, and resume support through the
  TVTest plugin UI.
- `BonDriver_Pipe.dll`, an optional named-pipe alternative to
  `BonDriver_UDP.dll`.
- Optional `TvtAudioStretchFilter.ax` for audio during speed-adjusted playback.

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

### Recorded EIT handling

Recorded schedule EIT sections (`table_id` `0x50`-`0x5F`) are removed from the
TS output. Present/following EIT is retained. This prevents stale schedule data
in recordings from polluting TVTest's program guide.

## Installation and use

Copy `TvtPlay.tvtp` to TVTest's `Plugins` directory. Copy
`BonDriver_Pipe.dll` to the directory that contains `TVTest.exe` when using the
pipe BonDriver.

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

## Further documentation

The original detailed manual, including all plugin settings and legacy
features, is available in Japanese in [TvtPlay_Readme.txt](TvtPlay_Readme.txt).
The high-speed subtitle-viewing variant is documented in
[TvtPlay_hsw_Readme.txt](TvtPlay_hsw_Readme.txt).
