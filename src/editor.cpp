/** @fileoverview Handles screenshot selection, annotation, and editor drawing.
 */
#include "editor.hpp"

#include "stitch.hpp"
#include "icons.hpp"
#include "eyedropper.hpp"
#include "output-config.hpp"
#include "overlay-chrome.hpp"
#include "palette-config.hpp"
#include "recent-snaps.hpp"
#include "scroll-capture.hpp"
#include "text-band.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QRandomGenerator>
#include <QScreen>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextLayout>
#include <QTextDocument>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <numbers>

/// The inline text editor: a multiline editor whose native caret is hidden (the
/// editor paints a shorter one over the active font's line box) and whose caret
/// rectangle is exposed for that. The caret is hidden through cursorWidth
/// because QPlainTextEdit never consults PM_TextCursorWidth, so a proxy style
/// zeroing that metric leaves the widget's own caret showing under the
/// painted one.
class InlineTextEdit final : public QPlainTextEdit {
public:
  explicit InlineTextEdit(QWidget *parent) : QPlainTextEdit(parent) {
    setCursorWidth(0);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Wrap like the committed text will: at word boundaries, anywhere within
    // a word too long to fit. The width clamp decides when wrapping bites.
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    document()->setDocumentMargin(0);
  }
  using QPlainTextEdit::cursorRect;
  using QPlainTextEdit::setViewportMargins;
};

namespace {
constexpr std::array<qreal, 3> kTextSizes{2.0, 5.0, 9.0};
constexpr std::array<const char *, 3> kTextSizeNames{"S", "M", "L"};
constexpr qreal kToolbarWidth = 840;
constexpr qreal kToolbarImageGap = 46;
constexpr qreal kMinimumRedactionExtent = 5.0;
constexpr int kBackdropDim = 143;

qreal highlighterPreviewHeight(qreal annotationSize) {
  return std::max<qreal>(6.0, annotationSize * 3.0);
}

QRectF highlighterIBeamBounds(const QPointF &center, qreal height,
                              qreal scale) {
  const qreal safeScale = std::max<qreal>(scale, 0.01);
  const qreal serifHalfWidth =
      std::clamp(height * 0.18, 5.0 / safeScale, 9.0 / safeScale);
  return {center.x() - serifHalfWidth, center.y() - height / 2.0,
          serifHalfWidth * 2.0, height};
}

void paintHighlighterIBeam(QPainter &painter, const QPointF &center,
                           qreal height, qreal scale) {
  const qreal safeScale = std::max<qreal>(scale, 0.01);
  const QRectF bounds = highlighterIBeamBounds(center, height, safeScale);
  const qreal spineHalfWidth = 1.5 / safeScale;
  const qreal serifHeight = std::min(
      height / 2.0,
      std::clamp(height * 0.08, 2.0 / safeScale, 4.0 / safeScale));
  const qreal innerTop = bounds.top() + serifHeight;
  const qreal innerBottom = bounds.bottom() - serifHeight;

  QPainterPath beam;
  beam.moveTo(bounds.topLeft());
  beam.lineTo(bounds.topRight());
  beam.lineTo(bounds.right(), innerTop);
  beam.lineTo(center.x() + spineHalfWidth, innerTop);
  beam.lineTo(center.x() + spineHalfWidth, innerBottom);
  beam.lineTo(bounds.right(), innerBottom);
  beam.lineTo(bounds.bottomRight());
  beam.lineTo(bounds.bottomLeft());
  beam.lineTo(bounds.left(), innerBottom);
  beam.lineTo(center.x() - spineHalfWidth, innerBottom);
  beam.lineTo(center.x() - spineHalfWidth, innerTop);
  beam.lineTo(bounds.left(), innerTop);
  beam.closeSubpath();

  painter.save();
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setBrush(QColor(255, 255, 255, 242));
  painter.setPen(QPen(QColor(0, 0, 0, 217), 1.6 / safeScale,
                      Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
  painter.drawPath(beam);
  painter.restore();
}

/// Beside the snapshots and the instance lock, in the private runtime dir.
/// Session scratch, not configuration: gone at reboot, and a region only
/// means anything for the screen it was drawn on. Mirrors the scroll
/// capture's stored region; the two can share helpers once both are in.
QString storedCaptureRegionPath() {
  const QString runtime = secureRuntimeDirectory();
  if (runtime.isEmpty())
    return {};
  return QDir(runtime).filePath(QStringLiteral("capture-region"));
}

QString formatStoredRegion(const QString &monitor, const QSize &surface,
                           const QRect &region) {
  return QStringLiteral("%1 %2 %3 %4 %5 %6 %7")
      .arg(monitor.isEmpty() ? QStringLiteral("?") : monitor)
      .arg(surface.width())
      .arg(surface.height())
      .arg(region.x())
      .arg(region.y())
      .arg(region.width())
      .arg(region.height());
}

/// The region in `line`, or a null rect when it was written for a different
/// monitor or surface size, does not fit, or is not what we wrote. Anything
/// unreadable is simply not offered; there is nothing to migrate.
QRect parseStoredRegion(const QString &line, const QString &monitor,
                        const QSize &surface) {
  const QStringList fields = line.trimmed().split(QLatin1Char(' '));
  if (fields.size() != 7)
    return {};
  if (fields.at(0) != (monitor.isEmpty() ? QStringLiteral("?") : monitor))
    return {};
  bool ok = true;
  int values[6] = {};
  for (int index = 0; index < 6; ++index) {
    bool field = false;
    values[index] = fields.at(index + 1).toInt(&field);
    ok = ok && field;
  }
  if (!ok || QSize(values[0], values[1]) != surface)
    return {};
  const QRect region(values[2], values[3], values[4], values[5]);
  if (region.width() < 32 || region.height() < 32 ||
      !QRect(QPoint(), surface).contains(region))
    return {};
  return region;
}
constexpr qreal kNudgeStep = 1.0;
constexpr qreal kNudgeStepShift = 10.0;

/** Keeps a downward submenu alive while the pointer travels toward it. */
bool inDownwardSubmenuTriangle(const QPointF &origin, const QRectF &submenu,
                               const QPointF &pointer) {
  if (origin.isNull() || pointer.y() < origin.y() ||
      pointer.y() > submenu.bottom() + 6.0)
    return false;
  QPolygonF triangle;
  triangle << origin << submenu.bottomLeft() + QPointF(-8, 6)
           << submenu.bottomRight() + QPointF(8, 6);
  return triangle.containsPoint(pointer, Qt::OddEvenFill);
}
/// Arrow presses closer together than this share one undo entry, so a held
/// key reads as one move.
constexpr qint64 kNudgeCoalesceMs = 100;
/// How long after the last wheel notch the selection chrome stays faint. Long
/// enough to cover a run of notches, short enough that it is back before the
/// hand has left the wheel.
constexpr int kAdjustSettleMs = 400;
/// How long the recents shelf takes to fan out or fold back.
constexpr int kRecentsFanMs = 180;
/// Box a shelf card fits in (logical px): small enough to stay out of the way.
constexpr qreal kRecentCardWidth = 112.0;
constexpr qreal kRecentCardHeight = 84.0;
constexpr qreal kRecentCardGap = 10.0;
constexpr qreal kRecentEdgeMargin = 18.0;

qreal toolbarScale(qreal availableWidth) {
  constexpr qreal sideMargins = 16.0;
  return std::min<qreal>(
      1.0,
      std::max<qreal>(0.1, (availableWidth - sideMargins) / kToolbarWidth));
}
// A spotlight at 1x is not a failed zoom, it is a plain highlight: the dimming
// still isolates the region. Name that state so it reads as somewhere to stop
// rather than the bottom of a range.
/// The whole state of a spotlight in one line. Adjusting one of the three
/// settings still reports all three: they interact (a ring reads differently
/// at 3× than at 1×), and the two you are not touching should not have to be
/// remembered or re-discovered by pressing keys to find out.
QString spotlightStatus(SpotlightShape shape, qreal magnification,
                        qreal border) {
  const QString shapeName = shape == SpotlightShape::Ellipse
                                ? QStringLiteral("ellipse")
                            : shape == SpotlightShape::Rectangle
                                ? QStringLiteral("rectangle")
                                : QStringLiteral("rounded");
  const QString zoom =
      magnification <= 1.0
          ? QStringLiteral("no zoom")
          : QStringLiteral("%1×").arg(magnification, 0, 'f', 1);
  const QString ring = border <= 0.0
                           ? QStringLiteral("no border")
                           : QStringLiteral("border %1").arg(qRound(border));
  return QStringLiteral("Spotlight · %1 · %2 · %3 · S cycles shape · wheel "
                        "zooms · Alt+wheel border")
      .arg(shapeName, zoom, ring);
}

} // namespace

QString spotlightStatusForTest(SpotlightShape shape, qreal magnification,
                               qreal border) {
  return spotlightStatus(shape, magnification, border);
}

namespace {
bool hasEndpointHandles(Annotation::Kind kind) {
  return kind == Annotation::Kind::Arrow || kind == Annotation::Kind::Line ||
         kind == Annotation::Kind::Rectangle ||
         kind == Annotation::Kind::Ellipse ||
         kind == Annotation::Kind::Redaction ||
         kind == Annotation::Kind::Spotlight;
}

/// Any handle drag on a layer (as opposed to a move, or a capture crop).
bool isLayerResize(CaptureEditor::Interaction interaction) {
  return interaction >= CaptureEditor::Interaction::ResizeStart &&
         interaction <= CaptureEditor::Interaction::ResizeLeft;
}

/// The eight handles that reshape a box, as opposed to the two ends of a line.
bool isBoxResize(CaptureEditor::Interaction interaction) {
  return interaction >= CaptureEditor::Interaction::ResizeTopLeft &&
         interaction <= CaptureEditor::Interaction::ResizeLeft;
}

/// How far from an edge a press still counts as grabbing it: wide enough to
/// hit without aiming, since some layers are grabbable only by their border.
constexpr qreal kEdgeGrabTolerance = 12.0;

bool showsSelectionBounds(Annotation::Kind kind) {
  return kind != Annotation::Kind::Arrow && kind != Annotation::Kind::Line;
}

bool isStrokeKind(Annotation::Kind kind) {
  return kind == Annotation::Kind::Freehand ||
         kind == Annotation::Kind::Highlighter;
}

qreal strokeHitTolerance(const Annotation &annotation) {
  if (annotation.kind == Annotation::Kind::Highlighter)
    return std::max<qreal>(8.0, annotation.size * 3.0 + 4.0);
  return std::max<qreal>(8.0, annotation.size + 4.0);
}
void translateAnnotation(Annotation &annotation, const QPointF &delta) {
  annotation.start += delta;
  if (hasEndpointHandles(annotation.kind) ||
      annotation.kind == Annotation::Kind::Freehand)
    annotation.end += delta;
  if (annotation.curveControl)
    *annotation.curveControl += delta;
  if (isStrokeKind(annotation.kind)) {
    for (QPointF &point : annotation.points)
      point += delta;
    if (annotation.kind == Annotation::Kind::Freehand) {
      for (QPointF &point : annotation.rawPoints)
        point += delta;
    }
  }
}

/// A redaction's mosaic seed; zero is reserved for "not yet seeded".
quint32 freshRedactionSeed() {
  const quint32 seed = QRandomGenerator::system()->generate();
  return seed == 0 ? 1 : seed;
}

/// Offset for a duplicated layer: down-left by default, flipped per axis
/// when that would push the copy off the canvas and the other way fits.
QPointF duplicateOffset(const QRectF &bounds, const QRectF &canvas) {
  constexpr qreal step = 100.0;
  qreal dx = -step;
  qreal dy = step;
  if (bounds.left() + dx < canvas.left() &&
      bounds.right() - dx <= canvas.right())
    dx = -dx;
  if (bounds.bottom() + dy > canvas.bottom() &&
      bounds.top() - dy >= canvas.top())
    dy = -dy;
  return {dx, dy};
}

/// Endpoint that keeps the original |dx|:|dy| ratio around the fixed
/// endpoint while a bounding-box shape is resized with Shift; the axis the
/// user has scaled more wins and the other follows.
QPointF aspectLockedEndpoint(const QPointF &fixed,
                             const QPointF &originalMoving,
                             const QPointF &candidate) {
  const QPointF original = originalMoving - fixed;
  if (qFuzzyIsNull(original.x()) || qFuzzyIsNull(original.y()))
    return candidate;
  const QPointF offset = candidate - fixed;
  const qreal scale = std::max(std::abs(offset.x()) / std::abs(original.x()),
                               std::abs(offset.y()) / std::abs(original.y()));
  const qreal signX = offset.x() < 0 ? -1.0 : 1.0;
  const qreal signY = offset.y() < 0 ? -1.0 : 1.0;
  return fixed + QPointF(signX * scale * std::abs(original.x()),
                         signY * scale * std::abs(original.y()));
}

/// Unit move for an arrow key; null for any other key.
QPointF arrowKeyDelta(int key, qreal step) {
  switch (key) {
  case Qt::Key_Left:
    return {-step, 0};
  case Qt::Key_Right:
    return {step, 0};
  case Qt::Key_Up:
    return {0, -step};
  case Qt::Key_Down:
    return {0, step};
  default:
    return {};
  }
}

bool supportsCreationConstraint(CaptureEditor::Tool tool) {
  return tool == CaptureEditor::Tool::Arrow ||
         tool == CaptureEditor::Tool::Line ||
         tool == CaptureEditor::Tool::Rectangle ||
         tool == CaptureEditor::Tool::Ellipse ||
         tool == CaptureEditor::Tool::Spotlight;
}

bool supportsCenteredCreation(CaptureEditor::Tool tool) {
  return tool == CaptureEditor::Tool::Rectangle ||
         tool == CaptureEditor::Tool::Ellipse ||
         tool == CaptureEditor::Tool::Spotlight;
}

QString toolAction(CaptureEditor::Tool tool) {
  switch (tool) {
  case CaptureEditor::Tool::Select:
    return QStringLiteral("tool-select");
  case CaptureEditor::Tool::Arrow:
    return QStringLiteral("tool-arrow");
  case CaptureEditor::Tool::Line:
    return QStringLiteral("tool-line");
  case CaptureEditor::Tool::Spotlight:
    return QStringLiteral("tool-spotlight");
  case CaptureEditor::Tool::Freehand:
    return QStringLiteral("tool-freehand");
  case CaptureEditor::Tool::Highlighter:
    return QStringLiteral("tool-highlighter");
  case CaptureEditor::Tool::Marker:
    return QStringLiteral("tool-marker");
  case CaptureEditor::Tool::Rectangle:
    return QStringLiteral("tool-rectangle");
  case CaptureEditor::Tool::Ellipse:
    return QStringLiteral("tool-ellipse");
  case CaptureEditor::Tool::Redact:
    return QStringLiteral("tool-redact");
  case CaptureEditor::Tool::Cut:
    return QStringLiteral("tool-cut");
  case CaptureEditor::Tool::Text:
    return QStringLiteral("tool-text");
  case CaptureEditor::Tool::Ocr:
    return QStringLiteral("tool-ocr");
  case CaptureEditor::Tool::Eyedropper:
    return QStringLiteral("tool-eyedropper");
  }
  return {};
}

// Annotation kind a drag-to-create tool previews and commits (OCR only
// previews its rectangle).
Annotation::Kind dragShapeKind(CaptureEditor::Tool tool) {
  switch (tool) {
  case CaptureEditor::Tool::Rectangle:
  case CaptureEditor::Tool::Ocr:
    return Annotation::Kind::Rectangle;
  case CaptureEditor::Tool::Ellipse:
    return Annotation::Kind::Ellipse;
  case CaptureEditor::Tool::Line:
    return Annotation::Kind::Line;
  case CaptureEditor::Tool::Arrow:
  default:
    return Annotation::Kind::Arrow;
  }
}

constexpr qreal kCornerRadiusStep = 2.0;
constexpr qreal kMaximumCornerRadius = 24.0;

QString fillName(bool filled) {
  return filled ? QStringLiteral("Filled") : QStringLiteral("Hollow");
}

QString cornerName(qreal radius) {
  return radius > 0.0 ? QStringLiteral("%1 px corners").arg(qRound(radius))
                      : QStringLiteral("square corners");
}

TextBackground nextTextBackground(TextBackground background) {
  switch (background) {
  case TextBackground::Pill:
    return TextBackground::Outline;
  case TextBackground::Outline:
    return TextBackground::Plain;
  case TextBackground::Plain:
    return TextBackground::Pill;
  }
  return TextBackground::Pill;
}

QString textBackgroundName(TextBackground background) {
  switch (background) {
  case TextBackground::Pill:
    return QStringLiteral("Pill");
  case TextBackground::Outline:
    return QStringLiteral("Outline");
  case TextBackground::Plain:
    return QStringLiteral("Plain");
  }
  return QStringLiteral("Pill");
}

TextFont nextTextFont(TextFont textFont) {
  switch (textFont) {
  case TextFont::Neucha:
    return TextFont::JetBrainsMono;
  case TextFont::JetBrainsMono:
    return TextFont::InterDisplay;
  case TextFont::InterDisplay:
    return TextFont::Neucha;
  }
  return TextFont::Neucha;
}

QString redactionStyleName(RedactionStyle style) {
  return style == RedactionStyle::Solid ? QStringLiteral("Solid")
                                        : QStringLiteral("Pixelate");
}

QString arrowStyleName(ArrowStyle style) {
  switch (style) {
  case ArrowStyle::Standard:
    return QStringLiteral("Standard");
  case ArrowStyle::Pointy:
    return QStringLiteral("Pointy");
  case ArrowStyle::Curved:
    return QStringLiteral("Curved");
  case ArrowStyle::Double:
    return QStringLiteral("Double");
  }
  return QStringLiteral("Standard");
}

QString arrowToolAction(ArrowStyle style) {
  switch (style) {
  case ArrowStyle::Standard:
    return QStringLiteral("tool-arrow-standard");
  case ArrowStyle::Pointy:
    return QStringLiteral("tool-arrow-pointy");
  case ArrowStyle::Curved:
    return QStringLiteral("tool-arrow-curved");
  case ArrowStyle::Double:
    return QStringLiteral("tool-arrow-double");
  }
  return QStringLiteral("tool-arrow-standard");
}

ArrowStyle nextArrowStyle(ArrowStyle style) {
  switch (style) {
  case ArrowStyle::Standard:
    return ArrowStyle::Pointy;
  case ArrowStyle::Pointy:
    return ArrowStyle::Curved;
  case ArrowStyle::Curved:
    return ArrowStyle::Double;
  case ArrowStyle::Double:
    return ArrowStyle::Standard;
  }
  return ArrowStyle::Standard;
}

qreal constrainedRedactionCoordinate(qreal candidate, qreal fixed,
                                     qreal original) {
  return original <= fixed
             ? std::min(candidate, fixed - kMinimumRedactionExtent)
             : std::max(candidate, fixed + kMinimumRedactionExtent);
}

QPointF constrainedRedactionEndpoint(const QPointF &candidate,
                                     const QPointF &fixed,
                                     const QPointF &original) {
  return {
      constrainedRedactionCoordinate(candidate.x(), fixed.x(), original.x()),
      constrainedRedactionCoordinate(candidate.y(), fixed.y(), original.y())};
}

/// One top-to-bottom pass of the OCR scan band.
constexpr qint64 kOcrSweepMs = 1200;

void drawInstantTooltip(QPainter &painter, const QRect &bounds,
                        const QRectF &anchor, const QString &text) {
  if (text.isEmpty())
    return;
  QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
  font.setPixelSize(12);
  painter.setFont(font);
  const qreal width = painter.fontMetrics().horizontalAdvance(text) + 20;
  const qreal height = 28;
  qreal x = std::clamp(anchor.center().x() - width / 2.0, 8.0,
                       std::max(8.0, bounds.width() - width - 8.0));
  qreal y = anchor.top() - height - 7;
  if (y < 6)
    y = anchor.bottom() + 7;
  const QRectF pill(x, y, width, height);
  painter.setPen(QPen(QColor(255, 255, 255, 42), 1));
  painter.setBrush(QColor(12, 12, 15, 248));
  painter.drawRoundedRect(pill, 7, 7);
  painter.setPen(Qt::white);
  painter.drawText(pill, Qt::AlignCenter, text);
}

/**
 * Formats a native pixel size for the measurement readout. The number is the
 * pixel count the export actually carries, which on a scaled monitor is not
 * the logical size the pointer moves through.
 */
QString formatPixelSize(const QSizeF &size) {
  return QStringLiteral("%1 × %2")
      .arg(qRound(size.width()))
      .arg(qRound(size.height()));
}

QString formatPixelPoint(const QPointF &point) {
  return QStringLiteral("%1, %2").arg(qRound(point.x())).arg(qRound(point.y()));
}

/**
 * Draws the measurement readout below-right of the pointer, flipping to the
 * opposite side instead of clipping at an overlay edge. Digits use the fixed
 * font so a live drag does not jitter the pill width per frame.
 */
void drawMeasureBadge(QPainter &painter, const QRect &bounds,
                      const QPointF &cursor, const QString &text) {
  if (text.isEmpty())
    return;
  QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  font.setPixelSize(12);
  font.setBold(true);
  painter.setFont(font);
  const qreal width = painter.fontMetrics().horizontalAdvance(text) + 16;
  constexpr qreal height = 22;
  constexpr qreal gap = 15;
  constexpr qreal margin = 6;
  qreal x = cursor.x() + gap;
  if (x + width > bounds.width() - margin)
    x = cursor.x() - gap - width;
  qreal y = cursor.y() + gap;
  if (y + height > bounds.height() - margin)
    y = cursor.y() - gap - height;
  const QRectF pill(
      std::clamp(x, margin, std::max(margin, bounds.width() - width - margin)),
      std::clamp(y, margin,
                 std::max(margin, bounds.height() - height - margin)),
      width, height);
  painter.setPen(QPen(QColor(255, 255, 255, 42), 1));
  painter.setBrush(QColor(12, 12, 15, 235));
  painter.drawRoundedRect(pill, 6, 6);
  painter.setPen(Qt::white);
  painter.drawText(pill, Qt::AlignCenter, text);
}

QString backgroundName(BackgroundStyle style) {
  switch (style) {
  case BackgroundStyle::None:
    return QStringLiteral("None");
  case BackgroundStyle::Off:
    return QStringLiteral("Off");
  case BackgroundStyle::Slate:
    return QStringLiteral("Window gray");
  case BackgroundStyle::Aurora:
    return QStringLiteral("Aurora");
  case BackgroundStyle::Sunset:
    return QStringLiteral("Sunset");
  case BackgroundStyle::Lagoon:
    return QStringLiteral("Lagoon");
  case BackgroundStyle::Violet:
    return QStringLiteral("Violet");
  }
  return {};
}

QString canvasBoundaryName(CanvasBoundaryMode mode) {
  switch (mode) {
  case CanvasBoundaryMode::Framed:
    return QStringLiteral("Framed");
  case CanvasBoundaryMode::Overflow:
    return QStringLiteral("Overflow");
  case CanvasBoundaryMode::Image:
    return QStringLiteral("Image");
  }
  return {};
}
} // namespace

QPointF constrainedCreationEndpoint(CaptureEditor::Tool tool,
                                    const QPointF &start, const QPointF &end) {
  const QPointF delta = end - start;
  if (tool == CaptureEditor::Tool::Rectangle ||
      tool == CaptureEditor::Tool::Ellipse ||
      tool == CaptureEditor::Tool::Spotlight) {
    const qreal extent = std::max(std::abs(delta.x()), std::abs(delta.y()));
    if (qFuzzyIsNull(extent))
      return end;
    const qreal xDirection = delta.x() < 0 ? -1.0 : 1.0;
    const qreal yDirection = delta.y() < 0 ? -1.0 : 1.0;
    return start + QPointF(xDirection * extent, yDirection * extent);
  }
  if (tool == CaptureEditor::Tool::Arrow || tool == CaptureEditor::Tool::Line) {
    const qreal length = QLineF(start, end).length();
    if (qFuzzyIsNull(length))
      return end;
    constexpr qreal angleStep = std::numbers::pi_v<qreal> / 4.0;
    const qreal angle =
        std::round(std::atan2(delta.y(), delta.x()) / angleStep) * angleStep;
    return start + QPointF(std::cos(angle) * length, std::sin(angle) * length);
  }
  return end;
}

QPointF centeredCreationStart(CaptureEditor::Tool tool, const QPointF &center,
                              const QPointF &end) {
  if (!supportsCenteredCreation(tool))
    return center;
  return center * 2.0 - end;
}

CaptureEditor::CaptureEditor(CaptureData capture, CaptureMode mode,
                             QuickOutputMode quickOutput, OperationLog log,
                             QWidget *parent)
    : QWidget(parent), capture_(std::move(capture)),
      quickOutputMode_(quickOutput) {
  pristineSource_ = capture_.source;
  pristineLogicalSize_ = capture_.previewSize;
  paletteConfig_ = loadPaletteConfig(defaultConfigPath());
  customColor_ = paletteConfig_.custom;
  if (!log.ops.isEmpty()) {
    ops_ = std::move(log.ops);
    opIndex_ = std::clamp(log.index, 0, static_cast<int>(ops_.size()));
    nextAnnotationId_ = std::max<quint64>(log.nextId, 1);
    nextMarker_ = std::max(log.nextMarker, 1);
  }
  setWindowTitle(QStringLiteral("Omasnap"));
  setWindowFlags(Qt::Window | Qt::FramelessWindowHint |
                 Qt::WindowStaysOnTopHint);
  setAttribute(Qt::WA_TranslucentBackground);
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);

  for (QScreen *screen : QGuiApplication::screens()) {
    if (screen->name() == capture_.monitor.name) {
      setGeometry(screen->geometry());
      break;
    }
  }
  if (geometry().isEmpty())
    setGeometry(QGuiApplication::primaryScreen()->geometry());
  cursor_ = mapFromGlobal(QCursor::pos());

  textEditor_ = new InlineTextEdit(this);
  textEditor_->hide();
  textEditor_->setViewportMargins(0, 0, 0, 0);
  textEditor_->setStyleSheet(
      QStringLiteral("QPlainTextEdit { color: #ff375f; background: transparent; "
                     "border: none; padding: 0;"
                     " selection-background-color: #0a84ff; }"));
  textEditor_->installEventFilter(this);
  // The editor paints its own, shorter caret (see paintEdit); blink it.
  textCaretTimer_.setInterval(530);
  connect(&textCaretTimer_, &QTimer::timeout, this, [this] {
    textCaretOn_ = !textCaretOn_;
    update(textEditor_->geometry().adjusted(-4, -4, 4, 4));
  });
  connect(textEditor_, &QPlainTextEdit::cursorPositionChanged, this, [this] {
    textCaretOn_ = true;
    textCaretTimer_.start();
    update();
  });
  // A run of nudges persists once, shortly after the last key, instead of
  // re-rendering the snapshot on every auto-repeat.
  nudgePersistTimer_.setSingleShot(true);
  nudgePersistTimer_.setInterval(kNudgeCoalesceMs);
  connect(&nudgePersistTimer_, &QTimer::timeout, this,
          [this] { endNudgeRun(); });
  connect(textEditor_, &QPlainTextEdit::textChanged, this, [this] {
        const QString text = textEditor_->toPlainText();
        const QFontMetrics metrics(textEditor_->font());
        int widestLine = 0;
        const QStringList lines = text.split('\n');
        for (const QString &line : lines)
          widestLine = std::max(widestLine,
                                metrics.horizontalAdvance(line + QStringLiteral("  ")));
        const int sidePadding = textEditPill_
                                    ? qRound(std::max(4.0, metrics.height() * 0.18))
                                    : 0;
        const qreal scale = std::max<qreal>(editScale(), 0.001);
        const qreal sourceRoom = selection_.width() - textPoint_.x();
        const qreal logicalWrap =
            textEditWrapWidth_ > 0.0
                ? textEditWrapWidth_
                : (sourceRoom >= kMinimumTextWrapWidth ? sourceRoom : 0.0);
        const int naturalWidth =
            std::max(48, widestLine + sidePadding * 2);
        const int wrappedWidth =
            logicalWrap > 0.0
                ? std::max(48, qRound(logicalWrap * scale) + sidePadding * 2)
                : naturalWidth;
        // The draft uses the same source-frame wrap rule as committed text.
        // A grown matte must not make the label reflow when it is committed,
        // and an explicit logical width must scale with the current view.
        const int width = textEditWrapWidth_ > 0.0
                              ? wrappedWidth
                              : std::min(naturalWidth, wrappedWidth);
          textEditor_->resize(width, textEditor_->height());
          // QPlainTextEdit needs a little more than QFontMetrics::height(): its
          // block layout keeps leading/descent outside the nominal line box.
          // Wrapped lines are not the newline count either, so the laid-out
          // document is the only thing that knows how tall the draft is now.
          const int wrapped =
              std::max(1, qRound(textEditor_->document()->size().height()));
          const int desiredHeight =
              wrapped * metrics.lineSpacing() + metrics.descent() + 4;
          textEditor_->resize(width, desiredHeight);
        textEditor_->verticalScrollBar()->setValue(0);
        QTimer::singleShot(0, textEditor_, [editor = textEditor_] {
          editor->verticalScrollBar()->setValue(0);
        });
        update();
      });

  textCardEditor_ = new QPlainTextEdit(this);
  textCardEditor_->hide();
  textCardEditor_->setFrameShape(QFrame::NoFrame);
  textCardEditor_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  textCardEditor_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  textCardEditor_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
  textCardEditor_->setWordWrapMode(
      QTextOption::WrapAtWordBoundaryOrAnywhere);
  textCardEditor_->setTabChangesFocus(false);
  textCardEditor_->setContentsMargins(0, 0, 0, 0);
  textCardEditor_->document()->setDocumentMargin(0.0);
  textCardEditor_->installEventFilter(this);
  connect(textCardEditor_, &QPlainTextEdit::textChanged, this,
          [this] { updateClipboardTextCardPreview(); });

  textCardFilenameEditor_ = new QLineEdit(this);
  textCardFilenameEditor_->hide();
  textCardFilenameEditor_->setFrame(false);
  textCardFilenameEditor_->setMaxLength(240);
  textCardFilenameEditor_->installEventFilter(this);
  connect(textCardFilenameEditor_, &QLineEdit::textEdited, this,
          [this](const QString &filename) {
            if (!textCardFilenameEditor_->isVisible())
              return;
            textCardFilename_ = filename.trimmed();
            updateClipboardTextCardPreview();
          });

  ocrAnimTimer_.setInterval(16);
  connect(&ocrAnimTimer_, &QTimer::timeout, this, [this] { update(); });
  ocrResultTimer_.setSingleShot(true);
  ocrResultTimer_.setInterval(6000);
  connect(&ocrResultTimer_, &QTimer::timeout, this,
          [this] { dismissOcrOverlay(); });
  connect(&ocrWatcher_, &QFutureWatcher<OcrResult>::finished, this, [this] {
    const OcrResult result = ocrWatcher_.result();
    busy_ = false;
    if (!result.error.isEmpty()) {
      dismissOcrOverlay();
      setStatus(result.error);
      return;
    }
    QString clipboardError;
    if (!copyTextToClipboard(result.text, clipboardError)) {
      dismissOcrOverlay();
      setStatus(clipboardError);
      return;
    }
    const QString shown = result.text.trimmed();
    if (shown.isEmpty()) {
      dismissOcrOverlay();
      setStatus(QStringLiteral("No text found in that area"));
      return;
    }
    setStatus(QStringLiteral("OCR copied to clipboard"));
    sendCaptureNotification(QStringLiteral("Copied text from screenshot"));
    // Let the scan band finish the sweep it is on (and always at least one
    // full pass) before the card appears: a result that pops up mid-sweep
    // reads as a glitch, however fast tesseract was.
    const qint64 elapsed = ocrClock_.elapsed();
    const qint64 sweeps =
        std::max<qint64>(1, (elapsed + kOcrSweepMs - 1) / kOcrSweepMs);
    const int wait = static_cast<int>(sweeps * kOcrSweepMs - elapsed);
    QTimer::singleShot(wait, this, [this, shown] {
      if (ocrRegion_.isEmpty() || ocrWatcher_.isRunning())
        return; // dismissed, or a newer OCR took over the region
      // The animation clock keeps ticking so the result card can fade out.
      ocrResultText_ = shown;
      ocrClock_.restart();
      ocrResultTimer_.start();
      update();
    });
  });

  connect(&finishWatcher_, &QFutureWatcher<FinishResult>::finished, this,
          [this] { completeFinish(finishWatcher_.result()); });

  connect(&snapshotWatcher_, &QFutureWatcher<bool>::finished, this, [this] {
    snapshotBusy_ = false;
    snapshotWriteOk_ = snapshotWatcher_.result();
    if (snapshotWriteOk_ && QFile::exists(snapshotPath_))
      sourceWritten_ = true;
    // Let the cut finish before chaining another snapshot so persistence sees
    // its final committed operation rather than an intermediate interaction.
    if (snapshotDirty_ && !cutDragActive_)
      startSnapshotRender();
  });

  connect(&captureWatcher_, &QFutureWatcher<CaptureJob>::finished, this,
          [this] {
            capturePending_ = false;
            const CaptureJob job = captureWatcher_.result();
            if (!job.ok) {
              setStatus(QStringLiteral("Screen capture failed"));
              emit captureReady(false, job.error);
              update();
              return;
            }
            capture_ = job.capture;
            liveMonitor_ = capture_.monitor;
            pristineSource_ = capture_.source;
            pristineLogicalSize_ = capture_.previewSize;
            cuts_.clear();
            redactionBaseStale_ = true;
            switch (pendingMode_) {
            case CaptureMode::Fullscreen:
              selection_ = QRectF(QPointF(), capture_.previewSize);
              editedKind_ = SelectTab::Fullscreen;
              enterEdit(QStringLiteral(
                  "Full screen selected · native resolution · outer handles "
                  "crop"));
              break;
            case CaptureMode::Window:
              windowMode_ = true;
              hoveredWindow_ = windowAt(cursor_);
              setStatus(QStringLiteral(
                  "Window mode · click or Super+Arrows then Enter · Space "
                  "returns to area"));
              break;
            case CaptureMode::Region:
              setStatus(QStringLiteral(
                  "Drag to select an area · Space selects a window"));
              break;
            case CaptureMode::Scroll:
              scrollMode_ = true;
              setStatus(QStringLiteral("Drag to select a scrolling region · "
                                       "the page inside stays live"));
              break;
            case CaptureMode::File:
              break;
            }
            updatePointerCursor();
            update();
            emit captureReady(true, {});
          });

  connect(&pinWatcher_, &QFutureWatcher<QImage>::finished, this, [this] {
    pinPending_ = false;
    const QString path = pendingPinPath_;
    const QImage image = pinWatcher_.result();
    QString error;
    if (image.isNull() ||
        !savePinnedSnapshot(image, path, pendingPinLogicalSize_, error)) {
      --pinCount_;
      setStatus(error.isEmpty()
                    ? QStringLiteral("Could not render pinned capture")
                    : error);
      return;
    }
    if (!QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                 {QStringLiteral("--pin"), path})) {
      QFile::remove(path);
      QFile::remove(operationLogPath(path));
      --pinCount_;
      setStatus(QStringLiteral("Could not start pinned capture"));
      return;
    }
    close();
  });

  captureMode_ = mode;
  liveMonitor_ = capture_.monitor;
  if (capture_.source.isNull()) {
    // The pixel capture has not landed yet; the overlay shows a Capturing…
    // state until startCapture() completes.
    capturePending_ = true;
    pendingMode_ = mode;
    setStatus(QStringLiteral("Capturing screen…"));
  } else if (mode == CaptureMode::Fullscreen || mode == CaptureMode::File) {
    editedKind_ = SelectTab::Fullscreen;
    if (ops_.isEmpty())
      selection_ = QRectF(QPointF(), capture_.previewSize);
    else
      replayLog();
    // A very long capture edits and saves here exactly as usual, so say what
    // actually differs: other software may refuse to open the file. Said once,
    // on opening, where it is useful, not as an alarm during capture.
    const bool veryLong =
        capture_.previewSize.width() > stitch::kWidelyOpenableEdge ||
        capture_.previewSize.height() > stitch::kWidelyOpenableEdge;
    enterEdit(
        veryLong
            ? QStringLiteral("Very long capture (%1 × %2) · edits and saves "
                             "here as usual, but many apps cannot open images "
                             "this large · crop it if you need it elsewhere")
                  .arg(capture_.previewSize.width())
                  .arg(capture_.previewSize.height())
        : mode == CaptureMode::File
            ? QStringLiteral("Editing image from file · Copy/Save to output")
            : QStringLiteral("Full screen selected · native resolution · "
                             "outer handles crop"));
  } else if (mode == CaptureMode::Window) {
    windowMode_ = true;
    hoveredWindow_ = windowAt(cursor_);
    setStatus(QStringLiteral("Window mode · click or Super+Arrows then Enter · "
                             "Space returns to area"));
  } else if (mode == CaptureMode::Scroll) {
    scrollMode_ = true;
    setStatus(QStringLiteral(
        "Drag to select a scrolling region · the page inside stays live"));
  }
  adjustSettleTimer_.setSingleShot(true);
  adjustSettleTimer_.setInterval(kAdjustSettleMs);
  connect(&adjustSettleTimer_, &QTimer::timeout, this, [this] {
    adjustingSelection_ = false;
    update();
  });

  recentsAnimTimer_.setInterval(16);
  connect(&recentsAnimTimer_, &QTimer::timeout, this, [this] {
    const qreal t = std::min(
        1.0, recentsAnimClock_.elapsed() / static_cast<qreal>(kRecentsFanMs));
    const qreal eased = 1.0 - std::pow(1.0 - t, 3.0);
    const qreal target = recentsOpen_ ? 1.0 : 0.0;
    recentsFan_ = recentsFanFrom_ + (target - recentsFanFrom_) * eased;
    if (t >= 1.0)
      recentsAnimTimer_.stop();
    update();
  });
  connect(&recentsWatcher_, &QFutureWatcher<QVector<RecentSnap>>::finished,
          this, [this] {
            recentsLoading_ = false;
            recents_ = recentsWatcher_.result();
            if (phase_ == Phase::Select)
              update();
          });
  // The shelf belongs to the select overlay only: a file being edited has no
  // select phase, and quick output never shows one long enough to use it.
  if (mode != CaptureMode::File && quickOutputMode_ == QuickOutputMode::None)
    loadRecents();
  updatePointerCursor();
}

CaptureEditor::~CaptureEditor() {
  // Never remove the working snapshot under an in-flight write; drain the
  // current render (dropping any coalesced follow-up) before cleanup.
  snapshotDirty_ = false;
  waitForSnapshot();
  const QString runtime = secureRuntimeDirectory();
  const auto removeWorking = [&](const QString &path) {
    if (path.isEmpty() || !QFile::exists(path))
      return;
    if (!runtime.isEmpty() && QFileInfo(path).absolutePath() == runtime)
      QFile::remove(path);
  };
  removeWorking(snapshotPath_);
  removeWorking(workingLogPath());
}

bool CaptureEditor::eventFilter(QObject *watched, QEvent *event) {
  if (watched == textCardFilenameEditor_ &&
      event->type() == QEvent::KeyPress) {
    auto *key = static_cast<QKeyEvent *>(event);
    if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
      endClipboardTextCardFilenameEdit(true);
      return true;
    }
    if (key->key() == Qt::Key_Escape) {
      endClipboardTextCardFilenameEdit(false);
      return true;
    }
  } else if (watched == textCardFilenameEditor_ &&
             event->type() == QEvent::FocusOut) {
    QTimer::singleShot(0, this, [this] {
      if (textCardFilenameEditor_->isVisible() &&
          !textCardFilenameEditor_->hasFocus())
        endClipboardTextCardFilenameEdit(true);
    });
  } else if (watched == textCardEditor_ && event->type() == QEvent::KeyPress) {
    auto *key = static_cast<QKeyEvent *>(event);
    if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
        key->modifiers().testFlag(Qt::ControlModifier)) {
      finishClipboardTextCard();
      return true;
    }
    if (!textCardInsertMode_)
      return handleClipboardTextCardNormalKey(key);
    if (key->key() == Qt::Key_Escape) {
      setClipboardTextCardInsertMode(false);
      return true;
    }
    if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) {
      QTextCursor cursor = textCardEditor_->textCursor();
      const bool outdent = key->key() == Qt::Key_Backtab ||
                           key->modifiers().testFlag(Qt::ShiftModifier);
      if (!outdent) {
        cursor.insertText(QStringLiteral("\t"));
      } else if (!cursor.hasSelection()) {
        const int original = cursor.position();
        const int lineStart = cursor.block().position();
        int remove = 0;
        while (remove < kTextCardTabSpaces && original - remove > lineStart) {
          cursor.setPosition(original - remove - 1);
          cursor.setPosition(original - remove, QTextCursor::KeepAnchor);
          const QString previous = cursor.selectedText();
          if (previous == QStringLiteral("\t")) {
            remove = 1;
            break;
          }
          if (previous != QStringLiteral(" ")) {
            remove = 0;
            break;
          }
          ++remove;
        }
        if (remove > 0) {
          cursor.setPosition(original - remove);
          cursor.setPosition(original, QTextCursor::KeepAnchor);
          cursor.removeSelectedText();
        }
      }
      return true;
    }
  } else if (watched == textEditor_ && event->type() == QEvent::KeyPress) {
    auto *key = static_cast<QKeyEvent *>(event);
    if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
      if (key->modifiers().testFlag(Qt::ControlModifier)) {
        acceptText();
        return true;
      }
      const int lineCount =
          std::max(1, static_cast<int>(textEditor_->document()->blockCount()));
      if (key->modifiers().testFlag(Qt::ShiftModifier)) {
        // Shift+Enter always makes room for one more line.
        textLineCapacity_ = std::max(textLineCapacity_, lineCount) + 1;
        textEditor_->insertPlainText(QStringLiteral("\n"));
        return true;
      }
      // Plain Enter fills the box line by line and commits on the last one.
      if (lineCount < textLineCapacity_) {
        textEditor_->insertPlainText(QStringLiteral("\n"));
        return true;
      }
      acceptText();
      return true;
    }
    if (key->key() == Qt::Key_Escape) {
      // Esc keeps what was typed, but leaves the layer selected so a stray
      // entry is one Backspace away from gone.
      acceptText(true);
      return true;
    }
  } else if (watched == textEditor_ && event->type() == QEvent::FocusOut) {
    QTimer::singleShot(0, this, [this] {
      if (textEditor_->isVisible() && !textEditor_->hasFocus())
        acceptText();
    });
  }
  return QWidget::eventFilter(watched, event);
}

void CaptureEditor::setClipboardTextCardInsertMode(bool insertMode) {
  if (!clipboardTextCardEditing_)
    return;
  textCardInsertMode_ = insertMode;
  textCardVisualLineMode_ = false;
  textCardVisualAnchor_ = -1;
  textCardVisualPosition_ = -1;
  textCardVisualSelectionStart_ = -1;
  textCardVisualSelectionEnd_ = -1;
  textCardEditor_->setExtraSelections({});
  textCardPendingCommand_ = {};
  updateClipboardTextCardEditorGeometry();
  setStatus(insertMode
                ? QStringLiteral("Text card · INSERT · Esc Normal · "
                                 "Ctrl+Enter renders")
                : QStringLiteral("Text card · NORMAL · hjkl move · i/a/o "
                                 "insert · F filename · Ctrl+Enter renders · "
                                 "q cancels"));
  updateClipboardTextCardPreview();
}

QString CaptureEditor::clipboardTextCardMode() const {
  if (textCardInsertMode_)
    return QStringLiteral("INSERT");
  if (textCardVisualAnchor_ >= 0)
    return textCardVisualLineMode_ ? QStringLiteral("VISUAL LINE")
                                   : QStringLiteral("VISUAL");
  return QStringLiteral("NORMAL");
}

void CaptureEditor::startClipboardTextCardVisualMode(bool linewise) {
  textCardPendingCommand_ = {};
  textCardVisualLineMode_ = linewise;
  const int lastCharacter = textCardEditor_->toPlainText().size() - 1;
  const int position =
      lastCharacter < 0
          ? 0
          : std::clamp(textCardEditor_->textCursor().position(), 0,
                       lastCharacter);
  textCardVisualAnchor_ = position;
  textCardVisualPosition_ = position;
  updateClipboardTextCardVisualSelection();
  setStatus(linewise
                ? QStringLiteral("Text card · VISUAL LINE · hjkl move · y "
                                 "copies · x/d delete · Esc Normal")
                : QStringLiteral("Text card · VISUAL · hjkl move · y copies "
                                 "· x/d delete · Esc Normal"));
  updateClipboardTextCardPreview();
}

void CaptureEditor::updateClipboardTextCardVisualSelection() {
  const int textLength = textCardEditor_->toPlainText().size();
  if (textLength <= 0) {
    textCardVisualSelectionStart_ = 0;
    textCardVisualSelectionEnd_ = 0;
    textCardEditor_->setExtraSelections({});
    textCardEditor_->setTextCursor(QTextCursor(textCardEditor_->document()));
    return;
  }

  textCardVisualAnchor_ =
      std::clamp(textCardVisualAnchor_, 0, textLength - 1);
  textCardVisualPosition_ =
      std::clamp(textCardVisualPosition_, 0, textLength - 1);
  textCardVisualSelectionStart_ =
      std::min(textCardVisualAnchor_, textCardVisualPosition_);
  textCardVisualSelectionEnd_ =
      std::max(textCardVisualAnchor_, textCardVisualPosition_) + 1;
  if (textCardVisualLineMode_) {
    const QTextBlock anchorBlock =
        textCardEditor_->document()->findBlock(textCardVisualAnchor_);
    const QTextBlock activeBlock =
        textCardEditor_->document()->findBlock(textCardVisualPosition_);
    const QTextBlock first = anchorBlock.position() <= activeBlock.position()
                                 ? anchorBlock
                                 : activeBlock;
    const QTextBlock last = anchorBlock.position() <= activeBlock.position()
                                ? activeBlock
                                : anchorBlock;
    textCardVisualSelectionStart_ = first.position();
    textCardVisualSelectionEnd_ =
        std::min(textLength, last.position() + last.length());
  }
  QTextEdit::ExtraSelection highlight;
  highlight.cursor = QTextCursor(textCardEditor_->document());
  highlight.cursor.setPosition(textCardVisualSelectionStart_);
  highlight.cursor.setPosition(textCardVisualSelectionEnd_,
                               QTextCursor::KeepAnchor);
  highlight.format.setBackground(textCardTheme_.selection);
  if (textCardVisualLineMode_)
    highlight.format.setProperty(QTextFormat::FullWidthSelection, true);
  textCardEditor_->setExtraSelections({highlight});

  // The highlight covers complete lines, but this real cursor remains at its
  // motion column so j/k feels like Vim instead of jumping to a line edge.
  QTextCursor active(textCardEditor_->document());
  active.setPosition(textCardVisualPosition_);
  textCardEditor_->setTextCursor(active);
}

void CaptureEditor::leaveClipboardTextCardVisualMode(int cursorPosition) {
  const int textLength = textCardEditor_->toPlainText().size();
  textCardVisualLineMode_ = false;
  textCardVisualAnchor_ = -1;
  textCardVisualPosition_ = -1;
  textCardVisualSelectionStart_ = -1;
  textCardVisualSelectionEnd_ = -1;
  textCardEditor_->setExtraSelections({});
  textCardPendingCommand_ = {};
  QTextCursor cursor(textCardEditor_->document());
  cursor.setPosition(std::clamp(cursorPosition, 0, textLength));
  textCardEditor_->setTextCursor(cursor);
  setStatus(QStringLiteral("Text card · NORMAL · hjkl move · i/a/o insert · "
                           "v selects · F filename · Ctrl+Enter renders · "
                           "q cancels"));
  updateClipboardTextCardPreview();
}

bool CaptureEditor::handleClipboardTextCardVisualKey(QKeyEvent *key) {
  const QString command = key->text();
  const bool shifted = key->modifiers().testFlag(Qt::ShiftModifier);
  if (key->key() == Qt::Key_Escape) {
    leaveClipboardTextCardVisualMode(textCardVisualPosition_);
    return true;
  }
  if (key->modifiers() &
      (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
    textCardPendingCommand_ = {};
    return true;
  }
  if (command == QStringLiteral("y") || command == QStringLiteral("d") ||
      command == QStringLiteral("x")) {
    const int first = textCardVisualSelectionStart_;
    const int last = textCardVisualSelectionEnd_;
    const QString source = textCardEditor_->toPlainText();
    const QString text = source.mid(first, last - first);
    textCardYank_ = text;
    textCardYankLinewise_ = textCardVisualLineMode_;
    if (command == QStringLiteral("y")) {
      QGuiApplication::clipboard()->setText(text, QClipboard::Clipboard);
      leaveClipboardTextCardVisualMode(first);
      setStatus(QStringLiteral("Text card · NORMAL · yanked %1 character%2 to "
                               "clipboard · cursor returned to selection start")
                    .arg(text.size())
                    .arg(text.size() == 1 ? QString() : QStringLiteral("s")));
      return true;
    }

    QTextCursor deletion(textCardEditor_->document());
    int deletionStart = first;
    if (textCardVisualLineMode_ && last == source.size() && first > 0)
      deletionStart = first - 1;
    deletion.beginEditBlock();
    deletion.setPosition(deletionStart);
    deletion.setPosition(last, QTextCursor::KeepAnchor);
    deletion.removeSelectedText();
    deletion.endEditBlock();
    const int remainingLength =
        static_cast<int>(textCardEditor_->toPlainText().size());
    leaveClipboardTextCardVisualMode(std::min(first, remainingLength));
    setStatus(QStringLiteral("Text card · NORMAL · selection deleted · "
                             "p puts it back · u undoes"));
    return true;
  }
  if (key->key() == Qt::Key_V) {
    if (shifted == textCardVisualLineMode_) {
      leaveClipboardTextCardVisualMode(textCardVisualPosition_);
    } else {
      textCardVisualLineMode_ = shifted;
      updateClipboardTextCardVisualSelection();
      setStatus(shifted
                    ? QStringLiteral("Text card · VISUAL LINE · hjkl move · y "
                                     "copies · x/d delete · Esc Normal")
                    : QStringLiteral("Text card · VISUAL · hjkl move · y "
                                     "copies · x/d delete · Esc Normal"));
      updateClipboardTextCardPreview();
    }
    return true;
  }

  QTextCursor motion(textCardEditor_->document());
  motion.setPosition(textCardVisualPosition_);
  if (textCardPendingCommand_ == QLatin1Char('g')) {
    textCardPendingCommand_ = {};
    if (command == QStringLiteral("g"))
      motion.movePosition(QTextCursor::Start);
    else
      return true;
  } else if (command == QStringLiteral("g")) {
    textCardPendingCommand_ = QLatin1Char('g');
    return true;
  } else if (shifted && key->key() == Qt::Key_G) {
    motion.movePosition(QTextCursor::End);
  } else if (command == QStringLiteral("h")) {
    motion.movePosition(QTextCursor::PreviousCharacter);
  } else if (command == QStringLiteral("j")) {
    motion.movePosition(QTextCursor::Down);
  } else if (command == QStringLiteral("k")) {
    motion.movePosition(QTextCursor::Up);
  } else if (command == QStringLiteral("l")) {
    motion.movePosition(QTextCursor::NextCharacter);
  } else if (command == QStringLiteral("0")) {
    motion.movePosition(QTextCursor::StartOfBlock);
  } else if (command == QStringLiteral("$")) {
    motion.movePosition(QTextCursor::EndOfBlock);
    if (!motion.block().text().isEmpty())
      motion.movePosition(QTextCursor::PreviousCharacter);
  } else if (command == QStringLiteral("w")) {
    motion.movePosition(QTextCursor::NextWord);
  } else if (command == QStringLiteral("b")) {
    motion.movePosition(QTextCursor::PreviousWord);
  } else {
    textCardPendingCommand_ = {};
    return true;
  }
  const int lastCharacter = textCardEditor_->toPlainText().size() - 1;
  textCardVisualPosition_ =
      lastCharacter < 0 ? 0 : std::clamp(motion.position(), 0, lastCharacter);
  updateClipboardTextCardVisualSelection();
  return true;
}

void CaptureEditor::joinClipboardTextCardLines() {
  QTextCursor cursor = textCardEditor_->textCursor();
  const QTextBlock block = cursor.block();
  const QTextBlock next = block.next();
  if (!next.isValid())
    return;

  const QString nextText = next.text();
  int leadingSpace = 0;
  while (leadingSpace < nextText.size() &&
         nextText.at(leadingSpace).isSpace())
    ++leadingSpace;
  const int joinPosition = block.position() + block.text().size();
  const bool needsSpace = !block.text().isEmpty() &&
                          !block.text().back().isSpace() &&
                          leadingSpace < nextText.size();
  cursor.beginEditBlock();
  cursor.setPosition(joinPosition);
  cursor.setPosition(next.position() + leadingSpace, QTextCursor::KeepAnchor);
  cursor.removeSelectedText();
  if (needsSpace)
    cursor.insertText(QStringLiteral(" "));
  cursor.endEditBlock();
  cursor.setPosition(joinPosition);
  textCardEditor_->setTextCursor(cursor);
  setStatus(QStringLiteral("Text card · NORMAL · joined lines · u undoes"));
}

void CaptureEditor::yankClipboardTextCardLine() {
  const QTextCursor cursor = textCardEditor_->textCursor();
  textCardYank_ = cursor.block().text() + QLatin1Char('\n');
  textCardYankLinewise_ = true;
  QGuiApplication::clipboard()->setText(textCardYank_, QClipboard::Clipboard);
  setStatus(QStringLiteral("Text card · NORMAL · line yanked to clipboard · "
                           "p below · P above"));
}

void CaptureEditor::putClipboardTextCard(bool before) {
  QString text = textCardYank_;
  bool linewise = textCardYankLinewise_;
  if (text.isEmpty()) {
    text = QGuiApplication::clipboard()->text(QClipboard::Clipboard);
    linewise = false;
  }
  if (text.isEmpty())
    return;

  QTextCursor cursor = textCardEditor_->textCursor();
  cursor.beginEditBlock();
  int placedAt = cursor.position();
  if (linewise) {
    if (text.endsWith(QLatin1Char('\n')))
      text.chop(1);
    if (before) {
      cursor.movePosition(QTextCursor::StartOfBlock);
      placedAt = cursor.position();
      cursor.insertText(text + QLatin1Char('\n'));
    } else {
      cursor.movePosition(QTextCursor::EndOfBlock);
      placedAt = cursor.position() + 1;
      cursor.insertText(QLatin1Char('\n') + text);
    }
  } else {
    if (!before)
      cursor.movePosition(QTextCursor::NextCharacter);
    placedAt = cursor.position();
    cursor.insertText(text);
  }
  cursor.endEditBlock();
  cursor.setPosition(placedAt);
  textCardEditor_->setTextCursor(cursor);
  setStatus(linewise
                ? QStringLiteral("Text card · NORMAL · put %1 current line")
                      .arg(before ? QStringLiteral("above")
                                  : QStringLiteral("below"))
                : QStringLiteral("Text card · NORMAL · text put %1 cursor")
                      .arg(before ? QStringLiteral("before")
                                  : QStringLiteral("after")));
}

bool CaptureEditor::handleClipboardTextCardNormalKey(QKeyEvent *key) {
  if (textCardVisualAnchor_ >= 0)
    return handleClipboardTextCardVisualKey(key);
  if (key->modifiers().testFlag(Qt::ControlModifier) &&
      key->key() == Qt::Key_R) {
    textCardEditor_->redo();
    textCardPendingCommand_ = {};
    return true;
  }
  if (key->modifiers() &
      (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
    textCardPendingCommand_ = {};
    return true;
  }

  QTextCursor cursor = textCardEditor_->textCursor();
  const QString command = key->text();
  const bool shifted = key->modifiers().testFlag(Qt::ShiftModifier);
  if (shifted && key->key() == Qt::Key_J) {
    textCardPendingCommand_ = {};
    joinClipboardTextCardLines();
    return true;
  }
  if (shifted && key->key() == Qt::Key_F) {
    textCardPendingCommand_ = {};
    beginClipboardTextCardFilenameEdit();
    return true;
  }
  if (key->key() == Qt::Key_P) {
    textCardPendingCommand_ = {};
    putClipboardTextCard(shifted);
    return true;
  }
  if (shifted && key->key() == Qt::Key_G) {
    textCardPendingCommand_ = {};
    cursor.movePosition(QTextCursor::End);
    textCardEditor_->setTextCursor(cursor);
    return true;
  }
  if (shifted && key->key() == Qt::Key_I) {
    textCardPendingCommand_ = {};
    const QTextBlock block = cursor.block();
    int first = 0;
    while (first < block.text().size() && block.text().at(first).isSpace())
      ++first;
    cursor.setPosition(block.position() + first);
    textCardEditor_->setTextCursor(cursor);
    setClipboardTextCardInsertMode(true);
    return true;
  }
  if (shifted && key->key() == Qt::Key_A) {
    textCardPendingCommand_ = {};
    cursor.movePosition(QTextCursor::EndOfBlock);
    textCardEditor_->setTextCursor(cursor);
    setClipboardTextCardInsertMode(true);
    return true;
  }
  if (shifted && key->key() == Qt::Key_O) {
    textCardPendingCommand_ = {};
    cursor.movePosition(QTextCursor::StartOfBlock);
    const int blankLine = cursor.position();
    cursor.insertText(QStringLiteral("\n"));
    cursor.setPosition(blankLine);
    textCardEditor_->setTextCursor(cursor);
    setClipboardTextCardInsertMode(true);
    return true;
  }
  if (textCardPendingCommand_ == QLatin1Char('g')) {
    textCardPendingCommand_ = {};
    if (command == QStringLiteral("g"))
      cursor.movePosition(QTextCursor::Start);
    textCardEditor_->setTextCursor(cursor);
    return true;
  }
  if (textCardPendingCommand_ == QLatin1Char('d')) {
    textCardPendingCommand_ = {};
    if (command == QStringLiteral("d")) {
      textCardYank_ = cursor.block().text() + QLatin1Char('\n');
      textCardYankLinewise_ = true;
      cursor.movePosition(QTextCursor::StartOfBlock);
      const int start = cursor.position();
      cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
      if (!cursor.atEnd())
        cursor.movePosition(QTextCursor::NextCharacter,
                            QTextCursor::KeepAnchor);
      else if (start > 0) {
        cursor.setPosition(start - 1);
        cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
      }
      cursor.removeSelectedText();
      setStatus(QStringLiteral("Text card · NORMAL · line deleted · "
                               "p puts it back · u undoes"));
    }
    textCardEditor_->setTextCursor(cursor);
    return true;
  }
  if (textCardPendingCommand_ == QLatin1Char('y')) {
    textCardPendingCommand_ = {};
    if (command == QStringLiteral("y"))
      yankClipboardTextCardLine();
    return true;
  }

  if (command == QStringLiteral("g")) {
    textCardPendingCommand_ = QLatin1Char('g');
    return true;
  }
  if (command == QStringLiteral("d")) {
    textCardPendingCommand_ = QLatin1Char('d');
    return true;
  }
  if (command == QStringLiteral("y")) {
    textCardPendingCommand_ = QLatin1Char('y');
    return true;
  }
  if (key->key() == Qt::Key_V) {
    startClipboardTextCardVisualMode(shifted);
    return true;
  }
  if (command == QStringLiteral("h"))
    cursor.movePosition(QTextCursor::PreviousCharacter);
  else if (command == QStringLiteral("j"))
    cursor.movePosition(QTextCursor::Down);
  else if (command == QStringLiteral("k"))
    cursor.movePosition(QTextCursor::Up);
  else if (command == QStringLiteral("l"))
    cursor.movePosition(QTextCursor::NextCharacter);
  else if (command == QStringLiteral("0"))
    cursor.movePosition(QTextCursor::StartOfBlock);
  else if (command == QStringLiteral("$"))
    cursor.movePosition(QTextCursor::EndOfBlock);
  else if (command == QStringLiteral("w"))
    cursor.movePosition(QTextCursor::NextWord);
  else if (command == QStringLiteral("b"))
    cursor.movePosition(QTextCursor::PreviousWord);
  else if (command == QStringLiteral("x"))
    cursor.deleteChar();
  else if (command == QStringLiteral("u")) {
    textCardEditor_->undo();
    return true;
  } else if (command == QStringLiteral("i")) {
    setClipboardTextCardInsertMode(true);
    return true;
  } else if (command == QStringLiteral("a")) {
    cursor.movePosition(QTextCursor::NextCharacter);
    textCardEditor_->setTextCursor(cursor);
    setClipboardTextCardInsertMode(true);
    return true;
  } else if (command == QStringLiteral("o")) {
    cursor.movePosition(QTextCursor::EndOfBlock);
    cursor.insertText(QStringLiteral("\n"));
    textCardEditor_->setTextCursor(cursor);
    setClipboardTextCardInsertMode(true);
    return true;
  } else if (command == QStringLiteral("q")) {
    cancelClipboardTextCard();
    return true;
  }
  textCardEditor_->setTextCursor(cursor);
  return true;
}

QColor CaptureEditor::annotationColor() const {
  if (usingCustomColor_)
    return customColor_;
  return paletteConfig_.palette.at(static_cast<std::size_t>(colorIndex_));
}
QRectF CaptureEditor::annotationBounds(const Annotation &annotation) const {
  if (annotation.kind == Annotation::Kind::Marker) {
    const qreal diameter = std::max<qreal>(24.0, annotation.size * 6.0);
    return {annotation.start.x() - diameter / 2.0,
            annotation.start.y() - diameter / 2.0, diameter, diameter};
  }
  if (annotation.kind == Annotation::Kind::Text)
    return annotationTextBounds(annotation, selection_.width());
  if (annotation.kind == Annotation::Kind::Arrow)
    return arrowVisualBounds(annotation);
  if (isStrokeKind(annotation.kind)) {
    if (annotation.points.isEmpty())
      return {};
    qreal left = annotation.points.first().x();
    qreal right = left;
    qreal top = annotation.points.first().y();
    qreal bottom = top;
    for (const QPointF &point : annotation.points) {
      left = std::min(left, point.x());
      right = std::max(right, point.x());
      top = std::min(top, point.y());
      bottom = std::max(bottom, point.y());
    }
    return {QPointF(left, top), QPointF(right, bottom)};
  }
  return QRectF(annotation.start, annotation.end).normalized();
}
void CaptureEditor::selectAllAnnotations() {
  if (annotations_.isEmpty()) {
    setStatus(QStringLiteral("No layers to select"));
    return;
  }
  selectedAnnotations_.clear();
  for (int index = 0; index < annotations_.size(); ++index)
    selectedAnnotations_.push_back(index);
  selectedAnnotation_ = annotations_.size() - 1;
  tool_ = Tool::Select;
  setStatus(QStringLiteral("All %1 layers selected · Delete removes them")
                .arg(annotations_.size()));
}

bool CaptureEditor::annotationSelected(int index) const {
  return selectedAnnotations_.contains(index);
}

QVector<QPair<QPointF, CaptureEditor::Interaction>>
CaptureEditor::annotationHandles(const Annotation &annotation) const {
  if (annotation.kind == Annotation::Kind::Arrow) {
    QVector<QPair<QPointF, Interaction>> handles{
        {annotation.start, Interaction::ResizeStart},
        {annotation.end, Interaction::ResizeEnd}};
    if (annotation.arrowStyle == ArrowStyle::Curved ||
        annotation.arrowStyle == ArrowStyle::Double) {
      handles.push_back(
          {arrowCurveHandlePoint(annotation), Interaction::ResizeControl});
    }
    return handles;
  }
  if (annotation.kind == Annotation::Kind::Line) {
    return {{annotation.start, Interaction::ResizeStart},
            {annotation.end, Interaction::ResizeEnd}};
  }
  const QRectF bounds = annotationBounds(annotation);
  // A text's handle is its wrap width, not its size: one handle, on the edge
  // the wrapping actually moves.
  if (annotation.kind == Annotation::Kind::Text) {
    // Text moved or loaded past the canvas edge would put this handle out of
    // reach, maybe off the screen itself; pull it back inside every edge so
    // the wrap can always be dragged back in. Multiline text can overrun the
    // bottom even when its width is already inside the right edge.
    QPointF handle = bounds.bottomRight();
    if (!canvasRect_.isEmpty()) {
      const qreal insetX = std::min<qreal>(4.0, canvasRect_.width() / 2.0);
      const qreal insetY = std::min<qreal>(4.0, canvasRect_.height() / 2.0);
      handle.setX(std::clamp(handle.x(), canvasRect_.left() + insetX,
                             canvasRect_.right() - insetX));
      handle.setY(std::clamp(handle.y(), canvasRect_.top() + insetY,
                             canvasRect_.bottom() - insetY));
    }
    return {{handle, Interaction::ResizeEnd}};
  }
  // A counter is a disc: any corner sets its size, and sides would say
  // something about width and height that a disc cannot honour.
  if (annotation.kind == Annotation::Kind::Marker) {
    return {{bounds.topLeft(), Interaction::ResizeTopLeft},
            {bounds.topRight(), Interaction::ResizeTopRight},
            {bounds.bottomRight(), Interaction::ResizeBottomRight},
            {bounds.bottomLeft(), Interaction::ResizeBottomLeft}};
  }
  QVector<QPair<QPointF, Interaction>> handles{
      {bounds.topLeft(), Interaction::ResizeTopLeft},
      {bounds.topRight(), Interaction::ResizeTopRight},
      {bounds.bottomRight(), Interaction::ResizeBottomRight},
      {bounds.bottomLeft(), Interaction::ResizeBottomLeft}};
  // Side handles only where there is room for them: on a layer barely wider
  // than the handles themselves they would be a blob of overlapping dots.
  const qreal room = 34.0 / std::max<qreal>(editScale(), 0.01);
  if (bounds.width() >= room) {
    handles.push_back({QPointF(bounds.center().x(), bounds.top()),
                       Interaction::ResizeTop});
    handles.push_back({QPointF(bounds.center().x(), bounds.bottom()),
                       Interaction::ResizeBottom});
  }
  if (bounds.height() >= room) {
    handles.push_back({QPointF(bounds.right(), bounds.center().y()),
                       Interaction::ResizeRight});
    handles.push_back({QPointF(bounds.left(), bounds.center().y()),
                       Interaction::ResizeLeft});
  }
  return handles;
}

CaptureEditor::Interaction
CaptureEditor::selectedHandleAt(const QPointF &point) const {
  // The single answer to "is this press a resize?", shared by every tool. A
  // handle can sit well outside the layer it belongs to, a spotlight's corner
  // being out in the dimmed surround, so hit-testing the shape alone would
  // miss it and start a marquee or a new drawing instead.
  if (selectedAnnotation_ < 0 || selectedAnnotation_ >= annotations_.size())
    return Interaction::None;
  const Annotation &selected = annotations_.at(selectedAnnotation_);
  const bool curvedArrow =
      selected.kind == Annotation::Kind::Arrow &&
      (selected.arrowStyle == ArrowStyle::Curved ||
       selected.arrowStyle == ArrowStyle::Double);
  const qreal tolerance = (curvedArrow ? 18.0 : 9.0) /
                          std::max<qreal>(editScale(), 0.01);
  Interaction nearest = Interaction::None;
  qreal nearestDistance = tolerance;
  for (const auto &[position, handle] :
       annotationHandles(annotations_.at(selectedAnnotation_))) {
    const qreal distance = QLineF(point, position).length();
    if (distance <= nearestDistance) {
      nearestDistance = distance;
      nearest = handle;
    }
  }
  return nearest;
}

CaptureEditor::Interaction CaptureEditor::pointerHandle() const {
  if (!editImageRect().contains(cursor_))
    return Interaction::None;
  return selectedHandleAt(toAnnotationPoint(cursor_));
}

Qt::CursorShape CaptureEditor::handleCursorShape(Interaction handle) const {
  // Never the move cursor: the handle resizes, the body moves, and the pointer
  // should say which one, and which way, before the press.
  switch (handle) {
  case Interaction::ResizeStart:
  case Interaction::ResizeEnd:
    if (selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size() &&
        annotations_.at(selectedAnnotation_).kind == Annotation::Kind::Text)
      return Qt::SizeHorCursor;
    return Qt::PointingHandCursor;
  case Interaction::ResizeControl:
    return Qt::PointingHandCursor;
  case Interaction::ResizeTopLeft:
  case Interaction::ResizeBottomRight:
    return Qt::SizeFDiagCursor;
  case Interaction::ResizeTopRight:
  case Interaction::ResizeBottomLeft:
    return Qt::SizeBDiagCursor;
  case Interaction::ResizeTop:
  case Interaction::ResizeBottom:
    return Qt::SizeVerCursor;
  case Interaction::ResizeLeft:
  case Interaction::ResizeRight:
    return Qt::SizeHorCursor;
  default:
    return Qt::ArrowCursor;
  }
}

void CaptureEditor::applyBoxResize(Annotation &annotation, Interaction handle,
                                   const QPointF &point,
                                   const QRectF &original) {
  if (annotation.kind == Annotation::Kind::Marker) {
    annotation.size =
        std::clamp(QLineF(annotation.start, point).length() / 3.0, 2.0, 30.0);
    return;
  }
  const bool movesLeft = handle == Interaction::ResizeTopLeft ||
                         handle == Interaction::ResizeLeft ||
                         handle == Interaction::ResizeBottomLeft;
  const bool movesRight = handle == Interaction::ResizeTopRight ||
                          handle == Interaction::ResizeRight ||
                          handle == Interaction::ResizeBottomRight;
  const bool movesTop = handle == Interaction::ResizeTopLeft ||
                        handle == Interaction::ResizeTop ||
                        handle == Interaction::ResizeTopRight;
  const bool movesBottom = handle == Interaction::ResizeBottomLeft ||
                           handle == Interaction::ResizeBottom ||
                           handle == Interaction::ResizeBottomRight;
  QPointF target = point;
  if (resizeConstraintActive_ && (movesLeft || movesRight) &&
      (movesTop || movesBottom)) {
    // Shift keeps the proportions, measured against the corner that stays.
    const QPointF fixed(movesLeft ? original.right() : original.left(),
                        movesTop ? original.bottom() : original.top());
    const QPointF moving(movesLeft ? original.left() : original.right(),
                         movesTop ? original.top() : original.bottom());
    target = aspectLockedEndpoint(fixed, moving, point);
  }
  // An edge dragged past its opposite turns the layer inside out and keeps
  // following the pointer, the way every drawing tool behaves. Pulling the
  // right edge left across the shape gives a shape on the left, not a sliver
  // pinned at the crossing point.
  QRectF box = original;
  if (movesLeft)
    box.setLeft(target.x());
  if (movesRight)
    box.setRight(target.x());
  if (movesTop)
    box.setTop(target.y());
  if (movesBottom)
    box.setBottom(target.y());
  box = box.normalized();
  // A redaction still has a floor: it covers something, so it may not be
  // reduced to a line. The edge nearer where it started is the one that holds.
  const qreal minimum = annotation.kind == Annotation::Kind::Redaction
                            ? kMinimumRedactionExtent
                            : 0.0;
  if (minimum > 0.0) {
    const qreal fixedX = movesLeft ? original.right() : original.left();
    const qreal fixedY = movesTop ? original.bottom() : original.top();
    if (box.width() < minimum) {
      if (std::abs(box.left() - fixedX) <= std::abs(box.right() - fixedX))
        box.setRight(box.left() + minimum);
      else
        box.setLeft(box.right() - minimum);
    }
    if (box.height() < minimum) {
      if (std::abs(box.top() - fixedY) <= std::abs(box.bottom() - fixedY))
        box.setBottom(box.top() + minimum);
      else
        box.setTop(box.bottom() - minimum);
    }
  }

  if (isStrokeKind(annotation.kind)) {
    // Mirrored when the drag crossed over: the stroke flips with the box
    // rather than piling up against the edge it crossed.
    const bool flippedX = movesLeft ? target.x() > original.right()
                          : movesRight ? target.x() < original.left()
                                       : false;
    const bool flippedY = movesTop ? target.y() > original.bottom()
                          : movesBottom ? target.y() < original.top()
                                        : false;
    const qreal scaleX =
        (original.width() > 0 ? box.width() / original.width() : 1.0) *
        (flippedX ? -1.0 : 1.0);
    const qreal scaleY =
        (original.height() > 0 ? box.height() / original.height() : 1.0) *
        (flippedY ? -1.0 : 1.0);
    const QPointF anchor(scaleX < 0 ? box.right() : box.left(),
                         scaleY < 0 ? box.bottom() : box.top());
    const auto resizePoints = [&](QVector<QPointF> &resized,
                                  const QVector<QPointF> &source) {
      resized.resize(source.size());
      for (qsizetype index = 0; index < source.size(); ++index) {
        const QPointF relative = source.at(index) - original.topLeft();
        resized[index] =
            anchor + QPointF(relative.x() * scaleX, relative.y() * scaleY);
      }
    };
    resizePoints(annotation.points, originalAnnotation_.points);
    resizePoints(annotation.rawPoints, originalAnnotation_.rawPoints);
    if (!annotation.points.isEmpty()) {
      annotation.start = annotation.points.first();
      annotation.end = annotation.points.last();
    }
    return;
  }
  annotation.start = box.topLeft();
  annotation.end = box.bottomRight();
}

QString CaptureEditor::highlighterStatus() const {
  if (highlighterMode_ == HighlighterMode::Snap) {
    return QStringLiteral("Highlighter · Snap · text height automatic · wheel "
                          "sets off-text size %1 · H toggles Normal")
        .arg(qRound(annotationSize_));
  }
  return QStringLiteral("Highlighter · Normal · freehand size %1 · wheel / "
                        "Alt+wheel resizes · H toggles Snap")
      .arg(qRound(annotationSize_));
}

QString CaptureEditor::highlighterTooltip() const {
  if (highlighterMode_ == HighlighterMode::Snap) {
    return QStringLiteral("Highlighter · Snap · text height automatic · wheel "
                          "sets off-text size %1 · H / click again: Normal")
        .arg(qRound(annotationSize_));
  }
  return QStringLiteral("Highlighter · Normal · freehand · size %1 · wheel / "
                        "Alt+wheel · H / click again: Snap")
      .arg(qRound(annotationSize_));
}

void CaptureEditor::activateHighlighter() {
  if (tool_ == Tool::Highlighter) {
    highlighterMode_ = highlighterMode_ == HighlighterMode::Snap
                           ? HighlighterMode::Normal
                           : HighlighterMode::Snap;
  } else {
    tool_ = Tool::Highlighter;
  }
  highlighterLock_.reset();
  highlighterPreview_.reset();
  setStatus(highlighterStatus());
}

QString CaptureEditor::toolStatus() const {
  const int size = qRound(annotationSize_);
  switch (tool_) {
  case Tool::Select:
    return QStringLiteral("Select · drag moves layers · Ctrl+wheel zooms · "
                          "outer handles crop");
  case Tool::Spotlight: {
    const QString shape =
        spotlightShape_ == SpotlightShape::Ellipse ? QStringLiteral("ellipse")
        : spotlightShape_ == SpotlightShape::Rectangle
            ? QStringLiteral("rectangle")
            : QStringLiteral("rounded");
    const QString zoom =
        spotlightMagnification_ <= 1.0
            ? QStringLiteral("no zoom")
            : QStringLiteral("%1×").arg(spotlightMagnification_, 0, 'f', 1);
    const QString ring =
        spotlightBorder_ <= 0.0
            ? QStringLiteral("no border")
            : QStringLiteral("border %1").arg(qRound(spotlightBorder_));
    return QStringLiteral("Spotlight · %1 · %2 · %3 · S cycles shape · wheel "
                          "zooms · Alt+wheel border")
        .arg(shape, zoom, ring);
  }
  case Tool::Redact:
    return QStringLiteral("Redact · %1 · D toggles style")
        .arg(redactionStyleName(redactionStyle_).toLower());
  case Tool::Text:
    return QStringLiteral("Text · %1 · size %2 · Shift+T cycles font · click "
                          "to type")
        .arg(annotationTextFontName(textFont_))
        .arg(QString::fromLatin1(
            kTextSizeNames.at(static_cast<std::size_t>(textSizeIndex_))));
  case Tool::Marker:
    return QStringLiteral("Marker · next %1 · size %2 · wheel resizes")
        .arg(nextMarker_)
        .arg(size);
  case Tool::Ocr:
    return QStringLiteral("Copy text · drag over the words to copy them");
  case Tool::Eyedropper:
    return QStringLiteral("Eyedropper · click to take a color from the "
                          "capture");
  case Tool::Rectangle:
    return QStringLiteral("Rectangle · size %1 · wheel resizes · Shift keeps "
                          "it square")
        .arg(size);
  case Tool::Ellipse:
    return QStringLiteral("Ellipse · size %1 · wheel resizes · Shift keeps "
                          "it circular")
        .arg(size);
  case Tool::Cut:
    return QStringLiteral("Cut · drag a band to remove it");
  case Tool::Highlighter:
    return highlighterStatus();
  case Tool::Freehand:
    return QStringLiteral("Pen · size %1 · smoothing %2/%3 · wheel size · "
                          "Alt+wheel smoothing")
        .arg(size)
        .arg(freehandSmoothingLevel_)
        .arg(stroke::maximumSmoothingLevel);
  case Tool::Arrow:
    return QStringLiteral("Arrow · %1 · A cycles style · size %2 · wheel "
                          "resizes · Shift snaps 45° / centers bend")
        .arg(arrowStyleName(arrowStyle_))
        .arg(size);
  case Tool::Line:
    break;
  }
  const QString name = tool_ == Tool::Line     ? QStringLiteral("Line")
                       : tool_ == Tool::Freehand ? QStringLiteral("Pen")
                                                 : QStringLiteral("Highlighter");
  return QStringLiteral("%1 · size %2 · wheel resizes · Shift constrains")
      .arg(name)
      .arg(size);
}

bool CaptureEditor::annotationContains(const Annotation &annotation,
                                       const QPointF &point,
                                       bool edgeOnly) const {
    if (annotation.kind == Annotation::Kind::Arrow) {
      return arrowContainsPoint(
          annotation, point,
          std::max<qreal>(8.0, annotation.size + 4.0));
    }
    if (annotation.kind == Annotation::Kind::Line) {
      const QPointF delta = annotation.end - annotation.start;
      const qreal lengthSquared = delta.x() * delta.x() + delta.y() * delta.y();
      if (lengthSquared <= 0)
        return false;
      const QPointF fromStart = point - annotation.start;
      const qreal t =
          std::clamp((fromStart.x() * delta.x() + fromStart.y() * delta.y()) /
                         lengthSquared,
                     0.0, 1.0);
      const QPointF closest = annotation.start + delta * t;
      if (QLineF(point, closest).length() <=
          std::max<qreal>(8.0, annotation.size + 4.0))
        return true;
    } else if (isStrokeKind(annotation.kind)) {
      const qreal tolerance = strokeHitTolerance(annotation);
      for (int pointIndex = 1; pointIndex < annotation.points.size();
           ++pointIndex) {
        const QLineF segment(annotation.points.at(pointIndex - 1),
                             annotation.points.at(pointIndex));
        if (segment.length() <= 0)
          continue;
        const QPointF delta = segment.p2() - segment.p1();
        const QPointF fromStart = point - segment.p1();
        const qreal lengthSquared =
            delta.x() * delta.x() + delta.y() * delta.y();
        const qreal t =
            std::clamp((fromStart.x() * delta.x() + fromStart.y() * delta.y()) /
                           lengthSquared,
                       0.0, 1.0);
        const QPointF closest = segment.p1() + delta * t;
        if (QLineF(point, closest).length() <= tolerance)
          return true;
      }
    } else {
      const QRectF bounds = annotationBounds(annotation);
      if (annotation.kind == Annotation::Kind::Rectangle) {
        // Filled rectangles hit anywhere inside unless this is an edge-only
        // grab; hollow ones only on the stroke band.
        const qreal tolerance =
            std::max<qreal>(kEdgeGrabTolerance, annotation.size + 3.0);
        const QRectF outer =
            bounds.adjusted(-tolerance, -tolerance, tolerance, tolerance);
        if (!edgeOnly && annotation.filled)
          return outer.contains(point);
        const QRectF inner =
            bounds.adjusted(tolerance, tolerance, -tolerance, -tolerance);
        return outer.contains(point) &&
               (inner.isEmpty() || !inner.contains(point));
      }
      if (annotation.kind == Annotation::Kind::Ellipse) {
        // Same rule for ellipses: filled hits the silhouette unless this is
        // an edge-only grab; hollow only a band around the outline.
        const qreal tolerance =
            std::max<qreal>(kEdgeGrabTolerance, annotation.size + 3.0);
        QPainterPath outline;
        outline.addEllipse(bounds);
        QPainterPathStroker band;
        band.setWidth(tolerance * 2.0);
        return band.createStroke(outline).contains(point) ||
               (!edgeOnly && annotation.filled && outline.contains(point));
      }
      if (edgeOnly && annotation.kind == Annotation::Kind::Redaction) {
        const QRectF outer =
            bounds.adjusted(kEdgeGrabTolerance, kEdgeGrabTolerance,
                            -kEdgeGrabTolerance, -kEdgeGrabTolerance);
        return bounds
                   .adjusted(-kEdgeGrabTolerance, -kEdgeGrabTolerance,
                             kEdgeGrabTolerance, kEdgeGrabTolerance)
                   .contains(point) &&
               (outer.isEmpty() || !outer.contains(point));
      }
      if (bounds.adjusted(-7, -7, 7, 7).contains(point))
        return true;
    }
    return false;
}

int CaptureEditor::annotationAt(const QPointF &point) const {
  for (const int layer : {0, 1, 2, 3, 4}) {
    for (int index = annotations_.size() - 1; index >= 0; --index) {
      const Annotation &annotation = annotations_.at(index);
      const int annotationLayer =
          annotation.kind == Annotation::Kind::Marker      ? 0
          : annotation.kind == Annotation::Kind::Text      ? 1
          : annotation.kind == Annotation::Kind::Spotlight ? 3
          : annotation.kind == Annotation::Kind::Redaction ? 4
                                                           : 2;
      if (annotationLayer != layer)
        continue;
      if (layer == 3) {
        if (spotlightPath(annotation).contains(point))
          return index;
        continue;
      }
      if (annotationContains(annotation, point, false))
        return index;
    }
  }
  return -1;
}

bool CaptureEditor::toolGrabsLayer(int index) const {
  if (index < 0 || index >= annotations_.size())
    return false;
  // A counter is stamped over anything that is not a counter. It still picks
  // up its own kind: two that must overlap are placed apart and dragged
  // together.
  if (tool_ == Tool::Marker)
    return annotations_.at(index).kind == Annotation::Kind::Marker;
  return true;
}

bool CaptureEditor::pointerGrabsLayer() const {
  return editImageRect().contains(cursor_) &&
         toolGrabsLayer(annotationEdgeAt(toAnnotationPoint(cursor_)));
}

int CaptureEditor::annotationEdgeAt(const QPointF &point) const {
  for (const int layer : {0, 1, 2, 3, 4}) {
    for (int index = annotations_.size() - 1; index >= 0; --index) {
      const Annotation &annotation = annotations_.at(index);
      const int annotationLayer =
          annotation.kind == Annotation::Kind::Marker      ? 0
          : annotation.kind == Annotation::Kind::Text      ? 1
          : annotation.kind == Annotation::Kind::Spotlight ? 3
          : annotation.kind == Annotation::Kind::Redaction ? 4
                                                           : 2;
      if (annotationLayer != layer)
        continue;
      if (layer == 3) {
        const QPainterPath opening = spotlightPath(annotation);
        if (!opening.contains(point))
          continue;
        QPainterPathStroker band;
        band.setWidth(kEdgeGrabTolerance * 2.0);
        if (band.createStroke(opening).contains(point))
          return index;
        continue;
      }
      if (annotationContains(annotation, point, true))
        return index;
    }
  }
  return -1;
}

int CaptureEditor::hoveredSpotlightAt(const QPointF &position) const {
  if (dragging_ || !editImageRect().contains(position))
    return -1;
  const int index = annotationAt(toAnnotationPoint(position));
  if (index < 0 || annotations_.at(index).kind != Annotation::Kind::Spotlight)
    return -1;
  return index;
}
void CaptureEditor::duplicateSelectedAnnotation() {
  if (dragging_ || textEditor_->isVisible() || selectedAnnotation_ < 0 ||
      selectedAnnotation_ >= annotations_.size())
    return;
  endNudgeRun();
  Annotation copy = annotations_.at(selectedAnnotation_);
  copy.id = 0;
  translateAnnotation(
      copy, duplicateOffset(annotationBounds(copy), canvasRect_));
  if (copy.kind == Annotation::Kind::Marker)
    copy.number = nextMarker_;
  if (copy.kind == Annotation::Kind::Redaction) {
    // A copy must not share the original's mosaic; give it its own seed.
    copy.redactionSeed = freshRedactionSeed();
  }
  commitAnnotate(std::move(copy));
  if (!annotations_.isEmpty()) {
    selectedAnnotation_ = annotations_.size() - 1;
    selectedAnnotations_ = {selectedAnnotation_};
  }
  setStatus(QStringLiteral("Duplicated · Alt+D again offsets further"));
}

bool CaptureEditor::adjustSelectedAnnotationRing(int step) {
  // Alt+wheel is the secondary control, and with a layer selected it belongs
  // to that layer rather than to what the next one will look like. Kinds with
  // no second setting say so, and the wheel falls through to the armed tool.
  if (selectedAnnotation_ < 0 || selectedAnnotation_ >= annotations_.size())
    return false;
  Annotation &annotation = annotations_[selectedAnnotation_];
  if (annotation.kind == Annotation::Kind::Rectangle) {
    // The selected rectangle's own corners, undoably; the armed tool's
    // default radius stays what it was.
    beginSelectionAdjust();
    annotation.cornerRadius =
        std::clamp(annotation.cornerRadius + step * kCornerRadiusStep, 0.0,
                   kMaximumCornerRadius);
    setStatus(QStringLiteral("Rectangle · %1 · Alt+wheel adjusts")
                  .arg(cornerName(annotation.cornerRadius)));
    commitPatch({selectedAnnotation_});
    return true;
  }
  if (annotation.kind == Annotation::Kind::Spotlight) {
    beginSelectionAdjust();
    annotation.size =
        std::clamp(annotation.size + step * 2.0, 0.0, 12.0);
    setStatus(spotlightStatus(annotation.spotlightShape,
                              annotation.magnification, annotation.size));
    commitPatch({selectedAnnotation_});
    return true;
  }
  if (annotation.kind != Annotation::Kind::Freehand)
    return false;

  beginSelectionAdjust();
  if (annotation.rawPoints.isEmpty()) {
    setStatus(QStringLiteral("Pen stroke has no smoothing baseline"));
    return true;
  }
  const int next = std::clamp(annotation.smoothingLevel + step,
                              stroke::minimumSmoothingLevel,
                              stroke::maximumSmoothingLevel);
  setStatus(QStringLiteral("Pen smoothing %1/%2 · Alt+wheel adjusts · "
                           "Ctrl+Z undoes")
                .arg(next)
                .arg(stroke::maximumSmoothingLevel));
  if (next != annotation.smoothingLevel) {
    annotation.smoothingLevel = next;
    annotation.points =
        stroke::smoothFreehand(annotation.rawPoints, annotation.smoothingLevel);
    annotation.start = annotation.points.first();
    annotation.end = annotation.points.last();
    commitPatch({selectedAnnotation_});
  }
  return true;
}

void CaptureEditor::beginSelectionAdjust() {
  adjustingSelection_ = true;
  adjustSettleTimer_.start();
}

void CaptureEditor::adjustSelectedAnnotation(int step) {
  // The wheel is the weight control: a layer keeps the shape and the place it
  // was drawn in, and only gets heavier or lighter. How big it is belongs to
  // the corner handle, where Shift keeps the proportions: one gesture each,
  // so neither has to be undone to reach the other.
  beginSelectionAdjust();
  if (selectedAnnotation_ < 0 || selectedAnnotation_ >= annotations_.size())
    return;
  Annotation &annotation = annotations_[selectedAnnotation_];
  const auto weigh = [step](qreal size, qreal lowest, qreal highest) {
    return std::clamp(size + step, lowest, highest);
  };
  switch (annotation.kind) {
  case Annotation::Kind::Spotlight:
    annotation.magnification =
        std::clamp(annotation.magnification + step * 0.25, 1.0, 4.0);
    setStatus(spotlightStatus(annotation.spotlightShape,
                              annotation.magnification, annotation.size));
    commitPatch({selectedAnnotation_});
    return;
  case Annotation::Kind::Marker:
    // A counter has no stroke to weigh: its size is the counter itself.
    annotation.size = weigh(annotation.size, 2.0, 30.0);
    setStatus(QStringLiteral("Counter %1 · size %2 · wheel resizes")
                  .arg(annotation.number)
                  .arg(qRound(annotation.size)));
    commitPatch({selectedAnnotation_});
    return;
  case Annotation::Kind::Text:
    // Type has no stroke either; its weight is its size.
    annotation.size = weigh(annotation.size, 1.0, 24.0);
    setStatus(QStringLiteral("Selected text · size %1 · wheel resizes")
                  .arg(qRound(annotation.size)));
    commitPatch({selectedAnnotation_});
    return;
  case Annotation::Kind::Redaction: {
    // A cover-up is all fill, so the only thing its wheel can mean is how
    // much it covers.
    const qreal factor = step > 0 ? 1.1 : 1.0 / 1.1;
    const QRectF bounds = annotationBounds(annotation);
    const QPointF center = bounds.center();
    const qreal scale =
        bounds.width() > 0 && bounds.height() > 0
            ? std::max({factor, kMinimumRedactionExtent / bounds.width(),
                        kMinimumRedactionExtent / bounds.height()})
            : factor;
    annotation.start = center + (annotation.start - center) * scale;
    annotation.end = center + (annotation.end - center) * scale;
    setStatus(QStringLiteral("Redaction · %1%").arg(qRound(scale * 100)));
    commitPatch({selectedAnnotation_});
    return;
  }
  case Annotation::Kind::Arrow:
  case Annotation::Kind::Line:
  case Annotation::Kind::Freehand:
  case Annotation::Kind::Highlighter:
  case Annotation::Kind::Rectangle:
  case Annotation::Kind::Ellipse:
    break;
  }
  // A filled shape has no stroke showing, so weighing it would be a gesture
  // that does nothing visible: it grows instead, like the redaction above,
  // and says so.
  if (annotation.filled && (annotation.kind == Annotation::Kind::Rectangle ||
                            annotation.kind == Annotation::Kind::Ellipse)) {
    const qreal factor = step > 0 ? 1.1 : 1.0 / 1.1;
    const QRectF bounds = annotationBounds(annotation);
    const QPointF center = bounds.center();
    const qreal scale =
        bounds.width() > 0 && bounds.height() > 0
            ? std::max({factor, kMinimumRedactionExtent / bounds.width(),
                        kMinimumRedactionExtent / bounds.height()})
            : factor;
    annotation.start = center + (annotation.start - center) * scale;
    annotation.end = center + (annotation.end - center) * scale;
    const QRectF grown = annotationBounds(annotation);
    setStatus(QStringLiteral("Filled shape · %1 × %2 · R unfills it to set "
                             "a thickness")
                  .arg(qRound(grown.width()))
                  .arg(qRound(grown.height())));
    commitPatch({selectedAnnotation_});
    return;
  }
  annotation.size = weigh(annotation.size, 2.0, 30.0);
  setStatus(QStringLiteral("Selected layer · thickness %1 · handle resizes")
                .arg(qRound(annotation.size)));
  commitPatch({selectedAnnotation_});
}

QLineF CaptureEditor::creationSpan(const QPointF &rawEnd) const {
  const QPointF end =
      creationConstraintActive_
          ? constrainedCreationEndpoint(tool_, dragStart_, rawEnd)
          : rawEnd;
  const QPointF start = creationCenteredActive_
                            ? centeredCreationStart(tool_, dragStart_, end)
                            : dragStart_;
  return {start, end};
}

void CaptureEditor::toggleShapeFill() {
  fillShapes_ = !fillShapes_;
  selectedAnnotation_ = -1;
  setStatus(QStringLiteral("%1 shapes · %2 again toggles fill")
                .arg(fillName(fillShapes_))
                .arg(tool_ == Tool::Ellipse ? QStringLiteral("E")
                                            : QStringLiteral("R")));
}

void CaptureEditor::toggleTextBackground() {
  if (selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size() &&
      annotations_.at(selectedAnnotation_).kind == Annotation::Kind::Text) {
    Annotation &text = annotations_[selectedAnnotation_];
    text.textBackground = nextTextBackground(text.textBackground);
    setStatus(QStringLiteral("Selected text: %1 · T again cycles")
                  .arg(textBackgroundName(text.textBackground).toLower()));
    commitPatch({selectedAnnotation_});
    return;
  }
  textBackground_ = nextTextBackground(textBackground_);
  selectedAnnotation_ = -1;
  setStatus(QStringLiteral("Text: %1 · T again cycles")
                .arg(textBackgroundName(textBackground_).toLower()));
}

void CaptureEditor::cycleArrowStyle() {
  ArrowStyle seed = arrowStyle_;
  QVector<int> selectedLayers;
  for (const int index : selectedAnnotations_) {
    if (index >= 0 && index < annotations_.size() &&
        !selectedLayers.contains(index))
      selectedLayers.push_back(index);
  }
  if (selectedLayers.isEmpty() && selectedAnnotation_ >= 0 &&
      selectedAnnotation_ < annotations_.size())
    selectedLayers.push_back(selectedAnnotation_);

  QVector<int> arrows;
  for (const int index : selectedLayers) {
    if (annotations_.at(index).kind == Annotation::Kind::Arrow)
      arrows.push_back(index);
  }
  // A single selected arrow continues from its own style. A mixed/group
  // selection uses the current tool style as the common seed.
  if (selectedLayers.size() == 1 && arrows.size() == 1)
    seed = annotations_.at(arrows.constFirst()).arrowStyle;
  arrowStyle_ = nextArrowStyle(seed);
  if (!arrows.isEmpty()) {
    for (const int index : arrows)
      annotations_[index].arrowStyle = arrowStyle_;
    setStatus(arrows.size() == 1
                  ? QStringLiteral("Selected arrow: %1 · A cycles style")
                        .arg(arrowStyleName(arrowStyle_))
                  : QStringLiteral("%1 selected arrows: %2 · A cycles style")
                        .arg(arrows.size())
                        .arg(arrowStyleName(arrowStyle_)));
    commitPatch(arrows);
    return;
  }
  setStatus(toolStatus());
}

void CaptureEditor::cycleTextFont() {
  if (selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size() &&
      annotations_.at(selectedAnnotation_).kind == Annotation::Kind::Text) {
    Annotation &text = annotations_[selectedAnnotation_];
    text.textFont = nextTextFont(text.textFont);
    setStatus(QStringLiteral("Selected text: %1 · Shift+T cycles font")
                  .arg(annotationTextFontName(text.textFont)));
    commitPatch({selectedAnnotation_});
    return;
  }
  textFont_ = nextTextFont(textFont_);
  selectedAnnotation_ = -1;
  tool_ = Tool::Text;
  setStatus(QStringLiteral("Text: %1 · Shift+T cycles font")
                .arg(annotationTextFontName(textFont_)));
}

QPointF CaptureEditor::constrainedResizeEndpoint(
    const Annotation &annotation, const QPointF &candidate,
    const QPointF &fixed, const QPointF &originalMoving) const {
  QPointF point = candidate;
  if (resizeConstraintActive_) {
    if (annotation.kind == Annotation::Kind::Arrow ||
        annotation.kind == Annotation::Kind::Line) {
      point = constrainedCreationEndpoint(Tool::Line, fixed, candidate);
    } else {
      point = aspectLockedEndpoint(fixed, originalMoving, candidate);
    }
  }
  if (annotation.kind == Annotation::Kind::Redaction)
    return constrainedRedactionEndpoint(point, fixed, originalMoving);
  return point;
}

void CaptureEditor::nudgeSelectedAnnotation(const QPointF &delta) {
  if (selectedAnnotation_ < 0 || selectedAnnotation_ >= annotations_.size())
    return;
  // Presses in quick succession (a held key) share one undo entry and one
  // deferred snapshot.
  if (!nudgeTimer_.isValid() || nudgeTimer_.elapsed() > kNudgeCoalesceMs)
    endNudgeRun();
  nudgeTimer_.restart();
  translateAnnotation(annotations_[selectedAnnotation_], delta);
  // Keyboard geometry has no pointer-to-canvas feedback loop, so refit it
  // immediately. The coalescing timer still keeps the whole key run in one
  // undo/snapshot operation.
  refreshCanvasRect();
  setStatus(QStringLiteral("Nudged · arrows move 1 px · Shift 10 px"));
  nudgePersistTimer_.start();
}

void CaptureEditor::endNudgeRun() {
  const bool hadRun = nudgeTimer_.isValid();
  nudgeTimer_.invalidate();
  if (nudgePersistTimer_.isActive())
    nudgePersistTimer_.stop();
  if (hadRun && selectedAnnotation_ >= 0 &&
      selectedAnnotation_ < annotations_.size())
    commitPatch({selectedAnnotation_});
}

qreal CaptureEditor::toolbarTop(qreal buttonHeight) const {
  if (windowedPresentation_) {
    // Pinned under the key guide, leaving a band tall enough for the tool
    // hint pill that hangs above the toolbar; a 10 px gap put the pill on
    // the guide's bottom rows. On a resized window the free space belongs
    // to the canvas below, not to a drifting toolbar.
    return 14 +
           hotkeyLegendAnchoredSize(editorHotkeyEntries(), width() - 28.0)
               .height() +
           42;
  }
  // The capture-kind tabs hang off the top edge and stay up in the edit
  // phase; zoomed far in, the toolbar stops beneath them instead of
  // sliding underneath. At fit the toolbar never reaches them, so the
  // floor only matters past fit.
  const qreal toolbarFloor =
      viewZoom_ > 1.0 && !selectTabItems().isEmpty() ? 44 : 10;
  return std::max<qreal>(toolbarFloor,
                         chromeAnchorTop() - buttonHeight - kToolbarImageGap);
}

QRectF CaptureEditor::colorPaletteRect() const {
  const qreal scale = toolbarScale(width());
  const qreal toolbarWidth = kToolbarWidth * scale;
  const qreal buttonHeight = 36 * scale;
  const qreal toolbarX = (width() - toolbarWidth) / 2.0;
  const qreal toolbarY = toolbarTop(buttonHeight);
  const QRectF anchor(toolbarX + 440 * scale, toolbarY, 36 * scale,
                      buttonHeight);
  const qreal paletteWidth =
      8.0 + (static_cast<qreal>(paletteConfig_.palette.size()) + 2.0) * 28.0;
  const qreal x = std::clamp(anchor.center().x() - paletteWidth / 2.0, 8.0,
                             std::max(8.0, width() - paletteWidth - 8.0));
  return {x, anchor.bottom() + 4, paletteWidth, 36};
}

QRectF CaptureEditor::customColorPanelRect() const {
  QRectF panel(colorPaletteRect().left(), colorPaletteRect().bottom() + 6, 220,
               150);
  if (panel.right() > width() - 8)
    panel.moveRight(width() - 8);
  if (panel.bottom() > height() - 8)
    panel.moveBottom(height() - 8);
  return panel;
}

QRectF CaptureEditor::shapeMenuRect() const {
  const qreal scale = toolbarScale(width());
  const qreal toolbarX = (width() - kToolbarWidth * scale) / 2.0;
  const qreal buttonHeight = 36 * scale;
  const qreal toolbarY = toolbarTop(buttonHeight);
  const QRectF anchor(toolbarX + 240 * scale, toolbarY, buttonHeight,
                      buttonHeight);
  return {anchor.center().x() - 58, anchor.bottom() + 4, 116, 36};
}

QRectF CaptureEditor::textSizePanelRect() const {
  const qreal scale = toolbarScale(width());
  const qreal toolbarWidth = kToolbarWidth * scale;
  const qreal buttonHeight = 36 * scale;
  const qreal toolbarX = (width() - toolbarWidth) / 2.0;
  const qreal toolbarY = toolbarTop(buttonHeight);
  const QRectF anchor(toolbarX + 400 * scale, toolbarY, 36 * scale,
                      buttonHeight);
  return {anchor.center().x() - 51, anchor.bottom() + 6, 102, 34};
}

void CaptureEditor::applyCustomColor(const QPointF &position) {
  const QRectF panel = customColorPanelRect();
  const QRectF field = panel.adjusted(12, 12, -36, -12);
  const QRectF hue(panel.right() - 26, panel.top() + 12, 14,
                   panel.height() - 24);
  qreal saturation = customColor_.hsvSaturationF();
  qreal value = customColor_.valueF();
  if (hue.contains(position)) {
    customHue_ =
        std::clamp((position.y() - hue.top()) / hue.height(), 0.0, 1.0);
  } else if (field.contains(position)) {
    saturation =
        std::clamp((position.x() - field.left()) / field.width(), 0.0, 1.0);
    value = 1.0 -
            std::clamp((position.y() - field.top()) / field.height(), 0.0, 1.0);
  } else {
    return;
  }
  customColor_ = QColor::fromHsvF(customHue_, saturation, value);
  usingCustomColor_ = true;
  if (selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size() &&
      annotations_.at(selectedAnnotation_).kind !=
          Annotation::Kind::Redaction) {
    annotations_[selectedAnnotation_].color = customColor_;
    commitPatch({selectedAnnotation_});
  }
  update();
}

QRectF CaptureEditor::normalizedSelection(const QPointF &first,
                                          const QPointF &second) const {
  const QRectF bounds(QPointF(), QSizeF(width(), height()));
  const QPointF a(std::clamp(first.x(), bounds.left(), bounds.right()),
                  std::clamp(first.y(), bounds.top(), bounds.bottom()));
  const QPointF b(std::clamp(second.x(), bounds.left(), bounds.right()),
                  std::clamp(second.y(), bounds.top(), bounds.bottom()));
  return QRectF(a, b).normalized();
}

qreal CaptureEditor::contentBandTop() const {
  if (!windowedPresentation_)
    return 60;
  return 14 +
         hotkeyLegendAnchoredSize(editorHotkeyEntries(), width() - 28.0)
             .height() +
         42 + 36;
}

QRectF CaptureEditor::baseImageRect() const {
  if (selection_.isEmpty() || canvasRect_.isEmpty())
    return {};
  // A windowed editor stacks the key guide above the toolbar, so its top
  // band is as tall as the guide actually is at this width, plus the
  // toolbar, the row the color dropdown and its popover peers hang into,
  // and clearance for the selection's top handles; the capture keeps a
  // generous mat margin on the other three sides.
  const qreal top = windowedPresentation_ ? contentBandTop() + 54 : 68;
  const qreal side = windowedPresentation_ ? 64 : 30;
  const qreal bottom = windowedPresentation_ ? 64 : 58;
  const QRectF available(side, top, std::max<qreal>(1, width() - 2 * side),
                         std::max<qreal>(1, height() - top - bottom));
  const qreal scale =
      std::min<qreal>({1.0, available.width() / canvasRect_.width(),
                       available.height() / canvasRect_.height()});
  const QSizeF shown = canvasRect_.size() * scale;
  // The canvas centers in its band, so a resized window keeps the image in
  // the middle while the chrome above stays pinned.
  return {available.center().x() - shown.width() / 2.0,
          available.center().y() - shown.height() / 2.0, shown.width(),
          shown.height()};
}

qreal CaptureEditor::chromeAnchorTop() const {
  // Zoomed past fit the content fills the viewport band, so anchoring to the
  // centered fit rect would float the chrome over it. Anchor to the band.
  const qreal base = baseImageRect().top();
  return viewZoom_ > 1.0 ? std::min<qreal>(base, 68.0) : base;
}

QRectF CaptureEditor::editImageRect() const {
  const QRectF base = baseImageRect();
  if (base.isEmpty() || qFuzzyCompare(viewZoom_, 1.0))
    return base.translated(viewOffset_);
  const QSizeF shown = base.size() * viewZoom_;
  const QPointF center = base.center();
  return QRectF(center.x() - shown.width() / 2.0,
                center.y() - shown.height() / 2.0, shown.width(),
                shown.height())
      .translated(viewOffset_);
}

QRectF CaptureEditor::visibleEditImageRect() const {
  const QRectF image = editImageRect();
  if (viewZoom_ <= 1.0)
    return image;
  const qreal bandTop = contentBandTop();
  const qreal bandBottom = windowedPresentation_ ? 64 : 56;
  return image.intersected(QRectF(
      0, bandTop, width(), std::max<qreal>(1, height() - bandTop - bandBottom)));
}

qreal CaptureEditor::maxViewZoom() const {
  const QRectF base = baseImageRect();
  if (base.isEmpty() || canvasRect_.width() <= 0)
    return 1.0;
  const qreal baseScale = base.width() / canvasRect_.width();
  return std::max<qreal>(1.0, 4.0 / std::max<qreal>(baseScale, 0.0001));
}

void CaptureEditor::clampViewOffset() {
  const QRectF base = baseImageRect();
  if (base.isEmpty())
    return;
  const QSizeF shown = base.size() * viewZoom_;
  const qreal bandTop = windowedPresentation_ ? contentBandTop() : 68;
  const qreal bandBottom = windowedPresentation_ ? 64 : 58;
  const QRectF available(
      windowedPresentation_ ? 0 : 30, bandTop,
      std::max<qreal>(1, width() - (windowedPresentation_ ? 0 : 60)),
      std::max<qreal>(1, height() - bandTop - bandBottom));
  // Keep the image covering the viewport where it is larger, and centered
  // (no free pan) on any axis where it is smaller.
  const auto clampAxis = [](qreal shownLen, qreal availLen, qreal &offset) {
    if (shownLen <= availLen) {
      offset = 0.0;
      return;
    }
    const qreal slack = (shownLen - availLen) / 2.0;
    offset = std::clamp(offset, -slack, slack);
  };
  clampAxis(shown.width(), available.width(), viewOffset_.rx());
  clampAxis(shown.height(), available.height(), viewOffset_.ry());
}

void CaptureEditor::setViewZoom(qreal zoom, const QPointF &focus) {
  // Down to a tenth: an overview of a tall stitch or a large capture is
  // as legitimate as a closeup.
  const qreal clamped = std::clamp(zoom, 0.10, maxViewZoom());
  if (qFuzzyCompare(clamped, viewZoom_))
    return;
  const QRectF before = editImageRect();
  const qreal beforeScale =
      before.width() > 0 ? before.width() / canvasRect_.width() : 1.0;
  const QPointF imagePoint((focus.x() - before.left()) / std::max(beforeScale, 1e-6),
                           (focus.y() - before.top()) / std::max(beforeScale, 1e-6));
  viewZoom_ = clamped;
  clampViewOffset();
  const QRectF after = editImageRect();
  const qreal afterScale =
      after.width() > 0 ? after.width() / canvasRect_.width() : 1.0;
  const QPointF mappedFocus(after.left() + imagePoint.x() * afterScale,
                            after.top() + imagePoint.y() * afterScale);
  viewOffset_ += focus - mappedFocus;
  clampViewOffset();
  updatePointerCursor();
  update();
}

void CaptureEditor::panView(const QPointF &delta) {
  if (viewZoom_ <= 1.0)
    return;
  viewOffset_ += delta;
  clampViewOffset();
  update();
}

void CaptureEditor::resetView() {
  if (viewZoom_ == 1.0 && viewOffset_.isNull())
    return;
  viewZoom_ = 1.0;
  viewOffset_ = {};
  updatePointerCursor();
  update();
}

QVector<QRectF> CaptureEditor::cropHandleRects() const {
  const QRectF image =
      sourceFrameWidgetRect().intersected(visibleEditImageRect());
  if (image.isEmpty())
    return {};
  constexpr qreal outside = 7;
  constexpr qreal size = 12;
  const qreal half = size / 2.0;
  const std::array<QPointF, 8> centers{
      image.topLeft() + QPointF(-outside, -outside),
      QPointF(image.center().x(), image.top() - outside),
      image.topRight() + QPointF(outside, -outside),
      QPointF(image.right() + outside, image.center().y()),
      image.bottomRight() + QPointF(outside, outside),
      QPointF(image.center().x(), image.bottom() + outside),
      image.bottomLeft() + QPointF(-outside, outside),
      QPointF(image.left() - outside, image.center().y())};
  QVector<QRectF> handles;
  handles.reserve(static_cast<qsizetype>(centers.size()));
  for (const QPointF &center : centers)
    handles.push_back({center.x() - half, center.y() - half, size, size});
  return handles;
}

int CaptureEditor::cropHandleAt(const QPointF &point) const {
  const QVector<QRectF> handles = cropHandleRects();
  for (int index = 0; index < handles.size(); ++index) {
    if (handles.at(index).contains(point))
      return index;
  }
  return -1;
}

qreal CaptureEditor::editScale() const {
  return canvasRect_.width() > 0 ? editImageRect().width() / canvasRect_.width()
                                : 1.0;
}

QRectF CaptureEditor::sourceFrameWidgetRect() const {
  const QRectF canvas = editImageRect();
  if (canvas.isEmpty() || canvasRect_.isEmpty())
    return {};
  const qreal scale = std::max<qreal>(editScale(), 0.001);
  return {canvas.left() - canvasRect_.left() * scale,
          canvas.top() - canvasRect_.top() * scale,
          selection_.width() * scale, selection_.height() * scale};
}

QPointF CaptureEditor::toAnnotationPoint(const QPointF &position) const {
  const QRectF image = editImageRect();
  const qreal scale = std::max<qreal>(editScale(), 0.001);
  return {std::clamp((position.x() - image.left()) / scale + canvasRect_.left(),
                     canvasRect_.left(), canvasRect_.right()),
          std::clamp((position.y() - image.top()) / scale + canvasRect_.top(),
                     canvasRect_.top(), canvasRect_.bottom())};
}

QPointF
CaptureEditor::toUnclampedAnnotationPoint(const QPointF &position) const {
  const QRectF image = editImageRect();
  const qreal scale = std::max<qreal>(editScale(), 0.001);
  return {(position.x() - image.left()) / scale + canvasRect_.left(),
          (position.y() - image.top()) / scale + canvasRect_.top()};
}

QPointF CaptureEditor::markerPlacementPoint(const QPointF &position) const {
  constexpr qreal kPointerLead = 9.0;
  const qreal lead = kPointerLead / std::max<qreal>(editScale(), 0.001);
  return toAnnotationPoint(position) - QPointF(lead, lead);
}

bool CaptureEditor::selectedLayerAcceptsPoint(const QPointF &point) const {
  if (selectedAnnotation_ < 0 || selectedAnnotation_ >= annotations_.size())
    return false;
  const Annotation &selected = annotations_.at(selectedAnnotation_);
  const qreal tolerance = 9.0 / std::max<qreal>(editScale(), 0.01);
  const QRectF bounds = annotationBounds(selected);
  const bool endpoints = hasEndpointHandles(selected.kind);
  const QPointF first = endpoints ? selected.start : bounds.topLeft();
  const QPointF last = endpoints ? selected.end : bounds.bottomRight();
  if (endpoints && QLineF(point, first).length() <= tolerance)
    return true;
  if (QLineF(point, last).length() <= tolerance)
    return true;
  return annotationAt(point) == selectedAnnotation_;
}

QRectF CaptureEditor::sourceRect(const QRectF &logicalRect) const {
  if (capture_.source.isNull() || capture_.previewSize.isEmpty())
    return {};
  const qreal scaleX =
      capture_.source.width() / static_cast<qreal>(capture_.previewSize.width());
  const qreal scaleY =
      capture_.source.height() / static_cast<qreal>(capture_.previewSize.height());
  return {logicalRect.x() * scaleX, logicalRect.y() * scaleY,
          logicalRect.width() * scaleX, logicalRect.height() * scaleY};
}

QPointF CaptureEditor::sourcePoint(const QPointF &logicalPoint) const {
  return sourceRect(QRectF(logicalPoint, QSizeF())).topLeft();
}

std::optional<CaptureEditor::HighlighterLock>
CaptureEditor::highlighterLockAt(const QPointF &annotationPoint) const {
  if (capture_.source.isNull() || capture_.previewSize.isEmpty() ||
      !QRectF(QPointF(), selection_.size()).contains(annotationPoint))
    return std::nullopt;

  const QSizeF sourceScale(
      capture_.source.width() /
          static_cast<qreal>(capture_.previewSize.width()),
      capture_.source.height() /
          static_cast<qreal>(capture_.previewSize.height()));
  const QPointF logicalPoint = selection_.topLeft() + annotationPoint;
  const auto band =
      detectTextBand(capture_.source, sourcePoint(logicalPoint), sourceScale);
  if (!band)
    return std::nullopt;

  // Match the text-band padding used for the committed stroke. Keeping this
  // in one helper makes the hover I-beam and mouse-down lock identical.
  constexpr qreal padPerSide = 0.05;
  const qreal centerY =
      band->center() / sourceScale.height() - selection_.top();
  const qreal highlightedHeight =
      band->height() * (1.0 + 2.0 * padPerSide) / sourceScale.height();
  return HighlighterLock{std::clamp(centerY, 0.0, selection_.height()),
                         highlightedHeight / 3.0};
}

QRectF CaptureEditor::highlighterPreviewRectForTest() const {
  if (!highlighterPreview_)
    return {};
  const qreal height =
      highlighterPreviewHeight(highlighterPreview_->annotationSize);
  const QPointF pointer = toAnnotationPoint(cursor_);
  const QPointF center(pointer.x(), dragging_ && highlighterLock_
                                        ? highlighterPreview_->centerY
                                        : pointer.y());
  return highlighterIBeamBounds(center, height, editScale());
}

QRectF CaptureEditor::highlighterToolbarRectForTest() const {
  for (const ToolbarButton &button : toolbarButtons()) {
    if (button.action == QStringLiteral("tool-highlighter"))
      return button.rect;
  }
  return {};
}

QRectF CaptureEditor::mapWidgetToPreview(const QRectF &widgetRect) const {
  const QSize widget = size();
  const QSize preview = capture_.previewSize;
  if (widget.isEmpty() || preview.isEmpty() || widget == preview)
    return widgetRect;
  const qreal scaleX = preview.width() / static_cast<qreal>(widget.width());
  const qreal scaleY = preview.height() / static_cast<qreal>(widget.height());
  return {widgetRect.x() * scaleX, widgetRect.y() * scaleY,
          widgetRect.width() * scaleX, widgetRect.height() * scaleY};
}

QRectF CaptureEditor::mapPreviewToWidget(const QRectF &previewRect) const {
  const QSize widget = size();
  const QSize preview = capture_.previewSize;
  if (widget.isEmpty() || preview.isEmpty() || widget == preview)
    return previewRect;
  const qreal scaleX = widget.width() / static_cast<qreal>(preview.width());
  const qreal scaleY = widget.height() / static_cast<qreal>(preview.height());
  return {previewRect.x() * scaleX, previewRect.y() * scaleY,
          previewRect.width() * scaleX, previewRect.height() * scaleY};
}

QString CaptureEditor::measurementText() const {
  if (capture_.source.isNull())
    return {};
  if (phase_ == Phase::Select) {
    if (windowMode_) {
      if (hoveredWindow_ < 0 || hoveredWindow_ >= capture_.windows.size())
        return {};
      return formatPixelSize(
          sourceRect(capture_.windows.at(hoveredWindow_).rect).size());
    }
    // A fresh drag reads 0 × 0 rather than falling back to the pointer
    // position: the number must track the frame the moment it starts.
    if (dragging_ || !selection_.isEmpty())
      return formatPixelSize(sourceRect(selection_).size());
    return formatPixelPoint(sourcePoint(cursor_));
  }
  if (tool_ == Tool::Select && dragging_ &&
      interaction_ >= Interaction::CropTopLeft)
    return formatPixelSize(sourceRect(selection_).size());
  return {};
}

int CaptureEditor::windowAt(const QPointF &position) const {
  const QPointF previewPoint =
      mapWidgetToPreview(QRectF(position, QSizeF())).topLeft();
  for (int index = capture_.windows.size() - 1; index >= 0; --index) {
    if (QRectF(capture_.windows.at(index).rect).contains(previewPoint))
      return index;
  }
  return -1;
}

int CaptureEditor::windowInDirection(int current, int key) const {
  if (capture_.windows.isEmpty())
    return -1;

  const QPointF origin = current >= 0 && current < capture_.windows.size()
                             ? QPointF(capture_.windows.at(current).rect.center())
                             : mapWidgetToPreview(QRectF(cursor_, QSizeF())).topLeft();
  int best = -1;
  qreal bestScore = std::numeric_limits<qreal>::max();
  for (int index = 0; index < capture_.windows.size(); ++index) {
    if (index == current)
      continue;
    const QPointF delta = capture_.windows.at(index).rect.center() - origin;
    qreal along = 0;
    qreal across = 0;
    if (key == Qt::Key_Left && delta.x() < 0) {
      along = -delta.x();
      across = std::abs(delta.y());
    } else if (key == Qt::Key_Right && delta.x() > 0) {
      along = delta.x();
      across = std::abs(delta.y());
    } else if (key == Qt::Key_Up && delta.y() < 0) {
      along = -delta.y();
      across = std::abs(delta.x());
    } else if (key == Qt::Key_Down && delta.y() > 0) {
      along = delta.y();
      across = std::abs(delta.x());
    } else {
      continue;
    }
    const qreal score = along + across * 1.75;
    if (score < bestScore) {
      best = index;
      bestScore = score;
    }
  }
  return best;
}

QVector<QPair<QString, QString>> editorHotkeyEntries() {
  return {{QStringLiteral("V"), QStringLiteral("Select / move layer")},
       {QStringLiteral("A"), QStringLiteral("Arrow")},
       {QStringLiteral("L"), QStringLiteral("Line")},
       {QStringLiteral("F / H"), QStringLiteral("Freehand / Highlighter")},
       {QStringLiteral("C"), QStringLiteral("Marker")},
       {QStringLiteral("R / E"), QStringLiteral("Rectangle / Ellipse")},
       {QStringLiteral("X"), QStringLiteral("Cut out a band")},
       {QStringLiteral("T"), QStringLiteral("Text")},
       {QStringLiteral("Double click"), QStringLiteral("Edit text layer")},
       {QStringLiteral("1–8"), QStringLiteral("Color")},
       {QStringLiteral("Wheel"), QStringLiteral("Zoom selected / tool size")},
       {QStringLiteral("D / O"), QStringLiteral("Redact / OCR text")},
       {QStringLiteral("B / G / P"),
        QStringLiteral("Backdrop / boundary / pin")},
       {QStringLiteral("W"), QStringLiteral("Editor to window / overlay")},
       {QStringLiteral("Ctrl+Z"), QStringLiteral("Undo")},
       {QStringLiteral("Ctrl+Shift+Z"), QStringLiteral("Redo")},
       {QStringLiteral("Enter"), QStringLiteral("Copy + save")},
       {QStringLiteral("Ctrl+C"), QStringLiteral("Copy only")},
       {QStringLiteral("Ctrl+S"), QStringLiteral("Save only")},
       {QStringLiteral("Esc"), QStringLiteral("Arrow / twice close")}};
}

QVector<CaptureEditor::ToolbarButton> CaptureEditor::toolbarButtons() const {
  QVector<ToolbarButton> buttons;
  const qreal scale = toolbarScale(width());
  const qreal height = 36 * scale;
  const qreal gap = 4 * scale;
  const qreal total = kToolbarWidth * scale;
  qreal x = (width() - total) / 2.0;
  const qreal y = toolbarTop(height);
  auto add = [&](qreal buttonWidth, QString action, QString label,
                 QString tooltip, QColor color = {}) {
    const qreal scaledWidth = buttonWidth * scale;
    buttons.push_back({QRectF(x, y, scaledWidth, height), std::move(action),
                       std::move(label), std::move(tooltip), color});
    x += scaledWidth + gap;
  };

  add(36, QStringLiteral("tool-select"), {},
      QStringLiteral("Select/move · V · Ctrl+wheel zoom · outer handles crop"));
  add(36, arrowToolAction(arrowStyle_), {},
      QStringLiteral("Arrow · %1 · A cycles · Shift snaps 45° / centers "
                     "bend · Size %2 · Wheel")
          .arg(arrowStyleName(arrowStyle_))
          .arg(qRound(annotationSize_)));
  add(36, QStringLiteral("tool-line"), {},
      QStringLiteral("Line · L · Shift snaps 45° · Size %1 · Wheel")
          .arg(qRound(annotationSize_)));
  add(36, QStringLiteral("tool-freehand"), {},
      QStringLiteral("Freehand · F · Size %1 · Wheel · Smoothing %2/%3 · "
                     "Alt+Wheel")
          .arg(qRound(annotationSize_))
          .arg(freehandSmoothingLevel_)
          .arg(stroke::maximumSmoothingLevel));
  add(36, QStringLiteral("tool-highlighter"), {}, highlighterTooltip());
  add(36, QStringLiteral("tool-marker"), {},
      QStringLiteral("Number marker · C · Size %1 · Wheel")
          .arg(qRound(annotationSize_)));
  const QString fillHint = fillShapes_ ? QStringLiteral("filled") : QString();
  const bool ellipseSelected = tool_ == Tool::Ellipse;
  add(36, ellipseSelected ? QStringLiteral("tool-ellipse")
                          : QStringLiteral("tool-rectangle"),
      fillHint,
      QStringLiteral("Shapes · R rectangle · E ellipse · hover for fill"));
  add(36, QStringLiteral("tool-spotlight"), {},
      QStringLiteral("Spotlight · S · %1 · %2× · S cycles shape")
          .arg(spotlightShape_ == SpotlightShape::Ellipse
                   ? QStringLiteral("ellipse")
                   : spotlightShape_ == SpotlightShape::Rectangle
                         ? QStringLiteral("rectangle")
                         : QStringLiteral("rounded"))
          .arg(spotlightMagnification_, 0, 'f', 1));
  add(36, QStringLiteral("tool-redact"), {},
      QStringLiteral("Redact · D · %1 · D again toggles")
          .arg(redactionStyleName(redactionStyle_)));
  add(36, QStringLiteral("tool-cut"), {},
      QStringLiteral("Cut out a band · X · drag across"));
  add(36, QStringLiteral("tool-text"), {},
      QStringLiteral("%1 text · T · %2 · %3 · T again cycles style · "
                     "Shift+T cycles font · Wheel")
          .arg(annotationTextFontName(textFont_))
          .arg(QString::fromLatin1(
              kTextSizeNames.at(static_cast<std::size_t>(textSizeIndex_))))
          .arg(textBackgroundName(textBackground_)));
  add(36, QStringLiteral("palette"), {}, QStringLiteral("Annotation color"),
      annotationColor());
  add(36, QStringLiteral("tool-ocr"), {},
      QStringLiteral("Copy all text in the image · O"));
  add(36, QStringLiteral("background"), {},
      QStringLiteral("Backdrop · B cycles · Shift+B toggles shadow"));
  add(36, QStringLiteral("undo"), {}, QStringLiteral("Undo · Ctrl+Z"));
  add(36, QStringLiteral("redo"), {},
      QStringLiteral("Redo · Ctrl+Shift+Z / Ctrl+Y"));
  add(36, QStringLiteral("pin"), {},
      QStringLiteral("Pin on screen · P · Ctrl+C on the pin copies it"));
  add(36, QStringLiteral("copy"), {}, QStringLiteral("Copy only · Ctrl+C"));
  add(40, QStringLiteral("both"), {}, QStringLiteral("Copy and save · Enter"));
  add(36, QStringLiteral("save"), {}, QStringLiteral("Save only · Ctrl+S"));
  add(36, QStringLiteral("close"), {}, QStringLiteral("Close · Esc twice"));

  if (shapeMenuOpen_) {
    const QRectF menu = shapeMenuRect();
    buttons.push_back({{menu.left() + 4, menu.top() + 4, 32, 28},
                       QStringLiteral("shape-rectangle"), {},
                       QStringLiteral("Rectangle · R"), {}});
    buttons.push_back({{menu.left() + 40, menu.top() + 4, 32, 28},
                       QStringLiteral("shape-ellipse"), {},
                       QStringLiteral("Ellipse · E"), {}});
    buttons.push_back({{menu.left() + 76, menu.top() + 4, 32, 28},
                       QStringLiteral("shape-fill"),
                       fillShapes_ ? QStringLiteral("filled") : QString(),
                       QStringLiteral("Toggle filled or outlined shapes"), {}});
  }
  if (colorPaletteOpen_) {
    const QRectF palette = colorPaletteRect();
    const int presetCount = static_cast<int>(paletteConfig_.palette.size());
    for (int index = 0; index < presetCount; ++index) {
      buttons.push_back(
          {{palette.left() + 4 + index * 28, palette.top() + 4, 24, 28},
           QStringLiteral("color-%1").arg(index),
           {},
           QStringLiteral("Color · %1").arg(index + 1),
           paletteConfig_.palette.at(static_cast<std::size_t>(index))});
    }
    buttons.push_back({{palette.left() + 4 + presetCount * 28, palette.top() + 4,
                        24, 28},
                       QStringLiteral("custom-color"), {},
                       QStringLiteral("Custom color"), {}});
    buttons.push_back({{palette.left() + 4 + (presetCount + 1) * 28,
                        palette.top() + 4, 24, 28},
                       QStringLiteral("tool-eyedropper"), {},
                       QStringLiteral("Sample from image · I"), {}});
  }
  return buttons;
}

void CaptureEditor::setStatus(QString status) {
  status_ = std::move(status);
  update();
}

CaptureEditor::EditState CaptureEditor::editState() const {
  return {annotations_,       backgroundStyle_,     imageShadow_,
          canvasBoundaryMode_, selection_,           selectedAnnotation_,
          selectedAnnotations_, nextMarker_,         cuts_};
}

void CaptureEditor::refreshCanvasRect() {
  const QRectF next = selection_.isEmpty()
                          ? QRectF()
                          : captureCanvasRect(selection_.size(), annotations_,
                                              canvasBoundaryMode_);
  if (next == canvasRect_)
    return;
  canvasRect_ = next;
  redactionBaseStale_ = true;
  viewZoom_ = std::min(viewZoom_, maxViewZoom());
  clampViewOffset();
}

bool CaptureEditor::canvasGrown() const {
  if (selection_.isEmpty() || canvasRect_.isEmpty())
    return false;
  const QRectF sourceFrame(QPointF(), selection_.size());
  return canvasRect_.left() < sourceFrame.left() - 0.001 ||
         canvasRect_.top() < sourceFrame.top() - 0.001 ||
         canvasRect_.right() > sourceFrame.right() + 0.001 ||
         canvasRect_.bottom() > sourceFrame.bottom() + 0.001;
}

BackgroundStyle CaptureEditor::effectiveBackgroundStyle() const {
  const bool automaticFramedBackground =
      canvasBoundaryMode_ == CanvasBoundaryMode::Framed && canvasGrown() &&
      backgroundStyle_ == BackgroundStyle::None;
  return automaticFramedBackground ? BackgroundStyle::Slate
                                   : backgroundStyle_;
}

void CaptureEditor::applyEditState(const EditState &state) {
  annotations_ = state.annotations;
  backgroundStyle_ = state.backgroundStyle;
  imageShadow_ = state.imageShadow;
  canvasBoundaryMode_ = state.canvasBoundary;
  selection_ = state.selection;
  selectedAnnotation_ = std::clamp(state.selectedAnnotation, -1,
                                   static_cast<int>(annotations_.size()) - 1);
  selectedAnnotations_.clear();
  for (const int index : state.selectedAnnotations) {
    if (index >= 0 && index < annotations_.size() &&
        !selectedAnnotations_.contains(index))
      selectedAnnotations_.push_back(index);
  }
  if (selectedAnnotation_ >= 0 &&
      !selectedAnnotations_.contains(selectedAnnotation_))
    selectedAnnotations_.push_back(selectedAnnotation_);
  nextMarker_ = state.nextMarker;
  if (state.cuts != cuts_) {
    cuts_ = state.cuts;
    refreshComposedCapture();
  }
  refreshCanvasRect();
  editingAnnotation_ = -1;
  interaction_ = Interaction::None;
  freehandPoints_.clear();
  highlighterLock_.reset();
  textEditor_->clear();
  textEditor_->hide();
  textCaretTimer_.stop();
  setFocus(Qt::OtherFocusReason);
  updatePointerCursor();
  update();
}

void CaptureEditor::cancelActiveDragForHistory() {
  if (dragStartStateValid_)
    replayLog();
  dragging_ = false;
  creationConstraintActive_ = false;
  creationCenteredActive_ = false;
  resizeConstraintActive_ = false;
  interaction_ = Interaction::None;
  dragStartStateValid_ = false;
  dragChanged_ = false;
  freehandPoints_.clear();
  highlighterLock_.reset();
  if (cutDragActive_) {
    cutDragActive_ = false;
    refreshComposedCapture();
  }
}

QString CaptureEditor::workingLogPath() const {
  return snapshotPath_.isEmpty() ? QString() : operationLogPath(snapshotPath_);
}

bool CaptureEditor::restoreOperationLog(const QString &path, QString &error) {
  OperationLog log;
  if (!loadOperationLog(path, log, error))
    return false;
  ops_ = std::move(log.ops);
  opIndex_ = std::clamp(log.index, 0, static_cast<int>(ops_.size()));
  nextAnnotationId_ = std::max<quint64>(log.nextId, 1);
  nextMarker_ = std::max(log.nextMarker, 1);
  replayLog();
  phase_ = Phase::Edit;
  scheduleSnapshot();
  return true;
}

void CaptureEditor::commitOp(Operation op) {
  if (opIndex_ < ops_.size())
    ops_.resize(opIndex_);
  ops_.push_back(std::move(op));
  constexpr qsizetype maximumOps = 100;
  while (ops_.size() > maximumOps) {
    // Replay starts at the full monitor; the first Crop is the selected
    // region/window. Dropping it leaves annotations in cropped space.
    if (ops_.constFirst().type == Operation::Type::Crop)
      ops_.removeAt(1);
    else
      ops_.removeFirst();
    if (opIndex_ > 0)
      --opIndex_;
  }
  opIndex_ = ops_.size();
  replayLog();
  scheduleSnapshot();
}

void CaptureEditor::commitAnnotate(Annotation annotation) {
  if (annotation.id == 0)
    annotation.id = nextAnnotationId_++;
  Operation op;
  op.type = Operation::Type::Annotate;
  op.annotations = {std::move(annotation)};
  commitOp(std::move(op));
}

void CaptureEditor::commitPatch(const QVector<int> &indices) {
  Operation op;
  op.type = Operation::Type::Patch;
  for (const int index : indices) {
    if (index < 0 || index >= annotations_.size())
      continue;
    Annotation annotation = annotations_.at(index);
    if (annotation.id == 0)
      annotation.id = nextAnnotationId_++;
    op.annotations.push_back(std::move(annotation));
  }
  if (op.annotations.isEmpty())
    return;
  commitOp(std::move(op));
}

void CaptureEditor::commitDelete(const QVector<int> &indices) {
  Operation op;
  op.type = Operation::Type::Delete;
  for (const int index : indices) {
    if (index >= 0 && index < annotations_.size() &&
        annotations_.at(index).id != 0)
      op.ids.push_back(annotations_.at(index).id);
  }
  if (op.ids.isEmpty())
    return;
  commitOp(std::move(op));
}

void CaptureEditor::commitCrop(const QRectF &crop) {
  Operation op;
  op.type = Operation::Type::Crop;
  op.crop = crop;
  commitOp(std::move(op));
}

void CaptureEditor::commitCut(CutOp cut) {
  Operation op;
  op.type = Operation::Type::Cut;
  op.cut = std::move(cut);
  commitOp(std::move(op));
}

void CaptureEditor::commitBackground(BackgroundStyle style, bool imageShadow) {
  Operation op;
  op.type = Operation::Type::Background;
  op.background = style;
  op.imageShadow = imageShadow;
  commitOp(std::move(op));
}

void CaptureEditor::commitCanvasBoundary(CanvasBoundaryMode mode) {
  Operation op;
  op.type = Operation::Type::CanvasBoundary;
  op.canvasBoundary = mode;
  commitOp(std::move(op));
}

void CaptureEditor::cycleCanvasBoundary(bool reverse) {
  CanvasBoundaryMode next = CanvasBoundaryMode::Framed;
  if (reverse) {
    switch (canvasBoundaryMode_) {
    case CanvasBoundaryMode::Framed:
      next = CanvasBoundaryMode::Image;
      break;
    case CanvasBoundaryMode::Overflow:
      next = CanvasBoundaryMode::Framed;
      break;
    case CanvasBoundaryMode::Image:
      next = CanvasBoundaryMode::Overflow;
      break;
    }
  } else {
    switch (canvasBoundaryMode_) {
    case CanvasBoundaryMode::Framed:
      next = CanvasBoundaryMode::Overflow;
      break;
    case CanvasBoundaryMode::Overflow:
      next = CanvasBoundaryMode::Image;
      break;
    case CanvasBoundaryMode::Image:
      next = CanvasBoundaryMode::Framed;
      break;
    }
  }
  setStatus(QStringLiteral("Canvas: %1 · G cycles · Shift+G reverses")
                .arg(canvasBoundaryName(next)));
  commitCanvasBoundary(next);
}

void CaptureEditor::cycleBackground() {
  BackgroundStyle next = BackgroundStyle::None;
  bool nextShadow = true;
  const bool normalWindowCycle =
      windowedPresentation_ && windowedBackdropOpaque_;
  switch (backgroundStyle_) {
  case BackgroundStyle::None:
  case BackgroundStyle::Off:
    next = BackgroundStyle::Aurora;
    break;
  case BackgroundStyle::Slate:
    if (normalWindowCycle) {
      if (imageShadow_) {
        next = BackgroundStyle::Off;
      } else {
        next = BackgroundStyle::Slate;
        nextShadow = true;
      }
    } else {
      if (imageShadow_) {
        next = BackgroundStyle::Slate;
        nextShadow = false;
      } else {
        next = BackgroundStyle::Off;
      }
    }
    break;
  case BackgroundStyle::Aurora:
    next = BackgroundStyle::Sunset;
    break;
  case BackgroundStyle::Sunset:
    next = BackgroundStyle::Lagoon;
    break;
  case BackgroundStyle::Lagoon:
    next = BackgroundStyle::Violet;
    break;
  case BackgroundStyle::Violet:
    next = BackgroundStyle::Slate;
    nextShadow = !normalWindowCycle;
    break;
  }
  if (next == BackgroundStyle::Off) {
    setStatus(QStringLiteral("Backdrop: Off · B cycles"));
  } else {
    setStatus(QStringLiteral("Backdrop: %1 · shadow %2 · B cycles · Shift+B "
                             "toggles shadow")
                  .arg(backgroundName(next),
                       nextShadow ? QStringLiteral("on")
                                  : QStringLiteral("off")));
  }
  commitBackground(next, nextShadow);
}

void CaptureEditor::replayLog() {
  QVector<quint64> selectedIds;
  for (const int index : selectedAnnotations_) {
    if (index >= 0 && index < annotations_.size() &&
        annotations_.at(index).id != 0)
      selectedIds.push_back(annotations_.at(index).id);
  }
  const quint64 selectedId =
      selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size()
          ? annotations_.at(selectedAnnotation_).id
          : 0;

  const QSize startSize =
      pristineLogicalSize_.isEmpty() ? capture_.previewSize
                                     : pristineLogicalSize_;
  QRectF selection{QPointF(), QSizeF(startSize)};
  BackgroundStyle background = BackgroundStyle::None;
  bool imageShadow = true;
  CanvasBoundaryMode canvasBoundary = CanvasBoundaryMode::Framed;
  QVector<Annotation> annotations;
  QVector<CutOp> cuts;
  int nextMarker = 1;
  for (int index = 0; index < opIndex_ && index < ops_.size(); ++index) {
    const Operation &op = ops_.at(index);
    switch (op.type) {
    case Operation::Type::Crop: {
      // Annotation coordinates are relative to the source frame. Preserve
      // their absolute capture position when a later crop moves that frame,
      // matching the live crop-handle preview and making undo/reload stable.
      const QPointF delta = selection.topLeft() - op.crop.topLeft();
      if (!delta.isNull()) {
        for (Annotation &annotation : annotations)
          translateAnnotation(annotation, delta);
      }
      selection = op.crop;
      break;
    }
    case Operation::Type::Background:
      background = op.background;
      imageShadow = op.imageShadow;
      break;
    case Operation::Type::CanvasBoundary:
      canvasBoundary = op.canvasBoundary;
      break;
    case Operation::Type::Annotate:
      for (const Annotation &annotation : op.annotations)
        annotations.push_back(annotation);
      break;
    case Operation::Type::Patch:
      // A patch is a pick-up: the layer you just changed comes to the top,
      // same as raiseAnnotation(), so a click-to-raise survives replay.
      for (const Annotation &annotation : op.annotations) {
        const auto match = std::ranges::find_if(
            annotations, [&](const Annotation &current) {
              return current.id != 0 && current.id == annotation.id;
            });
        if (match != annotations.end()) {
          Annotation updated = annotation;
          annotations.erase(match);
          annotations.push_back(std::move(updated));
        }
      }
      break;
    case Operation::Type::Delete:
      annotations.erase(std::remove_if(annotations.begin(), annotations.end(),
                                       [&](const Annotation &annotation) {
                                         return op.ids.contains(annotation.id);
                                       }),
                        annotations.end());
      break;
    case Operation::Type::Cut: {
      const bool horizontal = op.cut.orientation == Qt::Horizontal;
      const qreal lo = op.cut.logicalStart;
      const qreal hi = op.cut.logicalEnd;
      const qreal band = hi - lo;
      for (Annotation &annotation : annotations) {
        auto shift = [&](QPointF &point) {
          if (horizontal)
            point.setY(shiftForCut(point.y(), lo, hi));
          else
            point.setX(shiftForCut(point.x(), lo, hi));
        };
        shift(annotation.start);
        shift(annotation.end);
        if (annotation.curveControl)
          shift(*annotation.curveControl);
        for (QPointF &point : annotation.points)
          shift(point);
        for (QPointF &point : annotation.rawPoints)
          shift(point);
      }
      if (band > 0.0) {
        if (horizontal)
          selection.setHeight(std::max<qreal>(1.0, selection.height() - band));
        else
          selection.setWidth(std::max<qreal>(1.0, selection.width() - band));
      }
      cuts.push_back(op.cut);
      break;
    }
    }
  }

  annotations_ = std::move(annotations);
  backgroundStyle_ = background;
  imageShadow_ = imageShadow;
  canvasBoundaryMode_ = canvasBoundary;
  if (cuts != cuts_) {
    cuts_ = std::move(cuts);
    refreshComposedCapture();
  }
  if (!selection.isEmpty())
    selection_ = selection;
  refreshCanvasRect();
  nextMarker = 1;
  for (const Annotation &annotation : annotations_) {
    if (annotation.kind == Annotation::Kind::Marker)
      nextMarker = std::max(nextMarker, annotation.number + 1);
  }
  nextMarker_ = nextMarker;
  selectedAnnotations_.clear();
  selectedAnnotation_ = -1;
  for (int index = 0; index < annotations_.size(); ++index) {
    if (selectedIds.contains(annotations_.at(index).id))
      selectedAnnotations_.push_back(index);
    if (selectedId != 0 && annotations_.at(index).id == selectedId)
      selectedAnnotation_ = index;
  }
  if (selectedAnnotation_ < 0 && !selectedAnnotations_.isEmpty())
    selectedAnnotation_ = selectedAnnotations_.constLast();
  editingAnnotation_ = -1;
  redactionBaseStale_ = true;
  updatePointerCursor();
  update();
}

int CaptureEditor::raiseAnnotation(int index) {
  // Picking a layer up puts it on top of the ones it overlaps: the last thing
  // you touched is the thing you are working on. Text and counters are painted
  // above everything regardless, so this never buries a label or a callout.
  if (index < 0 || index >= annotations_.size() - 1)
    return index;
  Annotation raised = annotations_.takeAt(index);
  annotations_.push_back(std::move(raised));
  const int top = static_cast<int>(annotations_.size()) - 1;
  const auto remap = [index, top](int position) {
    if (position < 0)
      return position;
    if (position == index)
      return top;
    return position > index ? position - 1 : position;
  };
  selectedAnnotation_ = remap(selectedAnnotation_);
  for (int &position : selectedAnnotations_)
    position = remap(position);
  editingAnnotation_ = remap(editingAnnotation_);
  return top;
}

void CaptureEditor::undoEdit() {
  endNudgeRun();
  cancelActiveDragForHistory();
  if (opIndex_ <= 0) {
    setStatus(QStringLiteral("Nothing to undo"));
    return;
  }
  --opIndex_;
  replayLog();
  scheduleSnapshot();
  setStatus(QStringLiteral("Undo · Ctrl+Shift+Z or Ctrl+Y to redo"));
}

void CaptureEditor::redoEdit() {
  endNudgeRun();
  cancelActiveDragForHistory();
  if (opIndex_ >= ops_.size()) {
    setStatus(QStringLiteral("Nothing to redo"));
    return;
  }
  ++opIndex_;
  replayLog();
  scheduleSnapshot();
  setStatus(QStringLiteral("Redo · Ctrl+Z to undo"));
}

void CaptureEditor::scheduleSnapshot() {
  if (suppressSnapshots_ || capture_.source.isNull())
    return;
  if (snapshotPath_.isEmpty())
    snapshotPath_ = temporarySnapshotPath();
  if (snapshotBusy_) {
    snapshotDirty_ = true;
    return;
  }
  startSnapshotRender();
}

QImage CaptureEditor::renderCurrentOutput() const {
  return renderCapture(capture_, selection_, annotations_, backgroundStyle_,
                       imageShadow_, canvasBoundaryMode_);
}

void CaptureEditor::startSnapshotRender() {
  snapshotBusy_ = true;
  snapshotDirty_ = false;
  const QImage source = capture_.source;
  const QString path = snapshotPath_;
  const QString logPath = operationLogPath(path);
  const OperationLog log{ops_, opIndex_, nextAnnotationId_, nextMarker_,
                         pristineLogicalSize_};
  const bool writeSource = !sourceWritten_ || !QFile::exists(path);
  snapshotWatcher_.setFuture(QtConcurrent::run(
      [source, path, logPath, log, writeSource] {
        QString error;
        if (writeSource && !saveTemporarySnapshot(source, path, error, -1))
          return false;
        return saveOperationLog(logPath, log, error);
      }));
}

bool CaptureEditor::waitForSnapshot() {
  // Drain in-flight and coalesced snapshot renders, letting the event loop
  // run so the watcher signals can chain the next render. Used before
  // exporting so the working snapshot on disk reflects the newest state.
  while (snapshotBusy_ || snapshotDirty_) {
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    QThread::yieldCurrentThread();
  }
  return snapshotWriteOk_;
}

void CaptureEditor::waitForExport() {
  // Wait for the worker, then let the queued finished() signal reach
  // completeFinish(). On success that closes the editor; busy_ stays set.
  while (finishWatcher_.isRunning()) {
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    QThread::yieldCurrentThread();
  }
  for (int pass = 0; pass < 3; ++pass)
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

bool CaptureEditor::prepareHandoff(QString &path, QString &error) {
  scheduleSnapshot();
  if (!waitForSnapshot()) {
    error = QStringLiteral("Could not flush the working document");
    return false;
  }
  pruneEditorHandoffs();
  path = editorHandoffPath();
  if (path.isEmpty()) {
    error = QStringLiteral("Could not create private runtime directory");
    return false;
  }
  if (!QFile::copy(snapshotPath_, path) ||
      !QFile::copy(workingLogPath(), operationLogPath(path))) {
    QFile::remove(path);
    QFile::remove(operationLogPath(path));
    error = QStringLiteral("Could not copy the working document");
    return false;
  }
  return true;
}

void CaptureEditor::handOffEditor(bool toWindow) {
  if (busy_)
    return;
  QString path;
  QString error;
  if (!prepareHandoff(path, error)) {
    setStatus(error);
    return;
  }
  // The presentation is a property of the process (the shell integration is
  // chosen before Qt connects), so switching means handing the working
  // document to a fresh process and closing this one. The op log carries the
  // selection, the layers, and the undo history across.
  if (!QProcess::startDetached(
          QCoreApplication::applicationFilePath(),
          {path, QStringLiteral("--editor"),
           toWindow ? QStringLiteral("window") : QStringLiteral("overlay")})) {
    QFile::remove(path);
    QFile::remove(operationLogPath(path));
    setStatus(QStringLiteral("Could not start omasnap"));
    return;
  }
  close();
}

void CaptureEditor::pinSnapshot() {
  if (busy_ || pinPending_ || selection_.isEmpty())
    return;

  prunePinnedSnapshots();
  const QString path = pinnedSnapshotPath(++pinCount_);
  if (path.isEmpty()) {
    --pinCount_;
    setStatus(QStringLiteral("Could not create private runtime directory"));
    return;
  }
  pendingPinPath_ = path;
  pendingPinLogicalSize_ = canvasRect_.size().toSize();
  pinPending_ = true;
  setStatus(QStringLiteral("Preparing pinned capture…"));
  const CaptureData captureCopy = capture_;
  const QVector<Annotation> annotations = annotations_;
  const QRectF selection = selection_;
  const BackgroundStyle background = backgroundStyle_;
  const bool imageShadow = imageShadow_;
  const CanvasBoundaryMode canvasBoundary = canvasBoundaryMode_;
  pinWatcher_.setFuture(QtConcurrent::run(
      [captureCopy, annotations, selection, background, imageShadow,
       canvasBoundary] {
        return renderCapture(captureCopy, selection, annotations, background,
                             imageShadow, canvasBoundary);
      }));
}

void CaptureEditor::startCapture(CaptureMode mode, bool includeWindows) {
  if (captureStarted_ || !capture_.source.isNull())
    return;
  captureStarted_ = true;
  capturePending_ = true;
  pendingMode_ = mode;
  const MonitorInfo monitor = capture_.monitor;
  captureWatcher_.setFuture(QtConcurrent::run([monitor, includeWindows] {
    CaptureJob job;
    job.capture.monitor = monitor;
    job.ok =
        captureMonitorPixels(monitor, job.capture, includeWindows, job.error);
    return job;
  }));
}

void CaptureEditor::enterEdit(QString status) {
  phase_ = Phase::Edit;
  tool_ = Tool::Select;
  refreshCanvasRect();
  viewZoom_ = 1.0;
  viewOffset_ = {};
  setStatus(std::move(status));
  updatePointerCursor();
  if (quickOutputMode_ != QuickOutputMode::None) {
    const OutputMode output = quickOutputMode_ == QuickOutputMode::Copy
                                  ? OutputMode::Copy
                                  : quickOutputMode_ == QuickOutputMode::Save
                                        ? OutputMode::Save
                                        : OutputMode::Both;
    finish(output);
    return;
  }
  const QRectF full(QPointF(), capture_.previewSize);
  if (ops_.isEmpty() && !selection_.isEmpty() && selection_ != full)
    commitCrop(selection_);
  else
    scheduleSnapshot();
  if (windowedHandoffOnEdit_ && captureMode_ != CaptureMode::Scroll) {
    windowedHandoffOnEdit_ = false;
    handOffEditor(true);
  }
}

void CaptureEditor::handleEscape() {
  if (clipboardTextCardEditing_) {
    if (textCardInsertMode_)
      setClipboardTextCardInsertMode(false);
    return;
  }
  if (cutDragActive_) {
    cutDragActive_ = false;
    dragging_ = false;
    refreshComposedCapture();
    setStatus(QStringLiteral("Cut cancelled"));
    updatePointerCursor();
    update();
    return;
  }
  // Selecting: there is nothing to step back from, so one Esc closes (the
  // launch key then Esc is the quickest "never mind"). Only a drag in flight
  // is cancelled first. Editing: Esc steps back to the select tool, and a
  // second one close together closes.
  if (phase_ == Phase::Select) {
    if (!dragging_) {
      close();
      return;
    }
    dragging_ = false;
    selection_ = {};
    setStatus(windowMode_
                  ? QStringLiteral("Window mode · click or Super+Arrows then "
                                   "Enter · Space returns to area")
                  : QStringLiteral(
                        "Drag to select an area · Space selects a window"));
    updatePointerCursor();
    update();
    return;
  }
  const qint64 closeWindowMs =
      static_cast<qint64>(QApplication::doubleClickInterval()) * 2;
  if (escapeTimer_.isValid() && escapeTimer_.elapsed() <= closeWindowMs) {
    close();
    return;
  }
  escapeTimer_.restart();
  textEditor_->clear();
  textEditor_->hide();
  textCaretTimer_.stop();
  editingAnnotation_ = -1;
  if (dragStartStateValid_) {
    replayLog();
    scheduleSnapshot();
  }
  dragStartStateValid_ = false;
  dragChanged_ = false;
  creationConstraintActive_ = false;
  creationCenteredActive_ = false;
  resizeConstraintActive_ = false;
  dragging_ = false;
  interaction_ = Interaction::None;
  colorPaletteOpen_ = false;
  customColorPickerOpen_ = false;
  freehandPoints_.clear();
  highlighterLock_.reset();
  tool_ = Tool::Select;
  setStatus(QStringLiteral("Select/move · Esc again to close"));
  setFocus(Qt::OtherFocusReason);
  updatePointerCursor();
  update();
}

void CaptureEditor::chooseWindow(int index) {
  if (index < 0 || index >= capture_.windows.size())
    return;
  selection_ = QRectF(capture_.windows.at(index).rect);
  redactionBaseStale_ = true;
  windowMode_ = false;
  editedKind_ = SelectTab::Window;
  enterEdit(QStringLiteral(
      "Window selected · Select moves layers · Ctrl+wheel zooms · outer handles "
      "crop"));
}

void CaptureEditor::beginText(const QPointF &point, int annotationIndex,
                              int lineCapacity) {
  editingAnnotation_ = annotationIndex;
  QString existingText;
  if (annotationIndex >= 0 && annotationIndex < annotations_.size()) {
    const Annotation &annotation = annotations_.at(annotationIndex);
    textColor_ = annotation.color;
    textSize_ = annotation.size;
    textEditFont_ = annotation.textFont;
    textPoint_ =
        annotation.start -
        QPointF(0, QFontMetricsF(annotationTextFont(textSize_, textEditFont_))
                       .ascent());
    existingText = annotation.text;
    // An existing label has room for the lines it already has: Enter on its
    // last line commits, Shift+Enter adds one.
    lineCapacity = std::max(lineCapacity,
                            static_cast<int>(existingText.count('\n')) + 1);
  } else {
    textPoint_ = point;
    textColor_ = annotationColor();
    textSize_ = kTextSizes.at(static_cast<std::size_t>(textSizeIndex_));
    textEditFont_ = textFont_;
  }

  const QRectF sourceFrame = sourceFrameWidgetRect();
  const qreal scale = editScale();
  const QPointF position = sourceFrame.topLeft() + textPoint_ * scale;
  QFont displayFont = annotationTextFont(textSize_, textEditFont_);
  displayFont.setPixelSize(
      std::max(12, qRound(displayFont.pixelSize() * scale)));
  const QFontMetrics metrics(displayFont);
  textEditor_->setFont(displayFont);
  textLineCapacity_ = std::max(1, lineCapacity);
  // While typing, show the same cream pill the committed text will have.
  const TextBackground background =
      annotationIndex >= 0 && annotationIndex < annotations_.size()
          ? annotations_.at(annotationIndex).textBackground
          : textBackground_;
  const bool pill = background == TextBackground::Pill;
  textEditPill_ = pill;
  // Re-editing a wrapped layer keeps its width, so the draft breaks
  // exactly where the committed text did.
  textEditWrapWidth_ =
      annotationIndex >= 0 && annotationIndex < annotations_.size()
          ? annotations_.at(annotationIndex).textWidth
          : 0.0;
  const int pillPad = pill ? qRound(std::max(4.0, metrics.height() * 0.18)) : 0;
  textEditor_->setStyleSheet(
      QStringLiteral("QPlainTextEdit { color: %1; background: transparent; "
                     "border: none; margin: 0; padding: 0;"
                     " selection-background-color: #0a84ff; }")
          .arg(textColor_.name()));
  textEditor_->setViewportMargins(pillPad, 0, pillPad, 0);
  textEditor_->setGeometry(qRound(position.x()) - pillPad, qRound(position.y()),
                           72 + 2 * pillPad,
                           metrics.lineSpacing() + metrics.descent() + 4);
  textEditor_->setPlainText(existingText);
  textEditor_->show();
  textEditor_->raise();
  textEditor_->setFocus(Qt::MouseFocusReason);
  if (!existingText.isEmpty())
    textEditor_->selectAll();
  textCaretOn_ = true;
  textCaretTimer_.start();
}

void CaptureEditor::acceptText(bool keepSelected) {
  const QString text = textEditor_->toPlainText().trimmed();
  if (!text.isEmpty()) {
    Annotation annotation;
    annotation.kind = Annotation::Kind::Text;
    annotation.start =
        textPoint_ +
        QPointF(0, QFontMetricsF(annotationTextFont(textSize_, textEditFont_))
                       .ascent());
    annotation.text = text;
    annotation.color = textColor_;
    annotation.size = textSize_;
    annotation.textFont = textEditFont_;
    // Text that wrapped at the canvas edge freezes that shape on commit, as
    // tight as its widest line, so moving the layer later never reflows the
    // paragraph you just placed. The handle can still re-wrap it.
    annotation.textWidth = textEditWrapWidth_;
    if (annotation.textWidth <= 0.0) {
      const QStringList wrapped =
          annotationTextLines(annotation, selection_.width());
      if (wrapped.size() > 1) {
        const QFontMetricsF metrics(
            annotationTextFont(annotation.size, annotation.textFont));
        qreal widest = 0.0;
        for (const QString &line : wrapped)
          widest = std::max(widest, metrics.horizontalAdvance(line));
        annotation.textWidth = widest + 2.0;
      }
    }
    annotation.textBackground =
        editingAnnotation_ >= 0 && editingAnnotation_ < annotations_.size()
            ? annotations_.at(editingAnnotation_).textBackground
            : textBackground_;
    if (editingAnnotation_ >= 0 && editingAnnotation_ < annotations_.size()) {
      annotation.id = annotations_.at(editingAnnotation_).id;
      annotations_[editingAnnotation_] = annotation;
      selectedAnnotation_ = editingAnnotation_;
      selectedAnnotations_ = {editingAnnotation_};
      tool_ = Tool::Select;
      setStatus(QStringLiteral(
          "Text updated · Enter edits again · drag to move · handle resizes"));
      commitPatch({editingAnnotation_});
    } else {
      selectedAnnotation_ = -1;
      selectedAnnotations_.clear();
      setStatus(keepSelected
                    ? QStringLiteral(
                          "Text added · Backspace removes · Enter edits")
                    : QStringLiteral("Text added · Esc for select mode"));
      commitAnnotate(std::move(annotation));
      if (keepSelected && !annotations_.isEmpty()) {
        selectedAnnotation_ = annotations_.size() - 1;
        selectedAnnotations_ = {selectedAnnotation_};
        tool_ = Tool::Select;
      }
    }
  } else if (editingAnnotation_ >= 0 || keepSelected) {
    tool_ = Tool::Select;
  }
  editingAnnotation_ = -1;
  textEditor_->clear();
  textEditor_->hide();
  textCaretTimer_.stop();
  setFocus(Qt::OtherFocusReason);
  updatePointerCursor();
  update();
}

void CaptureEditor::selectWindowInDirection(int key) {
  int current = hoveredWindow_;
  if (current < 0)
    current = windowAt(cursor_);
  const int next = windowInDirection(current, key);
  if (next < 0)
    return;
  hoveredWindow_ = next;
  setStatus(QStringLiteral("%1 · Super+Arrows choose · Enter captures")
                .arg(capture_.windows.at(next).title));
}

void CaptureEditor::runOcr(const QRectF &localSelection) {
  if (busy_ || selection_.isEmpty())
    return;
  QRectF target = selection_;
  if (!localSelection.isEmpty()) {
    target = QRectF(selection_.topLeft() + localSelection.topLeft(),
                    localSelection.size())
                 .intersected(selection_);
  }
  if (target.width() < 2 || target.height() < 2)
    return;
  busy_ = true;
  ocrRegion_ = target.translated(-selection_.topLeft());
  ocrResultText_.clear();
  ocrResultTimer_.stop();
  ocrClock_.start();
  ocrAnimTimer_.start();
  setStatus(QStringLiteral("Reading selected text…"));

  QVector<Annotation> ocrAnnotations;
  const QPointF offset = selection_.topLeft() - target.topLeft();
  for (const Annotation &annotation : annotations_) {
    if (annotation.kind != Annotation::Kind::Redaction)
      continue;
    Annotation adjusted = annotation;
    adjusted.start += offset;
    adjusted.end += offset;
    for (QPointF &point : adjusted.points)
      point += offset;
    ocrAnnotations.push_back(std::move(adjusted));
  }

  // Render the OCR crop and recognize on the worker pool; a full-resolution
  // selection would otherwise stall the overlay while tesseract runs.
  const CaptureData captureCopy = capture_;
  ocrWatcher_.setFuture(QtConcurrent::run([captureCopy, target,
                                           ocrAnnotations]() mutable {
    OcrResult result;
    const QImage image = renderCapture(captureCopy, target, ocrAnnotations,
                                       BackgroundStyle::None);
    if (image.isNull())
      result.error = QStringLiteral("Could not prepare image for OCR");
    else
      result.text = recognizeText(image, result.error);
    return result;
  }));
}

void CaptureEditor::dismissOcrOverlay() {
  ocrAnimTimer_.stop();
  ocrResultTimer_.stop();
  ocrRegion_ = QRectF();
  ocrResultText_.clear();
  update();
}

void CaptureEditor::paintOcrOverlay(QPainter &painter, const QRectF &image,
                                    qreal scale) {
  if (ocrRegion_.isEmpty())
    return;
  const QRectF region(image.topLeft() + ocrRegion_.topLeft() * scale,
                      ocrRegion_.size() * scale);
  const QColor accent(QStringLiteral("#0a84ff"));
  painter.save();
  if (ocrResultText_.isEmpty()) {
    // Scanning: a tinted box with a bright band sweeping top to bottom, the
    // way a flatbed reads a page. Purely decorative; tesseract sets the pace.
    const qreal t = std::fmod(static_cast<qreal>(ocrClock_.elapsed()),
                              qreal(kOcrSweepMs)) /
                    qreal(kOcrSweepMs);
    const qreal bandHeight = std::clamp(region.height() * 0.35, 18.0, 64.0);
    const qreal y = region.top() - bandHeight + t * (region.height() + bandHeight);
    painter.setClipRect(region, Qt::IntersectClip);
    painter.fillRect(region, QColor(accent.red(), accent.green(), accent.blue(), 36));
    QLinearGradient gradient(0, y, 0, y + bandHeight);
    gradient.setColorAt(0.0, QColor(accent.red(), accent.green(), accent.blue(), 0));
    gradient.setColorAt(0.8, QColor(accent.red(), accent.green(), accent.blue(), 130));
    gradient.setColorAt(1.0, QColor(255, 255, 255, 230));
    painter.fillRect(QRectF(region.left(), y, region.width(), bandHeight),
                     gradient);
    painter.setClipping(false);
    painter.setPen(QPen(accent, 1.5));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(region);
    painter.restore();
    return;
  }

  // Result: the recognized text on a card anchored to the region, fading out
  // over the last part of its display time.
  constexpr int kFadeMs = 450;
  const int remaining = ocrResultTimer_.remainingTime();
  const qreal opacity =
      remaining < 0 ? 1.0 : std::clamp(remaining / qreal(kFadeMs), 0.0, 1.0);
  painter.setOpacity(opacity);

  QFont font(QStringLiteral("Noto Sans"));
  font.setPixelSize(13);
  painter.setFont(font);
  const QFontMetricsF metrics(font);
  constexpr qreal kPad = 12.0;
  constexpr qreal kHeaderGap = 6.0;
  // The card sits beside the image, not over it: in the gap to the right of
  // the image when there is room, otherwise pinned to the right edge of the
  // screen so the capture stays readable underneath.
  constexpr qreal kMargin = 12.0;
  constexpr qreal kGap = 16.0;
  const qreal rightGap = width() - image.right() - kMargin - kGap;
  const bool besideImage = rightGap >= 220.0;
  const qreal cardWidth =
      besideImage ? std::min(rightGap, 460.0)
                  : std::clamp(width() * 0.3, 260.0,
                               std::max(260.0, width() - 2 * kMargin));
  const qreal textWidth = cardWidth - 2 * kPad;
  const qreal maxTextHeight =
      std::max(metrics.lineSpacing() * 2, height() - 128 - 2 * kPad);
  const int flags = Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap;
  QRectF textBounds = metrics.boundingRect(
      QRectF(0, 0, textWidth, maxTextHeight), flags, ocrResultText_);
  const bool truncated = textBounds.height() > maxTextHeight;
  textBounds.setHeight(std::min(textBounds.height(), maxTextHeight));

  QFont headerFont = font;
  headerFont.setPixelSize(11);
  const qreal headerHeight = QFontMetricsF(headerFont).height();
  const qreal cardHeight =
      kPad + headerHeight + kHeaderGap + textBounds.height() + kPad;
  const qreal x = besideImage ? image.right() + kGap
                              : width() - cardWidth - kMargin;
  qreal y = std::max(region.top(), 68.0);
  if (y + cardHeight > height() - 60)
    y = std::max(68.0, height() - 60 - cardHeight);
  const QRectF card(x, y, cardWidth, cardHeight);

  painter.setPen(QPen(QColor(255, 255, 255, 40), 1));
  painter.setBrush(QColor(18, 18, 22, 240));
  painter.drawRoundedRect(card, 10, 10);
  painter.setPen(QPen(accent, 1.5));
  painter.setBrush(Qt::NoBrush);
  painter.drawRect(region);

  painter.setFont(headerFont);
  painter.setPen(QColor(accent.red(), accent.green(), accent.blue(), 255));
  painter.drawText(QRectF(card.left() + kPad, card.top() + kPad, textWidth,
                          headerHeight),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   truncated ? QStringLiteral("Copied to clipboard · shown in part")
                             : QStringLiteral("Copied to clipboard"));
  painter.setFont(font);
  painter.setPen(QColor(QStringLiteral("#f5f5f7")));
  const QRectF textRect(card.left() + kPad,
                        card.top() + kPad + headerHeight + kHeaderGap,
                        textWidth, textBounds.height());
  painter.setClipRect(textRect);
  painter.drawText(textRect, flags, ocrResultText_);
  painter.restore();
}

void CaptureEditor::finish(OutputMode mode) {
  if (busy_ || selection_.isEmpty())
    return;
  busy_ = true;
  setStatus(mode == OutputMode::Copy ? QStringLiteral("Copying screenshot…")
                                     : QStringLiteral("Saving screenshot…"));
  // Everything the export needs is copied out so the render, the PNG encode
  // and the wl-copy/wl-paste round trip can run on the worker pool. The
  // overlay keeps painting (and its status stays readable) while a tall
  // scroll capture grinds through libpng.
  const CaptureData captureCopy = capture_;
  const QRectF selection = selection_;
  const QVector<Annotation> annotations = annotations_;
  const BackgroundStyle background = backgroundStyle_;
  const bool imageShadow = imageShadow_;
  const CanvasBoundaryMode canvasBoundary = canvasBoundaryMode_;
  const QString appSlug =
      appFilenameSlug(dominantAppClass(capture_.windows, selection_));
  finishWatcher_.setFuture(QtConcurrent::run([captureCopy, selection,
                                              annotations, background,
                                              imageShadow, canvasBoundary,
                                              appSlug, mode]() {
    FinishResult result;
    result.mode = mode;
    const QImage image = renderCapture(captureCopy, selection, annotations,
                                       background, imageShadow,
                                       canvasBoundary);
    if (!image.isNull())
      result.thumbnail = image.scaled(kRecentThumbEdge, kRecentThumbEdge,
                                      Qt::KeepAspectRatio,
                                      Qt::SmoothTransformation);
    const QString exportPath = temporaryExportPath();
    QString error;
    if (image.isNull() || exportPath.isEmpty() ||
        !saveTemporarySnapshot(image, exportPath, error, -1)) {
      result.error = error.isEmpty()
                         ? QStringLiteral("Could not prepare screenshot snapshot")
                         : error;
      return result;
    }
    if (mode == OutputMode::Copy || mode == OutputMode::Both) {
      if (!copyPngFileToClipboard(exportPath, error)) {
        QFile::remove(exportPath);
        result.error = error;
        return result;
      }
    }
    if (mode == OutputMode::Save || mode == OutputMode::Both) {
      result.saved = moveSnapshotToScreenshots(exportPath, error, appSlug);
      if (result.saved.isEmpty()) {
        QFile::remove(exportPath);
        result.error = error;
        return result;
      }
    } else {
      QFile::remove(exportPath);
    }
    return result;
  }));
}

void CaptureEditor::completeFinish(const FinishResult &result) {
  if (!result.error.isEmpty()) {
    busy_ = false;
    setStatus(result.error);
    return;
  }
  if (!snapshotPath_.isEmpty()) {
    // The working document moves onto the recents shelf rather than being
    // thrown away: the select overlay offers it back, layers still editable.
    // Drain the last background write first so the log is the final state
    // (and so it cannot reappear a moment after the editor closed).
    QString recentError;
    const bool drained = waitForSnapshot();
    snapshotDirty_ = false;
    if (drained && recordRecentSnap(snapshotPath_, workingLogPath(),
                                    result.thumbnail, recentError)) {
      if (editingRecent_)
        removeRecentSnap(*editingRecent_);
    } else {
      if (!recentError.isEmpty())
        qWarning().noquote() << recentError;
      QFile::remove(workingLogPath());
      QFile::remove(snapshotPath_);
    }
    snapshotPath_.clear();
  }
  if (result.mode == OutputMode::Copy)
    sendCaptureNotification(QStringLiteral("Screenshot copied to clipboard"));
  else if (result.mode == OutputMode::Save)
    sendCaptureNotification(QStringLiteral("Screenshot saved"), result.saved);
  else
    sendCaptureNotification(QStringLiteral("Screenshot saved and copied"),
                            result.saved);
  close();
}

void CaptureEditor::handleToolbar(const QString &action) {
  const Tool toolBefore = tool_;
  const QString statusBefore = status_;
  if (action == QStringLiteral("tool-select"))
    tool_ = Tool::Select;
  else if (action.startsWith(QStringLiteral("tool-arrow-"))) {
    if (tool_ == Tool::Arrow)
      cycleArrowStyle();
    else
      tool_ = Tool::Arrow;
  }
  else if (action == QStringLiteral("tool-line"))
    tool_ = Tool::Line;
  else if (action == QStringLiteral("tool-freehand"))
    tool_ = Tool::Freehand;
  else if (action == QStringLiteral("tool-highlighter"))
    activateHighlighter();
  else if (action == QStringLiteral("tool-marker"))
    tool_ = Tool::Marker;
  else if (action == QStringLiteral("tool-rectangle") ||
           action == QStringLiteral("tool-ellipse") ||
           action == QStringLiteral("shape-rectangle") ||
           action == QStringLiteral("shape-ellipse")) {
    const Tool shape = action.endsWith(QStringLiteral("rectangle"))
                           ? Tool::Rectangle
                           : Tool::Ellipse;
    if (tool_ == shape && action.startsWith(QStringLiteral("tool-")))
      toggleShapeFill();
    else
      tool_ = shape;
  } else if (action == QStringLiteral("shape-fill")) {
    if (tool_ != Tool::Rectangle && tool_ != Tool::Ellipse)
      tool_ = Tool::Rectangle;
    toggleShapeFill();
  } else if (action == QStringLiteral("tool-spotlight")) {
    if (tool_ == Tool::Spotlight) {
      spotlightShape_ = spotlightShape_ == SpotlightShape::Ellipse
                            ? SpotlightShape::Rectangle
                            : spotlightShape_ == SpotlightShape::Rectangle
                                  ? SpotlightShape::RoundedRectangle
                                  : SpotlightShape::Ellipse;
      setStatus(toolStatus());
    } else {
      tool_ = Tool::Spotlight;
    }
    selectedAnnotation_ = -1;
  }
  else if (action == QStringLiteral("tool-redact")) {
    if (tool_ == Tool::Redact) {
      redactionStyle_ = redactionStyle_ == RedactionStyle::Solid
                            ? RedactionStyle::Pixelate
                            : RedactionStyle::Solid;
    } else {
      tool_ = Tool::Redact;
    }
    selectedAnnotation_ = -1;
    setStatus(QStringLiteral("Redact: %1 · drag sensitive content · D toggles")
                  .arg(redactionStyleName(redactionStyle_)));
  } else if (action == QStringLiteral("tool-cut")) {
    tool_ = Tool::Cut;
    selectedAnnotation_ = -1;
    setStatus(QStringLiteral("Cut: drag across a band to remove it"));
  } else if (action == QStringLiteral("tool-text")) {
    if (tool_ == Tool::Text)
      toggleTextBackground();
    else
      tool_ = Tool::Text;
  } else if (action == QStringLiteral("tool-eyedropper")) {
    if (tool_ != Tool::Eyedropper)
      toolBeforeEyedropper_ = tool_;
    tool_ = Tool::Eyedropper;
    customColorPickerOpen_ = false;
  }
  else if (action == QStringLiteral("tool-ocr")) {
    runOcr();
    return;
  }
  else if (action == QStringLiteral("palette"))
    colorPaletteOpen_ = true;
  else if (action.startsWith(QStringLiteral("color-"))) {
    colorIndex_ = std::clamp(action.sliced(6).toInt(), 0,
                             static_cast<int>(paletteConfig_.palette.size()) - 1);
    usingCustomColor_ = false;
    customColorPickerOpen_ = false;
    if (selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size() &&
        annotations_.at(selectedAnnotation_).kind !=
            Annotation::Kind::Redaction) {
      annotations_[selectedAnnotation_].color = annotationColor();
      commitPatch({selectedAnnotation_});
    }
  } else if (action == QStringLiteral("custom-color")) {
    usingCustomColor_ = true;
    customColorPickerOpen_ = !customColorPickerOpen_;
  } else if (action == QStringLiteral("ocr"))
    runOcr();
  else if (action == QStringLiteral("background"))
    cycleBackground();
  else if (action == QStringLiteral("undo")) {
    undoEdit();
  } else if (action == QStringLiteral("redo")) {
    redoEdit();
  } else if (action == QStringLiteral("pin"))
    pinSnapshot();
  else if (action == QStringLiteral("copy"))
    finish(OutputMode::Copy);
  else if (action == QStringLiteral("both"))
    finish(OutputMode::Both);
  else if (action == QStringLiteral("save"))
    finish(OutputMode::Save);
  else if (action == QStringLiteral("close"))
    close();
  if (tool_ != toolBefore && status_ == statusBefore)
    setStatus(toolStatus());
  updatePointerCursor();
  update();
}

void CaptureEditor::keyPressEvent(QKeyEvent *event) {
  modifiersSeen_ = true;
  if (clipboardTextCardEditing_) {
    if (textCardFilenameEditor_->isVisible())
      textCardFilenameEditor_->setFocus(Qt::ShortcutFocusReason);
    else
      textCardEditor_->setFocus(Qt::ShortcutFocusReason);
    event->accept();
    return;
  }
  if (!ocrResultText_.isEmpty()) {
    const int key = event->key();
    const bool modifierOnly = key == Qt::Key_Shift || key == Qt::Key_Control ||
                              key == Qt::Key_Alt || key == Qt::Key_Meta;
    if (!modifierOnly)
      dismissOcrOverlay();
    // Esc only puts the card away; it should not also back out of the tool.
    if (key == Qt::Key_Escape)
      return;
  }
  const Tool toolBefore = tool_;
  const QString statusBefore = status_;
  if (capturePending_) {
    if (event->key() == Qt::Key_Escape)
      handleEscape();
    event->accept();
    return;
  }
  if (event->key() == Qt::Key_Shift && phase_ == Phase::Edit && dragging_) {
    // Shift pressed mid-drag constrains the drag: creation for drawing
    // tools, or handle resizing whether Select or a drawing tool is armed.
    const bool resizing =
        isLayerResize(interaction_);
    if (resizing)
      resizeConstraintActive_ = true;
    else if (supportsCreationConstraint(tool_))
      creationConstraintActive_ = true;
    if (resizing || supportsCreationConstraint(tool_)) {
      event->accept();
      update();
      return;
    }
  }
  if (event->key() == Qt::Key_Alt && phase_ == Phase::Edit && dragging_ &&
      supportsCenteredCreation(tool_)) {
    creationCenteredActive_ = true;
    event->accept();
    update();
    return;
  }
  if (event->key() == Qt::Key_Escape) {
    handleEscape();
    return;
  }
  if (phase_ == Phase::Select) {
    const Qt::KeyboardModifiers modifiers = event->modifiers();
    if (event->key() == Qt::Key_V &&
        modifiers.testFlag(Qt::ControlModifier) &&
        modifiers.testFlag(Qt::ShiftModifier) &&
        !modifiers.testFlag(Qt::AltModifier) &&
        !modifiers.testFlag(Qt::MetaModifier)) {
      openClipboardTextCard();
      return;
    }
    if (event->matches(QKeySequence::SelectAll)) {
      selectFullscreen();
      return;
    }
    const bool directionalKey =
        event->key() == Qt::Key_Left || event->key() == Qt::Key_Right ||
        event->key() == Qt::Key_Up || event->key() == Qt::Key_Down;
    if (windowMode_ && directionalKey &&
        event->modifiers().testFlag(Qt::MetaModifier)) {
      selectWindowInDirection(event->key());
      event->accept();
      update();
      return;
    }
    if (windowMode_ &&
        (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
      chooseWindow(hoveredWindow_);
      return;
    }
    if (!windowMode_ && !dragging_ && event->key() == Qt::Key_R &&
        !event->modifiers()) {
      // R brings back the last region drawn this session, written for this
      // monitor at this size; anything else in the file is simply ignored.
      const QString path = storedCaptureRegionPath();
      if (!path.isEmpty()) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
          const QRect region =
              parseStoredRegion(QString::fromUtf8(file.readLine(256)),
                                capture_.monitor.name, size());
          if (!region.isEmpty()) {
            commitRegion(QRectF(region),
                         QStringLiteral("Last area restored · Select moves "
                                        "layers · Ctrl+wheel zooms · outer handles "
                                        "crop"));
            update();
          }
        }
      }
      return;
    }
    if (event->key() == Qt::Key_S && !event->modifiers()) {
      setScrollMode(!scrollMode_);
      return;
    }
    if (event->key() == Qt::Key_Space) {
      // Space steps along the tab strip. Fullscreen is skipped: it captures
      // on the spot, and a cycle key that fires it on the way past would be
      // a trap rather than a mode.
      const QVector<CaptureTab> tabs = selectTabItems();
      int current = -1;
      for (int index = 0; index < tabs.size(); ++index) {
        if (tabs.at(index).kind == selectKind()) {
          current = index;
          break;
        }
      }
      for (int step = 1; step <= tabs.size(); ++step) {
        const SelectTab next = tabs.at((current + step) % tabs.size()).kind;
        if (next != SelectTab::Fullscreen) {
          activateSelectTab(next);
          break;
        }
      }
      return;
    }
    QWidget::keyPressEvent(event);
    return;
  }

  if (cutDragActive_) {
    // Any key here (Esc's own cancel already returned above) leaves the
    // A tool-switch key would otherwise leave the preview active because the
    // release no longer reaches the Cut branch. Cancel first, then handle
    // the key against the unchanged committed capture.
    cutDragActive_ = false;
    dragging_ = false;
    refreshComposedCapture();
    setStatus(QStringLiteral("Cut cancelled"));
  }

  const bool redoShortcut = event->matches(QKeySequence::Redo) ||
                            (event->key() == Qt::Key_Y &&
                             event->modifiers().testFlag(Qt::ControlModifier));
  // Zoom keys. The bare keys work in the edit phase too, so zoom never
  // depends on a modifier reaching us: a remote keyboard bridge may inject
  // the modifier in a way the compositor never publishes as xkb state.
  const bool zoomModifier =
      event->modifiers().testFlag(Qt::ControlModifier) || phase_ == Phase::Edit;
  if (event->matches(QKeySequence::ZoomIn) ||
      (zoomModifier &&
       (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal))) {
    setViewZoom(viewZoom_ * 1.25, editImageRect().center());
    setStatus(QStringLiteral("Zoom %1% · + / - zoom · 0 fits · wheel scrolls")
                  .arg(qRound(viewZoom_ *
                              (baseImageRect().width() /
                               std::max<qreal>(canvasRect_.width(), 1)) *
                              100)));
    return;
  } else if (event->matches(QKeySequence::ZoomOut) ||
             (zoomModifier && (event->key() == Qt::Key_Minus ||
                               event->key() == Qt::Key_Underscore))) {
    setViewZoom(viewZoom_ / 1.25, editImageRect().center());
    setStatus(QStringLiteral("Zoom %1% · + / - zoom · 0 fits · wheel scrolls")
                  .arg(qRound(viewZoom_ *
                              (baseImageRect().width() /
                               std::max<qreal>(canvasRect_.width(), 1)) *
                              100)));
    return;
  } else if (zoomModifier && event->key() == Qt::Key_0) {
    resetView();
    return;
  }
  if (redoShortcut) {
    redoEdit();
  } else if (event->matches(QKeySequence::Undo)) {
    undoEdit();
  } else if (event->matches(QKeySequence::Copy)) {
    finish(OutputMode::Copy);
    return;
  } else if (event->matches(QKeySequence::Save)) {
    finish(OutputMode::Save);
    return;
  } else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
    // Enter on a selected label reopens it for editing; anywhere else it
    // finishes the capture.
    if (selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size() &&
        selectedAnnotations_.size() <= 1 &&
        annotations_.at(selectedAnnotation_).kind == Annotation::Kind::Text &&
        !dragging_ && !textEditor_->isVisible()) {
      tool_ = Tool::Select;
      beginText({}, selectedAnnotation_);
      update();
      return;
    }
    finish(OutputMode::Both);
    return;
  } else if (event->key() == Qt::Key_D &&
             event->modifiers() == Qt::AltModifier) {
    duplicateSelectedAnnotation();
  } else if (const QPointF nudge = arrowKeyDelta(
                 event->key(), heldModifiers(event->modifiers())
                                       .testFlag(Qt::ShiftModifier)
                                   ? kNudgeStepShift
                                   : kNudgeStep);
             !nudge.isNull() &&
             !heldModifiers(event->modifiers())
                  .testAnyFlags(Qt::ControlModifier | Qt::AltModifier |
                                Qt::MetaModifier) &&
             selectedAnnotation_ >= 0 &&
             selectedAnnotation_ < annotations_.size() && !dragging_ &&
             !textEditor_->isVisible()) {
    nudgeSelectedAnnotation(nudge);
  } else if (viewZoom_ > 1.0 && selectedAnnotation_ < 0 && !dragging_ &&
             !textEditor_->isVisible() &&
             !event->modifiers().testAnyFlags(Qt::ControlModifier |
                                              Qt::AltModifier |
                                              Qt::MetaModifier) &&
             (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right ||
              event->key() == Qt::Key_Up || event->key() == Qt::Key_Down)) {
    // With nothing selected the arrows have nothing else to do, so they walk
    // the zoomed capture. It is the one way to reach the middle of a long
    // capture without a middle mouse button. Shift takes a page at a time.
    const qreal step =
        event->modifiers().testFlag(Qt::ShiftModifier) ? 240.0 : 60.0;
    const QPointF delta =
        event->key() == Qt::Key_Left    ? QPointF(step, 0)
        : event->key() == Qt::Key_Right ? QPointF(-step, 0)
        : event->key() == Qt::Key_Up    ? QPointF(0, step)
                                        : QPointF(0, -step);
    panView(delta);
    setStatus(QStringLiteral("Panning · arrows move, Shift jumps · middle-drag "
                             "pans · Ctrl+0 fits"));
  } else if ((event->key() == Qt::Key_Delete ||
              event->key() == Qt::Key_Backspace) &&
             !selectedAnnotations_.isEmpty()) {
    commitDelete(selectedAnnotations_);
    selectedAnnotations_.clear();
    selectedAnnotation_ = -1;
  } else if (event->key() == Qt::Key_V) {
    tool_ = Tool::Select;
  } else if (event->matches(QKeySequence::SelectAll)) {
    selectAllAnnotations();
  } else if (event->key() == Qt::Key_A) {
    if (tool_ == Tool::Arrow)
      cycleArrowStyle();
    else
      tool_ = Tool::Arrow;
  } else if (event->key() == Qt::Key_L) {
    tool_ = Tool::Line;
  } else if (event->key() == Qt::Key_F) {
    tool_ = Tool::Freehand;
  } else if (event->key() == Qt::Key_H) {
    activateHighlighter();
  } else if (event->key() == Qt::Key_C || event->key() == Qt::Key_M) {
    tool_ = Tool::Marker;
  } else if (event->key() == Qt::Key_R || event->key() == Qt::Key_E) {
    const bool rectangle = event->key() == Qt::Key_R;
    const Tool shape = rectangle ? Tool::Rectangle : Tool::Ellipse;
    if (!dragging_ && selectedAnnotation_ >= 0 &&
        selectedAnnotation_ < annotations_.size() &&
        annotations_.at(selectedAnnotation_).kind == dragShapeKind(shape)) {
      Annotation &selected = annotations_[selectedAnnotation_];
      selected.filled = !selected.filled;
      setStatus(
          QStringLiteral("Selected %1: %2 · %3 again toggles fill")
              .arg(rectangle ? QStringLiteral("rectangle")
                             : QStringLiteral("ellipse"))
              .arg(fillName(selected.filled).toLower())
              .arg(rectangle ? QStringLiteral("R") : QStringLiteral("E")));
      commitPatch({selectedAnnotation_});
    } else if (tool_ == shape) {
      toggleShapeFill();
    } else {
      tool_ = shape;
    }
  } else if (event->key() == Qt::Key_S) {
    if (tool_ == Tool::Spotlight) {
      spotlightShape_ = spotlightShape_ == SpotlightShape::Ellipse
                            ? SpotlightShape::Rectangle
                            : spotlightShape_ == SpotlightShape::Rectangle
                                  ? SpotlightShape::RoundedRectangle
                                  : SpotlightShape::Ellipse;
      setStatus(toolStatus());
    } else {
      tool_ = Tool::Spotlight;
    }
    selectedAnnotation_ = -1;
  } else if (event->key() == Qt::Key_D) {
    if (selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size() &&
        annotations_.at(selectedAnnotation_).kind ==
            Annotation::Kind::Redaction) {
      Annotation &redaction = annotations_[selectedAnnotation_];
      redaction.redactionStyle =
          redaction.redactionStyle == RedactionStyle::Solid
              ? RedactionStyle::Pixelate
              : RedactionStyle::Solid;
      setStatus(QStringLiteral("Selected redaction: %1 · D toggles")
                    .arg(redactionStyleName(redaction.redactionStyle)));
      commitPatch({selectedAnnotation_});
    } else {
      if (tool_ == Tool::Redact) {
        redactionStyle_ = redactionStyle_ == RedactionStyle::Solid
                              ? RedactionStyle::Pixelate
                              : RedactionStyle::Solid;
      } else {
        tool_ = Tool::Redact;
      }
      selectedAnnotation_ = -1;
      setStatus(
          QStringLiteral("Redact: %1 · drag sensitive content · D toggles")
              .arg(redactionStyleName(redactionStyle_)));
    }
  } else if (event->key() == Qt::Key_X) {
    tool_ = Tool::Cut;
    setStatus(QStringLiteral("Cut: drag across a band to remove it"));
  } else if (event->key() == Qt::Key_T &&
             event->modifiers() == Qt::ShiftModifier) {
    cycleTextFont();
  } else if (event->key() == Qt::Key_T) {
    const bool textSelected =
        selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size() &&
        annotations_.at(selectedAnnotation_).kind == Annotation::Kind::Text;
    if (tool_ == Tool::Text || textSelected)
      toggleTextBackground();
    else
      tool_ = Tool::Text;
  } else if (event->key() == Qt::Key_I) {
    if (tool_ != Tool::Eyedropper)
      toolBeforeEyedropper_ = tool_;
    tool_ = Tool::Eyedropper;
  } else if (event->key() == Qt::Key_O) {
    runOcr();
    return;
  } else if (event->key() == Qt::Key_P) {
    pinSnapshot();
    return;
  } else if (event->key() == Qt::Key_W) {
    handOffEditor(!windowedPresentation_);
    return;
  } else if (event->key() == Qt::Key_G) {
    cycleCanvasBoundary(
        event->modifiers().testFlag(Qt::ShiftModifier));
  } else if (event->key() == Qt::Key_B) {
    if (event->modifiers().testFlag(Qt::ShiftModifier)) {
      const bool next = !imageShadow_;
      setStatus(QStringLiteral("Drop shadow: %1 · Shift+B toggles")
                    .arg(next ? QStringLiteral("on") : QStringLiteral("off")));
      commitBackground(backgroundStyle_, next);
    } else
      cycleBackground();
  } else if (event->key() >= Qt::Key_1 && event->key() <= Qt::Key_8) {
    colorIndex_ = event->key() - Qt::Key_1;
    usingCustomColor_ = false;
    if (selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size() &&
        annotations_.at(selectedAnnotation_).kind !=
            Annotation::Kind::Redaction) {
      annotations_[selectedAnnotation_].color = annotationColor();
      commitPatch({selectedAnnotation_});
    }
  } else {
    QWidget::keyPressEvent(event);
    return;
  }
  // Arming a tool says what it is set to, so its options are discoverable
  // without pressing keys to find out. Handlers that report something more
  // specific (a restyled layer, a toggled fill) have already set it.
  if (tool_ != toolBefore && status_ == statusBefore)
    setStatus(toolStatus());
  updatePointerCursor();
  update();
}

void CaptureEditor::keyReleaseEvent(QKeyEvent *event) {
  modifiersSeen_ = true;
  if (event->key() == Qt::Key_Shift &&
      (creationConstraintActive_ || resizeConstraintActive_)) {
    creationConstraintActive_ = false;
    resizeConstraintActive_ = false;
    event->accept();
    update();
    return;
  }
  if (event->key() == Qt::Key_Alt && creationCenteredActive_) {
    creationCenteredActive_ = false;
    event->accept();
    update();
    return;
  }
  QWidget::keyReleaseEvent(event);
}

void CaptureEditor::leaveEvent(QEvent *event) {
  highlighterPreview_.reset();
  QWidget::leaveEvent(event);
  update();
}

void CaptureEditor::mouseMoveEvent(QMouseEvent *event) {
  if (clipboardTextCardEditing_) {
    cursor_ = event->position();
    return;
  }
  if (panning_) {
    panView(event->position() - panAnchor_);
    panAnchor_ = event->position();
    return;
  }
  cursor_ = event->position();
  if (capturePending_)
    return;
  if (phase_ == Phase::Select) {
    if (!dragging_)
      trackRecentsHover();
    if (windowMode_)
      hoveredWindow_ = recentsOpen_ ? -1 : windowAt(cursor_);
    else if (dragging_)
      selection_ = normalizedSelection(dragStart_, cursor_);
    if (!dragging_)
      updatePointerCursor();
  } else {
    if (tool_ == Tool::Select && marqueeSelecting_) {
      marqueeRect_ = QRectF(dragStart_, toAnnotationPoint(cursor_)).normalized();
      update();
      return;
    }
    if (tool_ == Tool::Select && dragging_ &&
        interaction_ >= Interaction::CropTopLeft) {
      if (cropDragImageRect_.width() <= 0.0 ||
          cropDragImageRect_.height() <= 0.0)
        return;
      const int handle = static_cast<int>(interaction_) -
                         static_cast<int>(Interaction::CropTopLeft);
      const QSizeF previewSize = capture_.previewSize;
      if (previewSize.width() <= 0.0 || previewSize.height() <= 0.0)
        return;
      const qreal sourceX = originalSelection_.left() +
                            (cursor_.x() - cropDragImageRect_.left()) *
                                originalSelection_.width() /
                                cropDragImageRect_.width();
      const qreal sourceY =
          originalSelection_.top() + (cursor_.y() - cropDragImageRect_.top()) *
                                         originalSelection_.height() /
                                         cropDragImageRect_.height();
      constexpr qreal minimumCrop = 16;
      QRectF updated = originalSelection_;
      if (handle == 0 || handle == 6 || handle == 7) {
        const qreal maxLeft = std::clamp(
            originalSelection_.right() - minimumCrop, 0.0, previewSize.width());
        updated.setLeft(std::clamp(sourceX, 0.0, maxLeft));
      }
      if (handle == 2 || handle == 3 || handle == 4) {
        const qreal minRight = std::clamp(
            originalSelection_.left() + minimumCrop, 0.0, previewSize.width());
        updated.setRight(std::clamp(sourceX, minRight, previewSize.width()));
      }
      if (handle == 0 || handle == 1 || handle == 2) {
        const qreal maxTop = std::clamp(
            originalSelection_.bottom() - minimumCrop, 0.0, previewSize.height());
        updated.setTop(std::clamp(sourceY, 0.0, maxTop));
      }
      if (handle == 4 || handle == 5 || handle == 6) {
        const qreal minBottom = std::clamp(
            originalSelection_.top() + minimumCrop, 0.0, previewSize.height());
        updated.setBottom(
            std::clamp(sourceY, minBottom, previewSize.height()));
      }
      const QPointF annotationDelta = selection_.topLeft() - updated.topLeft();
      if (!annotationDelta.isNull()) {
        for (Annotation &annotation : annotations_) {
          annotation.start += annotationDelta;
          if (hasEndpointHandles(annotation.kind))
            annotation.end += annotationDelta;
          if (annotation.curveControl)
            *annotation.curveControl += annotationDelta;
          if (isStrokeKind(annotation.kind)) {
            for (QPointF &point : annotation.points)
              point += annotationDelta;
            for (QPointF &point : annotation.rawPoints)
              point += annotationDelta;
            if (!annotation.points.isEmpty()) {
              annotation.start = annotation.points.first();
              annotation.end = annotation.points.last();
            }
          }
        }
      }
      selection_ = updated;
      if (updated != originalSelection_)
        dragChanged_ = true;
    } else if ((tool_ == Tool::Select ||
                interaction_ == Interaction::Move ||
                isLayerResize(interaction_)) &&
               dragging_ && selectedAnnotation_ >= 0 &&
               selectedAnnotation_ < annotations_.size()) {
      // Keep the drag in the canvas mapping that existed at press time. The
      // canvas settles once on release; re-fitting it under the pointer on
      // every motion would feed the new scale back into the geometry and
      // make an edge drag jitter.
      const QPointF point = toUnclampedAnnotationPoint(cursor_);
      if (selectedAnnotations_.size() > 1 && interaction_ == Interaction::Move) {
        const QPointF delta = point - dragStart_;
        for (int position = 0;
             position < selectedAnnotations_.size() &&
             position < originalSelectedAnnotations_.size();
             ++position) {
          const int index = selectedAnnotations_.at(position);
          Annotation &annotation = annotations_[index];
          annotation = originalSelectedAnnotations_.at(position);
          translateAnnotation(annotation, delta);
        }
        dragChanged_ = true;
      } else {
        Annotation &annotation = annotations_[selectedAnnotation_];
        annotation = originalAnnotation_;
        const QRectF originalBox = annotationBounds(originalAnnotation_);
        if (interaction_ == Interaction::Move) {
          translateAnnotation(annotation, point - dragStart_);
        } else if (isBoxResize(interaction_)) {
          applyBoxResize(annotation, interaction_, point, originalBox);
        } else if (interaction_ == Interaction::ResizeStart) {
          if (hasEndpointHandles(annotation.kind)) {
            annotation.start = constrainedResizeEndpoint(
                annotation, point, annotation.end, originalAnnotation_.start);
          }
        } else if (interaction_ == Interaction::ResizeEnd) {
          if (hasEndpointHandles(annotation.kind)) {
            annotation.end = constrainedResizeEndpoint(
                annotation, point, annotation.start, originalAnnotation_.end);
          } else if (isStrokeKind(annotation.kind)) {
            const QRectF originalBounds = annotationBounds(originalAnnotation_);
            const qreal scaleX =
                originalBounds.width() > 0
                    ? std::max<qreal>(
                          0.05, (point.x() - originalBounds.left()) /
                                    originalBounds.width())
                    : 1.0;
            const qreal scaleY =
                originalBounds.height() > 0
                    ? std::max<qreal>(
                          0.05, (point.y() - originalBounds.top()) /
                                    originalBounds.height())
                    : 1.0;
            const auto resizePoints = [&](QVector<QPointF> &resized,
                                          const QVector<QPointF> &source) {
              resized.resize(source.size());
              for (qsizetype index = 0; index < source.size(); ++index) {
                const QPointF relative =
                    source.at(index) - originalBounds.topLeft();
                resized[index] =
                    originalBounds.topLeft() +
                    QPointF(relative.x() * scaleX, relative.y() * scaleY);
              }
            };
            resizePoints(annotation.points, originalAnnotation_.points);
            resizePoints(annotation.rawPoints, originalAnnotation_.rawPoints);
            if (!annotation.points.isEmpty()) {
              annotation.start = annotation.points.first();
              annotation.end = annotation.points.last();
            }
          } else if (annotation.kind == Annotation::Kind::Marker) {
            annotation.size = std::clamp(
                QLineF(annotation.start, point).length() / 3.0, 2.0, 30.0);
          } else if (annotation.kind == Annotation::Kind::Text) {
            // The handle sets the wrap width, which is what its horizontal
            // cursor has always promised. Size belongs to the wheel, so the
            // two are one gesture each rather than both scaling the layer.
            const QRectF originalBounds = annotationBounds(originalAnnotation_);
            annotation.textWidth = std::max<qreal>(
                kMinimumTextWrapWidth, point.x() - originalBounds.left());
          }
        } else if (interaction_ == Interaction::ResizeControl &&
                   annotation.kind == Annotation::Kind::Arrow &&
                   (annotation.arrowStyle == ArrowStyle::Curved ||
                    annotation.arrowStyle == ArrowStyle::Double)) {
          QPointF handlePoint = point;
          if (resizeConstraintActive_) {
            // Keep the visible t=.5 handle centered along the chord while its
            // perpendicular distance remains under the pointer. This preserves
            // an adjustable, symmetric bend instead of flattening the arrow.
            const QPointF chord = annotation.end - annotation.start;
            const qreal chordLengthSquared = QPointF::dotProduct(chord, chord);
            if (!qFuzzyIsNull(chordLengthSquared)) {
              const QPointF midpoint =
                  (annotation.start + annotation.end) * 0.5;
              handlePoint -=
                  chord *
                  (QPointF::dotProduct(handlePoint - midpoint, chord) /
                   chordLengthSquared);
            }
          }
          // The pointer owns the visible t=.5 point on the quadratic. Solve
          // M=.25*S+.5*C+.25*E for C so the handle tracks it exactly.
          annotation.curveControl =
              handlePoint * 2.0 - (annotation.start + annotation.end) * 0.5;
        }
      }
      dragChanged_ = true;
    }
    if (tool_ == Tool::Cut && dragging_) {
      QPointF point = toUnclampedAnnotationPoint(cursor_);
      point.setX(std::clamp(point.x(), 0.0, selection_.width()));
      point.setY(std::clamp(point.y(), 0.0, selection_.height()));
      const QPointF delta = point - cutDragStart_;
      if (!cutDragActive_ &&
          std::max(std::abs(delta.x()), std::abs(delta.y())) > 3.0) {
        cutDragActive_ = true;
        // Dominant vertical delta cuts a horizontal band (rows); dominant
        // horizontal delta cuts a vertical band (columns).
        liveCut_.orientation = std::abs(delta.y()) >= std::abs(delta.x())
                                    ? Qt::Horizontal
                                    : Qt::Vertical;
        // Lock the source/preview mapping together with the drag axis. The
        // capture remains unchanged until this preview is committed.
        cutDragOriginOffset_ = liveCut_.orientation == Qt::Horizontal
                                    ? selection_.top()
                                    : selection_.left();
        cutDragRatio_ =
            liveCut_.orientation == Qt::Horizontal
                ? capture_.source.height() /
                      static_cast<qreal>(capture_.previewSize.height())
                : capture_.source.width() /
                      static_cast<qreal>(capture_.previewSize.width());
      }
      if (cutDragActive_) {
        const bool horizontal = liveCut_.orientation == Qt::Horizontal;
        const qreal a = horizontal ? cutDragStart_.y() : cutDragStart_.x();
        const qreal b = horizontal ? point.y() : point.x();
        const qreal lo = std::min(a, b);
        const qreal hi = std::max(a, b);
        cutBandLo_ = lo;
        cutBandHi_ = hi;
        // Annotation space is selection-relative logical px; map to
        // absolute logical, then to native source px via the cached ratio.
        liveCut_.sourceStart = static_cast<int>(
            std::round((cutDragOriginOffset_ + lo) * cutDragRatio_));
        liveCut_.sourceEnd = static_cast<int>(
            std::round((cutDragOriginOffset_ + hi) * cutDragRatio_));
        liveCut_.logicalStart =
            static_cast<int>(std::round(cutDragOriginOffset_ + lo));
        liveCut_.logicalEnd =
            static_cast<int>(std::round(cutDragOriginOffset_ + hi));
      }
    }
    if ((tool_ == Tool::Freehand || tool_ == Tool::Highlighter) && dragging_ &&
        interaction_ == Interaction::None) {
      QPointF point = toUnclampedAnnotationPoint(cursor_);
      if (tool_ == Tool::Highlighter && highlighterLock_)
        point.setY(highlighterLock_->centerY);
      if (freehandPoints_.isEmpty() ||
          QLineF(freehandPoints_.last(), point).length() >= 1.5)
        freehandPoints_.push_back(point);
    }
    bool overPaletteAnchor = false;
    bool overCustomAnchor = false;
    bool overShapeAnchor = false;
    bool overTextAnchor = false;
    for (const ToolbarButton &button : toolbarButtons()) {
      if (button.action == QStringLiteral("palette") &&
          button.rect.contains(cursor_)) {
        overPaletteAnchor = true;
        paletteIntentOrigin_ = cursor_;
      } else if (button.action == QStringLiteral("custom-color") &&
                 button.rect.contains(cursor_)) {
        overCustomAnchor = true;
        customColorIntentOrigin_ = cursor_;
      } else if ((button.action == QStringLiteral("tool-rectangle") ||
                  button.action == QStringLiteral("tool-ellipse")) &&
                 button.rect.contains(cursor_)) {
        overShapeAnchor = true;
        shapeIntentOrigin_ = cursor_;
      } else if (button.action == QStringLiteral("tool-text") &&
                 button.rect.contains(cursor_)) {
        overTextAnchor = true;
        textSizeIntentOrigin_ = cursor_;
      }
    }
    const bool overPalette =
        colorPaletteOpen_ &&
        colorPaletteRect().adjusted(0, -4, 0, 0).contains(cursor_);
    const bool overCustom =
        customColorPickerOpen_ && customColorPanelRect().contains(cursor_);
    const bool approachingPalette =
        colorPaletteOpen_ &&
        inDownwardSubmenuTriangle(paletteIntentOrigin_, colorPaletteRect(),
                                  cursor_);
    const bool approachingCustom =
        customColorPickerOpen_ &&
        inDownwardSubmenuTriangle(customColorIntentOrigin_,
                                  customColorPanelRect(), cursor_);
    const bool overShapes = shapeMenuOpen_ && shapeMenuRect().contains(cursor_);
    const bool approachingShapes =
        shapeMenuOpen_ &&
        inDownwardSubmenuTriangle(shapeIntentOrigin_, shapeMenuRect(), cursor_);
    shapeMenuOpen_ = overShapeAnchor || overShapes || approachingShapes;
    if (!shapeMenuOpen_)
      shapeIntentOrigin_ = {};
    const bool overTextSizes =
        textSizeMenuOpen_ && textSizePanelRect().contains(cursor_);
    const bool approachingTextSizes =
        textSizeMenuOpen_ &&
        inDownwardSubmenuTriangle(textSizeIntentOrigin_, textSizePanelRect(),
                                  cursor_);
    textSizeMenuOpen_ = overTextAnchor || overTextSizes || approachingTextSizes;
    if (!textSizeMenuOpen_)
      textSizeIntentOrigin_ = {};
    colorPaletteOpen_ = overPaletteAnchor || overPalette || overCustom ||
                        overCustomAnchor || approachingPalette ||
                        approachingCustom;
    if (!colorPaletteOpen_) {
      customColorPickerOpen_ = false;
      paletteIntentOrigin_ = {};
      customColorIntentOrigin_ = {};
    }
  }
  updatePointerCursor();
  update();
}

void CaptureEditor::mouseDoubleClickEvent(QMouseEvent *event) {
  if (clipboardTextCardEditing_)
    return;
  if (phase_ != Phase::Edit || event->button() != Qt::LeftButton ||
      !editImageRect().contains(event->position()))
    return;
  const int index = annotationAt(toAnnotationPoint(event->position()));
  if (index < 0 || annotations_.at(index).kind != Annotation::Kind::Text)
    return;
  selectedAnnotation_ = index;
  tool_ = Tool::Select;
  beginText({}, index);
  event->accept();
  update();
}

void CaptureEditor::mousePressEvent(QMouseEvent *event) {
  if (busy_ || capturePending_)
    return;
  if (clipboardTextCardEditing_) {
    if (event->button() == Qt::LeftButton &&
        textCardFilenameEditor_->geometry().contains(
            event->position().toPoint())) {
      beginClipboardTextCardFilenameEdit();
      event->accept();
      return;
    }
    textCardEditor_->setFocus(Qt::MouseFocusReason);
    event->accept();
    return;
  }
  if (!ocrResultText_.isEmpty())
    dismissOcrOverlay();
  if (event->button() == Qt::RightButton) {
    if (phase_ == Phase::Select) {
      if (dragging_) {
        dragging_ = false;
        selection_ = {};
        update();
      }
    } else if (phase_ == Phase::Edit) {
      if (textEditor_->isVisible())
        acceptText();
      if (dragging_) {
        handleEscape();
      } else if (tool_ != Tool::Select || selectedAnnotation_ >= 0) {
        tool_ = Tool::Select;
        selectedAnnotation_ = -1;
        setStatus(QStringLiteral("Select/move · Esc again to close"));
        updatePointerCursor();
        update();
      }
    }
    return;
  }
  if (event->button() == Qt::MiddleButton) {
    if (phase_ == Phase::Edit && viewZoom_ > 1.0 &&
        !textEditor_->isVisible()) {
      panning_ = true;
      panAnchor_ = event->position();
      setCursor(Qt::ClosedHandCursor);
    }
    return;
  }
  if (event->button() != Qt::LeftButton)
    return;
  cursor_ = event->position();
  endNudgeRun();
  if (const int tab = selectTabAt(cursor_); tab >= 0) {
    if (phase_ == Phase::Edit && textEditor_->isVisible())
      acceptText();
    activateSelectTab(selectTabItems().at(tab).kind);
    return;
  }
  if (phase_ == Phase::Edit && scrollPillRect().contains(cursor_)) {
    if (textEditor_->isVisible())
      acceptText();
    const QRect region = selection_.toRect();
    returnToSelect(false);
    setScrollMode(true);
    startScrollCapture(region);
    return;
  }
  if (phase_ == Phase::Select) {
    trackRecentsHover();
    if (recentsOpen_) {
      if (const int recent = recentAt(cursor_); recent >= 0)
        reopenRecent(recent);
      return; // a click in the shelf's margin is not the start of a drag
    }
    if (windowMode_) {
      chooseWindow(windowAt(cursor_));
      return;
    }
    dragStart_ = cursor_;
    selection_ = {};
    dragging_ = true;
    return;
  }
  if (customColorPickerOpen_) {
    if (customColorPanelRect().contains(cursor_)) {
      applyCustomColor(cursor_);
      return;
    }
    customColorPickerOpen_ = false;
    if (!colorPaletteRect().contains(cursor_)) {
      update();
      return;
    }
  }
  if (textSizeMenuOpen_ && !colorPaletteOpen_ && !customColorPickerOpen_ &&
      textSizePanelRect().contains(cursor_)) {
    tool_ = Tool::Text;
    const qreal localX = cursor_.x() - textSizePanelRect().left();
    textSizeIndex_ = std::clamp(static_cast<int>(localX / 34.0), 0, 2);
    setStatus(QStringLiteral("%1 · size %2 · wheel changes size · Shift+T "
                             "cycles font")
                  .arg(annotationTextFontName(textFont_))
                  .arg(QString::fromLatin1(kTextSizeNames.at(
                      static_cast<std::size_t>(textSizeIndex_)))));
    update();
    return;
  }

  // Clicking away keeps whatever was typed; Enter belongs to multiline text.
  if (textEditor_->isVisible()) {
    const bool committed = !textEditor_->toPlainText().trimmed().isEmpty();
    acceptText();
    bool overToolbar = false;
    for (const ToolbarButton &button : toolbarButtons())
      overToolbar = overToolbar || button.rect.contains(cursor_);
    // Committing is the whole of that click: the text tool stays armed, but
    // the click that put one text down must not also open the next where it
    // landed. An empty editor committed nothing, so the click simply moves it.
    if (committed && !overToolbar)
      return;
  }

  for (const ToolbarButton &button : toolbarButtons()) {
    if (button.rect.contains(cursor_)) {
      handleToolbar(button.action);
      return;
    }
  }
  if (tool_ == Tool::Select && selectedAnnotations_.isEmpty()) {
    const int cropHandle = cropHandleAt(cursor_);
    if (cropHandle >= 0) {
      originalSelection_ = selection_;
      cropDragImageRect_ = sourceFrameWidgetRect();
      interaction_ = static_cast<Interaction>(
          static_cast<int>(Interaction::CropTopLeft) + cropHandle);
      dragStartState_ = editState();
      selectedAnnotation_ = -1;
      dragStartStateValid_ = true;
      dragChanged_ = false;
      dragging_ = true;
      setStatus(QStringLiteral("Cropping screenshot · drag outer handle"));
      updatePointerCursor();
      update();
      return;
    }
  }
  // A layer that ran off the capture keeps its handles and body live outside
  // the canvas, so it can be resized or dragged back in instead of being
  // stranded. Anything else outside the canvas stays inert.
  const bool insideImage = editImageRect().contains(cursor_);
  const QPointF point = insideImage ? toAnnotationPoint(cursor_)
                                    : toUnclampedAnnotationPoint(cursor_);
  if (!insideImage &&
      (tool_ != Tool::Select || !selectedLayerAcceptsPoint(point)))
    return;
  if (tool_ == Tool::Eyedropper) {
    if (!sourceFrameWidgetRect().contains(cursor_))
      return;
    customColor_ = sampleSourceColor(capture_.source, capture_.previewSize,
                                     selection_, sourceFrameWidgetRect(),
                                     cursor_);
    usingCustomColor_ = true;
    if (customColor_.hsvHueF() >= 0)
      customHue_ = customColor_.hsvHueF();
    if (selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size() &&
        annotations_.at(selectedAnnotation_).kind !=
            Annotation::Kind::Redaction &&
        annotations_.at(selectedAnnotation_).kind !=
            Annotation::Kind::Spotlight) {
      annotations_[selectedAnnotation_].color = customColor_;
      commitPatch({selectedAnnotation_});
    }
    QString clipboardError;
    static_cast<void>(copyTextToClipboard(
        customColor_.name(QColor::HexRgb).toUpper(), clipboardError));
    // Sampling a color is something you do in order to keep working: it
    // hands back the tool that was in hand, and leaves the layer it just
    // recolored selected so another color can be tried on it. Dropping both
    // meant that taking a color cost you your place twice over.
    tool_ = toolBeforeEyedropper_;
    setStatus(QStringLiteral("Sampled %1").arg(
        customColor_.name(QColor::HexRgb).toUpper()));
    updatePointerCursor();
    update();
    return;
  }
  if (tool_ == Tool::Select) {
    const bool additive =
        heldModifiers(event->modifiers()).testFlag(Qt::ControlModifier) ||
        heldModifiers(event->modifiers()).testFlag(Qt::MetaModifier);
    // A handle of the layer already selected is a resize wherever it sits:
    // a box's corner can lie well outside the shape it belongs to.
    const Interaction handle = selectedHandleAt(point);
    const int hit =
        handle != Interaction::None ? selectedAnnotation_ : annotationAt(point);
    if (additive) {
      if (hit >= 0) {
        if (selectedAnnotations_.contains(hit)) {
          selectedAnnotations_.removeAll(hit);
          if (selectedAnnotation_ == hit)
            selectedAnnotation_ = selectedAnnotations_.isEmpty()
                                      ? -1
                                      : selectedAnnotations_.constLast();
        } else {
          selectedAnnotations_.push_back(hit);
          selectedAnnotation_ = hit;
        }
      }
      interaction_ = Interaction::None;
      dragging_ = false;
      setStatus(selectedAnnotations_.isEmpty()
                    ? QStringLiteral("No layer selected")
                    : QStringLiteral("%1 layers selected · Ctrl-click toggles")
                          .arg(selectedAnnotations_.size()));
      updatePointerCursor();
      update();
      return;
    }

    if (hit < 0) {
      marqueeSelecting_ = true;
      marqueeAdditive_ = additive;
      marqueeRect_ = QRectF(point, point);
      dragStart_ = point;
      dragging_ = true;
      interaction_ = Interaction::None;
      setStatus(QStringLiteral("Drag to select layers"));
      update();
      return;
    } else {
      if (!selectedAnnotations_.contains(hit))
        selectedAnnotations_ = {hit};
      selectedAnnotation_ = hit;
      if (selectedAnnotations_.size() > 1) {
        interaction_ = Interaction::Move;
      } else {
        // selectedAnnotation_ is the layer under the press now, so this asks
        // about that layer's own handles.
        const Interaction onHit = selectedHandleAt(point);
        interaction_ = onHit != Interaction::None ? onHit : Interaction::Move;
      }
    }
    if (selectedAnnotation_ >= 0) {
      // Snapshot before the raise, so undo puts the order back too.
      dragStartState_ = editState();
      const int wasAt = selectedAnnotation_;
      const bool raised = selectedAnnotations_.size() == 1 &&
                          raiseAnnotation(selectedAnnotation_) != wasAt;
      originalAnnotation_ = annotations_.at(selectedAnnotation_);
      originalSelectedAnnotations_.clear();
      for (const int index : selectedAnnotations_)
        originalSelectedAnnotations_.push_back(annotations_.at(index));
      dragStartStateValid_ = true;
      dragChanged_ = raised;
      dragStart_ = point;
      dragging_ = true;
      resizeConstraintActive_ = isLayerResize(interaction_) &&
                                heldModifiers(event->modifiers())
                                    .testFlag(Qt::ShiftModifier);
      setStatus(selectedAnnotations_.size() > 1
                    ? QStringLiteral("%1 layers selected · drag to move")
                          .arg(selectedAnnotations_.size())
                    : QStringLiteral(
                          "Vector layer selected · drag to move · handles "
                          "resize · double-click text"));
    } else {
      dragStartStateValid_ = false;
      dragChanged_ = false;
      dragging_ = false;
      setStatus(QStringLiteral("No layer selected"));
    }
    updatePointerCursor();
    update();
    return;
  }
  // Edges move, interiors are canvas: with any tool armed, an edge under the
  // pointer grabs that layer and the tool is left alone.
  if (tool_ != Tool::Select && !dragging_) {
    // A handle of the layer already selected wins over grabbing anything, so
    // a selected layer can still be resized without switching to Select. The
    // text tool's wrap handle is the one that would otherwise be unreachable.
    int grabbed = annotationEdgeAt(point);
    if (!toolGrabsLayer(grabbed))
      grabbed = -1;
    Interaction grabInteraction = Interaction::Move;
    // Eight handles (and a text wrap handle) of the layer already selected
    // win over grabbing anything, so a selected layer can still be resized
    // without switching to Select.
    const Interaction handle = selectedHandleAt(point);
    if (handle != Interaction::None) {
      grabbed = selectedAnnotation_;
      grabInteraction = handle;
    }
    if (grabbed >= 0) {
      const EditState before = editState();
      selectedAnnotation_ = grabbed;
      selectedAnnotations_ = {grabbed};
      const bool raised = raiseAnnotation(grabbed) != grabbed;
      grabbed = selectedAnnotation_;
      originalAnnotation_ = annotations_.at(grabbed);
      originalSelectedAnnotations_.clear();
      originalSelectedAnnotations_.push_back(annotations_.at(grabbed));
      interaction_ = grabInteraction;
      resizeConstraintActive_ =
          isLayerResize(interaction_) &&
          heldModifiers(event->modifiers()).testFlag(Qt::ShiftModifier);
      dragStartState_ = before;
      dragStartStateValid_ = true;
      dragChanged_ = raised;
      dragStart_ = point;
      dragging_ = true;
      setStatus(QStringLiteral("Moving layer · release to keep drawing"));
      updatePointerCursor();
      update();
      return;
    }
    // Nothing under the pointer: put down whatever was selected. A click that
    // goes nowhere then only deselects, because a drag shorter than the commit
    // dead-zone draws nothing; a real drag still draws, so dismissing costs no
    // extra click.
    if (selectedAnnotation_ >= 0 || !selectedAnnotations_.isEmpty()) {
      selectedAnnotation_ = -1;
      selectedAnnotations_.clear();
      setStatus(QStringLiteral("No layer selected · drag to draw"));
      // Tools that place on press must not place on the click that was only
      // meant to put the last layer down. The tool stays armed, so the next
      // click starts the next one.
      if (tool_ == Tool::Text || tool_ == Tool::Marker) {
        updatePointerCursor();
        update();
        return;
      }
    }
  }
  if (tool_ == Tool::Marker) {
    Annotation annotation;
    annotation.kind = Annotation::Kind::Marker;
    annotation.start = markerPlacementPoint(cursor_);
    annotation.number = nextMarker_;
    annotation.color = annotationColor();
    annotation.size = annotationSize_;
    selectedAnnotation_ = -1;
    setStatus(QStringLiteral("Marker %1 added · Esc for select mode")
                  .arg(annotation.number));
    commitAnnotate(std::move(annotation));
    updatePointerCursor();
  } else if (tool_ == Tool::Text) {
    // A click places a one-line label; a drag draws a box whose height says
    // how many lines Enter may fill before it commits (see mouseReleaseEvent).
    dragStart_ = point;
    dragging_ = true;
    interaction_ = Interaction::None;
  } else if (tool_ == Tool::Cut) {
    // Activation waits for a dominant drag axis (see mouseMoveEvent); a
    // plain click never crosses that threshold and mouseReleaseEvent treats
    // it as a no-op.
    cutDragStart_ = point;
    cutDragActive_ = false;
    dragging_ = true;
  } else {
    dragStart_ = point;
    dragging_ = true;
    interaction_ = Interaction::None;
    creationConstraintActive_ = supportsCreationConstraint(tool_) &&
                                heldModifiers(event->modifiers()).testFlag(Qt::ShiftModifier);
    creationCenteredActive_ = supportsCenteredCreation(tool_) &&
                              heldModifiers(event->modifiers()).testFlag(Qt::AltModifier);
    if (tool_ == Tool::Redact)
      activeRedactionSeed_ = freshRedactionSeed();
    if (tool_ == Tool::Freehand || tool_ == Tool::Highlighter) {
      freehandPoints_.clear();
      freehandPoints_.reserve(256);
      highlighterLock_.reset();
      QPointF strokeStart = point;
      if (tool_ == Tool::Highlighter &&
          highlighterMode_ == HighlighterMode::Snap)
        highlighterLock_ = highlighterLockAt(point);
      if (highlighterLock_)
        strokeStart.setY(highlighterLock_->centerY);
      freehandPoints_.push_back(strokeStart);
      if (tool_ == Tool::Highlighter)
        updatePointerCursor();
    }
  }
  update();
}

void CaptureEditor::mouseReleaseEvent(QMouseEvent *event) {
  if (clipboardTextCardEditing_) {
    event->accept();
    return;
  }
  if (event->button() == Qt::MiddleButton && panning_) {
    panning_ = false;
    updatePointerCursor();
    return;
  }
  if (capturePending_ || event->button() != Qt::LeftButton || !dragging_)
    return;
  if (phase_ == Phase::Select) {
    selection_ = normalizedSelection(dragStart_, event->position());
    dragging_ = false;
    if (selection_.width() >= 2 && selection_.height() >= 2) {
      // Remember the drawn region for this session, so R can bring it back
      // on the next capture. A convenience, so failing to write is no error.
      const QString path = storedCaptureRegionPath();
      if (!path.isEmpty()) {
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
          file.write(formatStoredRegion(capture_.monitor.name, size(),
                                        selection_.toRect())
                         .toUtf8());
      }
      commitRegion(selection_,
                   QStringLiteral("Area selected · Select moves layers · wheel "
                                  "zooms · outer handles crop"));
    }
    updatePointerCursor();
    update();
    return;
  }
  if ((interaction_ == Interaction::Move || isLayerResize(interaction_)) &&
      tool_ != Tool::Select) {
    // A grab with a drawing tool armed: keep the move, keep the tool. It is
    // an edit like any other, so it takes its own undo step. Ctrl+Z after
    // nudging a layer must put that layer back, not remove the one before it.
    dragging_ = false;
    resizeConstraintActive_ = false;
    interaction_ = Interaction::None;
    if (dragStartStateValid_ && dragChanged_)
      commitPatch(selectedAnnotations_);
    dragStartStateValid_ = false;
    dragChanged_ = false;
    setStatus(QStringLiteral("Layer moved · keep drawing, or Esc to select"));
    updatePointerCursor();
    update();
    return;
  }
  if (tool_ == Tool::Select) {
    if (marqueeSelecting_) {
      const QRectF area = marqueeRect_.normalized();
      QVector<int> matches;
      if (area.width() >= 2.0 && area.height() >= 2.0) {
        for (int index = 0; index < annotations_.size(); ++index) {
          const QRectF bounds = annotationBounds(annotations_.at(index));
          if (!bounds.isEmpty() && area.contains(bounds))
            matches.push_back(index);
        }
      }
      if (!marqueeAdditive_)
        selectedAnnotations_.clear();
      for (const int index : matches) {
        if (!selectedAnnotations_.contains(index))
          selectedAnnotations_.push_back(index);
      }
      selectedAnnotation_ = selectedAnnotations_.isEmpty()
                                ? -1
                                : selectedAnnotations_.constLast();
      marqueeSelecting_ = false;
      marqueeRect_ = {};
      dragging_ = false;
      interaction_ = Interaction::None;
      setStatus(selectedAnnotations_.isEmpty()
                    ? QStringLiteral("No layers selected")
                    : QStringLiteral("%1 layers selected · drag to move")
                          .arg(selectedAnnotations_.size()));
      updatePointerCursor();
      update();
      return;
    }
    const bool cropped = interaction_ >= Interaction::CropTopLeft;
    const bool changed = dragStartStateValid_ && dragChanged_;
    if (changed) {
      if (cropped)
        commitCrop(selection_);
      else
        commitPatch(selectedAnnotations_);
    }
    dragStartStateValid_ = false;
    dragChanged_ = false;
    dragging_ = false;
    resizeConstraintActive_ = false;
    interaction_ = Interaction::None;
    if (cropped)
      setStatus(QStringLiteral(
          "Crop updated · Select moves layers · wheel zooms selected layer"));
    updatePointerCursor();
    update();
    return;
  }

  if (tool_ == Tool::Cut) {
    dragging_ = false;
    if (!cutDragActive_) {
      // Plain click: never crossed the activation threshold, no-op.
      update();
      return;
    }
    const qreal lo = cutBandLo_;
    const qreal hi = cutBandHi_;
    const bool horizontal = liveCut_.orientation == Qt::Horizontal;
    const qreal band = hi - lo;
    const qreal extent =
        horizontal ? selection_.height() : selection_.width();
    if (band <= 0.0 || band >= extent) {
      // Full-extent (or empty) band: toAnnotationPoint clamps lo/hi to
      // [0, extent], so an edge-to-edge drag on a full selection lands
      // exactly here. removeBand() no-ops on this band, so applying it
      // would still shrink composedLogicalSize/selection_ while the actual
      // pixels don't shrink -- desyncing preview size from the source
      // aspect. Bail out instead.
      cutDragActive_ = false;
      refreshComposedCapture();
      setStatus(QStringLiteral("Cut too large — nothing left"));
      updatePointerCursor();
      update();
      return;
    }
    cutDragActive_ = false;
    commitCut(liveCut_);
    setStatus(QStringLiteral("Cut applied · Ctrl+Z to undo"));
    updatePointerCursor();
    update();
    return;
  }

  const QLineF span =
      creationSpan(toUnclampedAnnotationPoint(event->position()));
  const QPointF start = span.p1();
  QPointF end = span.p2();
  if (tool_ == Tool::Highlighter && highlighterLock_)
    end.setY(highlighterLock_->centerY);
  creationConstraintActive_ = false;
  creationCenteredActive_ = false;
  if (tool_ == Tool::Freehand || tool_ == Tool::Highlighter) {
    if (tool_ == Tool::Freehand) {
      if (freehandPoints_.isEmpty())
        freehandPoints_.push_back(end);
      else if (freehandPoints_.size() == 1 ||
               QLineF(freehandPoints_.last(), end).length() >= 0.001)
        freehandPoints_.push_back(end);
      else
        freehandPoints_.last() = end;
      // Live smoothing intentionally trails slow input. The post-stroke pass
      // starts from that cleaner trace, but the visible gesture must still
      // begin and finish exactly under the pointer.
      freehandPoints_.first() = dragStart_;
      freehandPoints_.last() = end;
    } else if (freehandPoints_.isEmpty() ||
               QLineF(freehandPoints_.last(), end).length() >= 1.0)
      freehandPoints_.push_back(end);
    qreal length = 0;
    for (int index = 1; index < freehandPoints_.size(); ++index)
      length += QLineF(freehandPoints_.at(index - 1), freehandPoints_.at(index))
                    .length();
    if (length > 4) {
      const bool highlighter = tool_ == Tool::Highlighter;
      Annotation annotation;
      annotation.kind = highlighter ? Annotation::Kind::Highlighter
                                    : Annotation::Kind::Freehand;
      annotation.start = freehandPoints_.first();
      annotation.end = freehandPoints_.last();
      annotation.color = annotationColor();
      annotation.size = highlighter && highlighterLock_
                            ? highlighterLock_->annotationSize
                            : annotationSize_;
      if (highlighter) {
        annotation.points = std::move(freehandPoints_);
      } else {
        // Preserve exact endpoints and raw intermediate samples so level zero
        // is truly unsmoothed and later level changes never compound.
        annotation.rawPoints = std::move(freehandPoints_);
        annotation.smoothingLevel = freehandSmoothingLevel_;
        annotation.points = stroke::smoothFreehand(annotation.rawPoints,
                                                    annotation.smoothingLevel);
        annotation.start = annotation.points.first();
        annotation.end = annotation.points.last();
      }
      selectedAnnotation_ = -1;
      setStatus(highlighter
                    ? highlighterStatus()
                    : QStringLiteral("Stroke added · smoothing %1/%2 · Esc "
                                     "for select mode")
                          .arg(annotation.smoothingLevel)
                          .arg(stroke::maximumSmoothingLevel));
      commitAnnotate(std::move(annotation));
    }
    freehandPoints_.clear();
    highlighterLock_.reset();
    dragging_ = false;
    updatePointerCursor();
    update();
    return;
  }
  if (tool_ == Tool::Text) {
    dragging_ = false;
    const QRectF box = QRectF(dragStart_, end).normalized();
    if (QLineF(dragStart_, end).length() <= 4) {
      beginText(dragStart_);
    } else {
      const qreal size = kTextSizes.at(static_cast<std::size_t>(textSizeIndex_));
      const qreal lineHeight =
          QFontMetricsF(annotationTextFont(size, textFont_)).lineSpacing();
      const int lines = std::max(
          1, static_cast<int>(std::floor(box.height() / lineHeight + 0.25)));
      beginText(box.topLeft(), -1, lines);
    }
    update();
    return;
  }
  if (tool_ == Tool::Ocr) {
    const QRectF ocrRect(dragStart_, end);
    dragging_ = false;
    if (ocrRect.normalized().width() > 4 && ocrRect.normalized().height() > 4) {
      runOcr(ocrRect.normalized());
      tool_ = Tool::Select;
      updatePointerCursor();
    }
    update();
    return;
  }
  const QRectF draggedRect(start, end);
  const bool validRedaction =
      tool_ != Tool::Redact ||
      (draggedRect.normalized().width() >= kMinimumRedactionExtent &&
       draggedRect.normalized().height() >= kMinimumRedactionExtent);
  if (QLineF(dragStart_, end).length() > 4 && validRedaction) {
    Annotation annotation;
    if (tool_ == Tool::Redact) {
      annotation.kind = Annotation::Kind::Redaction;
      annotation.redactionStyle = redactionStyle_;
      annotation.redactionSeed = activeRedactionSeed_;
    } else if (tool_ == Tool::Spotlight) {
      annotation.kind = Annotation::Kind::Spotlight;
      annotation.magnification = spotlightMagnification_;
      annotation.spotlightShape = spotlightShape_;
    } else {
      annotation.kind = dragShapeKind(tool_);
      if (tool_ == Tool::Arrow)
        annotation.arrowStyle = arrowStyle_;
      if (tool_ == Tool::Rectangle || tool_ == Tool::Ellipse)
        annotation.filled = fillShapes_;
      if (tool_ == Tool::Rectangle)
        annotation.cornerRadius = cornerRadius_;
    }
    annotation.start = start;
    annotation.end = end;
    annotation.color = annotationColor();
    // A spotlight's size is its ring width, which is its own setting: it can
    // be zero, and it is not the stroke size the other tools share.
    annotation.size =
        tool_ == Tool::Spotlight ? spotlightBorder_ : annotationSize_;
    selectedAnnotation_ = -1;
    const bool redacted = tool_ == Tool::Redact;
    setStatus(redacted
                  ? QStringLiteral("%1 redaction added · Esc for select mode")
                        .arg(redactionStyleName(redactionStyle_))
                  : QStringLiteral("Layer added · Esc for select mode"));
    commitAnnotate(std::move(annotation));
    updatePointerCursor();
  } else if (tool_ == Tool::Redact) {
    setStatus(QStringLiteral(
        "Redaction needs both width and height · drag a rectangle"));
  }
  dragging_ = false;
  update();
}

void CaptureEditor::wheelEvent(QWheelEvent *event) {
  if (clipboardTextCardEditing_) {
    event->accept();
    return;
  }
  if (phase_ != Phase::Edit) {
    QWidget::wheelEvent(event);
    return;
  }
  if (textEditor_->isVisible()) {
    // The draft widget is laid out for the view it opened in; a zoom or pan
    // under it would leave the text adrift mid-edit. The view holds still
    // until the text commits.
    event->accept();
    return;
  }

  // High-resolution touchpads can arrive with an empty angleDelta and only a
  // pixelDelta; treat the two interchangeably, preferring the exact pixels
  // for panning and the notch units for discrete steps.
  const QPoint angle = event->angleDelta();
  const QPoint pixel = event->pixelDelta();
  const Qt::KeyboardModifiers modifiers = event->modifiers();
  const int vertical = angle.y() != 0 ? angle.y() : pixel.y();
  const int horizontal = angle.x() != 0 ? angle.x() : pixel.x();
  if (vertical == 0 && horizontal == 0) {
    event->accept();
    return; // scroll begin/end markers carry no delta
  }
  // Discrete steps follow whichever axis carried the notch. Alt+wheel often
  // arrives as a horizontal delta, so reading only the vertical one left every
  // Alt-adjusted setting able to rise and never fall.
  const int notch = vertical != 0 ? vertical : horizontal;
  const int step = notch > 0 ? 1 : -1;
  // A selected layer owns the plain wheel whatever tool is armed. Selecting a
  // layer to adjust it is the same gesture as selecting it to move it, and the
  // tool still in hand should not change the answer, and the color keys
  // already act on the selection this way. The spotlight tool keeps its own
  // wheel,
  // which deliberately follows the spotlight under the pointer.
  const bool layerSelected = selectedAnnotation_ >= 0 &&
                             selectedAnnotation_ < annotations_.size();
  const bool overLayer = layerSelected && tool_ != Tool::Spotlight &&
                         !modifiers.testFlag(Qt::AltModifier);
  const auto showZoom = [&] {
    setStatus(QStringLiteral("Zoom %1% · wheel scrolls · Ctrl+wheel zooms · "
                             "arrows and middle-drag pan · Shift+wheel goes "
                             "sideways · Ctrl+0 fits")
                  .arg(qRound(viewZoom_ *
                              (baseImageRect().width() /
                               std::max<qreal>(canvasRect_.width(), 1)) *
                              100)));
  };
  // One notch is one step; a touchpad's finer increments are a fraction of
  // one, so a swipe glides instead of leaping a quarter at a time.
  const auto zoomByNotch = [&](const QPointF &focus) {
    const qreal steps = std::clamp(qreal(notch) / 120.0, -1.0, 1.0);
    setViewZoom(viewZoom_ * std::pow(1.25, steps), focus);
    showZoom();
  };
  if (modifiers.testFlag(Qt::ControlModifier)) {
    zoomByNotch(event->position());
    event->accept();
    return;
  }
  // Pan by the actual delta on both axes: a touchpad's sideways swipe arrives
  // on x, and its fine increments must not each jump a step. Inert at fit.
  const QPointF scrollDelta(
      pixel.x() != 0 ? qreal(pixel.x()) : angle.x() * 0.75,
      pixel.y() != 0 ? qreal(pixel.y()) : angle.y() * 0.75);
  // Shift+wheel, or Alt+wheel, goes sideways across a wide stitch: the
  // classic mapping of a vertical wheel to horizontal scrolling.
  if (modifiers.testFlag(Qt::ShiftModifier) ||
      (tool_ == Tool::Select && !layerSelected &&
       modifiers.testFlag(Qt::AltModifier))) {
    if (viewZoom_ > 1.0)
      panView(QPointF(scrollDelta.y() + scrollDelta.x(), 0));
    event->accept();
    return;
  }
  // A plain wheel scrolls a zoomed capture like a document. Where only the
  // horizontal axis overflows, the vertical wheel scrolls that one rather
  // than doing nothing.
  if (tool_ == Tool::Select && !layerSelected) {
    if (viewZoom_ > 1.0) {
      const QSizeF shown = baseImageRect().size() * viewZoom_;
      const bool verticalSlack = shown.height() > std::max(1, height() - 126);
      const bool horizontalSlack = shown.width() > std::max(1, width() - 60);
      QPointF pan = scrollDelta;
      if (!verticalSlack && horizontalSlack && pan.x() == 0.0)
        pan = QPointF(pan.y(), 0);
      panView(pan);
    }
    event->accept();
    return;
  }
  // A selected layer owns Alt+wheel too: its ring, its corners.
  if (layerSelected && modifiers.testFlag(Qt::AltModifier) &&
      adjustSelectedAnnotationRing(step)) {
    event->accept();
    update();
    return;
  }
  if (overLayer) {
    adjustSelectedAnnotation(step);
  } else if (tool_ == Tool::Text) {
    textSizeIndex_ = std::clamp(textSizeIndex_ + step, 0, 2);
    setStatus(QStringLiteral("%1 · size %2 · wheel changes size · Shift+T "
                             "cycles font")
                  .arg(annotationTextFontName(textFont_))
                  .arg(QString::fromLatin1(kTextSizeNames.at(
                      static_cast<std::size_t>(textSizeIndex_)))));
  } else if (tool_ == Tool::Freehand &&
             modifiers.testFlag(Qt::AltModifier)) {
    freehandSmoothingLevel_ =
        std::clamp(freehandSmoothingLevel_ + step,
                   stroke::minimumSmoothingLevel,
                   stroke::maximumSmoothingLevel);
    setStatus(toolStatus());
  } else if (tool_ == Tool::Spotlight &&
             event->modifiers().testFlag(Qt::AltModifier)) {
    // Alt+wheel is the spotlight's secondary control: the ring around the
    // opening, down to none for a clean spotlight. Independent of the zoom,
    // so a plain highlight can keep a ring and a loupe can go without one.
    const int hoveredRing = hoveredSpotlightAt(event->position());
    const qreal nextBorder =
        std::clamp((hoveredRing >= 0 ? annotations_.at(hoveredRing).size
                                     : spotlightBorder_) +
                       step * 2.0,
                   0.0, 12.0);
    if (hoveredRing >= 0) {
      annotations_[hoveredRing].size = nextBorder;
      commitPatch({hoveredRing});
    }
    spotlightBorder_ = nextBorder;
    setStatus(hoveredRing >= 0
                  ? spotlightStatus(annotations_.at(hoveredRing).spotlightShape,
                                    annotations_.at(hoveredRing).magnification,
                                    nextBorder)
                  : spotlightStatus(spotlightShape_, spotlightMagnification_,
                                    nextBorder));
  } else if (tool_ == Tool::Spotlight) {
    const qreal delta = step > 0 ? 0.25 : -0.25;
    const int hovered = hoveredSpotlightAt(event->position());
    if (hovered >= 0) {
      Annotation &annotation = annotations_[hovered];
      annotation.magnification =
          std::clamp(annotation.magnification + delta, 1.0, 4.0);
      spotlightMagnification_ = annotation.magnification;
      setStatus(spotlightStatus(annotation.spotlightShape,
                                annotation.magnification, annotation.size));
      commitPatch({hovered});
    } else {
      spotlightMagnification_ =
          std::clamp(spotlightMagnification_ + delta, 1.0, 4.0);
      setStatus(spotlightStatus(spotlightShape_, spotlightMagnification_,
                                spotlightBorder_));
    }
  } else if (tool_ == Tool::Rectangle &&
             event->modifiers().testFlag(Qt::AltModifier)) {
    // Alt+wheel is the rectangle's secondary control: corner rounding.
    cornerRadius_ = std::clamp(cornerRadius_ + step * kCornerRadiusStep, 0.0,
                               kMaximumCornerRadius);
    setStatus(QStringLiteral("Rectangle · %1 · Alt+wheel adjusts")
                  .arg(cornerName(cornerRadius_)));
  } else if (tool_ == Tool::Arrow || tool_ == Tool::Line ||
             tool_ == Tool::Freehand || tool_ == Tool::Highlighter ||
             tool_ == Tool::Marker || tool_ == Tool::Rectangle ||
             tool_ == Tool::Ellipse) {
    annotationSize_ = std::clamp(annotationSize_ + step, 2.0, 12.0);
    setStatus(tool_ == Tool::Highlighter
                  ? highlighterStatus()
                  : QStringLiteral("Size %1 · mouse wheel changes size")
                        .arg(qRound(annotationSize_)));
  } else {
    QWidget::wheelEvent(event);
    return;
  }
  event->accept();
  update();
}

void CaptureEditor::updatePointerCursor() {
  highlighterPreview_.reset();
  if (phase_ == Phase::Select) {
    setCursor(windowMode_ || selectTabAt(cursor_) >= 0 ||
                      (recentsOpen_ && recentAt(cursor_) >= 0)
                  ? Qt::PointingHandCursor
              : recentsOpen_ ? Qt::ArrowCursor
                             : Qt::CrossCursor);
    return;
  }
  if (selectTabAt(cursor_) >= 0 || scrollPillRect().contains(cursor_)) {
    setCursor(Qt::PointingHandCursor);
    return;
  }
  if ((colorPaletteOpen_ && colorPaletteRect().contains(cursor_)) ||
      (customColorPickerOpen_ && customColorPanelRect().contains(cursor_)) ||
      (shapeMenuOpen_ && shapeMenuRect().contains(cursor_))) {
    setCursor(Qt::PointingHandCursor);
    return;
  }
  if (textSizeMenuOpen_ && !colorPaletteOpen_ && !customColorPickerOpen_ &&
      textSizePanelRect().contains(cursor_)) {
    setCursor(Qt::PointingHandCursor);
    return;
  }
  for (const ToolbarButton &button : toolbarButtons()) {
    if (button.rect.contains(cursor_)) {
      setCursor(Qt::PointingHandCursor);
      return;
    }
  }
  const bool resizing = dragging_ && isLayerResize(interaction_);
  if (resizing || (!dragging_ && pointerHandle() != Interaction::None)) {
    setCursor(handleCursorShape(resizing ? interaction_ : pointerHandle()));
    return;
  }
  if (tool_ == Tool::Select) {
    int cropHandle =
        selectedAnnotations_.isEmpty() ? cropHandleAt(cursor_) : -1;
    if (dragging_ && interaction_ >= Interaction::CropTopLeft) {
      cropHandle = static_cast<int>(interaction_) -
                   static_cast<int>(Interaction::CropTopLeft);
    }
    if (cropHandle == 0 || cropHandle == 4)
      setCursor(Qt::SizeFDiagCursor);
    else if (cropHandle == 2 || cropHandle == 6)
      setCursor(Qt::SizeBDiagCursor);
    else if (cropHandle == 1 || cropHandle == 5)
      setCursor(Qt::SizeVerCursor);
    else if (cropHandle == 3 || cropHandle == 7)
      setCursor(Qt::SizeHorCursor);
    else
      setCursor(Qt::ArrowCursor);
  } else if (interaction_ == Interaction::Move && dragging_)
    setCursor(Qt::SizeAllCursor);
  else if (!dragging_ && pointerGrabsLayer())
    // An I-beam over a committed text reads as "click to edit" when the click
    // will move it, so what is under the pointer decides the cursor.
    setCursor(Qt::SizeAllCursor);
  else if (tool_ == Tool::Marker)
    setCursor(Qt::PointingHandCursor);
  else if (tool_ == Tool::Text)
    setCursor(Qt::IBeamCursor);
  else if (tool_ == Tool::Cut)
    setCursor(Qt::CrossCursor);
  else if (tool_ == Tool::Highlighter) {
    if (highlighterMode_ == HighlighterMode::Snap) {
      highlighterPreview_ =
          dragging_ ? highlighterLock_
                    : sourceFrameWidgetRect().contains(cursor_)
                          ? highlighterLockAt(toAnnotationPoint(cursor_))
                          : std::nullopt;
    }
    // Qt's stock I-beam has a fixed text-editor height. Paint the measured
    // one ourselves so its serifs span exactly the row the stroke will lock.
    setCursor(highlighterPreview_ ? Qt::BlankCursor : Qt::CrossCursor);
  } else
    setCursor(Qt::CrossCursor);
}

void CaptureEditor::refreshComposedCapture() {
  capture_.source = composeCuts(pristineSource_, cuts_);
  capture_.previewSize = composedLogicalSize(pristineLogicalSize_, cuts_);
  backdropKey_ = 0;           // force backdrop pixmap rebuild
  redactionBaseStale_ = true; // force redaction layer rebuild
  update();
}

void CaptureEditor::refreshBackdropCache() {
  const qreal ratio = devicePixelRatioF();
  const QSize deviceSize = (QSizeF(size()) * ratio).toSize();
  const qint64 sourceKey = capture_.source.cacheKey();
  if (deviceSize.isEmpty()) {
    backdrop_ = {};
    dimmedBackdrop_ = {};
    backdropSize_ = {};
    return;
  }
  if (!backdrop_.isNull() && backdropSize_ == deviceSize &&
      backdropKey_ == sourceKey && qFuzzyCompare(backdropRatio_, ratio))
    return;

  backdropSize_ = deviceSize;
  backdropRatio_ = ratio;
  backdropKey_ = sourceKey;
  backdrop_ = QPixmap(deviceSize);
  backdrop_.setDevicePixelRatio(ratio);
  backdrop_.fill(Qt::transparent);
  {
    QPainter cache(&backdrop_);
    cache.setRenderHint(QPainter::SmoothPixmapTransform, true);
    cache.drawImage(QRectF(QPointF(), QSizeF(deviceSize) / ratio),
                    capture_.source);
  }
  dimmedBackdrop_ = backdrop_.copy();
  {
    QPainter cache(&dimmedBackdrop_);
    cache.fillRect(QRectF(QPointF(), QSizeF(deviceSize) / ratio),
                   QColor(0, 0, 0, kBackdropDim));
  }
}

QVector<CaptureTab> CaptureEditor::selectTabItems() const {
  // In the edit phase the strip stays as the way back: a tab there returns
  // to the select phase in that mode. A file has no screen to go back to.
  if (capture_.source.isNull() || (phase_ == Phase::Edit && !hasLiveScreen()))
    return {};
  return captureTabLayout(rect());
}

int CaptureEditor::selectTabAt(const QPointF &position) const {
  return captureTabAt(selectTabItems(), position);
}

void CaptureEditor::activateSelectTab(SelectTab tab) {
  if (scrollPanel_)
    endScrollCapture();
  const bool fromEdit = phase_ == Phase::Edit;
  const QRect edited = selection_.toRect();
  if (fromEdit)
    returnToSelect(tab == SelectTab::Window);
  switch (tab) {
  case SelectTab::Region:
    setScrollMode(false);
    setWindowMode(false);
    break;
  case SelectTab::Scroll:
    setScrollMode(true);
    // From the editor the drawn region goes along, so the scroll frame
    // starts where this capture was.
    if (fromEdit)
      startScrollCapture(edited);
    break;
  case SelectTab::Window:
    setScrollMode(false);
    setWindowMode(true);
    break;
  case SelectTab::Fullscreen:
    selectFullscreen();
    break;
  }
}

CaptureKind CaptureEditor::selectKind() const {
  return windowMode_ ? CaptureKind::Window
         : scrollMode_ ? CaptureKind::Scroll
                       : CaptureKind::Region;
}

bool CaptureEditor::hasLiveScreen() const {
  return captureMode_ != CaptureMode::File && !liveMonitor_.name.isEmpty();
}

void CaptureEditor::setScrollMode(bool enabled) {
  scrollMode_ = enabled;
  if (enabled)
    windowMode_ = false;
  dragging_ = false;
  selection_ = {};
  hoveredWindow_ = -1;
  setStatus(enabled ? QStringLiteral("Drag to select a scrolling region · the "
                                     "page inside stays live")
                    : QStringLiteral(
                          "Drag to select an area · Space selects a window"));
  updatePointerCursor();
  update();
}

void CaptureEditor::commitRegion(const QRectF &region,
                                 const QString &editStatus) {
  selection_ = region;
  if (scrollMode_) {
    startScrollCapture(region.toRect());
    return;
  }
  editedKind_ = SelectTab::Region;
  enterEdit(editStatus);
}

void CaptureEditor::startScrollCapture(const QRect &region) {
  if (scrollPanel_ || liveMonitor_.name.isEmpty())
    return;
  phase_ = Phase::Select;
  scrollMode_ = true;
  windowMode_ = false;
  dragging_ = false;
  selection_ = {};
  auto *panel = new ScrollCapturePanel(liveMonitor_, layer_, this);
  scrollPanel_ = panel;
  connect(panel, &ScrollCapturePanel::stitched, this,
          [this](const QImage &image) {
            endScrollCapture();
            adoptStitched(image);
          });
  connect(panel, &ScrollCapturePanel::dismissed, this, [this] {
    endScrollCapture();
    setScrollMode(true);
  });
  connect(panel, &ScrollCapturePanel::tabRequested, this,
          [this](CaptureKind kind) { activateSelectTab(kind); });
  panel->show();
  panel->raise();
  panel->setFocus(Qt::OtherFocusReason);
  panel->begin(region);
  update();
}

void CaptureEditor::endScrollCapture() {
  if (!scrollPanel_)
    return;
  // Deleting restores the surface's mask and keyboard grab.
  delete scrollPanel_;
  scrollPanel_ = nullptr;
  setFocus(Qt::OtherFocusReason);
  updatePointerCursor();
  update();
}

void CaptureEditor::adoptStitched(const QImage &image) {
  if (image.isNull()) {
    setScrollMode(true);
    return;
  }
  const bool veryLong = image.width() > stitch::kWidelyOpenableEdge ||
                        image.height() > stitch::kWidelyOpenableEdge;
  adoptImage(image, OperationLog(), SelectTab::Scroll,
             veryLong
                 ? QStringLiteral("Very long capture (%1 × %2) · edits and "
                                  "saves here as usual, but many apps cannot "
                                  "open images this large · crop it if you "
                                  "need it elsewhere")
                       .arg(image.width())
                       .arg(image.height())
                 : QStringLiteral("Scroll capture stitched · Select moves "
                                  "layers · Ctrl+wheel zooms · outer handles "
                                  "crop"));
}

void CaptureEditor::openClipboardTextCard() {
  QString text;
  QString error;
  if (!loadClipboardText(text, error)) {
    setStatus(error);
    return;
  }
  textCardTheme_ = loadTextCardTheme();
  textCardFilename_ = defaultTextCardFilename(text);
  TextCardRender card = renderTextCardLayout(
      text, textCardTheme_, error, false, QStringLiteral("NORMAL"),
      textCardFilename_);
  if (card.image.isNull()) {
    setStatus(error.isEmpty() ? QStringLiteral("Could not render text card")
                              : error);
    return;
  }

  // This frame is a live draft, not a working document. The finalized card
  // gets the one persisted source after Ctrl+Enter.
  const bool suppressSnapshots = suppressSnapshots_;
  suppressSnapshots_ = true;
  adoptImage(std::move(card.image), OperationLog(), SelectTab::Region,
             QStringLiteral("Text card · NORMAL · hjkl move · i/a/o insert · "
                            "v selects · F filename · Ctrl+Enter renders · "
                            "q cancels"));
  suppressSnapshots_ = suppressSnapshots;

  clipboardTextCardEditing_ = true;
  textCardInsertMode_ = false;
  textCardVisualLineMode_ = false;
  textCardVisualAnchor_ = -1;
  textCardVisualPosition_ = -1;
  textCardVisualSelectionStart_ = -1;
  textCardVisualSelectionEnd_ = -1;
  textCardYank_.clear();
  textCardYankLinewise_ = false;
  textCardEditor_->setExtraSelections({});
  textCardPendingCommand_ = {};
  textCardSourceEditorRect_ = card.editorRect;
  textCardSourceTitleRect_ = card.titleRect;
  delete textCardHighlighter_;
  textCardHighlighter_ = nullptr;
  {
    const QSignalBlocker blocker(textCardEditor_);
    textCardEditor_->setPlainText(text);
  }
  reinstallClipboardTextCardHighlighter();
  textCardEditor_->setStyleSheet(
      QStringLiteral("QPlainTextEdit { color: %1; background: %2; border: "
                     "none; padding: 0; selection-color: %1; "
                     "selection-background-color: %3; }")
          .arg(textCardTheme_.foreground.name(), textCardTheme_.panel.name(),
               textCardTheme_.selection.name()));
  textCardFilenameEditor_->setStyleSheet(
      QStringLiteral("QLineEdit { color: %1; background: %2; border: none; "
                     "padding: 0; selection-color: %1; "
                     "selection-background-color: %3; }")
          .arg(textCardTheme_.foreground.name(), textCardTheme_.header.name(),
               textCardTheme_.selection.name()));
  textCardFilenameEditor_->setText(textCardFilename_);
  textCardEditor_->show();
  textCardEditor_->raise();
  textCardEditor_->setFocus(Qt::ShortcutFocusReason);
  updateClipboardTextCardEditorGeometry();
  update();
}

void CaptureEditor::updateClipboardTextCardPreview() {
  if (!clipboardTextCardEditing_)
    return;
  QString previewText = textCardEditor_->toPlainText();
  if (previewText.trimmed().isEmpty())
    previewText = QString(QChar(0x200b));
  QString error;
  TextCardRender frame = renderTextCardLayout(
      previewText, textCardTheme_, error, false, clipboardTextCardMode(),
      textCardFilename_);
  if (frame.image.isNull()) {
    setStatus(error);
    return;
  }
  capture_.source = std::move(frame.image);
  capture_.previewSize = capture_.source.size();
  pristineSource_ = capture_.source;
  pristineLogicalSize_ = capture_.previewSize;
  selection_ = QRectF(QPointF(), QSizeF(capture_.previewSize));
  canvasRect_ = selection_;
  textCardSourceEditorRect_ = frame.editorRect;
  textCardSourceTitleRect_ = frame.titleRect;
  redactionBaseStale_ = true;
  backdropKey_ = 0;
  updateClipboardTextCardEditorGeometry();
  update();
}

void CaptureEditor::updateClipboardTextCardEditorGeometry() {
  if (!clipboardTextCardEditing_ || capture_.previewSize.isEmpty())
    return;
  const QRectF source = sourceFrameWidgetRect();
  if (source.isEmpty())
    return;
  const qreal scale = source.width() / capture_.previewSize.width();
  const QRectF editor(source.left() + textCardSourceEditorRect_.left() * scale,
                      source.top() + textCardSourceEditorRect_.top() * scale,
                      textCardSourceEditorRect_.width() * scale,
                      textCardSourceEditorRect_.height() * scale);
  const QRect geometry = editor.toAlignedRect();
  if (textCardEditor_->geometry() != geometry)
    textCardEditor_->setGeometry(geometry);

  const QRectF title(source.left() + textCardSourceTitleRect_.left() * scale,
                     source.top() + textCardSourceTitleRect_.top() * scale,
                     textCardSourceTitleRect_.width() * scale,
                     textCardSourceTitleRect_.height() * scale);
  const QRect titleGeometry = title.toAlignedRect().adjusted(0, 2, 0, -2);
  if (textCardFilenameEditor_->geometry() != titleGeometry)
    textCardFilenameEditor_->setGeometry(titleGeometry);

  QFont font(annotationTextFontName(TextFont::JetBrainsMono));
  font.setPixelSize(std::max(8, qRound(kTextCardCodePixelSize * scale)));
  font.setStyleHint(QFont::Monospace);
  textCardEditor_->setFont(font);
  textCardEditor_->setCursorWidth(
      textCardInsertMode_
          ? std::max(1, qRound(2 * scale))
          : std::max(2, QFontMetrics(font).horizontalAdvance(QLatin1Char('M'))));
  QTextOption option = textCardEditor_->document()->defaultTextOption();
  option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
  option.setTabStopDistance(QFontMetricsF(font).horizontalAdvance(' ') *
                            kTextCardTabSpaces);
  textCardEditor_->document()->setDefaultTextOption(option);

  QFont filenameFont = font;
  filenameFont.setPixelSize(std::max(8, qRound(14 * scale)));
  textCardFilenameEditor_->setFont(filenameFont);
}

void CaptureEditor::beginClipboardTextCardFilenameEdit() {
  if (!clipboardTextCardEditing_ || textCardFilenameEditor_->isVisible())
    return;
  textCardFilenameBeforeEdit_ = textCardFilename_;
  textCardFilenameEditor_->setText(textCardFilename_);
  textCardFilenameEditor_->selectAll();
  textCardFilenameEditor_->show();
  textCardFilenameEditor_->raise();
  textCardFilenameEditor_->setFocus(Qt::ShortcutFocusReason);
  setStatus(QStringLiteral("Text card · FILENAME · type a path or name · "
                           "Enter accepts · Esc cancels"));
}

void CaptureEditor::endClipboardTextCardFilenameEdit(bool accept) {
  if (!textCardFilenameEditor_->isVisible())
    return;
  const QString entered = textCardFilenameEditor_->text().trimmed();
  textCardFilename_ = accept && !entered.isEmpty()
                          ? entered
                          : textCardFilenameBeforeEdit_;
  textCardFilenameEditor_->hide();
  textCardFilenameEditor_->setText(textCardFilename_);
  reinstallClipboardTextCardHighlighter();
  updateClipboardTextCardPreview();
  textCardEditor_->setFocus(Qt::ShortcutFocusReason);
  setStatus(QStringLiteral("Text card · NORMAL · %1 · %2 syntax · "
                           "F renames")
                .arg(QFileInfo(textCardFilename_).fileName(),
                     detectTextCardLanguage(textCardEditor_->toPlainText(),
                                            textCardFilename_)));
}

void CaptureEditor::reinstallClipboardTextCardHighlighter() {
  delete textCardHighlighter_;
  textCardHighlighter_ = installTextCardHighlighter(
      textCardEditor_->document(), textCardTheme_, textCardFilename_);
  textCardHighlighter_->rehighlight();
}

void CaptureEditor::finishClipboardTextCard() {
  if (!clipboardTextCardEditing_)
    return;
  QString error;
  const QImage card = renderTextCardLayout(
                          textCardEditor_->toPlainText(), textCardTheme_, error,
                          true, QStringLiteral("NORMAL"), textCardFilename_)
                          .image;
  if (card.isNull()) {
    setStatus(error);
    textCardEditor_->setFocus(Qt::ShortcutFocusReason);
    return;
  }
  resetClipboardTextCardEditor();
  capture_.source = card;
  capture_.previewSize = card.size();
  pristineSource_ = card;
  pristineLogicalSize_ = card.size();
  selection_ = QRectF(QPointF(), QSizeF(card.size()));
  canvasRect_ = selection_;
  cuts_.clear();
  ops_.clear();
  opIndex_ = 0;
  sourceWritten_ = false;
  redactionBaseStale_ = true;
  backdropKey_ = 0;
  setFocus(Qt::OtherFocusReason);
  setStatus(QStringLiteral("Text card rendered · annotate normally · Ctrl+C "
                           "copies · Enter copies + saves"));
  scheduleSnapshot();
  updatePointerCursor();
  update();
}

void CaptureEditor::cancelClipboardTextCard() {
  if (!clipboardTextCardEditing_)
    return;
  resetClipboardTextCardEditor();
  setFocus(Qt::OtherFocusReason);
  returnToSelect(false);
}

void CaptureEditor::resetClipboardTextCardEditor() {
  clipboardTextCardEditing_ = false;
  textCardInsertMode_ = false;
  textCardVisualLineMode_ = false;
  textCardVisualAnchor_ = -1;
  textCardVisualPosition_ = -1;
  textCardVisualSelectionStart_ = -1;
  textCardVisualSelectionEnd_ = -1;
  textCardYank_.clear();
  textCardYankLinewise_ = false;
  textCardEditor_->setExtraSelections({});
  textCardPendingCommand_ = {};
  textCardEditor_->hide();
  textCardFilenameEditor_->hide();
  delete textCardHighlighter_;
  textCardHighlighter_ = nullptr;
  const QSignalBlocker blocker(textCardEditor_);
  textCardEditor_->clear();
  textCardFilenameEditor_->clear();
  textCardFilename_.clear();
  textCardFilenameBeforeEdit_.clear();
  textCardSourceTitleRect_ = {};
}

void CaptureEditor::adoptImage(QImage image, OperationLog log, SelectTab kind,
                               const QString &status) {
  // The editor normally works on a region of the frozen screen. Here it is
  // handed an image instead (a stitched scroll, a shelved capture, a file)
  // and edits that: the image is the whole capture, at the scale its log was
  // written in, and the screen stays known by name so the tabs can capture
  // it again.
  if (scrollPanel_)
    endScrollCapture();
  if (clipboardTextCardEditing_)
    resetClipboardTextCardEditor();
  if (textEditor_->isVisible()) {
    textEditor_->clear();
    textEditor_->hide();
    textCaretTimer_.stop();
  }
  editingAnnotation_ = -1;
  dragging_ = false;
  interaction_ = Interaction::None;
  const MonitorInfo live = liveMonitor_;
  describeFileCapture(capture_, std::move(image), log);
  capture_.monitor.name = live.name;
  liveMonitor_ = live;
  pristineSource_ = capture_.source;
  pristineLogicalSize_ = capture_.previewSize;
  cuts_.clear();
  ops_ = std::move(log.ops);
  opIndex_ = std::clamp(log.index, 0, static_cast<int>(ops_.size()));
  nextAnnotationId_ = std::max<quint64>(log.nextId, 1);
  nextMarker_ = std::max(log.nextMarker, 1);
  redactionBaseStale_ = true;
  backdropKey_ = 0;
  scrollMode_ = false;
  windowMode_ = false;
  hoveredWindow_ = -1;
  handedImage_ = true;
  editedKind_ = kind;
  selectedAnnotation_ = -1;
  selectedAnnotations_.clear();
  if (ops_.isEmpty())
    selection_ = QRectF(QPointF(), capture_.previewSize);
  else
    replayLog();
  // A fresh working document: the old snapshot belonged to the screen.
  snapshotPath_.clear();
  sourceWritten_ = false;
  enterEdit(status);
}

void CaptureEditor::returnToSelect(bool windowMode) {
  if (textEditor_->isVisible()) {
    textEditor_->clear();
    textEditor_->hide();
    textCaretTimer_.stop();
  }
  editingAnnotation_ = -1;
  dragging_ = false;
  cutDragActive_ = false;
  marqueeSelecting_ = false;
  interaction_ = Interaction::None;
  colorPaletteOpen_ = false;
  customColorPickerOpen_ = false;
  shapeMenuOpen_ = false;
  textSizeMenuOpen_ = false;
  dismissOcrOverlay();
  // Everything edited derives from the op log; an empty log is the untouched
  // screen again.
  ops_.clear();
  opIndex_ = 0;
  replayLog();
  if (handedImage_) {
    // A stitched result is not the screen; take the monitor again so the
    // frozen backdrop behind the next selection is current.
    handedImage_ = false;
    capture_ = CaptureData();
    capture_.monitor = liveMonitor_;
    pristineSource_ = {};
    captureStarted_ = false;
    startCapture(windowMode ? CaptureMode::Window : CaptureMode::Region, true);
  }
  phase_ = Phase::Select;
  tool_ = Tool::Select;
  viewZoom_ = 1.0;
  viewOffset_ = {};
  selection_ = {};
  windowMode_ = windowMode;
  hoveredWindow_ = windowMode_ ? windowAt(cursor_) : -1;
  redactionBaseStale_ = true;
  scheduleSnapshot();
  setStatus(windowMode_
                ? QStringLiteral("Window mode · click or Super+Arrows then "
                                 "Enter · Space returns to area")
                : QStringLiteral(
                      "Drag to select an area · Space selects a window"));
  updatePointerCursor();
  update();
}

QRectF CaptureEditor::scrollPillRect() const {
  if (phase_ != Phase::Edit || !hasLiveScreen() || dragging_ ||
      capturePending_ || busy_)
    return {};
  const QRectF image = editImageRect();
  if (image.isEmpty())
    return {};
  QFont font(QStringLiteral("Noto Sans"));
  font.setPixelSize(11);
  font.setBold(true);
  const qreal width =
      QFontMetricsF(font).horizontalAdvance(QStringLiteral("SCROLL CAPTURE")) +
      28;
  constexpr qreal height = 22.0;
  // Just under the image, clear of the crop handles; inside the viewport if
  // the image reaches the bottom band.
  qreal y = image.bottom() + 14;
  if (y + height > this->height() - 60)
    y = image.bottom() - height - 10;
  return QRectF(image.center().x() - width / 2.0, y, width, height);
}

void CaptureEditor::setWindowMode(bool enabled) {
  windowMode_ = enabled;
  if (enabled)
    scrollMode_ = false;
  dragging_ = false;
  selection_ = {};
  hoveredWindow_ = windowMode_ ? windowAt(cursor_) : -1;
  setStatus(windowMode_
                ? QStringLiteral("Window mode · click or Super+Arrows then "
                                 "Enter · Space returns to area")
                : QStringLiteral(
                      "Drag to select an area · Space selects a window"));
  updatePointerCursor();
  update();
}

void CaptureEditor::loadRecents() {
  recentsLoading_ = true;
  recentsWatcher_.setFuture(
      QtConcurrent::run([] { return listRecentSnaps(true); }));
}

bool CaptureEditor::waitForRecents() {
  while (recentsLoading_) {
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    QThread::yieldCurrentThread();
  }
  return !recents_.isEmpty();
}

QVector<CaptureEditor::RecentCard>
CaptureEditor::recentCards(qreal fan) const {
  if (phase_ != Phase::Select || recents_.isEmpty())
    return {};
  // Card 0 is the newest and sits on top of the stack; fanned, it is the
  // topmost of the column. Each keeps its own aspect inside the card box.
  QVector<QSizeF> sizes;
  qreal column = 0.0;
  for (const RecentSnap &snap : recents_) {
    QSizeF size(kRecentCardWidth, kRecentCardHeight);
    if (!snap.thumbnail.isNull()) {
      size = QSizeF(snap.thumbnail.size())
                 .scaled(kRecentCardWidth, kRecentCardHeight,
                         Qt::KeepAspectRatio);
      size.setWidth(std::max(size.width(), 36.0));
      size.setHeight(std::max(size.height(), 28.0));
    }
    sizes.push_back(size);
    column += size.height() + kRecentCardGap;
  }
  column -= kRecentCardGap;

  const qreal stackX = width() - kRecentEdgeMargin - kRecentCardWidth / 2.0;
  const qreal fanX = stackX - 8.0 * fan;
  const qreal stackY = height() / 2.0;
  // Column centred on the stack, kept clear of the legend at the top and the
  // status pill at the bottom.
  qreal top = stackY - column / 2.0;
  top = std::max(top, 170.0);
  top = std::min(top, std::max(170.0, height() - 70.0 - column));

  QVector<RecentCard> cards;
  qreal y = top;
  for (int index = 0; index < sizes.size(); ++index) {
    const QSizeF size = sizes.at(index);
    const QPointF stacked(stackX + index * 1.5, stackY + index * 3.0);
    const QPointF fanned(fanX, y + size.height() / 2.0);
    const QPointF centre = stacked + (fanned - stacked) * fan;
    // Alternate the lean of the cards beneath so the stack reads as a deck
    // rather than a slide; the top card lies straight.
    const qreal lean = (index % 2 == 0 ? 1.0 : -1.0) * index * 3.0;
    RecentCard card;
    card.rect = QRectF(centre - QPointF(size.width() / 2.0, size.height() / 2.0),
                       size);
    card.rotation = lean * (1.0 - fan);
    cards.push_back(card);
    y += size.height() + kRecentCardGap;
  }
  return cards;
}

QRectF CaptureEditor::recentsHotZone() const {
  const QVector<RecentCard> cards = recentCards(recentsOpen_ ? 1.0 : 0.0);
  if (cards.isEmpty())
    return {};
  QRectF zone = cards.constFirst().rect;
  for (const RecentCard &card : cards)
    zone = zone.united(card.rect);
  // Open, the zone reaches the screen edge and a little past the cards so
  // moving between them never folds the shelf; closed, it is tighter so the
  // crosshair can pass nearby without waking it.
  return recentsOpen_ ? QRectF(zone.left() - 28.0, zone.top() - 20.0,
                               width() - zone.left() + 28.0,
                               zone.height() + 40.0)
                      : zone.adjusted(-12.0, -22.0, 12.0, 26.0);
}

int CaptureEditor::recentAt(const QPointF &position) const {
  const QVector<RecentCard> cards = recentCards(1.0);
  for (int index = 0; index < cards.size(); ++index) {
    if (cards.at(index).rect.adjusted(-4.0, -kRecentCardGap / 2.0, 4.0,
                                      kRecentCardGap / 2.0)
            .contains(position))
      return index;
  }
  return -1;
}

QRectF CaptureEditor::recentCardRectForTest(int index) const {
  const QVector<RecentCard> cards = recentCards(recentsOpen_ ? 1.0 : 0.0);
  return index >= 0 && index < cards.size() ? cards.at(index).rect : QRectF();
}

void CaptureEditor::setRecentsOpen(bool open) {
  if (recentsOpen_ == open)
    return;
  recentsOpen_ = open;
  recentsFanFrom_ = recentsFan_;
  recentsAnimClock_.start();
  recentsAnimTimer_.start();
  if (!open)
    hoveredRecent_ = -1;
  update();
}

void CaptureEditor::trackRecentsHover() {
  if (recents_.isEmpty())
    return;
  setRecentsOpen(recentsHotZone().contains(cursor_));
  hoveredRecent_ = recentsOpen_ ? recentAt(cursor_) : -1;
}

namespace {
QString relativeAge(qint64 stampMs) {
  if (stampMs <= 0)
    return {};
  const qint64 seconds =
      std::max<qint64>(0, (QDateTime::currentMSecsSinceEpoch() - stampMs) /
                              1000);
  if (seconds < 60)
    return QStringLiteral("just now");
  if (seconds < 3600)
    return QStringLiteral("%1 min ago").arg(seconds / 60);
  if (seconds < 86400)
    return QStringLiteral("%1 h ago").arg(seconds / 3600);
  return QStringLiteral("%1 d ago").arg(seconds / 86400);
}
} // namespace

void CaptureEditor::paintRecents(QPainter &painter) {
  const QVector<RecentCard> cards = recentCards(recentsFan_);
  if (cards.isEmpty())
    return;
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setRenderHint(QPainter::SmoothPixmapTransform);
  // Bottom of the deck first so the newest lands on top.
  for (int index = cards.size() - 1; index >= 0; --index) {
    const RecentCard &card = cards.at(index);
    const bool hovered = recentsOpen_ && index == hoveredRecent_;
    const QImage &thumb = recents_.at(index).thumbnail;
    painter.save();
    painter.translate(card.rect.center());
    painter.rotate(card.rotation);
    if (hovered) {
      painter.scale(1.08, 1.08);
      painter.translate(-4.0, 0.0);
    }
    const QRectF local(-card.rect.width() / 2.0, -card.rect.height() / 2.0,
                       card.rect.width(), card.rect.height());
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, hovered ? 140 : 100));
    painter.drawRoundedRect(local.translated(0, 3).adjusted(-1, -1, 1, 1), 6,
                            6);
    QPainterPath clip;
    clip.addRoundedRect(local, 4, 4);
    painter.save();
    painter.setClipPath(clip);
    if (thumb.isNull())
      painter.fillRect(local, QColor(40, 40, 46));
    else
      painter.drawImage(local, thumb);
    // Cards beneath the top one are dimmed while stacked so the deck reads
    // as depth; fanned, every card shows at full strength.
    if (index > 0 && recentsFan_ < 1.0)
      painter.fillRect(local, QColor(0, 0, 0, static_cast<int>(
                                                  90 * (1.0 - recentsFan_))));
    painter.restore();
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(hovered ? QColor(QStringLiteral("#30d158"))
                                : QColor(255, 255, 255, 120),
                        hovered ? 2.0 : 1.0));
    painter.drawRoundedRect(local, 4, 4);
    painter.restore();

    if (hovered) {
      const QString age = relativeAge(recents_.at(index).stampMs);
      if (!age.isEmpty()) {
        QFont ageFont(QStringLiteral("Noto Sans"));
        ageFont.setPixelSize(10);
        ageFont.setBold(true);
        painter.setFont(ageFont);
        const QFontMetricsF metrics(ageFont);
        const qreal w = metrics.horizontalAdvance(age) + 12.0;
        const QRectF pill(card.rect.left() - 12.0 - w,
                          card.rect.center().y() - 9.0, w, 18.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(18, 18, 22, 235));
        painter.drawRoundedRect(pill, 9, 9);
        painter.setPen(QColor(255, 255, 255, 220));
        painter.drawText(pill, Qt::AlignCenter, age);
      }
    }
  }

  // Caption under the stack names it; under the fan it says what a click does.
  QFont captionFont(QStringLiteral("Noto Sans"));
  captionFont.setPixelSize(10);
  captionFont.setBold(true);
  captionFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);
  painter.setFont(captionFont);
  const QString caption = recentsFan_ < 0.5 ? QStringLiteral("RECENT")
                                            : QStringLiteral("CLICK TO REOPEN");
  const qreal fade = std::abs(recentsFan_ - 0.5) * 2.0;
  painter.setPen(QColor(255, 255, 255, static_cast<int>(150 * fade)));
  qreal bottom = cards.constFirst().rect.bottom();
  for (const RecentCard &card : cards)
    bottom = std::max(bottom, card.rect.bottom());
  const qreal captionCentreX = cards.constFirst().rect.center().x();
  painter.drawText(QRectF(captionCentreX - 80.0, bottom + 10.0, 160.0, 16.0),
                   Qt::AlignCenter, caption);
  painter.restore();
}

void CaptureEditor::reopenRecent(int index) {
  if (index < 0 || index >= recents_.size())
    return;
  // The earlier working document opens here, in place of a new capture:
  // same surface, layers still editable. Nothing was captured yet, so there
  // is no snapshot to clean up.
  const RecentSnap recent = recents_.at(index);
  QImage image;
  if (!image.load(recent.sourcePath)) {
    setStatus(QStringLiteral("Could not load that capture"));
    return;
  }
  OperationLog log;
  QString error;
  if (!recent.logPath.isEmpty() &&
      !loadOperationLog(recent.logPath, log, error)) {
    setStatus(QStringLiteral("Could not restore its layers: %1").arg(error));
    return;
  }
  setRecentsOpen(false);
  editingRecent_ = recent;
  adoptImage(std::move(image), std::move(log), SelectTab::Region,
             QStringLiteral("Reopened recent capture · Copy/Save to output"));
}

void CaptureEditor::selectFullscreen() {
  windowMode_ = false;
  dragging_ = false;
  hoveredWindow_ = -1;
  selection_ = QRectF(QPointF(), capture_.previewSize);
  editedKind_ = SelectTab::Fullscreen;
  enterEdit(QStringLiteral(
      "Full screen selected · native resolution · outer handles crop"));
  update();
}

void CaptureEditor::paintSelectTabs(QPainter &painter) {
  // In the select phase the lit one is the mode the pointer is in; in the
  // edit phase it is how this capture was taken.
  const CaptureKind active =
      phase_ == Phase::Edit ? editedKind_ : selectKind();
  drawCaptureTabs(painter, selectTabItems(), active, cursor_);
}

void CaptureEditor::paintSelect(QPainter &painter) {
  if (capture_.source.isNull()) {
    painter.fillRect(rect(), QColor(0, 0, 0, kBackdropDim));
    drawStatusPill(painter, rect(), status_);
    return;
  }
  refreshBackdropCache();
  painter.drawPixmap(rect(), dimmedBackdrop_);

  const bool haveHole =
      windowMode_ ? hoveredWindow_ >= 0 && hoveredWindow_ < capture_.windows.size()
                  : !selection_.isEmpty();
  if (haveHole) {
    const QRectF previewHole =
        windowMode_ ? QRectF(capture_.windows.at(hoveredWindow_).rect)
                    : mapWidgetToPreview(selection_);
    const QRectF destHole =
        windowMode_ ? mapPreviewToWidget(previewHole) : selection_;
    painter.save();
    painter.setClipRect(destHole, Qt::IntersectClip);
    painter.drawImage(destHole, capture_.source, sourceRect(previewHole));
    painter.restore();
  }

  if (windowMode_) {
    for (int index = 0; index < capture_.windows.size(); ++index) {
      const WindowTarget &window = capture_.windows.at(index);
      painter.setPen(QPen(
          index == hoveredWindow_ ? Qt::white : QColor(255, 255, 255, 72), 2));
      painter.setBrush(Qt::NoBrush);
      painter.drawRect(mapPreviewToWidget(QRectF(window.rect)));
    }
  } else if (!selection_.isEmpty()) {
    painter.setPen(QPen(Qt::white, 2));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(selection_);
  }

  if (!windowMode_ && !dragging_ && !recentsOpen_) {
    painter.setPen(QPen(QColor(255, 255, 255, 56), 1));
    painter.drawLine(QPointF(cursor_.x(), 0), QPointF(cursor_.x(), height()));
    painter.drawLine(QPointF(0, cursor_.y()), QPointF(width(), cursor_.y()));
  }
  paintRecents(painter);

  paintSelectTabs(painter);
  drawHotkeyLegend(painter, rect(), cursor_,
                   {{QStringLiteral("Drag"), QStringLiteral("Area")},
                    {QStringLiteral("Space"), QStringLiteral("Window")},
                    {QStringLiteral("Ctrl+A"), QStringLiteral("Fullscreen")},
                    {QStringLiteral("Ctrl+Shift+V"),
                     QStringLiteral("Clipboard text card")},
                    {QStringLiteral("R"), QStringLiteral("Last region")},
                    {QStringLiteral("S"), QStringLiteral("Scrolling region")},
                    {QStringLiteral("Esc"), QStringLiteral("Close")}});
  drawStatusPill(painter, rect(), status_);
  drawMeasureBadge(painter, rect(), cursor_, measurementText());
}

qreal selectionBoundsRadius(const Annotation &annotation, qreal inset) {
  if (annotation.kind == Annotation::Kind::Rectangle &&
      annotation.cornerRadius > 0.0)
    return annotation.cornerRadius + inset;
  if (annotation.kind == Annotation::Kind::Text &&
      annotation.textBackground == TextBackground::Pill) {
    const QRectF pill = annotationTextBounds(annotation);
    return std::min(pill.height() / 4.0, 6.0) + inset;
  }
  return 0.0;
}

void CaptureEditor::paintClipboardTextCardEditing(QPainter &painter) {
  painter.setCompositionMode(QPainter::CompositionMode_Source);
  const bool opaqueBackdrop = windowedPresentation_ && windowedBackdropOpaque_;
  painter.fillRect(rect(), opaqueBackdrop ? textCardTheme_.background
                                          : QColor(0, 0, 0, 176));
  painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
  const QRectF image = sourceFrameWidgetRect();
  if (!image.isEmpty())
    painter.drawImage(image, capture_.source);
  drawStatusPill(painter, rect(), status_);
}

void CaptureEditor::paintEdit(QPainter &painter) {
  painter.setCompositionMode(QPainter::CompositionMode_Source);
  // The overlay dims the screen it covers; a windowed editor has its own
  // backdrop, a solid gray mat by default so the desktop does not bleed
  // through and the capture reads as a picture on a table.
  const bool opaqueBackdrop = windowedPresentation_ && windowedBackdropOpaque_;
  painter.fillRect(rect(), opaqueBackdrop ? QColor(36, 36, 36)
                                          : QColor(0, 0, 0, 160));
  painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
  const QRectF image = editImageRect();
  const QRectF visibleImage = visibleEditImageRect();
  const QRectF sourceImage = sourceFrameWidgetRect();
  const bool grown = canvasGrown();
  const BackgroundStyle background = effectiveBackgroundStyle();
  const bool hasBackground = background != BackgroundStyle::None &&
                             background != BackgroundStyle::Off;
  const bool framedBackground =
      hasBackground && canvasBoundaryMode_ == CanvasBoundaryMode::Framed;
  const bool shownBackground = grown ? hasBackground : framedBackground;
  if (imageShadow_ && opaqueBackdrop && !grown && !shownBackground) {
    // Drawn around the visible rect and before the viewport clip: zoomed past
    // fit, the shared soft shadow frames the band the image shows through.
    paintCaptureImageShadow(painter, visibleImage);
  }
  // When zoomed past fit the image is larger than the viewport; clip content
  // to the band between the toolbar and the status so it cannot overdraw them.
  const bool clipViewport = viewZoom_ > 1.0;
  if (clipViewport) {
    painter.save();
    // The band between the pinned chrome above and the status below; zoomed
    // content must not overdraw either.
    const qreal bandTop = contentBandTop();
    const qreal bandBottom = windowedPresentation_ ? 64 : 56;
    painter.setClipRect(QRectF(
        0, bandTop, width(), std::max<qreal>(1, height() - bandTop - bandBottom)));
  }
  if (grown) {
    // Extension is the canvas itself, while the source remains the image card
    // floating above it. Never shadow the expanded canvas edge.
    paintCaptureBackground(painter, image, background);
    if (imageShadow_ && hasBackground)
      paintCaptureImageShadow(painter, sourceImage);
  } else if (framedBackground) {
    const QRectF backing = image.adjusted(-28, -28, 28, 28);
    paintCaptureBackground(painter, backing, background);
    if (imageShadow_)
      paintCaptureImageShadow(painter, image);
  }

  QPainterPath clip;
  const qreal sourceRadius = !grown && framedBackground ? 10.0 : 0.0;
  clip.addRoundedRect(sourceImage, sourceRadius, sourceRadius);
  const QSize targetSize(qRound(sourceImage.width() * devicePixelRatioF()),
                         qRound(sourceImage.height() * devicePixelRatioF()));
  // Cache the un-annotated selection at display resolution. Rebuilding it
  // from the native source every pointer move would stall large captures.
  if (redactionBaseStale_ || redactionBaseSize_ != targetSize ||
      cachedRedactionSelection_ != selection_) {
    redactionBase_ = renderSelectionBase(capture_, selection_, targetSize);
    redactionBaseSize_ = targetSize;
    cachedRedactionSelection_ = selection_;
    redactionLayerCache_ = {};
    cachedCommittedRedactions_.clear();
    redactionBaseStale_ = false;
  }

  QVector<Annotation> committedRedactions;
  for (const Annotation &annotation : annotations_) {
    if (annotationLayer(annotation.kind) == AnnotationLayer::Redaction)
      committedRedactions.push_back(annotation);
  }
  if (redactionLayerCache_.isNull() ||
      cachedCommittedRedactions_ != committedRedactions) {
    cachedCommittedRedactions_ = committedRedactions;
    redactionLayerCache_ =
        committedRedactions.isEmpty()
            ? redactionBase_
            : applyRedactionsScaled(redactionBase_, committedRedactions,
                                    selection_, QSizeF(targetSize));
  }

  QImage redactionLayer = redactionLayerCache_;
  if (dragging_ && interaction_ == Interaction::None &&
      tool_ == Tool::Redact) {
    Annotation preview;
    preview.kind = Annotation::Kind::Redaction;
    preview.start = dragStart_;
    preview.end = toUnclampedAnnotationPoint(cursor_);
    preview.redactionStyle = redactionStyle_;
    preview.redactionSeed = activeRedactionSeed_;
    redactionLayer = applyRedactionsScaled(redactionLayerCache_, {preview},
                                           selection_, QSizeF(targetSize));
  }

  painter.save();
  painter.setClipPath(clip, Qt::IntersectClip);
  if (!redactionLayer.isNull())
    painter.drawImage(sourceImage, redactionLayer);
  else
    painter.drawImage(sourceImage, capture_.source, sourceRect(selection_));

  painter.restore();

  QImage defaultLayerSource = redactionLayer;
  QRectF defaultLayerBounds(QPointF(), selection_.size());

  painter.save();
  painter.translate(sourceImage.topLeft());
  painter.scale(editScale(), editScale());
  painter.save();
  QVector<Annotation> defaultAnnotations;
  defaultAnnotations.reserve(annotations_.size() + 1);
  for (int index = 0; index < annotations_.size(); ++index) {
    if (index == editingAnnotation_)
      continue;
    if (annotationLayer(annotations_.at(index).kind) ==
        AnnotationLayer::Default)
      defaultAnnotations.push_back(annotations_.at(index));
  }
  if (dragging_ && interaction_ == Interaction::None &&
      tool_ != Tool::Select && tool_ != Tool::Redact &&
      tool_ != Tool::Cut) {
    Annotation preview;
    if (tool_ == Tool::Freehand || tool_ == Tool::Highlighter) {
      preview.kind = tool_ == Tool::Highlighter ? Annotation::Kind::Highlighter
                                                : Annotation::Kind::Freehand;
      preview.points = freehandPoints_;
    } else {
      if (tool_ == Tool::Spotlight) {
        preview.kind = Annotation::Kind::Spotlight;
        preview.magnification = spotlightMagnification_;
        preview.spotlightShape = spotlightShape_;
      } else if (tool_ == Tool::Text) {
        // The box being dragged: its height is how many lines it will hold.
        preview.kind = Annotation::Kind::Rectangle;
        preview.cornerRadius = 2.0;
      } else {
        preview.kind = dragShapeKind(tool_);
        if (tool_ == Tool::Arrow)
          preview.arrowStyle = arrowStyle_;
        if (tool_ == Tool::Rectangle || tool_ == Tool::Ellipse)
          preview.filled = fillShapes_;
        if (tool_ == Tool::Rectangle)
          preview.cornerRadius = cornerRadius_;
      }
      const QLineF span =
          creationSpan(toUnclampedAnnotationPoint(cursor_));
      preview.start = span.p1();
      preview.end = span.p2();
    }
    preview.color = tool_ == Tool::Ocr ? QColor(Qt::white) : annotationColor();
    if (tool_ == Tool::Text)
      preview.color.setAlpha(150);
    preview.size = tool_ == Tool::Ocr         ? 2.0
                   : tool_ == Tool::Text      ? 1.0
                   : tool_ == Tool::Spotlight ? spotlightBorder_
                   : tool_ == Tool::Highlighter && highlighterLock_
                       ? highlighterLock_->annotationSize
                       : annotationSize_;
    defaultAnnotations.push_back(std::move(preview));
  } else if (tool_ == Tool::Marker && image.contains(cursor_) && !dragging_ &&
             !pointerGrabsLayer()) {
    // The ghost counter shows where the next one would land, so it belongs
    // only where the press would actually place one: over another counter the
    // press moves that counter instead.
    Annotation preview;
    preview.kind = Annotation::Kind::Marker;
    preview.start = markerPlacementPoint(cursor_);
    preview.number = nextMarker_;
    preview.color = annotationColor();
    preview.color.setAlpha(185);
    preview.size = annotationSize_;
    defaultAnnotations.push_back(std::move(preview));
  }
  QRectF previewClip = canvasRect_;
  if (dragging_ && canvasBoundaryMode_ != CanvasBoundaryMode::Framed) {
    previewClip = captureCanvasRect(selection_.size(), defaultAnnotations,
                                    canvasBoundaryMode_);
  }
  if (!dragging_ && grown && editScale() < 1.0) {
    // Standard and Pointy keep their tail legible while zoomed out. That
    // screen-only widening must not grow the exported canvas, but clipping it
    // to the natural bounds would cut the settled preview at a canvas edge.
    for (const Annotation &annotation : defaultAnnotations) {
      if (annotation.kind == Annotation::Kind::Arrow &&
          (annotation.arrowStyle == ArrowStyle::Standard ||
           annotation.arrowStyle == ArrowStyle::Pointy)) {
        previewClip = previewClip.united(
            arrowVisualBounds(annotation, editScale()).adjusted(-1, -1, 1, 1));
      }
    }
  }
  if (previewClip != canvasRect_) {
    QPainterPath overscan;
    overscan.addRect(previewClip);
    QPainterPath settledCanvas;
    settledCanvas.addRect(canvasRect_);
    overscan = overscan.subtracted(settledCanvas);
    const bool previewGrown =
        previewClip != QRectF(QPointF(), selection_.size());
    const bool automaticPreviewBackground =
        canvasBoundaryMode_ == CanvasBoundaryMode::Framed && previewGrown &&
        backgroundStyle_ == BackgroundStyle::None;
    const BackgroundStyle previewBackground =
        automaticPreviewBackground ? BackgroundStyle::Slate : background;
    painter.save();
    painter.setClipPath(overscan, Qt::IntersectClip);
    paintCaptureBackground(painter, previewClip, previewBackground);
    painter.restore();
  }
  // Framed mode shows the complete layer while it is being carried and
  // settles the background on release. Overflow and Image preview their final
  // canvas bounds live.
  if (!dragging_ || canvasBoundaryMode_ != CanvasBoundaryMode::Framed)
    painter.setClipRect(previewClip, Qt::IntersectClip);
  const bool hasSpotlight = std::any_of(
      defaultAnnotations.cbegin(), defaultAnnotations.cend(),
      [](const Annotation &item) {
        return item.kind == Annotation::Kind::Spotlight;
      });
  if (hasSpotlight && !redactionLayer.isNull()) {
    // Spotlights sample the complete composed canvas. Build that source only
    // when one is present; a dragged preview may temporarily make it larger
    // than the settled canvas, so its opening stays live beyond the old edge.
    const QRectF spotlightCanvas =
        captureCanvasRect(selection_.size(), defaultAnnotations,
                          canvasBoundaryMode_);
    const bool spotlightGrown = spotlightCanvas !=
                                QRectF(QPointF(), selection_.size());
    if (grown || spotlightGrown) {
      const qreal pixelScale = editScale() * devicePixelRatioF();
      const QSize canvasPixels(
          std::max(1, qCeil(spotlightCanvas.width() * pixelScale)),
          std::max(1, qCeil(spotlightCanvas.height() * pixelScale)));
      defaultLayerSource =
          QImage(canvasPixels, QImage::Format_ARGB32_Premultiplied);
      defaultLayerSource.fill(Qt::transparent);
      QPainter basePainter(&defaultLayerSource);
      basePainter.setRenderHints(QPainter::SmoothPixmapTransform);
      const bool automaticSpotlightBackground =
          canvasBoundaryMode_ == CanvasBoundaryMode::Framed &&
          spotlightGrown && backgroundStyle_ == BackgroundStyle::None;
      const BackgroundStyle spotlightBackground =
          automaticSpotlightBackground ? BackgroundStyle::Slate : background;
      paintCaptureBackground(basePainter, defaultLayerSource.rect(),
                             spotlightBackground);
      const QRectF sourcePixels(-spotlightCanvas.left() * pixelScale,
                                -spotlightCanvas.top() * pixelScale,
                                selection_.width() * pixelScale,
                                selection_.height() * pixelScale);
      const bool spotlightHasBackground =
          spotlightBackground != BackgroundStyle::None &&
          spotlightBackground != BackgroundStyle::Off;
      if (imageShadow_ && spotlightHasBackground)
        paintCaptureImageShadow(basePainter, sourcePixels, pixelScale,
                                pixelScale);
      basePainter.drawImage(sourcePixels, redactionLayer);
      basePainter.end();
      defaultLayerBounds = spotlightCanvas;
    }
  }
  // Default-layer tools (including spotlight) sample the redacted composed
  // canvas, never the raw capture. Once grown this includes the background,
  // so a spotlight carried past the old frame has valid pixels to sample.
  if (!defaultLayerSource.isNull()) {
    paintDefaultLayer(painter, defaultLayerSource, defaultLayerBounds,
                      defaultAnnotations, selection_.width(), editScale());
  } else {
    for (const Annotation &annotation : defaultAnnotations)
      paintAnnotation(painter, annotation, selection_.width(), editScale());
  }
  painter.restore();
  if (highlighterPreview_) {
    const qreal height =
        highlighterPreviewHeight(highlighterPreview_->annotationSize);
    const QPointF pointer = toAnnotationPoint(cursor_);
    paintHighlighterIBeam(
        painter, QPointF(pointer.x(), dragging_ && highlighterLock_
                                          ? highlighterPreview_->centerY
                                          : pointer.y()),
        height, editScale());
  }
  if (tool_ == Tool::Select && marqueeSelecting_ &&
      !marqueeRect_.isEmpty()) {
    const qreal scale = std::max<qreal>(editScale(), 0.01);
    painter.setPen(
        QPen(QColor(QStringLiteral("#0a84ff")), 2.0 / scale));
    painter.setBrush(QColor(10, 132, 255, 38));
    painter.drawRect(marqueeRect_.normalized());
  }
  if (cutDragActive_) {
    const qreal scale = std::max<qreal>(editScale(), 0.01);
    const QRectF band = liveCut_.orientation == Qt::Horizontal
                            ? QRectF(0, cutBandLo_, selection_.width(),
                                     cutBandHi_ - cutBandLo_)
                            : QRectF(cutBandLo_, 0, cutBandHi_ - cutBandLo_,
                                     selection_.height());
    painter.save();
    painter.setClipRect(band);
    painter.fillRect(band, QColor(104, 110, 120, 175));

    const qreal spacing = 14.0 / scale;
    painter.setPen(QPen(QColor(255, 255, 255, 72), 1.0 / scale));
    for (qreal x = band.left() - band.height(); x < band.right(); x += spacing)
      painter.drawLine(QPointF(x, band.bottom()),
                       QPointF(x + band.height(), band.top()));

    const QPointF center = band.center();
    const qreal crossRadius =
        std::min<qreal>(10.0 / scale,
                        std::min(band.width(), band.height()) * 0.3);
    if (crossRadius >= 3.0 / scale) {
      painter.setPen(QPen(QColor(255, 255, 255, 230), 2.0 / scale,
                          Qt::SolidLine, Qt::RoundCap));
      painter.drawLine(center + QPointF(-crossRadius, -crossRadius),
                       center + QPointF(crossRadius, crossRadius));
      painter.drawLine(center + QPointF(-crossRadius, crossRadius),
                       center + QPointF(crossRadius, -crossRadius));
    }
    painter.restore();

    painter.setPen(QPen(QColor(255, 255, 255, 190), 1.0 / scale,
                        Qt::DashLine));
    if (liveCut_.orientation == Qt::Horizontal) {
      painter.drawLine(band.topLeft(), band.topRight());
      painter.drawLine(band.bottomLeft(), band.bottomRight());
    } else {
      painter.drawLine(band.topLeft(), band.bottomLeft());
      painter.drawLine(band.topRight(), band.bottomRight());
    }
  }

  if (selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size() &&
      selectedAnnotation_ != editingAnnotation_) {
    const qreal scale = std::max<qreal>(editScale(), 0.01);
    const bool multiple = selectedAnnotations_.size() > 1;
    // Faint while a wheel adjustment is in flight: the handles sit exactly
    // where the change shows, so at full strength they hide it.
    const qreal chromeOpacity = adjustingSelection_ ? 0.25 : 1.0;
    painter.setOpacity(chromeOpacity);
    if (!multiple &&
        showsSelectionBounds(annotations_.at(selectedAnnotation_).kind)) {
      const Annotation &selected = annotations_.at(selectedAnnotation_);
      const QRectF bounds =
          annotationBounds(selected).adjusted(-4, -4, 4, 4);
      painter.setPen(
          QPen(QColor(255, 255, 255, 220), 1.0 / scale, Qt::DashLine));
      painter.setBrush(Qt::NoBrush);
      const qreal boxRadius = selectionBoundsRadius(selected, 4.0);
      if (boxRadius > 0.0)
        painter.drawRoundedRect(bounds, boxRadius, boxRadius);
      else
        painter.drawRect(bounds);
    }
    if (multiple) {
      painter.setPen(
          QPen(QColor(255, 255, 255, 220), 1.0 / scale, Qt::DashLine));
      painter.setBrush(Qt::NoBrush);
      // A multi-selection has no synthetic outer object: that reads as if the
      // whole grown canvas were selected. Outline each actual layer instead,
      // consistently for Ctrl-click, marquee, and Ctrl+A groups.
      for (const int index : selectedAnnotations_) {
        if (index < 0 || index >= annotations_.size() ||
            index == editingAnnotation_)
          continue;
        QRectF member = annotationBounds(annotations_.at(index));
        if (member.isNull())
          continue;
        member = member.normalized();
        // Flat arrows and lines have no extent across; the inset gives every
        // member a visible outline whatever its shape.
        painter.drawRect(member.adjusted(-3, -3, 3, 3));
      }
    }
    if (!multiple) {
      const Annotation &selected = annotations_.at(selectedAnnotation_);
      const qreal radius = 5.0 / scale;
      painter.setPen(QPen(Qt::white, 1.0 / scale));
      painter.setBrush(QColor(QStringLiteral("#0a84ff")));
      for (const auto &[position, handle] : annotationHandles(selected))
        painter.drawEllipse(position, radius, radius);
    }
    painter.setOpacity(1.0);
  }
  painter.restore();
  paintOcrOverlay(painter, sourceImage, editScale());
  if (shapeMenuOpen_) {
    painter.setPen(QPen(QColor(255, 255, 255, 34), 1));
    painter.setBrush(QColor(22, 22, 28, 248));
    painter.drawRoundedRect(shapeMenuRect(), 9, 9);
  }
  if (colorPaletteOpen_) {
    painter.setPen(QPen(QColor(255, 255, 255, 34), 1));
    painter.setBrush(QColor(22, 22, 28, 248));
    painter.drawRoundedRect(colorPaletteRect(), 9, 9);
  }
  if (customColorPickerOpen_) {
    const QRectF panel = customColorPanelRect();
    const QRectF field = panel.adjusted(12, 12, -36, -12);
    const QRectF hue(panel.right() - 26, panel.top() + 12, 14,
                     panel.height() - 24);
    painter.setPen(QPen(QColor(255, 255, 255, 38), 1));
    painter.setBrush(QColor(20, 20, 25, 250));
    painter.drawRoundedRect(panel, 10, 10);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor::fromHsvF(customHue_, 1.0, 1.0));
    painter.drawRoundedRect(field, 5, 5);
    QLinearGradient white(field.topLeft(), field.topRight());
    white.setColorAt(0, Qt::white);
    white.setColorAt(1, QColor(255, 255, 255, 0));
    painter.setBrush(white);
    painter.drawRoundedRect(field, 5, 5);
    QLinearGradient black(field.topLeft(), field.bottomLeft());
    black.setColorAt(0, QColor(0, 0, 0, 0));
    black.setColorAt(1, Qt::black);
    painter.setBrush(black);
    painter.drawRoundedRect(field, 5, 5);

    QLinearGradient hues(hue.topLeft(), hue.bottomLeft());
    hues.setColorAt(0.0, QColor::fromHsvF(0.0, 1, 1));
    hues.setColorAt(1.0 / 6.0, QColor::fromHsvF(1.0 / 6.0, 1, 1));
    hues.setColorAt(2.0 / 6.0, QColor::fromHsvF(2.0 / 6.0, 1, 1));
    hues.setColorAt(3.0 / 6.0, QColor::fromHsvF(3.0 / 6.0, 1, 1));
    hues.setColorAt(4.0 / 6.0, QColor::fromHsvF(4.0 / 6.0, 1, 1));
    hues.setColorAt(5.0 / 6.0, QColor::fromHsvF(5.0 / 6.0, 1, 1));
    hues.setColorAt(1.0, QColor::fromHsvF(1.0, 1, 1));
    painter.setBrush(hues);
    painter.drawRoundedRect(hue, 4, 4);

    const QPointF fieldMarker(
        field.left() + customColor_.hsvSaturationF() * field.width(),
        field.bottom() - customColor_.valueF() * field.height());
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(Qt::white, 2));
    painter.drawEllipse(fieldMarker, 5, 5);
    const qreal hueY = hue.top() + customHue_ * hue.height();
    painter.drawLine(QPointF(hue.left() - 2, hueY),
                     QPointF(hue.right() + 2, hueY));
  }

  if (textEditor_->isVisible()) {
    // Cream pill under the transparent inline editor, and
    // a caret spanning the glyph box rather than the face's whole line height.
    const QRectF box = textEditor_->geometry();
    if (textEditPill_) {
      // The widget keeps typing slack (a 48px floor plus room for the next
      // glyph), so its geometry cannot shape the pill. Rebuild the committed
      // pill's rect from the draft text instead, so nothing shifts on commit.
      const QFontMetricsF pillMetrics(textEditor_->font());
      qreal widest = 0.0;
      for (QTextBlock block = textEditor_->document()->begin();
           block.isValid(); block = block.next()) {
        QTextLayout *layout = block.layout();
        for (int lineIndex = 0; lineIndex < layout->lineCount(); ++lineIndex)
          widest = std::max(
              widest, layout->lineAt(lineIndex).naturalTextWidth());
      }
      const int lineCount =
          std::max(1, qRound(textEditor_->document()->size().height()));
      const qreal pad = std::max(4.0, pillMetrics.height() * 0.18);
      const QPointF glyphOrigin =
          box.topLeft() + textEditor_->viewport()->pos();
      const QRectF glyphs(glyphOrigin.x(), glyphOrigin.y(), widest,
                          pillMetrics.height() +
                              (lineCount - 1) * pillMetrics.lineSpacing());
      const QRectF pill = glyphs.adjusted(
          -pad, -pad, pad,
          std::max(pad, pillMetrics.descent() + 2.0) - pillMetrics.descent());
      const qreal radius = std::min(pill.height() / 4.0, 6.0);
      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor(248, 245, 235));
      painter.drawRoundedRect(pill, radius, radius);
    }
    if (textCaretOn_ && textEditor_->hasFocus()) {
      const QFontMetricsF metrics(textEditor_->font());
      const QRectF cursor =
          QRectF(textEditor_->cursorRect())
              .translated(box.topLeft() + textEditor_->viewport()->pos());
      const qreal baseline = cursor.top() + metrics.ascent();
      const qreal top = baseline - metrics.capHeight() * 1.15;
      const qreal bottom = baseline + metrics.descent() * 0.35;
      // Scale the pen to one line, not the widget: the multiline editor grows
      // taller with every Return, and the caret must not thicken with it.
      const qreal lineBox = metrics.lineSpacing() + metrics.descent() + 4.0;
      painter.setPen(QPen(textColor_, std::max(1.0, lineBox / 18.0)));
      painter.drawLine(QPointF(cursor.center().x(), top),
                       QPointF(cursor.center().x(), bottom));
    }
  }
  if (clipViewport)
    painter.restore();

  // Screenshot chrome means "crop this source", not "this is another
  // selected object". Hide it whenever layer-selection chrome is active;
  // clicking empty canvas puts the layers down and brings cropping back.
  if (tool_ == Tool::Select && selectedAnnotations_.isEmpty()) {
    painter.setPen(QPen(QColor(QStringLiteral("#0a84ff")), 1, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(visibleImage.adjusted(-1, -1, 1, 1));
    if (grown) {
      const QRectF visibleSource = sourceImage.intersected(visibleImage);
      if (!visibleSource.isEmpty()) {
        painter.setPen(QPen(QColor(10, 132, 255, 100), 1, Qt::DashLine));
        painter.drawRect(visibleSource);
      }
    }
    painter.setPen(QPen(QColor(QStringLiteral("#0a84ff")), 2));
    painter.setBrush(QColor(QStringLiteral("#f5f5f7")));
    for (const QRectF &handle : cropHandleRects())
      painter.drawRoundedRect(handle, 3, 3);
  }

  const QString currentTool = toolAction(tool_);
  QFont buttonFont = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
  buttonFont.setPixelSize(11);
  buttonFont.setBold(true);
  painter.setFont(buttonFont);
  const QVector<ToolbarButton> buttons = toolbarButtons();
  const ToolbarButton *hoveredButton = nullptr;
  for (const ToolbarButton &button : buttons) {
    const bool selected =
        button.action == currentTool ||
        (button.action.startsWith(QStringLiteral("tool-arrow-")) &&
         tool_ == Tool::Arrow) ||
        (button.action == QStringLiteral("shape-rectangle") &&
         tool_ == Tool::Rectangle) ||
        (button.action == QStringLiteral("shape-ellipse") &&
         tool_ == Tool::Ellipse) ||
        (button.action == QStringLiteral("shape-fill") && fillShapes_) ||
        (button.action == QStringLiteral("background") && hasBackground) ||
        (button.action == QStringLiteral("palette") && colorPaletteOpen_) ||
        (button.action == QStringLiteral("custom-color") &&
         usingCustomColor_) ||
        (!usingCustomColor_ &&
         button.action == QStringLiteral("color-%1").arg(colorIndex_));
    const bool hovered = button.rect.contains(cursor_);
    if (hovered)
      hoveredButton = &button;
    painter.setPen(QPen(QColor(255, 255, 255, selected ? 64 : 26), 1));
    painter.setBrush(selected ? QColor(66, 66, 75, 250)
                              : (hovered ? QColor(48, 48, 56, 248)
                                         : QColor(34, 34, 40, 244)));
    if (button.action == QStringLiteral("both"))
      painter.setBrush(QColor(QStringLiteral("#0a84ff")));
    painter.drawRoundedRect(button.rect, 8, 8);
    if (button.color.isValid()) {
      const QPointF center = button.rect.center();
      painter.setPen(QPen(selected ? Qt::white : QColor(255, 255, 255, 80),
                          selected ? 2 : 1));
      painter.setBrush(button.color);
      painter.drawEllipse(center, 7, 7);
    } else {
      const QString icon = button.action == QStringLiteral("shape-rectangle")
                               ? QStringLiteral("tool-rectangle")
                               : button.action == QStringLiteral("shape-ellipse")
                                     ? QStringLiteral("tool-ellipse")
                                     : button.action == QStringLiteral("shape-fill")
                                           ? QStringLiteral("tool-rectangle")
                                           : button.action;
      drawToolbarIcon(painter, button.rect, icon, button.label,
                      QColor(245, 245, 247));
    }
  }
  if (textSizeMenuOpen_ && !colorPaletteOpen_ && !customColorPickerOpen_) {
    const QRectF panel = textSizePanelRect();
    painter.setPen(QPen(QColor(255, 255, 255, 34), 1));
    painter.setBrush(QColor(22, 22, 28, 248));
    painter.drawRoundedRect(panel, 9, 9);
    QFont sizeFont = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    sizeFont.setPixelSize(11);
    sizeFont.setBold(true);
    painter.setFont(sizeFont);
    for (int index = 0; index < 3; ++index) {
      const QRectF item(panel.left() + index * 34, panel.top(), 34,
                        panel.height());
      if (index == textSizeIndex_) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(QStringLiteral("#0a84ff")));
        painter.drawRoundedRect(item.adjusted(3, 3, -3, -3), 7, 7);
      }
      painter.setPen(QColor(QStringLiteral("#f5f5f7")));
      painter.drawText(item, Qt::AlignCenter,
                       QString::fromLatin1(
                           kTextSizeNames.at(static_cast<std::size_t>(index))));
    }
  }
  paintSelectTabs(painter);
  if (const QRectF pill = scrollPillRect(); !pill.isNull()) {
    // A way into scroll capture from a region already drawn: the scroll
    // overlay opens with this frame in place.
    const bool hot = pill.contains(cursor_);
    QFont pillFont(QStringLiteral("Noto Sans"));
    pillFont.setPixelSize(11);
    pillFont.setBold(true);
    painter.setFont(pillFont);
    painter.setPen(QPen(QColor(255, 255, 255, hot ? 90 : 40), 1));
    painter.setBrush(hot ? QColor(30, 32, 38, 240) : QColor(18, 18, 22, 220));
    painter.drawRoundedRect(pill, 11, 11);
    painter.setPen(QColor(255, 255, 255, hot ? 255 : 200));
    painter.drawText(pill, Qt::AlignCenter, QStringLiteral("SCROLL CAPTURE"));
  }
  drawStatusPill(painter, rect(), status_);
  QVector<QPointF> keepVisible;
  if (selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size() &&
      hasEndpointHandles(annotations_.at(selectedAnnotation_).kind)) {
    const Annotation &selected = annotations_.at(selectedAnnotation_);
    const qreal scale = editScale();
    keepVisible = {sourceImage.topLeft() + selected.start * scale,
                   sourceImage.topLeft() + selected.end * scale};
  }
  // A windowed editor has no spare corner for the tall card; the guide
  // spreads over the toolbar instead.
  QRectF legendAnchor;
  if (windowedPresentation_) {
    for (const ToolbarButton &button : buttons)
      legendAnchor = legendAnchor.isNull() ? button.rect
                                           : legendAnchor.united(button.rect);
  }
  drawHotkeyLegend(painter, rect(), cursor_, editorHotkeyEntries(),
                   keepVisible, legendAnchor);
  if (hoveredButton) {
    drawInstantTooltip(painter, rect(), hoveredButton->rect,
                       hoveredButton->tooltip);
  } else if (tool_ == Tool::Arrow || tool_ == Tool::Line ||
             tool_ == Tool::Freehand || tool_ == Tool::Highlighter ||
             tool_ == Tool::Marker || tool_ == Tool::Redact ||
             tool_ == Tool::Rectangle || tool_ == Tool::Ellipse ||
             tool_ == Tool::Text) {
    const QString selectedAction = toolAction(tool_);
    for (const ToolbarButton &button : buttons) {
      if (button.action == selectedAction ||
          (tool_ == Tool::Arrow &&
           button.action.startsWith(QStringLiteral("tool-arrow-")))) {
        QString tooltip;
        if (tool_ == Tool::Text) {
          tooltip = QStringLiteral(
                        "%1 · S  M  L · current %2 · Scroll wheel · %3 · T "
                        "again cycles style · Shift+T cycles font")
                        .arg(annotationTextFontName(textFont_))
                        .arg(QString::fromLatin1(kTextSizeNames.at(
                            static_cast<std::size_t>(textSizeIndex_))))
                        .arg(textBackgroundName(textBackground_));
        } else if (tool_ == Tool::Redact) {
          tooltip = QStringLiteral("Redact · %1 · D toggles style")
                        .arg(redactionStyleName(redactionStyle_));
        } else if (tool_ == Tool::Highlighter) {
          tooltip = highlighterTooltip();
        } else if (tool_ == Tool::Rectangle) {
          tooltip = QStringLiteral("Rectangle · %1 · Size %2 · Scroll wheel · "
                                   "Alt+Wheel %3")
                        .arg(fillName(fillShapes_))
                        .arg(qRound(annotationSize_))
                        .arg(cornerName(cornerRadius_));
        } else if (tool_ == Tool::Ellipse) {
          tooltip = QStringLiteral("Ellipse · %1 · Size %2 · Scroll wheel")
                        .arg(fillName(fillShapes_))
                        .arg(qRound(annotationSize_));
        } else if (tool_ == Tool::Freehand) {
          tooltip = QStringLiteral("Size %1 · Scroll wheel · Smoothing %2/%3 "
                                   "· Alt+Wheel")
                        .arg(qRound(annotationSize_))
                        .arg(freehandSmoothingLevel_)
                        .arg(stroke::maximumSmoothingLevel);
        } else {
          tooltip = QStringLiteral("Size %1 · Scroll wheel")
                        .arg(qRound(annotationSize_));
        }
        drawInstantTooltip(painter, rect(), button.rect, tooltip);
        break;
      }
    }
  }
  drawMeasureBadge(painter, rect(), cursor_, measurementText());
}

void CaptureEditor::paintEvent(QPaintEvent *event) {
  Q_UNUSED(event)
  if (scrollPanel_)
    return; // the panel owns the surface; the page shows through its hole
  if (clipboardTextCardEditing_) {
    updateClipboardTextCardEditorGeometry();
    QPainter painter(this);
    painter.setRenderHints(QPainter::Antialiasing |
                           QPainter::SmoothPixmapTransform |
                           QPainter::TextAntialiasing);
    paintClipboardTextCardEditing(painter);
    return;
  }
  QPainter painter(this);
  painter.setRenderHints(QPainter::Antialiasing |
                         QPainter::SmoothPixmapTransform |
                         QPainter::TextAntialiasing);
  if (phase_ == Phase::Select)
    paintSelect(painter);
  else
    paintEdit(painter);
}
