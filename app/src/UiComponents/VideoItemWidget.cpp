#include "UiComponents/VideoItemWidget.h"
#include "UiComponents/LabelMimeType.h"
#include "UiComponents/ThumbnailWidget.h"
#include "Theme/Theme.h"
#include "Utils.h"

#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
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

	void setColors(const QList<QColor>& colors)
	{
		m_colors = colors;
		updateGeometry();   // sizeHint changed; let the footer layout re-fit the strip
		update();
	}

	[[nodiscard]] QSize sizeHint() const override
	{
		const int n = static_cast<int>(m_colors.size());
		if (n == 0)
			return { 0, 0 };
		return { n * DOT + (n - 1) * GAP, DOT };
	}

protected:
	void paintEvent(QPaintEvent*) override
	{
		if (m_colors.isEmpty())
			return;

		QPainter p{ this };
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);

		const int y = (height() - DOT) / 2;
		int x = 0;
		for (const QColor& c : m_colors)
		{
			p.setBrush(c.isValid() ? c : QColor("#888888"));  // unset label color -> neutral grey
			p.drawEllipse(x, y, DOT, DOT);
			x += DOT + GAP;
		}
	}

private:
	static constexpr int DOT = 8;  // dot diameter
	static constexpr int GAP = 4;  // spacing between dots

	QList<QColor> m_colors;
};

// A QLabel that elides its text to the available width, keeping the full string for the tooltip. Used for
// the footer video name so a long name truncates instead of forcing the card wider.
class ElidedLabel final : public QLabel {
public:
	using QLabel::QLabel;

	void setFullText(const QString& text)
	{
		m_full = text;
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
		setText(fontMetrics().elidedText(m_full, Qt::ElideRight, width()));
	}

	QString m_full;
};

// A tiny mouse-transparent badge marking a card whose video hasn't had its full frame set extracted yet
// (only its permanent preview frames exist so far) - see Catalog::isSplitIntoFrames. Mirrors LabelDotStrip's
// translucent-backdrop look but lives in the thumbnail's top-right corner instead of its top-left.
class SplitPendingBadge final : public QWidget {
public:
	explicit SplitPendingBadge(QWidget* parent) : QWidget(parent)
	{
		setAttribute(Qt::WA_TransparentForMouseEvents);
		setToolTip(tr("Only preview frames extracted so far - open the frame viewer to extract the rest"));
		resize(sizeHint());
	}

	[[nodiscard]] QSize sizeHint() const override { return { 2 * PAD + SIZE, 2 * PAD + SIZE }; }

protected:
	void paintEvent(QPaintEvent*) override
	{
		QPainter p{ this };
		p.setRenderHint(QPainter::Antialiasing);

		const double radius = (2 * PAD + SIZE) / 2.0;
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0, 0, 0, 90));
		p.drawRoundedRect(rect(), radius, radius);

		auto font = p.font();
		font.setPointSize(SIZE - 5);
		p.setFont(font);
		p.setPen(QColor(255, 255, 255, 220));
		p.drawText(rect(), Qt::AlignCenter, QStringLiteral("⏳"));
	}

private:
	static constexpr int SIZE = 16; // badge diameter
	static constexpr int PAD = 3;   // backdrop padding around the glyph
};

}  // namespace

VideoItemWidget::VideoItemWidget(
	QSize maxImageSize, const QStringList& previewPaths, const QString& label,
	const VideoId& videoId,
	bool inBest, std::function<void()> onToggleBest,
	std::function<void()> onDoubleClick,
	std::function<void(QPoint)> onContextMenu,
	bool dynamicSizeHint,
	QWidget* parent
)
	: QWidget{ parent }
	, m_videoId{ videoId }
	, m_onDoubleClick{ std::move(onDoubleClick) }
	, m_onContextMenu{ std::move(onContextMenu) }
{
	setObjectName("videoCard");
	setAcceptDrops(true);  // accept a label dragged from the sidebar (see dropEvent / setOnLabelDropped)

	// The card owns the frame: a bordered, rounded container that highlights on hover - styled by #videoCard in
	// the central sheet (Style.cpp), not per-instance (which polished slowly at grid scale). The thumbnail inside
	// is borderless (just the matte well); the footer below carries the star, label dots and name. The normal
	// background stays transparent so the grid's item-selection highlight shows through behind it.
	static constexpr int CARD_BORDER = 1;
	static constexpr int CARD_PADDING = 6;
	setAttribute(Qt::WA_StyledBackground);
	setAttribute(Qt::WA_Hover);   // card keeps its hover state even while the cursor is over a child widget
	setContentsMargins(CARD_BORDER + CARD_PADDING, CARD_BORDER + CARD_PADDING, CARD_BORDER + CARD_PADDING, CARD_BORDER + CARD_PADDING);

	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(CARD_FOOTER_SPACING);

	m_thumb = new ThumbnailWidget(previewPaths, QString(), this, maxImageSize, dynamicSizeHint, /*framed*/ false);
	m_thumb->installEventFilter(this);

	// "Not fully split" badge in the thumbnail's top-right corner. Hidden until setSplitPending(true).
	m_splitPendingBadge = new SplitPendingBadge(m_thumb);
	m_splitPendingBadge->hide();

	if (m_onContextMenu)
	{
		m_thumb->setContextMenuPolicy(Qt::CustomContextMenu);
		connect(m_thumb, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
			// m_onContextMenu runs the menu's exec() synchronously, and a menu action (e.g. Labels/Delete) can
			// rebuild the grid and delete this very card mid-call - so guard with QPointer before touching it.
			QPointer<VideoItemWidget> self(this);
			m_onContextMenu(m_thumb->mapToGlobal(pos));
			if (self)
				clearStuckHoverIfCursorLeft(self);  // else the popup grab leaves #videoCard:hover stuck on
		});
	}

	layout->addWidget(m_thumb, 1);

	// Footer row: Best-star toggle, label dots, then the name (right-aligned, filling the rest).
	m_footer = new QWidget(this);
	m_footer->setObjectName("cardFooter");
	auto* footerLayout = new QHBoxLayout(m_footer);
	footerLayout->setContentsMargins(0, 0, 0, 0);
	footerLayout->setSpacing(6);

	auto* starButton = new QPushButton("★", m_footer);
	starButton->setObjectName("cardStar");   // styled via #cardStar in the central sheet (Style.cpp)
	starButton->setCheckable(true);
	starButton->setChecked(inBest);
	starButton->setFlat(true);
	starButton->setCursor(Qt::PointingHandCursor);
	starButton->setFixedSize(18, 18);
	connect(starButton, &QPushButton::clicked, this, [onToggleBest = std::move(onToggleBest)]() {
		onToggleBest();
	});
	footerLayout->addWidget(starButton, 0, Qt::AlignVCenter);

	// Label-dot strip. Hidden until setLabelDots() supplies colors.
	m_labelDots = new LabelDotStrip(m_footer);
	m_labelDots->setObjectName("cardLabelDots");
	m_labelDots->hide();
	footerLayout->addWidget(m_labelDots, 0, Qt::AlignVCenter);

	auto* name = new ElidedLabel(m_footer);
	name->setObjectName("cardName");
	name->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);  // takes the stretch space; elides within it
	QFont nameFont = name->font();
	nameFont.setPointSize(9);
	name->setFont(nameFont);
	name->setFullText(label);
	m_name = name;
	footerLayout->addWidget(name, 1);

	layout->addWidget(m_footer, 0);
}

void VideoItemWidget::setLabel(const QString& label)
{
	static_cast<ElidedLabel*>(m_name)->setFullText(label);
}

void VideoItemWidget::setLabelDots(const QList<QColor>& colors, const QString& tooltip)
{
	auto* strip = static_cast<LabelDotStrip*>(m_labelDots);
	strip->setColors(colors);            // updates its sizeHint; the footer layout re-fits it
	strip->setVisible(!colors.isEmpty());
	m_thumb->setToolTip(tooltip);        // hovering the card lists its label names
}

void VideoItemWidget::setSplitPending(bool pending)
{
	m_splitPendingBadge->setVisible(pending);
	if (pending)
	{
		repositionSplitPendingBadge();
		m_splitPendingBadge->raise();
	}
}

void VideoItemWidget::repositionSplitPendingBadge()
{
	static constexpr int MARGIN = 4;
	m_splitPendingBadge->move(m_thumb->width() - m_splitPendingBadge->width() - MARGIN, MARGIN);
}

void VideoItemWidget::setOnMiddleButtonClick(std::function<void()> onClick)
{
	m_onMiddleButtonClick = std::move(onClick);
	m_thumb->installEventFilter(this); // idempotent; ensures filter is active
}

void VideoItemWidget::setOnMouseWheelCallback(std::function<void(int)> handler)
{
	m_thumb->setOnMouseWheelCallback(std::move(handler));
}

void VideoItemWidget::setOnLabelDropped(std::function<void(const QString&)> handler)
{
	m_onLabelDropped = std::move(handler);
}

void VideoItemWidget::dragEnterEvent(QDragEnterEvent* event)
{
	if (m_onLabelDropped && event->mimeData()->hasFormat(LabelMimeType))
		event->acceptProposedAction();
}

void VideoItemWidget::dragMoveEvent(QDragMoveEvent* event)
{
	// Re-accept across the whole card (the child thumbnail doesn't accept drops, so events bubble up here).
	if (m_onLabelDropped && event->mimeData()->hasFormat(LabelMimeType))
		event->acceptProposedAction();
}

void VideoItemWidget::dropEvent(QDropEvent* event)
{
	if (!m_onLabelDropped || !event->mimeData()->hasFormat(LabelMimeType))
		return;

	const QString labelId = QString::fromUtf8(event->mimeData()->data(LabelMimeType));
	if (labelId.isEmpty())
		return;

	event->acceptProposedAction();
	m_onLabelDropped(labelId);  // the handler defers the catalog mutation + grid rebuild (which deletes this card)
}

QSize VideoItemWidget::sizeHint() const
{
	if (!m_thumb)
		return QWidget::sizeHint();

	// Width is driven by the thumbnail (the footer elides to fit). Height stacks thumbnail + spacing +
	// footer, all inside the card's border+padding margins.
	const QMargins m = contentsMargins();
	const QSize thumb = m_thumb->sizeHint();
	const int footerHeight = m_footer->sizeHint().height();
	return QSize(thumb.width() + m.left() + m.right(),
	            thumb.height() + CARD_FOOTER_SPACING + footerHeight + m.top() + m.bottom());
}

bool VideoItemWidget::eventFilter(QObject* watched, QEvent* event)
{
	if (watched != m_thumb)
		return QWidget::eventFilter(watched, event);

	// Check for double click first, no need to do anything else in that case
	if (event->type() == QEvent::MouseButtonDblClick)
	{
		const auto* me = static_cast<QMouseEvent*>(event);

		if (me->button() == Qt::LeftButton && m_onDoubleClick)
		{
			m_onDoubleClick();
			return true;
		}
	}
	else if (event->type() == QEvent::MouseButtonRelease)
	{
		const auto* me = static_cast<QMouseEvent*>(event);
		if (me->button() == Qt::MiddleButton && m_onMiddleButtonClick)
		{
			m_onMiddleButtonClick();
			return true; // This is middle mouse button - OK to consume, doesn't interfere with context menu or dbl click
		}
	}
	else if (event->type() == QEvent::Resize && m_splitPendingBadge->isVisible())
	{
		repositionSplitPendingBadge();
	}

	return QWidget::eventFilter(watched, event);
}
