/** @fileoverview Declares screenshot capture, rendering, and output types. */
#pragma once

#include <cstdint>

#include <QPainterPath>
#include <QColor>
#include <QImage>
#include <QRect>
#include <QString>
#include <QVector>

class QFont;
class QPainter;
class QProcess;

struct MonitorInfo {
  QString name;
  QRect geometry;
  QSize pixelSize;
  qreal scale = 1.0;
  int workspaceId = 0;
};

struct WindowTarget {
  QRect rect;
  QString stableId;
  QString title;
};

struct CaptureData {
  MonitorInfo monitor;
  QImage source;
  /** Logical size the native source image is presented at. */
  QSize previewSize;
  QVector<WindowTarget> windows;
};

enum class BackgroundStyle { None, Aurora, Sunset, Lagoon, Violet };
enum class QuickOutputMode { None, Copy, Save, Both };

enum class SpotlightShape { Ellipse, Rectangle, RoundedRectangle };
enum class RedactionStyle { Solid, Pixelate };
enum class TextBackground { Plain, Pill };

struct Annotation {
  enum class Kind {
    Arrow,
    Line,
    Freehand,
    Highlighter,
    Marker,
    Rectangle,
    Text,
    Redaction,
    Spotlight
  };

  Kind kind = Kind::Arrow;
  QPointF start;
  QPointF end;
  QString text;
  QColor color;
  qreal size = 4.0;
  int number = 0;
  QVector<QPointF> points;
  RedactionStyle redactionStyle = RedactionStyle::Pixelate;
  qreal magnification = 2.0;
  SpotlightShape spotlightShape = SpotlightShape::Ellipse;
  quint32 redactionSeed = 0;
  TextBackground textBackground = TextBackground::Pill;
  /// Wrap width for text layers in image px; 0 keeps the text on one line.
  qreal textWidth = 0.0;

  bool operator==(const Annotation &) const = default;
};

enum class AnnotationLayer { Redaction, Default };

[[nodiscard]] constexpr AnnotationLayer annotationLayer(Annotation::Kind kind) {
  return kind == Annotation::Kind::Redaction ? AnnotationLayer::Redaction
                                             : AnnotationLayer::Default;
}

[[nodiscard]] bool loadCaptureFonts();
[[nodiscard]] QFont annotationTextFont(qreal size);
/**
 * Discovers the focused monitor (name, geometry, scale). Fast: only one
 * `hyprctl monitors` call. Safe to call on the main thread to position the
 * overlay before the pixel capture itself runs in the background.
 */
[[nodiscard]] bool probeFocusedMonitor(MonitorInfo &monitor, QString &error);
/**
 * Captures the focused monitor's pixels onto the given monitor, and its window
 * list when `includeWindows` is set. Window discovery runs alongside the screen
 * grab, so callers that never show the overlay should skip it. Pure I/O and
 * image work (no GUI objects): safe on any thread.
 */
[[nodiscard]] bool captureMonitorPixels(const MonitorInfo &monitor,
                                        CaptureData &capture,
                                        bool includeWindows, QString &error);
/** Convenience: probes the focused monitor, then captures its pixels. */
[[nodiscard]] bool captureFocusedMonitor(CaptureData &capture,
                                         bool includeWindows, QString &error);
/** One laid-out line of a text layer: its trimmed text, baseline y and
 *  advance width; lines start at `Annotation::start.x()`. `start`/`length`
 *  give the line's range in `Annotation::text` (untrimmed) so a caret index
 *  can be mapped to a line while the text is being edited. */
struct TextLine {
  QString text;
  qreal baseline = 0.0;
  qreal width = 0.0;
  int start = 0;
  int length = 0;
};
/// Narrowest wrap width worth producing; below this text stays on one line.
constexpr qreal kMinimumTextWrapWidth = 24.0;
/** The width a text layer wraps to: its own `textWidth`, else the room left
 *  before the canvas' right edge (`canvasWidth`, 0 = unbounded) when that is
 *  at least `kMinimumTextWrapWidth`, else 0 (no wrapping). */
[[nodiscard]] qreal annotationTextWrapWidth(const Annotation &annotation,
                                            qreal canvasWidth = 0.0);
/** Word-wraps a text layer to `annotationTextWrapWidth()`, so text never
 *  runs off the capture. */
[[nodiscard]] QVector<TextLine>
layoutAnnotationText(const Annotation &annotation, qreal canvasWidth = 0.0);
/** Padding the readability pill adds around a text layer's glyphs (0 when
 *  the layer is plain). */
[[nodiscard]] qreal textPillPadding(const Annotation &annotation);
/** Bounds of a text layer's glyph box, or of its readability pill when it
 *  has one; `start` is the first line's baseline origin and, when wrapping,
 *  the box spans the wrap width so the resize handle stays put. */
[[nodiscard]] QRectF annotationTextBounds(const Annotation &annotation,
                                          qreal canvasWidth = 0.0);
/** Where a caret at character index `cursor` sits: x on the line and that
 *  line's baseline, in the same coordinates as `Annotation::start`. */
[[nodiscard]] QPointF annotationTextCaret(const Annotation &annotation,
                                          int cursor, qreal canvasWidth = 0.0);
/** The character index nearest `point` in the laid-out text (for placing the
 *  caret with a click); clamps to the closest line. */
[[nodiscard]] int annotationTextCursorAt(const Annotation &annotation,
                                         const QPointF &point,
                                         qreal canvasWidth = 0.0);
/** Glyph-box rectangles covering characters [from, to) across the laid-out
 *  lines, for painting a selection highlight. */
[[nodiscard]] QVector<QRectF>
annotationTextSelectionRects(const Annotation &annotation, int from, int to,
                             qreal canvasWidth = 0.0);
[[nodiscard]] bool captureWindowSurface(const WindowTarget &window,
                                        QImage &image, QString &error);
/** Returns an upright image for captured Wayland buffer contents. */
[[nodiscard]] QImage normalizeWaylandCapture(const QImage &image,
                                             std::uint32_t transform);
[[nodiscard]] QImage renderCapture(const CaptureData &capture,
                                   const QRectF &selection,
                                   const QVector<Annotation> &annotations,
                                   BackgroundStyle backgroundStyle);
[[nodiscard]] bool copyPngFileToClipboard(const QString &path, QString &error);
[[nodiscard]] bool quickOutput(const QImage &image, QuickOutputMode mode,
                               QString &error);
[[nodiscard]] bool copyTextToClipboard(const QString &text, QString &error);
void paintAnnotation(QPainter &painter, const Annotation &annotation,
                     qreal canvasWidth = 0.0);
[[nodiscard]] QPainterPath spotlightPath(const Annotation &annotation);
void paintSpotlights(QPainter &painter, const QImage &source,
                     const QRectF &targetBounds, const QRectF &sourceRect,
                     const QVector<Annotation> &annotations);
/**
 * Paints the default annotation layer (spotlights, then vectors) in selection
 * space. Spotlights sample `redacted`, which must already include the
 * redaction layer so a loupe cannot magnify source pixels.
 */
void paintDefaultLayer(QPainter &painter, const QImage &redacted,
                       const QRectF &logicalBounds,
                       const QVector<Annotation> &annotations);
void paintCaptureBackground(QPainter &painter, const QRectF &bounds,
                            BackgroundStyle backgroundStyle);
/**
 * Renders the selection region at `targetSize` for the redaction layer. The
 * result carries no annotations; callers overlay redactions with
 * applyRedactionsScaled and cache it while the selection is unchanged.
 */
[[nodiscard]] QImage renderSelectionBase(const CaptureData &capture,
                                         const QRectF &selection,
                                         const QSize &targetSize);
/**
 * Paints redaction annotations over a display-resolution selection image. The
 * source image MUST be the exact selection region scaled to `targetSize`;
 * annotations are selection-relative, spanning 0..`selection` size.
 */
QImage applyRedactionsScaled(QImage image, const QVector<Annotation> &redactions,
                             const QRectF &selection, const QSizeF &targetSize);
/** Creates or repairs a private directory owned by the current user. */
[[nodiscard]] bool ensurePrivateDirectory(const QString &path);
/** Returns Omasnap's private runtime directory, or empty on failure. */
[[nodiscard]] QString secureRuntimeDirectory();
[[nodiscard]] QString moveSnapshotToScreenshots(const QString &sourcePath,
                                                QString &error);
[[nodiscard]] QString temporarySnapshotPath();
[[nodiscard]] QString pinnedSnapshotPath(int index);
void prunePinnedSnapshots();
/**
 * Writes `image` into the private runtime directory. `quality` is the Qt PNG
 * quality knob, which maps inversely onto zlib levels: -1 keeps the default
 * level, higher values compress less and encode faster.
 */
[[nodiscard]] bool saveTemporarySnapshot(const QImage &image, QString path,
                                         QString &error, int quality = -1);
[[nodiscard]] QString recognizeText(const QImage &image, QString &error);
void sendCaptureNotification(const QString &message,
                             const QString &imagePath = {});
