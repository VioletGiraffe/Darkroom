#include "Theme/Style.h"
#include "Theme/Theme.h"

#include "compiler/compiler_warnings_control.h"
#include "theme/cbasepalette.h"
#include "theme/cstylefixups.h"
#include "theme/cthemecontroller.h"
#include "theme/cthemeiconhandler.h"

DISABLE_COMPILER_WARNINGS
#include <QApplication>
#include <QWidget>
RESTORE_COMPILER_WARNINGS

#include <utility>

namespace {

constexpr char kButtons[] = R"(
	QPushButton {
		border: 1px solid %borderStrong%;
		border-radius: %controlRadius%px;
		padding: 4px 12px;
		background: palette(button);
	}
	/* QSS owns the button frame, so the native style's default-button ring is not drawn - mark it ourselves. */
	QPushButton:default { border-color: %accent%; }
	QPushButton:hover { border-color: %accent%; }
	QPushButton:pressed, QPushButton:checked { background: %accentBg%; border-color: %accent%; }

	QPushButton#addLabelButton {
		background: transparent;
		border: 2px dashed %borderStrong%;
		color: %text%;
		text-align: left;
		padding-left: 6px;
	}
	QPushButton#addLabelButton:hover { border-color: %accent%; color: %accent%; }
)";

constexpr char kTextInputs[] = R"(
	QLineEdit, QPlainTextEdit, QTextEdit {
		border: 1px solid %borderStrong%;
		border-radius: %controlRadius%px;
		padding: 4px 8px;
		background: palette(base);
		selection-background-color: %selectionBg%;
		selection-color: %selectionFg%;
	}
	/* Compensate for the thicker focus border to prevent layout movement. */
	QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus { border: 2px solid %accent%; padding: 3px 7px; }
)";

constexpr char kComboBoxes[] = R"(
	QComboBox {
		border: 1px solid %borderStrong%;
		border-radius: %controlRadius%px;
		padding: 3px 8px;
		background: palette(button);
	}
	/* QSS-owned combo frames do not receive a native focus ring. */
	QComboBox:hover { border-color: %accent%; }
	QComboBox:focus { border: 2px solid %accent%; padding: 2px 7px; }
	QComboBox::drop-down {
		subcontrol-origin: padding;
		subcontrol-position: center right;
		border: none;
		background: transparent;
		width: 18px;
	}
	QComboBox::down-arrow { image: url(%comboArrowIcon%); width: 10px; height: 7px; }
	QComboBox QAbstractItemView {
		border: 1px solid %border%;
		border-radius: %controlRadius%px;
		padding: 4px;
		background: palette(base);
		outline: none;
	}
	/* QStyleSheetStyle otherwise drops the native per-item margins. */
	QComboBox QAbstractItemView::item { padding: 5px 8px; }
)";

constexpr char kMenus[] = R"(
	QMenu {
		border: 1px solid %border%;
		border-radius: %menuRadius%px;
		padding: 4px;
		background: palette(window);
	}
	QMenu::item { border-radius: %menuItemRadius%px; padding: 5px 24px 5px 12px; }
	QMenu::item:selected { background: %accentBg%; color: %accentText%; }
	QMenu::separator { height: 1px; background: %borderSubtle%; margin: 4px 8px; }
)";

constexpr char kMenuBar[] = R"(
	QMenuBar { background: transparent; }
	QMenuBar::item { background: transparent; border-radius: %menuItemRadius%px; padding: 4px 10px; }
	QMenuBar::item:selected { background: %accentBg%; color: %accentText%; }
	QMenuBar::item:pressed { background: %accentBg%; color: %accentText%; }
)";

constexpr char kLists[] = R"(
	QListWidget { border: 1px solid %border%; border-radius: %controlRadius%px; background: transparent; }
)";

constexpr char kScrollBars[] = R"(
	QScrollBar:vertical { border: none; background: transparent; width: %scrollBarThickness%px; margin: 0; }
	QScrollBar::handle:vertical { background: %textDim%; border-radius: %scrollBarHandleRadius%px; min-height: 24px; }
	QScrollBar::handle:vertical:hover { background: %accent%; }
	QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
	QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }

	QScrollBar:horizontal { border: none; background: transparent; height: %scrollBarThickness%px; margin: 0; }
	QScrollBar::handle:horizontal { background: %textDim%; border-radius: %scrollBarHandleRadius%px; min-width: 24px; }
	QScrollBar::handle:horizontal:hover { background: %accent%; }
	QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
	QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }
)";

// QStyleSheetStyle does not render slider tick marks.
constexpr char kSliders[] = R"(
	QSlider::groove:horizontal { border: none; height: %sliderGrooveThickness%px; background: %borderSubtle%; border-radius: %sliderGrooveRadius%px; }
	QSlider::sub-page:horizontal { background: %accent%; border-radius: %sliderGrooveRadius%px; }
	QSlider::handle:horizontal {
		width: %sliderHandleContentWidth%px;
		margin: -%sliderHandleOverhang%px 0;
		background: palette(button);
		border: 1px solid %borderStrong%;
		border-radius: %sliderHandleRadius%px;
	}
	QSlider::handle:horizontal:hover { border-color: %accent%; }
	QSlider::handle:horizontal:pressed { background: %accentBg%; border-color: %accent%; }
)";

constexpr char kCheckBoxes[] = R"(
	QCheckBox { spacing: 8px; }
	QCheckBox::indicator {
		width: 14px; height: 14px;
		border: 1px solid %borderStrong%;
		border-radius: %checkboxRadius%px;
		background: transparent;
	}
	QCheckBox::indicator:hover { border-color: %accent%; }
	QCheckBox::indicator:checked {
		background: %selectionBg%;
		border-color: %selectionBg%;
		image: url(%checkboxCheckIcon%);
	}
)";

constexpr char kGroupBoxes[] = R"(
	QGroupBox {
		border: 1px solid %borderSubtle%;
		border-radius: %controlRadius%px;
		margin-top: 1ex;
		padding-top: 2px;
	}
	QGroupBox::title {
		subcontrol-origin: margin;
		subcontrol-position: top left;
		left: 8px;
		padding: 0 3px;
		background-color: palette(window);
		color: %instructionText%;
	}
)";

constexpr char kSplitters[] = R"(
	QSplitter::handle { background: transparent; }
	QSplitter::handle:hover, QSplitter::handle:pressed { background: %accentBg%; }
)";

constexpr char kToolTips[] = R"(
	QToolTip {
		background-color: %windowBg%;
		color: %text%;
		border: 1px solid %border%;
		padding: 4px 8px;
	}
)";

// Per-card stylesheets create a QStyleSheetStyle proxy per widget and make grid rebuilds expensive.
constexpr char kGridCards[] = R"(
	QWidget#mediaItemCard { background: transparent; border: 1px solid %borderSubtle%; border-radius: %controlRadius%px; }
	QWidget#mediaItemCard:hover { background-color: %accentBg%; border-color: %accent%; }

	QWidget#cardThumbnailWell { background-color: %thumbnailMatte%; border: none; border-radius: %thumbnailMatteRadius%px; font-size: 9pt; }

	QWidget#framedThumbnail { background-color: %thumbnailMatte%; border: 1px solid %borderSubtle%; border-radius: %controlRadius%px; padding: 2px; font-size: 9pt; }
	QWidget#framedThumbnail:hover { background-color: %accentBg%; border-color: %accent%; }
)";

constexpr char kStarButtons[] = R"(
	QPushButton#cardStar { border: none; background: transparent; font-size: 11pt; color: %starButtonDefault%; padding: 0; }
	QPushButton#cardStar:checked { color: %starActive%; background: transparent; }
	QPushButton#cardStar:hover:!checked { color: %starButtonHoverUnchecked%; }
	QPushButton#cardStar:checked:hover { color: %starButtonCheckedHover%; }
)";

QString styleSheetString()
{
	const Theme::Theme& t = Theme::current();

	QString sheet;
	for (const char* section : { kButtons, kTextInputs, kComboBoxes, kMenus, kMenuBar, kLists, kScrollBars,
	                             kSliders, kCheckBoxes, kGroupBoxes, kSplitters, kToolTips, kGridCards, kStarButtons })
		sheet += QLatin1String(section);
	sheet += t.qssFragment; // last, so a theme's rules win equal-specificity ties; tokens work here too

	const Theme::Metrics& m = t.metrics;
	const std::pair<QString, QString> tokens[] = {
		{ QStringLiteral("%border%"),                   t.palette.border.name() },
		{ QStringLiteral("%borderSubtle%"),             t.palette.borderSubtle.name() },
		{ QStringLiteral("%borderStrong%"),             t.palette.borderStrong.name() },
		{ QStringLiteral("%accent%"),                   t.palette.accent.name() },
		{ QStringLiteral("%accentBg%"),                 t.palette.accentBg.name() },
		{ QStringLiteral("%accentText%"),               t.palette.accentText.name() },
		{ QStringLiteral("%selectionBg%"),              t.palette.selectionBg.name() },
		{ QStringLiteral("%selectionFg%"),              t.palette.selectionFg.name() },
		{ QStringLiteral("%textDim%"),                  t.palette.textDim.name() },
		{ QStringLiteral("%instructionText%"),          t.instructionText.name() },
		{ QStringLiteral("%windowBg%"),                 t.palette.windowBg.name() },
		{ QStringLiteral("%text%"),                     t.palette.text.name() },
		{ QStringLiteral("%thumbnailMatte%"),           t.thumbnailMatte.name() },
		{ QStringLiteral("%starButtonDefault%"),        t.starButtonDefault.name() },
		{ QStringLiteral("%starButtonHoverUnchecked%"), t.starButtonHoverUnchecked.name() },
		{ QStringLiteral("%starButtonCheckedHover%"),   t.starButtonCheckedHover.name() },
		{ QStringLiteral("%starActive%"),               t.starActive.name() },
		{ QStringLiteral("%controlRadius%"),            QString::number(m.controlRadius) },
		{ QStringLiteral("%menuRadius%"),               QString::number(m.menuRadius) },
		{ QStringLiteral("%menuItemRadius%"),           QString::number(m.menuItemRadius) },
		{ QStringLiteral("%checkboxRadius%"),           QString::number(m.checkboxRadius) },
		{ QStringLiteral("%thumbnailMatteRadius%"),     QString::number(m.thumbnailMatteRadius) },
		{ QStringLiteral("%scrollBarThickness%"),       QString::number(m.scrollBarThickness) },
		{ QStringLiteral("%scrollBarHandleRadius%"),    QString::number(m.scrollBarHandleRadius()) },
		{ QStringLiteral("%sliderGrooveThickness%"),    QString::number(m.sliderGrooveThickness) },
		{ QStringLiteral("%sliderGrooveRadius%"),       QString::number(m.sliderGrooveRadius()) },
		{ QStringLiteral("%sliderHandleRadius%"),       QString::number(m.sliderHandleRadius()) },
		{ QStringLiteral("%sliderHandleContentWidth%"), QString::number(m.sliderHandleDiameter - 2) },
		{ QStringLiteral("%sliderHandleOverhang%"),     QString::number((m.sliderHandleDiameter - m.sliderGrooveThickness) / 2) },
		// Monochrome sources tinted per theme and served by CThemeIconHandler - QSS url() only takes a path.
		{ QStringLiteral("%comboArrowIcon%"),           themeIconUrl(QStringLiteral("combobox_down_arrow"), t.palette.text) },
		{ QStringLiteral("%checkboxCheckIcon%"),        themeIconUrl(QStringLiteral("checkbox_check"), t.palette.selectionFg) },
	};
	for (const auto& [token, value] : tokens)
		sheet.replace(token, value);

	return sheet;
}

void apply()
{
	Theme::selectActiveTheme();
	qApp->setPalette(qtPaletteFor(Theme::current().palette));
	qApp->setStyleSheet(styleSheetString());
}

} // namespace

namespace Style {

void install()
{
	// Serves the tinted QSS glyphs; must exist before any stylesheet references themeicon:/ URLs
	// and for the application's whole lifetime.
	static const CThemeIconHandler iconHandler{ QStringLiteral(":/UI") };

	// Install the proxy before QSS wraps it. The providers resolve through Theme::current() at each
	// use, so all three fixups follow theme switches with nothing to re-install.
	qApp->setStyle(new CFocusFrameStyle{ [] { return Theme::current().metrics.focusRectOutset; } });
	qApp->installEventFilter(new CComboPopupRounder{ [] {
		const Theme::Theme& t = Theme::current();
		return CComboPopupRounder::Frame{ .borderColor = t.palette.border, .radius = qreal(t.metrics.controlRadius) };
	}, qApp });
	qApp->installEventFilter(new CSplitterHandleHoverEnabler{ qApp });

	QObject::connect(&CThemeController::instance(), &CThemeController::themeChanged, qApp, &apply);
	apply();
}

void applyThemedSheet(QWidget* widget, std::function<QString()> makeSheet)
{
	widget->setStyleSheet(makeSheet());
	QObject::connect(&CThemeController::instance(), &CThemeController::themeChanged, widget,
	                 [widget, makeSheet = std::move(makeSheet)] { widget->setStyleSheet(makeSheet()); });
}

} // namespace Style
