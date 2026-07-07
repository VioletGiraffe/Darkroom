#pragma once

#include "Core/MediaId.h"

#include <QColor>
#include <QSize>
#include <QStringList>
#include <QWidget>

#include <functional>
#include <vector>

class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QLabel;
class QPushButton;
class ThumbnailWidget;

class MediaItemWidget final : public QWidget {
public:
	// maxImageSize bounds the thumbnail's preview area; the actual size best-fits the loaded image within it.
	MediaItemWidget(
		QSize maxImageSize, const QStringList& previewPaths, const QString& label,
		const MediaId& mediaId,
		bool inBest, std::function<void()> onToggleBest,
		std::function<void()> onDoubleClick = {},
		std::function<void(QPoint globalPos)> onContextMenu = {},
		bool dynamicSizeHint = true,
		QWidget* parent = nullptr
	);

	[[nodiscard]] QSize sizeHint() const override;

	// Card chrome (frame border + inner padding) reserved around the thumbnail canvas on every side. Public so
	// the grid can size cards to tile together (see videoCanvasWidthForTiling).
	static constexpr int CardBorder = 1;
	static constexpr int CardPadding = 6;
	static constexpr int CardChromePerSide = CardBorder + CardPadding;

	// A video card shows `frameCount` preview frames in a horizontal strip; a photo card shows one square image
	// (side = photoSide). Returns the video card's thumbnail-canvas width chosen so the video card spans exactly
	// `frameCount` photo-card columns plus the gaps between them - i.e. (video width + gap) == frameCount x
	// (photo width + gap). This makes mixed video/photo cards align on a single column grid in the media grid's
	// flow layout. `gridGap` is the grid's item spacing (QListView::spacing()).
	[[nodiscard]] inline static constexpr int videoCanvasWidthForTiling(int photoSide, int frameCount, int gridGap)
	{
		// A card's width is its canvas + 2*CardChromePerSide; solving the tiling equation above for the canvas.
		return frameCount * photoSide + (frameCount - 1) * (gridGap + 2 * CardChromePerSide);
	}

	// The card's stable identity (source video name + size). Carried so label ops and the label-drop target
	// (see setOnLabelDropped) address the video directly. Invalid if the source video is missing.
	[[nodiscard]] const MediaId& mediaId() const { return m_mediaId; }

	// Updates the card's caption (the "N:  name" label) without re-rendering its thumbnail.
	void setLabel(const QString& label);

	// Reflects a Best change made elsewhere (e.g. a context-menu toggle) on the card's star, without invoking the
	// onToggleBest callback - the caller owns the Best state; this only syncs the visual.
	void setInBest(bool inBest);

	// Sets the colored dots overlaid on the thumbnail (one per label the item carries, including Best);
	// tooltip lists their names. An empty list hides the overlay. Colors are computed by the caller
	// (MainWindow) from the Catalog.
	void setLabelDots(const std::vector<QColor>& colors, const QString& tooltip);

	// Shows/hides a small top-right badge marking a video that hasn't had its full frame set extracted yet
	// (only its permanent preview frames exist so far) - see Catalog::isSplitIntoFrames.
	void setSplitPending(bool pending);

	void setOnMiddleButtonClick(std::function<void()> onClick);

	// See ThumbnailWidget::setOnMouseWheelCallback — Ctrl+wheel over the card delegates the zoom policy here.
	void setOnMouseWheelCallback(std::function<void(int steps)> handler);

	// Makes the card a drop target for a label dragged from the LabelSidebar: dropping invokes handler with
	// the dropped label's id (the caller assigns it). Only set on grid cards; unset cards reject the drop.
	void setOnLabelDropped(std::function<void(const QString& labelId)> handler);

protected:
	bool eventFilter(QObject* watched, QEvent* event) override;
	void dragEnterEvent(QDragEnterEvent* event) override;
	void dragMoveEvent(QDragMoveEvent* event) override;
	void dropEvent(QDropEvent* event) override;

private:
	// Keeps the badge pinned to the thumbnail's top-right corner across resizes (unlike the top-left label
	// dots, its position depends on the thumbnail's width).
	void repositionSplitPendingBadge();

private:
	ThumbnailWidget*             m_thumb = nullptr;
	QWidget*                     m_footer = nullptr;     // bottom row: star + dots + name (sibling of m_thumb)
	QPushButton*                 m_starButton = nullptr; // Best toggle in the footer; setInBest() syncs its checked state
	QLabel*                      m_name = nullptr;       // elided, right-aligned item name in the footer
	QWidget*                     m_labelDots = nullptr;  // colored-dot strip (LabelDotStrip), child of m_footer
	QWidget*                     m_splitPendingBadge = nullptr;  // top-right "not fully split" badge, child of m_thumb
	MediaId                      m_mediaId;
	std::function<void()>               m_onMiddleButtonClick;
	std::function<void()>               m_onDoubleClick;
	std::function<void(QPoint)>         m_onContextMenu;
	std::function<void(const QString&)> m_onLabelDropped;
};
