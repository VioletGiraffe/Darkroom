# Darkroom

A Qt desktop application (Windows-first) for extracting frames from videos with ffmpeg and organizing the
resulting frame collections: labels, thumbnail grids, a frame viewer, and a built-in video player.

## Features

- **Import**: drag & drop video files (MP4, MOV, AVI, MKV, FLV) onto the window, or use the Quick Import
  staging dialog — with duplicate detection, so the same video isn't ingested twice
- **Frame extraction**: via ffmpeg, to JPEG or TIFF, with configurable encoding settings
- **Labels**: organize videos with colored labels — assign from the sidebar by drag & drop or context menu;
  filter the grid by label
- **Video grid**: cards with preview thumbnails, adjustable zoom, name filter, sorting, multi-select
- **Frame browsing**: per-video thumbnail viewer; open any frame in the system image viewer or in Explorer
- **Video player**: built-in player with seeking and A-B loops, saved per video
- **Frame comparison**: view frames side by side
- **Catalog maintenance**: integrity check and untracked-file discovery
- **Dark & light themes**

## Requirements

- Windows (primary target)
- [ffmpeg](https://ffmpeg.org/download.html) — found on `PATH`, or set the explicit path in the settings dialog

## Building

qmake-based; requires Qt 6.5+ and a C++ toolchain (MSVC 2022 on Windows):

```
qmake -tp vc -r
msbuild /t:Build /p:Configuration=Release
```

or open `Darkroom.pro` in Qt Creator. See [.github/workflows/CI.yml](.github/workflows/CI.yml) for the
complete build and packaging steps.

## License

Apache-2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE).
