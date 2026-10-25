#pragma once

#include <functional>

class Library;
class PhotoCompareWindow;
class QString;

// PhotoCompareWindow with the library's deletion workflow.
namespace LibraryPhotoCompare
{
	// The pane menu deletes a library item through MediaItemManagement, any other file after its own confirmation.
	// onDeleted receives each deleted path.
	void installDeleteAction(PhotoCompareWindow& compareWindow, Library& library, std::function<void(const QString& path)> onDeleted = {});
}
