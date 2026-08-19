#pragma once

namespace Settings {
	constexpr const char* RootFolder    = "settings/rootFolder";
	constexpr const char* RecentLibraries = "settings/recentLibraries";
	constexpr const char* FfmpegPath    = "settings/ffmpegPath";
	constexpr const char* UseTiff       = "settings/useTiff";
	constexpr const char* JpegQuality   = "settings/jpegQuality";
	constexpr const char* FrameStep     = "settings/frameStep";
	// No settings UI; editable in the settings file.
	constexpr const char* ExtractedLabelName        = "settings/extractedLabelName";
	constexpr const char* PreviewFrameCount = "mainWindow/previewFrameCount";
}

namespace Defaults {
	constexpr bool        UseTiff      = false;
	constexpr int         JpegQuality  = 1;
	constexpr int         FrameStep    = 3;
	constexpr const char* ExtractedLabelName = "Extracted";
	constexpr int         PreviewFrameCount = 4;
}
