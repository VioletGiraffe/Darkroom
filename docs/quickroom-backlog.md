# Quickroom improvement backlog

- Leaving fullscreen closes the viewer; a setting could keep it open behind the browser instead. The
  exit-fullscreen handler's return value already selects between the two.
- The player has no such hand-off: a video opened from the command line never reaches the browser.
- Drive/computer view above filesystem roots; today the path field is the way across drives.
- Opening several files at once starts one process each - no single-instance handover.
- Replace `QFileSystemWatcher` with a folder watcher of our own. On Windows and Linux Qt's directory watch misses a file
  rewritten in place (stale tile), a file still being copied in by another program (tile built from a partial file),
  and the shown folder being renamed, moved or sent to Trash.
- Video thumbnails: needs per-file ffmpeg extraction and a cache; tiles show the shell icon today.
- Pasting a clipboard bitmap as a new file.
- Deduplicate the logging/message-handler block shared verbatim by both apps' `main.cpp`.
- SVG opens through the raster image path; zooming should re-render from the vector.
- AVIF, HEIC and JPEG XL are unsupported; all need the `kimageformats` plugins.
- Camera raws LibRaw cannot render:
  - JPEG XL DNG (DNG 1.7): LibRaw decodes it only through Adobe's DNG SDK.
  - Lossy DNG: needs LibRaw built with libjpeg; our build has none.
  - Nikon High Efficiency NEF (Z 8, Z 9, Z f, Z6III): JPEG XS-based; LibRaw has no decoder for it.
