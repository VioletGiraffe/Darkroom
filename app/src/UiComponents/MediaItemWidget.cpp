#include "UiComponents/MediaItemWidget.h"
#include "UiComponents/LabelMimeType.h"
#include "UiComponents/ThumbnailWidget.h"
#include "Theme/Theme.h"
#include "Theme/Icons.h"
#include "Utils.h"

#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPolygonF>
#include <QPushButton>
#include <QResizeEvent>
#include <QStyleHints>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// Vertical gap between the thumbnail and the footer row. Shared so the constructor's layout and sizeHint()
// agree on the card's height.
static constexpr int CARD_FOOTER_SPACING = 5;

// A row of colored dots (one per label) painted bare on the card surface in the footer. Display-only; the
// caller sets the colors. Mouse-transparent so clicks/drags fall through to the card underneath.
class LabelDotStrip final : public QWidget {
public:
	explicit LabelDotStrip(QWidget* parent) : QWidget(parent)
	{
		setAttribute(Qt::WA_TransparentForMouseEvents);
	}

	void setColors(const std::vector<QColor>& colors)
	{
		_colors = colors;
		updateGeometry();   // sizeHint changed; let the footer layout re-fit the strip
		update();
	}

	[[nodiscard]] QSize sizeHint() const override
	{
		const int n = static_cast<int>(_colors.size());
		if (n == 0)
			return { 0, 0 };
		return { n * DOT + (n - 1) * GAP, DOT };
	}

protected:
	void paintEvent(QPaintEvent*) override
	{
		if (_colors.empty())
			return;

		QPainter p{ this };
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);

		const int y = (height() - DOT) / 2;
		int x = 0;
		for (const QColor& c : _colors)
		{
			p.setBrush(c.isValid() ? c : QColor("#888888"));  // unset label color -> neutral grey
			p.drawEllipse(x, y, DOT, DOT);
			x += DOT + GAP;
		}
	}

private:
	static constexpr int DOT = 8;  // dot diameter
	static constexpr int GAP = 4;  // spacing between dots

	std::vector<QColor> _colors;
};

// A QLabel that elides its text to the available width, keeping the full string for the tooltip. Used for
// the footer item name so a long name truncates instead of forcing the card wider.
class ElidedLabel final : public QLabel {
public:
	using QLabel::QLabel;

	void setFullText(const QString& text)
	{
		_full = text;
		setToolTip(text);
		updateElision();
	}

protected:
	void resizeEvent(QResizeEvent* event) override
	{
		QLabel::resizeEvent(event);
		updateElision();
	}

private:
	void updateElision()
	{
		setText(fontMetrics().elidedText(_full, Qt::ElideRight, width()));
	}

	QString _full;
};

// A tiny mouse-transparent badge in the thumbnail's top-right corner, shown on a video whose full frame set has
// been extracted (Catalog::isSplitIntoFrames): a green contact-sheet grid icon that reads "frames ready" at a glance.
// Hidden while only preview frames exist, and never shown on photos. Mouse-transparent so hover falls through
// to the thumbnail, which carries the card's single tooltip (extraction state + labels; composed in MainWindow).
class FramesReadyBadge final : public QWidget {
public:
	explicit FramesReadyBadge(QWidget* parent)
		: QWidget(parent), _icon(Theme::tintedIcon(QStringLiteral(":/UI/icon_grid.svg"), &Theme::ThemeColors::ReadyGreen))
	{
		setAttribute(Qt::WA_TransparentForMouseEvents);
		resize(sizeHint());
	}

	[[nodiscard]] QSize sizeHint() const override { return { 2 * PAD + SIZE, 2 * PAD + SIZE }; }

protected:
	void paintEvent(QPaintEvent*) override
	{
		QPainter p{ this };
		p.setRenderHint(QPainter::Antialiasing);

		// Rounded-square backdrop matching the grid icon's own outline (a full circle would clash with the square icon).
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0, 0, 0, 90));
		p.drawRoundedRect(rect(), BACKDROP_RADIUS, BACKDROP_RADIUS);

		// The icon engine tints the SVG with Theme::ReadyGreen and renders it at the badge's device pixel ratio.
		_icon.paint(&p, QRect(PAD, PAD, SIZE, SIZE));
	}

private:
	static constexpr int SIZE = 16;            // the icon's box; the backdrop adds PAD around it
	static constexpr int PAD = 3;              // backdrop padding around the icon
	static constexpr int BACKDROP_RADIUS = 6;  // rounded-square corners, ~concentric with the grid icon's outline (not a circle)
	QIcon _icon;                              // green "frames extracted" contact-sheet grid glyph
};

// Compact video-duration text: M:SS under an hour, H:MM:SS at or past it, no leading zero on the leading
// unit (1:34, 0:08, 1:02:17). The sub-second tail is truncated, matching how media players show total length.
QString formatDuration(qint64 ms)
{
	const qint64 totalSeconds = ms / 1000;
	const qint64 h = totalSeconds / 3600;
	const qint64 m = (totalSeconds / 60) % 60;
	const qint64 s = totalSeconds % 60;
	return h > 0 ? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'))
	             : QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
}

// A mouse-transparent overlay pinned to the thumbnail's bottom-right corner: a filled play triangle then the
// duration text, on a translucent dark pill. The dark backdrop keeps the white content legible over any frame
// (bright or dark), so it stays dark in both themes. The triangle is drawn rather than a glyph so it sizes and
// vertically centres with the digits regardless of the font. setText resizes the pill to hug its content;
// MediaItemWidget repositions it after.
class DurationBadge final : public QWidget {
public:
	explicit DurationBadge(QWidget* parent) : QWidget(parent)
	{
		setAttribute(Qt::WA_TransparentForMouseEvents);
		QFont f = font();
		f.setPointSize(8);
		setFont(f);
	}

	void setText(const QString& text)
	{
		if (_text == text)
			return;
		_text = text;
		resize(sizeHint());   // hug the content; the owner pins the corner
		update();
	}

	[[nodiscard]] QSize sizeHint() const override
	{
		const QFontMetrics fm(font());
		return { PAD_H * 2 + triangleWidth(fm) + GAP + fm.horizontalAdvance(_text), fm.height() + PAD_V * 2 };
	}

protected:
	void paintEvent(QPaintEvent*) override
	{
		QPainter p{ this };
		p.setRenderHint(QPainter::Antialiasing);

		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0, 0, 0, 128));   // option A ("subtle"): ~0.5-opacity dark backdrop
		p.drawRoundedRect(rect(), RADIUS, RADIUS);

		const QColor fg(255, 255, 255, 245);
		const QFontMetrics fm(font());
		const double cy = height() / 2.0;
		const int triH = triangleHeight(fm);
		const int triW = triangleWidth(fm);

		QPolygonF triangle;   // right-pointing play symbol, vertically centred
		triangle << QPointF(PAD_H, cy - triH / 2.0) << QPointF(PAD_H, cy + triH / 2.0) << QPointF(PAD_H + triW, cy);
		p.setBrush(fg);
		p.drawPolygon(triangle);

		const int textX = PAD_H + triW + GAP;
		p.setPen(fg);
		p.drawText(QRect(textX, 0, width() - textX - PAD_H, height()), Qt::AlignVCenter | Qt::AlignLeft, _text);
	}

private:
	static constexpr int PAD_H = 6;   // horizontal padding inside the pill
	static constexpr int PAD_V = 3;   // vertical padding inside the pill
	static constexpr int GAP = 4;     // triangle-to-text gap
	static constexpr int RADIUS = 5;  // pill corner radius (option A)

	// Triangle sized off the font so it tracks the digits; kept in one place so sizeHint and paint agree.
	static int triangleHeight(const QFontMetrics& fm) { return qRound(fm.ascent() * 0.62); }
	static int triangleWidth(const QFontMetrics& fm)  { return qRound(triangleHeight(fm) * 0.85); }

	QString _text;
};

}  // namespace

MediaItemWidget::MediaItemWidget(
	QSize maxImageSize, const QStringList& previewPaths, const QString& label,
	const MediaId& mediaId,
	bool inBest, std::function<void()> onToggleBest,
	std::function<void()> onDoubleClick,
	std::function<void(QPoint)> onContextMenu,
	bool dynamicSizeHint,
	bool filmStrip,
	QWidget* parent
)
	: QWidget{ parent }
	, _mediaId{ mediaId }
	, _filmStrip{ filmStrip }
	, _onDoubleClick{ std::move(onDoubleClick) }
	, _onContextMenu{ std::move(onContextMenu) }
{
	setObjectName("mediaItemCard");
	setAcceptDrops(true);  // accept a label dragged from the sidebar (see dropEvent / setOnLabelDropped)

	// The card owns the frame: a bordered, rounded container that highlights on hover - styled by #mediaItemCard in
	// the central sheet (Style.cpp), not per-instance (which polished slowly at grid scale). The thumbnail inside
	// is borderless (just the matte well); the footer below carries the star, label dots and name. The normal
	// background stays transparent so the grid's item-selection highlight shows through behind it.
	setAttribute(Qt::WA_StyledBackground);
	setAttribute(Qt::WA_Hover);   // card keeps its hover state even while the cursor is over a child widget
	// Reserve the frame's border + padding on each side; the #mediaItemCard border (Style.cpp) paints within it.
	setContentsMargins(CardChromePerSide, CardChromePerSide, CardChromePerSide, CardChromePerSide);

	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(CARD_FOOTER_SPACING);

	_thumb = new ThumbnailWidget(previewPaths, QString(), this, maxImageSize, dynamicSizeHint, /*framed*/ false, /*filmStrip*/ _filmStrip);
	_thumb->installEventFilter(this);

	// "Frames extracted" badge in the thumbnail's top-right corner. Hidden until setFramesExtracted(true).
	_framesReadyBadge = new FramesReadyBadge(_thumb);
	_framesReadyBadge->hide();

	// Duration overlay in the thumbnail's bottom-right corner. Hidden until setDuration() is given a video's length.
	_durationBadge = new DurationBadge(_thumb);
	_durationBadge->hide();

	if (_onContextMenu)
	{
		_thumb->setContextMenuPolicy(Qt::CustomContextMenu);
		connect(_thumb, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
			// _onContextMenu runs the menu's exec() synchronously, and a menu action (e.g. Labels/Delete) can
			// rebuild the grid and delete this very card mid-call - so guard with QPointer before touching it.
			QPointer<MediaItemWidget> self(this);
			_onContextMenu(_thumb->mapToGlobal(pos));
			if (self)
				clearStuckHoverIfCursorLeft(self);  // else the popup grab leaves #mediaItemCard:hover stuck on
		});
	}

	layout->addWidget(_thumb, 1);

	// Footer row: Best-star toggle, label dots, then the name (right-aligned, filling the rest).
	_footer = new QWidget(this);
	_footer->setObjectName("cardFooter");
	auto* footerLayout = new QHBoxLayout(_footer);
	footerLayout->setContentsMargins(0, 0, 0, 0);
	footerLayout->setSpacing(6);

	_starButton = new QPushButton("★", _footer);
	_starButton->setObjectName("cardStar");   // styled via #cardStar in the central sheet (Style.cpp)
	_starButton->setCheckable(true);
	_starButton->setChecked(inBest);
	_starButton->setFlat(true);
	_starButton->setCursor(Qt::PointingHandCursor);
	_starButton->setFocusPolicy(Qt::NoFocus);
	_starButton->setFixedSize(18, 18);
	connect(_starButton, &QPushButton::clicked, this, [onToggleBest = std::move(onToggleBest)]() {
		onToggleBest();
	});
	footerLayout->addWidget(_starButton, 0, Qt::AlignVCenter);

	// Label-dot strip. Hidden until setLabelDots() supplies colors.
	_labelDots = new LabelDotStrip(_footer);
	_labelDots->setObjectName("cardLabelDots");
	_labelDots->hide();
	footerLayout->addWidget(_labelDots, 0, Qt::AlignVCenter);

	auto* name = new ElidedLabel(_footer);
	name->setObjectName("cardName");
	name->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);  // takes the stretch space; elides within it
	QFont nameFont = name->font();
	nameFont.setPointSize(9);
	name->setFont(nameFont);
	name->setFullText(label);
	_name = name;
	footerLayout->addWidget(name, 1);

	layout->addWidget(_footer, 0);
}

void MediaItemWidget::setInBest(bool inBest)
{
	_starButton->setChecked(inBest);   // visual only - does not emit clicked(), so onToggleBest isn't re-invoked
}

void MediaItemWidget::setLabel(const QString& label)
{
	static_cast<ElidedLabel*>(_name)->setFullText(label);
}

void MediaItemWidget::setLabelDots(const std::vector<QColor>& colors, const QString& tooltip)
{
	auto* strip = static_cast<LabelDotStrip*>(_labelDots);
	strip->setColors(colors);            // updates its sizeHint; the footer layout re-fits it
	strip->setVisible(!colors.empty());
	_thumb->setToolTip(tooltip);        // the card's single tooltip, composed by the caller (video extraction state + labels)
}

void MediaItemWidget::setFramesExtracted(bool extracted)
{
	_framesReadyBadge->setVisible(extracted);
	if (extracted)
	{
		repositionFramesReadyBadge();
		_framesReadyBadge->raise();
	}
}

void MediaItemWidget::repositionFramesReadyBadge()
{
	static constexpr int MARGIN = 4;
	// On a film-strip card, drop below the top sprocket band so the badge sits over a frame, not the perforations.
	const int bandOffset = _filmStrip ? ThumbnailWidget::filmStripBandHeight(_thumb->height()) : 0;
	_framesReadyBadge->move(_thumb->width() - _framesReadyBadge->width() - MARGIN, MARGIN + bandOffset);
}

void MediaItemWidget::setDuration(qint64 durationMs)
{
	const bool show = durationMs > 0;
	_durationBadge->setVisible(show);
	if (!show)
		return;

	static_cast<DurationBadge*>(_durationBadge)->setText(formatDuration(durationMs));
	repositionDurationBadge();
	_durationBadge->raise();
}

void MediaItemWidget::repositionDurationBadge()
{
	static constexpr int MARGIN = 4;
	// On a film-strip card, lift above the bottom sprocket band so the pill sits over a frame, not the perforations.
	const int bandOffset = _filmStrip ? ThumbnailWidget::filmStripBandHeight(_thumb->height()) : 0;
	_durationBadge->move(_thumb->width() - _durationBadge->width() - MARGIN,
	                      _thumb->height() - _durationBadge->height() - MARGIN - bandOffset);
}

void MediaItemWidget::setOnMiddleButtonClick(std::function<void()> onClick)
{
	_onMiddleButtonClick = std::move(onClick);
	_thumb->installEventFilter(this); // idempotent; ensures filter is active
}

void MediaItemWidget::setOnMouseWheelCallback(std::function<void(int)> handler)
{
	_thumb->setOnMouseWheelCallback(std::move(handler));
}

void MediaItemWidget::setOnLabelDropped(std::function<void(const QString&)> handler)
{
	_onLabelDropped = std::move(handler);
}

void MediaItemWidget::dragEnterEvent(QDragEnterEvent* event)
{
	if (_onLabelDropped && event->mimeData()->hasFormat(LabelMimeType))
		event->acceptProposedAction();
}

void MediaItemWidget::dragMoveEvent(QDragMoveEvent* event)
{
	// Re-accept across the whole card (the child thumbnail doesn't accept drops, so events bubble up here).
	if (_onLabelDropped && event->mimeData()->hasFormat(LabelMimeType))
		event->acceptProposedAction();
}

void MediaItemWidget::dropEvent(QDropEvent* event)
{
	if (!_onLabelDropped || !event->mimeData()->hasFormat(LabelMimeType))
		return;

	const QString labelId = QString::fromUtf8(event->mimeData()->data(LabelMimeType));
	if (labelId.isEmpty())
		return;

	event->acceptProposedAction();
	_onLabelDropped(labelId);  // the handler defers the catalog mutation + grid rebuild (which deletes this card)
}

QSize MediaItemWidget::sizeHint() const
{
	if (!_thumb)
		return QWidget::sizeHint();

	// Width is driven by the thumbnail (the footer elides to fit). Height stacks thumbnail + spacing +
	// footer, all inside the card's border+padding margins.
	const QMargins m = contentsMargins();
	const QSize thumb = _thumb->sizeHint();
	const int footerHeight = _footer->sizeHint().height();
	return QSize(thumb.width() + m.left() + m.right(),
	            thumb.height() + CARD_FOOTER_SPACING + footerHeight + m.top() + m.bottom());
}

bool MediaItemWidget::eventFilter(QObject* watched, QEvent* event)
{
	if (watched != _thumb)
		return QWidget::eventFilter(watched, event);

	// Check for double click first, no need to do anything else in that case
	if (event->type() == QEvent::MouseButtonDblClick)
	{
		const auto* me = static_cast<QMouseEvent*>(event);

		if (me->button() == Qt::LeftButton && _onDoubleClick)
		{
			_onDoubleClick();
			return true;
		}
	}
	else if (event->type() == QEvent::MouseButtonRelease)
	{
		const auto* me = static_cast<QMouseEvent*>(event);
		if (me->button() == Qt::MiddleButton && _onMiddleButtonClick)
		{
			_onMiddleButtonClick();
			return true; // This is middle mouse button - OK to consume, doesn't interfere with context menu or dbl click
		}
	}
	else if (event->type() == QEvent::Resize || event->type() == QEvent::Show)
	{
		// Pin both corner overlays whenever the thumbnail gains real geometry. Unconditional, and on Show as
		// well as Resize: with a fixed size hint the thumbnail's only resize (0 -> final) can arrive while the
		// card is still hidden during grid layout, and no further resize follows on show - so a visibility- or
		// resize-only handler would leave a badge stuck at its pre-size position. Moving a hidden badge is a no-op.
		repositionFramesReadyBadge();
		repositionDurationBadge();
	}

	return QWidget::eventFilter(watched, event);
}
