/** @fileoverview Clipboard text-card theming, highlighting, and rendering. */
#pragma once

#include <QColor>
#include <QImage>
#include <QRect>
#include <QSize>
#include <QString>

class QSyntaxHighlighter;
class QTextDocument;

inline constexpr int kTextCardCodePixelSize = 25;
/// Auto-fit shrinks toward this floor before letting long lines wrap or clip.
inline constexpr int kTextCardAutoMinPixelSize = 12;
/// Manual +/- overrides stay inside these bounds.
inline constexpr int kTextCardManualMinPixelSize = 8;
inline constexpr int kTextCardManualMaxPixelSize = 48;
/// Default tab-stop width in spaces; keypad 2/4 switches it per card.
inline constexpr int kTextCardTabSpaces = 2;
/// Header/statusline text size; the live filename overlay must match it.
inline constexpr int kTextCardHeaderPixelSize = 14;

/** Share framing for export, or a tight canvas for the live floating editor. */
enum class TextCardLayout { Share, Compact };

/** Resolved Omarchy colors used by both the live editor and flattened card. */
struct TextCardTheme {
  QString name;
  QColor background;
  QColor panel;
  QColor header;
  QColor outline;
  QColor foreground;
  QColor muted;
  QColor selection;
  QColor keyword;
  QColor command;
  QColor flag;
  QColor number;
  QColor string;
  QColor comment;
};

/** The flattened card plus the code box needed to place the live editor. */
struct TextCardRender {
  QImage image;
  QRect editorRect;
  QRect titleRect;
  /// Code pixel size actually used; reports what auto-fit chose.
  int codePixelSize = kTextCardCodePixelSize;
};

/** Reads Omarchy's UI and semantic syntax palettes, falling back to Nord. */
[[nodiscard]] TextCardTheme loadTextCardTheme();
/** Detects the compact syntax profile shown in the card status line. */
[[nodiscard]] QString detectTextCardLanguage(const QString &text,
                                             const QString &filename = {});
/** Chooses a useful editable default name from the detected language. */
[[nodiscard]] QString defaultTextCardFilename(const QString &text);
/** Empty when the text fits a card; the user-facing reason otherwise. */
[[nodiscard]] QString textCardSourceError(const QString &text);
/** Adds the detected filename/content syntax profile to an editable document. */
[[nodiscard]] QSyntaxHighlighter *
installTextCardHighlighter(QTextDocument *document, const TextCardTheme &theme,
                           const QString &filename = {});
/** Renders a card and reports the image-space rectangle used by its editor.
 * pixelRatio > 1 supersamples the image for crisp on-screen scaling; the
 * reported rectangles stay in logical card coordinates. codePixelSize 0
 * auto-fits the longest line inside the panel; noWrap clips long lines at
 * the panel edge instead of wrapping them. */
[[nodiscard]] TextCardRender renderTextCardLayout(const QString &text,
                                                  const TextCardTheme &theme,
                                                  QString &error,
                                                  bool drawText = true,
                                                  const QString &mode =
                                                      QStringLiteral("NORMAL"),
                                                  const QString &filename = {},
                                                  TextCardLayout layout =
                                                      TextCardLayout::Share,
                                                  qreal pixelRatio = 1.0,
                                                  int codePixelSize = 0,
                                                  bool noWrap = false,
                                                  int tabSpaces =
                                                      kTextCardTabSpaces);
/** Window size that hugs a compact live card plus its snippet-only toolbar. */
[[nodiscard]] QSize textCardEditorWindowSize(const QSize &card,
                                             const QSize &available);
/** Renders a shareable, syntax-colored clipboard snippet as an image. */
[[nodiscard]] QImage renderTextCard(const QString &text, QString &error);
