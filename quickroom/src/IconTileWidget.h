#pragma once

#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QIcon>
#include <QWidget>
RESTORE_COMPILER_WARNINGS

#include <functional>

// Grid tile for entries without an image preview (folders, videos): a file icon above a caption.
// Mirrors ThumbnailWidget's framed styling so mixed rows read as one grid.
class IconTileWidget final : public QWidget
{
public:
	IconTileWidget(const QIcon& icon, const QString& caption, int tileSize, QWidget* parent = nullptr);

	void setOnActivatedCallback(std::function<void()> handler);
	// Ctrl+wheel emits signed steps and is consumed; plain wheel propagates to the surrounding view.
	void setOnMouseWheelCallback(std::function<void(int steps)> handler);

protected:
	void mouseDoubleClickEvent(QMouseEvent* event) override;
	void wheelEvent(QWheelEvent* event) override;
	void paintEvent(QPaintEvent* event) override;

private:
	QIcon _icon;
	QString _caption;
	std::function<void()> _onActivated;
	std::function<void(int steps)> _onWheel;
};
