/** @fileoverview Renders editable clipboard snippets in the active Omarchy
 * theme. */
#include "text-card.hpp"

#include "capture.hpp"

#include <QAbstractTextDocumentLayout>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontMetricsF>
#include <QHash>
#include <QPainter>
#include <QPalette>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTextOption>

#include <algorithm>

namespace {
constexpr int kOutputWidth = 1200;
constexpr int kMinimumOutputHeight = 675;
constexpr int kPanelWidth = 1040;
constexpr int kHeaderHeight = 58;
constexpr int kStatusHeight = 32;
constexpr int kHorizontalTextPadding = 46;
constexpr int kGutterWidth = 54;
constexpr int kTopTextPadding = 34;
constexpr int kBottomTextPadding = 40;
constexpr int kMinimumEditorHeight = 220;

class TextCardHighlighter final : public QSyntaxHighlighter {
public:
  TextCardHighlighter(QTextDocument *document, const TextCardTheme &theme)
      : QSyntaxHighlighter(document) {
    addRule(
        QStringLiteral(
            R"(\b(?:alignas|auto|bool|break|case|catch|class|const|constexpr|continue|def|do|done|elif|else|enum|export|false|fi|final|finally|float|for|foreach|from|function|if|import|in|int|interface|let|namespace|new|null|nullptr|override|private|protected|public|return|static|string|struct|switch|then|throw|true|try|type|using|var|void|while|yield)\b)"),
        theme.keyword, QFont::DemiBold);
    addRule(
        QStringLiteral(
            R"((?:^|(?<=\s))(?:\$\s*)?(?:cd|cmake|curl|docker|echo|git|make|npm|omasnap|pacman|pnpm|python|sudo|tar|yarn)(?=\s|$))"),
        theme.command, QFont::DemiBold);
    addRule(QStringLiteral(R"((?<!\w)--?[A-Za-z][\w-]*)"), theme.flag);
    addRule(QStringLiteral(R"(https?://[^\s)\]}>]+)"), theme.flag);
    addRule(QStringLiteral(R"(\b(?:0x[0-9A-Fa-f]+|\d+(?:\.\d+)?)\b)"),
            theme.number);
    addRule(QStringLiteral(R"("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|`[^`]*`)"),
            theme.string);
    addRule(QStringLiteral(R"((?:^|\s)(?:#|//).*$)"), theme.comment,
            QFont::Normal, true);
    addRule(QStringLiteral(R"(^\s*```.*$)"), theme.comment);
  }

protected:
  void highlightBlock(const QString &text) override {
    for (const Rule &rule : rules_) {
      QRegularExpressionMatchIterator matches = rule.pattern.globalMatch(text);
      while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        setFormat(match.capturedStart(), match.capturedLength(), rule.format);
        if (rule.toEnd)
          break;
      }
    }
  }

private:
  struct Rule {
    QRegularExpression pattern;
    QTextCharFormat format;
    bool toEnd = false;
  };

  void addRule(const QString &pattern, const QColor &color,
               QFont::Weight weight = QFont::Normal, bool toEnd = false) {
    QTextCharFormat format;
    format.setForeground(color);
    format.setFontWeight(weight);
    rules_.push_back({QRegularExpression(pattern), format, toEnd});
  }

  QVector<Rule> rules_;
};

QColor readableColor(const QHash<QString, QColor> &colors, const QString &key,
                     const QColor &fallback) {
  const auto match = colors.constFind(key);
  return match != colors.cend() && match->isValid() ? *match : fallback;
}

QHash<QString, QColor> readOmarchyColors(const QString &path) {
  QHash<QString, QColor> colors;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    return colors;
  const QRegularExpression assignment(QStringLiteral(
      R"(^\s*([A-Za-z][A-Za-z0-9_]*)\s*=\s*["']?(#[0-9A-Fa-f]{6,8})["']?\s*$)"));
  while (!file.atEnd()) {
    const QRegularExpressionMatch match =
        assignment.match(QString::fromUtf8(file.readLine()));
    if (!match.hasMatch())
      continue;
    const QColor color(match.captured(2));
    if (color.isValid())
      colors.insert(match.captured(1), color);
  }
  return colors;
}

QString currentThemeName() {
  const QString override =
      qEnvironmentVariable("OMASNAP_TEST_OMARCHY_THEME_NAME").trimmed();
  if (!override.isEmpty())
    return override;
  QFile file(QDir::homePath() +
             QStringLiteral("/.local/state/omarchy/current/theme.name"));
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    return QStringLiteral("Omarchy");
  QString name = QString::fromUtf8(file.readLine()).trimmed();
  name.replace('-', ' ');
  return name.isEmpty() ? QStringLiteral("Omarchy") : name;
}

QString textCardSnippet(QString text, QString &error) {
  text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
  text.replace('\r', '\n');
  if (text.trimmed().isEmpty()) {
    error = QStringLiteral("Clipboard text is empty");
    return {};
  }
  if (text.size() > 16000 || text.count('\n') >= 120) {
    error = QStringLiteral(
        "Clipboard text is too long for a share card (16,000 characters or "
        "120 lines max)");
    return {};
  }
  for (qsizetype index = 0; index < text.size(); ++index) {
    const QChar character = text.at(index);
    if (character.unicode() < 0x20 && character != '\n' && character != '\t')
      text[index] = QLatin1Char(' ');
  }
  return text;
}

QFont textCardCodeFont() {
  static_cast<void>(loadCaptureFonts());
  QFont codeFont(annotationTextFontName(TextFont::JetBrainsMono));
  codeFont.setPixelSize(kTextCardCodePixelSize);
  codeFont.setStyleHint(QFont::Monospace);
  return codeFont;
}

void prepareTextDocument(QTextDocument &document, const QString &snippet,
                         const TextCardTheme &theme, int textWidth) {
  const QFont codeFont = textCardCodeFont();
  document.setDocumentMargin(0.0);
  document.setDefaultFont(codeFont);
  QTextOption option;
  option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
  option.setTabStopDistance(QFontMetricsF(codeFont).horizontalAdvance(' ') *
                            kTextCardTabSpaces);
  document.setDefaultTextOption(option);
  document.setPlainText(snippet);
  document.setTextWidth(textWidth);
  auto *highlighter = new TextCardHighlighter(&document, theme);
  highlighter->rehighlight();
}
} // namespace

TextCardTheme loadTextCardTheme() {
  const QString override =
      qEnvironmentVariable("OMASNAP_TEST_OMARCHY_COLORS").trimmed();
  const QString path =
      override.isEmpty()
          ? QDir::homePath() + QStringLiteral("/.local/state/omarchy/"
                                              "current/theme/colors.toml")
          : override;
  const QHash<QString, QColor> colors = readOmarchyColors(path);
  const QColor background = readableColor(colors, QStringLiteral("background"),
                                          QColor(QStringLiteral("#2e3440")));
  const QColor panel = readableColor(colors, QStringLiteral("dark_background"),
                                     QColor(QStringLiteral("#222730")));
  const QColor header =
      readableColor(colors, QStringLiteral("darker_background"),
                    QColor(QStringLiteral("#191c23")));
  const QColor accent = readableColor(colors, QStringLiteral("accent"),
                                      QColor(QStringLiteral("#81a1c1")));
  return {currentThemeName(),
          background,
          panel,
          header,
          accent,
          readableColor(colors, QStringLiteral("foreground"),
                        QColor(QStringLiteral("#d8dee9"))),
          readableColor(colors, QStringLiteral("muted"),
                        QColor(QStringLiteral("#4c566a"))),
          readableColor(colors, QStringLiteral("selection"),
                        QColor(QStringLiteral("#434c5e"))),
          readableColor(colors, QStringLiteral("magenta"),
                        QColor(QStringLiteral("#b48ead"))),
          readableColor(colors, QStringLiteral("blue"), accent),
          readableColor(colors, QStringLiteral("cyan"),
                        QColor(QStringLiteral("#88c0d0"))),
          readableColor(colors, QStringLiteral("orange"),
                        QColor(QStringLiteral("#d5967a"))),
          readableColor(colors, QStringLiteral("yellow"),
                        QColor(QStringLiteral("#ebcb8b"))),
          readableColor(colors, QStringLiteral("dark_foreground"),
                        QColor(QStringLiteral("#667080")))};
}

QSyntaxHighlighter *installTextCardHighlighter(QTextDocument *document,
                                               const TextCardTheme &theme) {
  return new TextCardHighlighter(document, theme);
}

TextCardRender renderTextCardLayout(const QString &text,
                                    const TextCardTheme &theme, QString &error,
                                    bool drawText, bool insertMode) {
  error.clear();
  const QString snippet = textCardSnippet(text, error);
  if (snippet.isEmpty())
    return {};

  const int textWidth = kPanelWidth - kHorizontalTextPadding * 2 - kGutterWidth;
  QTextDocument document;
  prepareTextDocument(document, snippet, theme, textWidth);
  const int documentHeight =
      std::max(1, qCeil(document.documentLayout()->documentSize().height()));
  const int editorHeight = std::max(kMinimumEditorHeight, documentHeight);
  const int panelHeight = kHeaderHeight + kTopTextPadding + editorHeight +
                          kBottomTextPadding + kStatusHeight;
  const int outputHeight = std::max(kMinimumOutputHeight, panelHeight + 160);

  QImage image(kOutputWidth, outputHeight, QImage::Format_ARGB32_Premultiplied);
  image.fill(theme.background);
  QPainter painter(&image);
  painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);

  // Hard-edged blocks and an offset frame echo Omarchy's terminal/window
  // chrome without borrowing macOS traffic lights or soft card corners.
  const int panelX = (kOutputWidth - kPanelWidth) / 2;
  const int panelY = (outputHeight - panelHeight) / 2;
  const QRect panel(panelX, panelY, kPanelWidth, panelHeight);
  painter.fillRect(panel.translated(12, 12), theme.header);
  painter.fillRect(panel, theme.panel);
  painter.fillRect(
      QRect(panel.left(), panel.top(), panel.width(), kHeaderHeight),
      theme.header);
  painter.fillRect(QRect(panel.left(), panel.top(), 6, kHeaderHeight),
                   theme.outline);
  painter.setPen(QPen(theme.muted, 1));
  painter.drawLine(panel.left(), panel.top() + kHeaderHeight, panel.right(),
                   panel.top() + kHeaderHeight);

  QFont headerFont = textCardCodeFont();
  headerFont.setPixelSize(14);
  painter.setFont(headerFont);
  painter.setPen(theme.outline);
  const QRect headerText(panel.left() + 24, panel.top(), panel.width() - 48,
                         kHeaderHeight);
  painter.drawText(headerText, Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("NVIM"));
  painter.setPen(theme.foreground);
  painter.drawText(headerText.adjusted(58, 0, 0, 0),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("~/clipboard/snippet"));
  painter.setPen(theme.muted);
  painter.drawText(headerText, Qt::AlignRight | Qt::AlignVCenter,
                   theme.name.toUpper());

  const QRect editorRect(panel.left() + kHorizontalTextPadding + kGutterWidth,
                         panel.top() + kHeaderHeight + kTopTextPadding,
                         textWidth, editorHeight);
  painter.setPen(QPen(theme.header, 1));
  painter.drawLine(editorRect.left() - 18, editorRect.top(),
                   editorRect.left() - 18, editorRect.bottom());
  const QFont codeFont = textCardCodeFont();
  painter.setFont(codeFont);
  painter.setPen(theme.muted);
  const QFontMetrics codeMetrics(codeFont);
  int lineNumber = 1;
  for (QTextBlock block = document.begin(); block.isValid();
       block = block.next(), ++lineNumber) {
    const qreal top = document.documentLayout()->blockBoundingRect(block).top();
    painter.drawText(
        QRectF(editorRect.left() - kGutterWidth, editorRect.top() + top,
               kGutterWidth - 28, codeMetrics.lineSpacing()),
        Qt::AlignRight | Qt::AlignTop, QString::number(lineNumber));
  }
  if (drawText) {
    painter.save();
    painter.translate(editorRect.topLeft());
    QAbstractTextDocumentLayout::PaintContext context;
    context.clip = QRectF(0, 0, textWidth, documentHeight);
    context.palette.setColor(QPalette::Text, theme.foreground);
    document.documentLayout()->draw(&painter, context);
    painter.restore();
  }

  const QRect status(panel.left(), panel.bottom() - kStatusHeight + 1,
                     panel.width(), kStatusHeight);
  painter.fillRect(status, theme.header);
  const QString mode =
      insertMode ? QStringLiteral(" INSERT ") : QStringLiteral(" NORMAL ");
  const QRect modeRect(status.left(), status.top(), 104, status.height());
  painter.fillRect(modeRect, theme.outline);
  painter.setFont(headerFont);
  painter.setPen(theme.outline.lightness() > 130 ? theme.header
                                                 : theme.foreground);
  painter.drawText(modeRect, Qt::AlignCenter, mode);
  painter.setPen(theme.foreground);
  painter.drawText(status.adjusted(122, 0, -24, 0),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("clipboard"));
  painter.setPen(theme.muted);
  painter.drawText(status.adjusted(24, 0, -24, 0),
                   Qt::AlignRight | Qt::AlignVCenter,
                   QStringLiteral("%1L  UTF-8  LF").arg(lineNumber - 1));
  painter.setPen(QPen(theme.outline, 2));
  painter.setBrush(Qt::NoBrush);
  painter.drawRect(panel.adjusted(1, 1, -1, -1));
  painter.end();
  return {std::move(image), editorRect};
}

QImage renderTextCard(const QString &text, QString &error) {
  return renderTextCardLayout(text, loadTextCardTheme(), error).image;
}
