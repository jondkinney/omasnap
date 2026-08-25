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
inline constexpr int kTextCardTabSpaces = 4;

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
};

/** Reads Omarchy's UI and semantic syntax palettes, falling back to Nord. */
[[nodiscard]] TextCardTheme loadTextCardTheme();
/** Detects the compact syntax profile shown in the card status line. */
[[nodiscard]] QString detectTextCardLanguage(const QString &text,
                                             const QString &filename = {});
/** Chooses a useful editable default name from the detected language. */
[[nodiscard]] QString defaultTextCardFilename(const QString &text);
/** Adds the detected filename/content syntax profile to an editable document. */
[[nodiscard]] QSyntaxHighlighter *
installTextCardHighlighter(QTextDocument *document, const TextCardTheme &theme,
                           const QString &filename = {});
/** Renders a card and reports the image-space rectangle used by its editor. */
[[nodiscard]] TextCardRender renderTextCardLayout(const QString &text,
                                                  const TextCardTheme &theme,
                                                  QString &error,
                                                  bool drawText = true,
                                                  const QString &mode =
                                                      QStringLiteral("NORMAL"),
                                                  const QString &filename = {},
                                                  TextCardLayout layout =
                                                      TextCardLayout::Share);
/** Window size that hugs a compact live card plus its snippet-only toolbar. */
[[nodiscard]] QSize textCardEditorWindowSize(const QSize &card,
                                             const QSize &available);
/** Renders a shareable, syntax-colored clipboard snippet as an image. */
[[nodiscard]] QImage renderTextCard(const QString &text, QString &error);
