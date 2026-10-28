#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QSettings>
RESTORE_COMPILER_WARNINGS

constexpr const char* DarkroomApplicationName = "Darkroom";
constexpr const char* QuickroomApplicationName = "Quickroom";

// Preferences that mean the same in both apps; everything else stays in each app's own store.
[[nodiscard]] QSettings sharedSettings();

// Must run at startup before the theme is first used: points CThemeController at the shared store.
// Also moves shared values left in the apps' own stores by earlier versions.
void initSharedSettings();

// Every key of the shared store except CThemeController's.
namespace Settings {
	constexpr const char* FfmpegPath                  = "settings/ffmpegPath";
	constexpr const char* UseTiff                     = "settings/useTiff";
	constexpr const char* JpegQuality                 = "settings/jpegQuality";
	constexpr const char* VideoPlayerPauseOnSeek      = "VideoPlayer/PauseOnSeek";
	constexpr const char* VideoPlayerVolume           = "VideoPlayer/Volume";
	constexpr const char* VideoPlayerMuted            = "VideoPlayer/Muted";
	constexpr const char* VideoPlayerOscillationCurve = "VideoPlayer/OscillationCurve";
	constexpr const char* PhotoCompareIgnoreRotation  = "photoCompare/ignoreRotation";
	constexpr const char* RawRendering                = "settings/rawRendering"; // read through rawRenderingSetting()
	constexpr const char* ColorManagement             = "settings/colorManagement";
}

namespace Defaults {
	constexpr bool        UseTiff                     = false;
	constexpr int         JpegQuality                 = 1;
	constexpr bool        VideoPlayerPauseOnSeek      = true;
	constexpr int         VideoPlayerVolume           = 100;
	constexpr bool        VideoPlayerMuted            = false;
	constexpr const char* VideoPlayerOscillationCurve = "cosine";
	constexpr bool        PhotoCompareIgnoreRotation  = false;
	constexpr const char* RawRendering                = "halfSize";
	constexpr bool        ColorManagement             = true;
}
