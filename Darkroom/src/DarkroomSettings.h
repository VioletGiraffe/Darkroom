#pragma once

// Darkroom-only keys; the shared ones are in Settings.h.
namespace Settings {
	constexpr const char* RootFolder    = "settings/rootFolder";
	constexpr const char* RecentLibraries = "settings/recentLibraries";
	constexpr const char* FrameStep     = "settings/frameStep";
	// No settings UI; editable in the settings file.
	constexpr const char* ExtractedLabelName        = "settings/extractedLabelName";
	constexpr const char* PreviewFrameCount = "mainWindow/previewFrameCount";
}

namespace Defaults {
	constexpr int         FrameStep    = 3;
	constexpr const char* ExtractedLabelName = "Extracted";
	constexpr int         PreviewFrameCount = 4;
}
