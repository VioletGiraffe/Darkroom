# Quickroom improvement backlog

- Leaving fullscreen closes the viewer; a setting could keep it open behind the browser instead. The
  exit-fullscreen handler's return value already selects between the two.
- The player has no such hand-off: a video opened from the command line never reaches the browser.
- Drive/computer view above filesystem roots; today the path field is the way across drives.
- Opening several files at once starts one process each - no single-instance handover.
- Name filter over the current folder.
- Video thumbnails: needs per-file ffmpeg extraction and a cache; tiles show the shell icon today.
- Copy/paste of the grid selection as file URLs, pasting URLs from other apps too, and dropping files onto the
  browser. Paste copies everything given (folders included) off the GUI thread and asks on a name collision.
- Cut/move, and pasting a clipboard bitmap as a new file.
- Deduplicate the logging/message-handler block shared verbatim by both apps' `main.cpp`.
- Camera RAW support (DNG, CR2/CR3, NEF, ARW, ...). Needs a third file-type icon family: sRGB has no room
  for a dozen more body colours, so reuse colours across body shapes or vary formats within one family hue;
  light bodies would need ink marks and a 1px darker-shade frame.
- SVG opens through the raster image path; zooming should re-render from the vector.
- AVIF, HEIC and JPEG XL are unsupported; all need the `kimageformats` plugins.
