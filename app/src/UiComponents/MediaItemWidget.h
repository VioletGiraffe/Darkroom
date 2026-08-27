#pragma once

#include "Core/MediaId.h"
#include "UiComponents/ThumbnailWidget.h"
#include "compiler/compiler_warnings_control.h"

DISABLE_COMPILER_WARNINGS
#include <QColor>
#include <QSize>
#include <QStringList>
#include <QWidget>
RESTORE_COMPILER_WARNINGS

#include <functional>
#include <vector>

class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QLabel;
class QPushButton;

class MediaItemWidget final : public QWidget {
public:
	MediaItemWidget(
		QSize maxImageSize, const QStringList& previewPaths, const QString& label,
		const MediaId& mediaId,
		bool inBest, std::function<void()> onToggleBest,
		std::function<void()> onDoubleClick = {},
		std::function<void(QPoint globalPos)> onContextMenu = {},
		bool dynamicSizeHint = true,
		bool filmStrip = false,
		QWidget* parent = nullptr
	);

	[[nodiscard]] QSize sizeHint() const override;

	// Public so the grid can solve mixed-card tiling.
	static constexpr int CardBorder = 1;
	static constexpr int CardPadding = 6;
	static constexpr int CardChromePerSide = CardBorder + CardPadding;

	// Solves (video card width + gap) == frameCount * (photo card width + gap).
	[[nodiscard]] inline static constexpr int videoCanvasWidthForTiling(int photoSide, int frameCount, int gridGap)
	{
		return frameCount * photoSide + (frameCount - 1) * (gridGap + 2 * CardChromePerSide);
	}

	[[nodiscard]] const MediaId& mediaId() const { return _mediaId; }

	void setLabel(const QString& label);

	// Updates the visual without invoking onToggleBest.
	void setInBest(bool inBest);

	// Empty colors hide the overlay; the caller computes both colors and tooltip.
	void setLabelDots(const std::vector<QColor>& colors, const QString& tooltip);

	void setFramesExtracted(bool extracted);

	// Non-positive duration hides the video overlay.
	void setDuration(qint64 durationMs);

	void setOnMiddleButtonClick(std::function<void()> onClick);

	void setOnMouseWheelCallback(std::function<void(int steps)> handler);

	// Unset cards reject label drops.
	void setOnLabelDropped(std::function<void(const QString& labelId)> handler);

protected:
	bool eventFilter(QObject* watched, QEvent* event) override;
	void dragEnterEvent(QDragEnterEvent* event) override;
	void dragMoveEvent(QDragMoveEvent* event) override;
	void dropEvent(QDropEvent* event) override;

private:
	void repositionFramesReadyBadge();
	void repositionDurationBadge();

private:
	ThumbnailWidget*             _thumb = nullptr;
	QWidget*                     _footer = nullptr;
	QPushButton*                 _starButton = nullptr;
	QLabel*                      _name = nullptr;
	QWidget*                     _labelDots = nullptr;
	QWidget*                     _framesReadyBadge = nullptr;
	QWidget*                     _durationBadge = nullptr;
	MediaId                      _mediaId;
	bool                         _filmStrip = false;
	std::function<void()>               _onMiddleButtonClick;
	std::function<void()>               _onDoubleClick;
	std::function<void(QPoint)>         _onContextMenu;
	std::function<void(const QString&)> _onLabelDropped;
};
