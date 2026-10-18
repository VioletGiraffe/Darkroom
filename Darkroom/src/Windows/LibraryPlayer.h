#pragma once

class Library;
class QString;
class QWidget;
class VideoPlayerWindow;

// The video player with the library's features: per-video speed and saved loops persist in its metadata, and frames
// extract into the Extracted label.
namespace LibraryPlayer
{
	// Opens a self-managing player; videoPath need not be a library item.
	VideoPlayerWindow* open(Library& library, const QString& videoPath, QWidget* parent);
}
