/** @fileoverview Clipboard text-card theming, highlighting, and rendering. */
#pragma once

#include <QColor>
#include <QImage>
#include <QRect>
#include <QString>

class QSyntaxHighlighter;
class QTextDocument;

inline constexpr int kTextCardCodePixelSize = 25;
inline constexpr int kTextCardTabSpaces = 4;

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
};

/** Reads the resolved active Omarchy palette, falling back to Nord colors. */
[[nodiscard]] TextCardTheme loadTextCardTheme();
/** Adds the card's generic code/command highlighter to an editable document. */
[[nodiscard]] QSyntaxHighlighter *
installTextCardHighlighter(QTextDocument *document, const TextCardTheme &theme);
/** Renders a card and reports the image-space rectangle used by its editor. */
[[nodiscard]] TextCardRender renderTextCardLayout(const QString &text,
                                                  const TextCardTheme &theme,
                                                  QString &error,
                                                  bool drawText = true,
                                                  const QString &mode =
                                                      QStringLiteral("NORMAL"));
/** Renders a shareable, syntax-colored clipboard snippet as an image. */
[[nodiscard]] QImage renderTextCard(const QString &text, QString &error);
