/** @fileoverview Renders editable clipboard snippets in the active Omarchy
 * theme. */
#include "text-card.hpp"

#include "capture.hpp"

#include <QAbstractTextDocumentLayout>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTextOption>
#include <QStringList>

#include <algorithm>
#include <cmath>

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
constexpr int kPowerlineAngle = 14;
constexpr int kCompactPadding = 16;
constexpr int kPanelShadowOffset = 12;
constexpr int kCompactWindowSide = 24;
constexpr int kCompactWindowTop = 64;
constexpr int kCompactWindowBottom = 24;

class TextCardHighlighter final : public QSyntaxHighlighter {
public:
  TextCardHighlighter(QTextDocument *document, const TextCardTheme &theme,
                      const QString &language)
      : QSyntaxHighlighter(document) {
    // The string rule claims its spans first; every later rule skips them,
    // so a "//" or keyword inside a literal keeps the string color.
    addRule(QStringLiteral(R"("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|`[^`]*`)"),
            theme.string, QFont::Normal, false, RuleScope::ClaimsStrings);
    addRule(QStringLiteral(R"(\b(?:0x[0-9A-Fa-f]+|\d+(?:\.\d+)?)\b)"),
            theme.number);
    addRule(QStringLiteral(R"(https?://[^\s)\]}>]+)"), theme.flag);

    if (language == QStringLiteral("C++") ||
        language == QStringLiteral("JavaScript") ||
        language == QStringLiteral("TypeScript") ||
        language == QStringLiteral("QML") ||
        language == QStringLiteral("Rust") || language == QStringLiteral("Go") ||
        language == QStringLiteral("CSS") ||
        language == QStringLiteral("SCSS") ||
        language == QStringLiteral("Sass")) {
      addBlockSpan(QStringLiteral("/*"), QStringLiteral("*/"), theme.comment);
    } else if (language == QStringLiteral("Python")) {
      addBlockSpan(QStringLiteral("\"\"\""), QStringLiteral("\"\"\""),
                   theme.string);
      addBlockSpan(QStringLiteral("'''"), QStringLiteral("'''"), theme.string);
    } else if (language == QStringLiteral("Lua")) {
      addBlockSpan(QStringLiteral("--[["), QStringLiteral("]]"),
                   theme.comment);
    }

    if (language == QStringLiteral("Shell")) {
      addRule(QStringLiteral(
                  R"(\b(?:case|do|done|elif|else|esac|export|fi|for|function|if|in|select|then|time|until|while)\b)"),
              theme.keyword, QFont::DemiBold);
      addRule(QStringLiteral(
                  R"((?:^|(?<=\s))(?:\$\s*)?(?:cd|cmake|curl|docker|echo|git|make|npm|omasnap|pacman|pnpm|python|sudo|tar|yarn)(?=\s|$))"),
              theme.command, QFont::DemiBold);
      addRule(QStringLiteral(R"(\$\{?[A-Za-z_][A-Za-z0-9_]*\}?)"),
              theme.number);
      addRule(QStringLiteral(R"((?<!\w)--?[A-Za-z][\w-]*)"), theme.flag);
      addRule(QStringLiteral(R"(^\s*#.*$)"), theme.comment, QFont::Normal,
              true);
    } else if (language == QStringLiteral("C++")) {
      addRule(QStringLiteral(
                  R"(\b(?:alignas|auto|bool|break|case|catch|class|const|constexpr|continue|default|delete|do|double|else|enum|explicit|false|final|float|for|friend|if|inline|int|namespace|new|nullptr|operator|override|private|protected|public|return|sizeof|static|struct|switch|template|this|throw|true|try|typedef|typename|union|using|virtual|void|while)\b)"),
              theme.keyword, QFont::DemiBold);
      addRule(QStringLiteral(R"(^\s*#\s*\w+.*$)"), theme.command);
      addRule(QStringLiteral(R"(//.*$|/\*.*\*/)"), theme.comment,
              QFont::Normal, true);
    } else if (language == QStringLiteral("JavaScript") ||
               language == QStringLiteral("TypeScript")) {
      addRule(QStringLiteral(
                  R"(\b(?:async|await|break|case|catch|class|const|continue|debugger|default|delete|do|else|enum|export|extends|false|finally|for|from|function|if|implements|import|in|instanceof|interface|let|new|null|of|private|protected|public|return|static|super|switch|this|throw|true|try|type|typeof|undefined|var|void|while|yield)\b)"),
              theme.keyword, QFont::DemiBold);
      addRule(QStringLiteral(R"(//.*$|/\*.*\*/)"), theme.comment,
              QFont::Normal, true);
    } else if (language == QStringLiteral("QML")) {
      addRule(QStringLiteral(
                  R"(\b(?:as|break|case|catch|const|continue|default|do|else|false|finally|for|function|if|import|in|instanceof|let|null|on|property|readonly|required|return|signal|switch|this|throw|true|try|typeof|var|void|while)\b)"),
              theme.keyword, QFont::DemiBold);
      addRule(QStringLiteral(R"(^\s*[A-Z][A-Za-z0-9_.]*\s*(?=\{))"),
              theme.command, QFont::DemiBold);
      addRule(QStringLiteral(R"(\bon[A-Z][A-Za-z0-9_]*\b)"), theme.flag);
      addRule(QStringLiteral(R"(//.*$|/\*.*\*/)"), theme.comment,
              QFont::Normal, true);
    } else if (language == QStringLiteral("Python")) {
      addRule(QStringLiteral(
                  R"(\b(?:and|as|assert|async|await|break|case|class|continue|def|del|elif|else|except|False|finally|for|from|global|if|import|in|is|lambda|match|None|nonlocal|not|or|pass|raise|return|True|try|while|with|yield)\b)"),
              theme.keyword, QFont::DemiBold);
      addRule(QStringLiteral(R"(^\s*@[A-Za-z_][\w.]*)"), theme.command);
      addRule(QStringLiteral(R"(#.*$)"), theme.comment, QFont::Normal, true);
    } else if (language == QStringLiteral("Ruby")) {
      addRule(QStringLiteral(
                  R"(\b(?:alias|and|begin|break|case|class|def|defined\?|do|else|elsif|end|ensure|false|for|if|in|module|next|nil|not|or|redo|rescue|retry|return|self|super|then|true|undef|unless|until|when|while|yield)\b)"),
              theme.keyword, QFont::DemiBold);
      addRule(QStringLiteral(R"((?:@@?|\$)[A-Za-z_][A-Za-z0-9_]*)"),
              theme.number);
      addRule(QStringLiteral(R"(:[A-Za-z_][A-Za-z0-9_]*[!?=]?)"),
              theme.flag);
      addRule(QStringLiteral(R"(#.*$)"), theme.comment, QFont::Normal, true);
    } else if (language == QStringLiteral("Rust")) {
      addRule(QStringLiteral(
                  R"(\b(?:as|async|await|break|const|continue|crate|dyn|else|enum|extern|false|fn|for|if|impl|in|let|loop|match|mod|move|mut|pub|ref|return|self|Self|static|struct|super|trait|true|type|unsafe|use|where|while)\b)"),
              theme.keyword, QFont::DemiBold);
      addRule(QStringLiteral(R"(\b[A-Za-z_][A-Za-z0-9_]*!)"), theme.command);
      addRule(QStringLiteral(R"(//.*$|/\*.*\*/)"), theme.comment,
              QFont::Normal, true);
    } else if (language == QStringLiteral("Go")) {
      addRule(QStringLiteral(
                  R"(\b(?:break|case|chan|const|continue|default|defer|else|fallthrough|for|func|go|goto|if|import|interface|map|package|range|return|select|struct|switch|type|var)\b)"),
              theme.keyword, QFont::DemiBold);
      addRule(QStringLiteral(R"(//.*$|/\*.*\*/)"), theme.comment,
              QFont::Normal, true);
    } else if (language == QStringLiteral("JSON")) {
      addRule(QStringLiteral(R"("(?:\\.|[^"\\])*"\s*(?=:))"), theme.keyword,
              QFont::Normal, false, RuleScope::OverStrings);
      addRule(QStringLiteral(R"(\b(?:false|null|true)\b)"), theme.keyword,
              QFont::DemiBold);
    } else if (language == QStringLiteral("YAML")) {
      addRule(QStringLiteral(R"(^\s*[A-Za-z_][\w.-]*\s*(?=:))"),
              theme.keyword, QFont::DemiBold);
      addRule(QStringLiteral(R"(^\s*-\s+)"), theme.command);
      addRule(QStringLiteral(R"(#.*$)"), theme.comment, QFont::Normal, true);
    } else if (language == QStringLiteral("TOML")) {
      addRule(QStringLiteral(R"(^\s*\[\[?[^\]]+\]\]?\s*$)"), theme.command,
              QFont::DemiBold);
      addRule(QStringLiteral(R"(^\s*[A-Za-z0-9_.-]+\s*(?==))"),
              theme.keyword, QFont::DemiBold);
      addRule(QStringLiteral(R"(\b(?:false|true)\b)"), theme.keyword,
              QFont::DemiBold);
      addRule(QStringLiteral(R"(#.*$)"), theme.comment, QFont::Normal, true);
    } else if (language == QStringLiteral("CSS") ||
               language == QStringLiteral("SCSS") ||
               language == QStringLiteral("Sass")) {
      addRule(QStringLiteral(R"(@[A-Za-z-]+)"), theme.keyword,
              QFont::DemiBold);
      addRule(QStringLiteral(R"(\$[A-Za-z_-][A-Za-z0-9_-]*)"),
              theme.number);
      addRule(QStringLiteral(R"((?:--)?[A-Za-z_-][A-Za-z0-9_-]*\s*(?=:))"),
              theme.command);
      addRule(QStringLiteral(R"(#[0-9A-Fa-f]{3,8}\b)"), theme.number);
      addRule(QStringLiteral(R"(/\*.*\*/|//.*$)"), theme.comment,
              QFont::Normal, true);
    } else if (language == QStringLiteral("Markdown")) {
      addRule(QStringLiteral(R"(^\s{0,3}#{1,6}\s+.*$)"), theme.keyword,
              QFont::DemiBold);
      addRule(QStringLiteral(R"(`[^`]+`|^\s*```.*$)"), theme.command);
      addRule(QStringLiteral(R"(\[[^\]]+\]\([^)]+\))"), theme.flag);
    } else if (language == QStringLiteral("Lua")) {
      addRule(QStringLiteral(
                  R"(\b(?:and|break|do|else|elseif|end|false|for|function|goto|if|in|local|nil|not|or|repeat|return|then|true|until|while)\b)"),
              theme.keyword, QFont::DemiBold);
      addRule(QStringLiteral(R"(--.*$)"), theme.comment, QFont::Normal, true);
    } else if (language == QStringLiteral("CMake")) {
      addRule(QStringLiteral(
                  R"(\b(?:add_executable|add_library|cmake_minimum_required|find_package|if|else|elseif|endif|function|endfunction|include|install|project|set|target_compile_features|target_include_directories|target_link_libraries)\b)"),
              theme.command, QFont::DemiBold);
      addRule(QStringLiteral(R"(\$\{[A-Za-z_][A-Za-z0-9_]*\})"),
              theme.number);
      addRule(QStringLiteral(R"(#.*$)"), theme.comment, QFont::Normal, true);
    } else {
      // Plain text gets no keyword rule: bolding English "if" and "for" in
      // prose reads as a glitch, not highlighting.
      addRule(QStringLiteral(R"((?:^|\s)(?:#|//).*$)"), theme.comment,
              QFont::Normal, true);
    }
  }

protected:
  void highlightBlock(const QString &text) override {
    QVector<QPair<int, int>> stringSpans;
    QVector<QPair<int, int>> claimedSpans;
    for (const Rule &rule : rules_) {
      int offset = 0;
      while (offset <= text.size()) {
        const QRegularExpressionMatch match = rule.pattern.match(text, offset);
        if (!match.hasMatch())
          break;
        const int start = match.capturedStart();
        const int length = std::max(1, static_cast<int>(match.capturedLength()));
        if (rule.scope == RuleScope::SkipsStrings) {
          // Only a match STARTING inside a literal is blocked; a comment
          // that begins outside overpaints the literal, editor-style.
          const auto blocked =
              std::find_if(stringSpans.cbegin(), stringSpans.cend(),
                           [&](const QPair<int, int> &span) {
                             return start >= span.first &&
                                    start < span.second;
                           });
          if (blocked != stringSpans.cend()) {
            offset = blocked->second;
            continue;
          }
        }
        setFormat(start, length, rule.format);
        if (rule.scope == RuleScope::ClaimsStrings) {
          stringSpans.append({start, start + length});
          claimedSpans.append({start, start + length});
        }
        if (rule.toEnd) {
          claimedSpans.append({start, static_cast<int>(text.size())});
          break;
        }
        offset = start + length;
      }
    }

    // Multi-line constructs carry across blocks: state index + 1 means this
    // block starts inside blockSpans_[index]. An opener strictly inside a
    // string literal or a line comment does not open a span (a co-starting
    // claim is the opener's own quote run, e.g. Python's triple quote).
    const auto claimedInside = [&](int position) {
      return std::any_of(claimedSpans.cbegin(), claimedSpans.cend(),
                         [&](const QPair<int, int> &span) {
                           return position > span.first &&
                                  position < span.second;
                         });
    };
    setCurrentBlockState(0);
    int active = previousBlockState() - 1;
    int searchFrom = 0;
    while (true) {
      int start = 0;
      if (active < 0) {
        start = -1;
        for (int index = 0; index < blockSpans_.size(); ++index) {
          int found = text.indexOf(blockSpans_[index].open, searchFrom);
          while (found >= 0 && claimedInside(found))
            found = text.indexOf(blockSpans_[index].open,
                                 found + static_cast<int>(
                                             blockSpans_[index].open.size()));
          if (found >= 0 && (start < 0 || found < start)) {
            start = found;
            active = index;
          }
        }
        if (active < 0)
          break;
        searchFrom =
            start + static_cast<int>(blockSpans_[active].open.size());
      }
      const int end = text.indexOf(blockSpans_[active].close, searchFrom);
      if (end < 0) {
        setFormat(start, text.size() - start, blockSpans_[active].format);
        setCurrentBlockState(active + 1);
        break;
      }
      const int stop =
          end + static_cast<int>(blockSpans_[active].close.size());
      setFormat(start, stop - start, blockSpans_[active].format);
      searchFrom = stop;
      active = -1;
    }
  }

private:
  enum class RuleScope { SkipsStrings, ClaimsStrings, OverStrings };

  struct Rule {
    QRegularExpression pattern;
    QTextCharFormat format;
    bool toEnd = false;
    RuleScope scope = RuleScope::SkipsStrings;
  };

  struct BlockSpan {
    QString open;
    QString close;
    QTextCharFormat format;
  };

  void addRule(const QString &pattern, const QColor &color,
               QFont::Weight weight = QFont::Normal, bool toEnd = false,
               RuleScope scope = RuleScope::SkipsStrings) {
    // A highlighter is rebuilt per preview render; compiled patterns are
    // shared across instances. GUI-thread only.
    static QHash<QString, QRegularExpression> compiled;
    auto entry = compiled.constFind(pattern);
    if (entry == compiled.cend())
      entry = compiled.insert(pattern, QRegularExpression(pattern));
    QTextCharFormat format;
    format.setForeground(color);
    format.setFontWeight(weight);
    rules_.push_back({*entry, format, toEnd, scope});
  }

  void addBlockSpan(const QString &open, const QString &close,
                    const QColor &color) {
    QTextCharFormat format;
    format.setForeground(color);
    blockSpans_.push_back({open, close, format});
  }

  QVector<Rule> rules_;
  QVector<BlockSpan> blockSpans_;
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

QColor jsonThemeColor(const QJsonValue &value) {
  const QString valueText =
      value.isObject()
          ? value.toObject().value(QStringLiteral("foreground")).toString()
          : value.toString();
  const QColor color(valueText);
  return color.isValid() ? color : QColor{};
}

QHash<QString, QColor> readOmarchySyntaxColors(const QString &path) {
  QHash<QString, QColor> colors;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    return colors;
  const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
  if (!document.isObject())
    return colors;
  const QJsonObject root = document.object();
  const QJsonObject semanticColors =
      root.value(QStringLiteral("semanticTokenColors")).toObject();
  for (auto color = semanticColors.constBegin();
       color != semanticColors.constEnd(); ++color) {
    const QColor parsed = jsonThemeColor(color.value());
    if (parsed.isValid())
      colors.insert(color.key(), parsed);
  }
  const QJsonObject editorColors =
      root.value(QStringLiteral("colors")).toObject();
  const QColor editorBackground =
      jsonThemeColor(editorColors.value(QStringLiteral("editor.background")));
  if (editorBackground.isValid())
    colors.insert(QStringLiteral("editor.background"), editorBackground);
  return colors;
}

double relativeLuminance(const QColor &color) {
  const auto linear = [](double channel) {
    return channel <= 0.03928 ? channel / 12.92
                              : std::pow((channel + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) +
         0.0722 * linear(color.blueF());
}

double contrastRatio(const QColor &first, const QColor &second) {
  const double one = relativeLuminance(first);
  const double two = relativeLuminance(second);
  return (std::max(one, two) + 0.05) / (std::min(one, two) + 0.05);
}

/** Nudges a text color toward the panel's opposite pole until it reads.
 * Omarchy mixes colors.toml text colors with vscode-theme.json's
 * editor.background, and nothing guarantees the pairing is legible. */
QColor readableOn(QColor color, const QColor &panel) {
  constexpr double kMinimumContrast = 2.5;
  const QColor pole = relativeLuminance(panel) < 0.18 ? QColor(Qt::white)
                                                      : QColor(Qt::black);
  for (int step = 0;
       step < 20 && contrastRatio(color, panel) < kMinimumContrast; ++step)
    color = QColor::fromRgbF(color.redF() * 0.9 + pole.redF() * 0.1,
                             color.greenF() * 0.9 + pole.greenF() * 0.1,
                             color.blueF() * 0.9 + pole.blueF() * 0.1);
  // Snapped to 8-bit so painted pixels compare equal to the theme color.
  return QColor(color.red(), color.green(), color.blue());
}

QColor semanticColor(const QHash<QString, QColor> &colors,
                     const QStringList &keys, const QColor &fallback) {
  for (const QString &key : keys) {
    const auto match = colors.constFind(key);
    if (match != colors.cend() && match->isValid())
      return *match;
  }
  return fallback;
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

// The full language roster: filename detection and the card's default
// extension both read from this one table.
struct LanguageProfile {
  const char *language;
  const char *primaryExtension;
  const char *extensions[8];
  const char *filenames[4];
};

constexpr LanguageProfile kLanguageProfiles[] = {
    {"CMake", "cmake", {"cmake"}, {"cmakelists.txt"}},
    {"Shell", "sh", {"sh", "bash", "zsh", "fish"}, {"dockerfile", "pkgbuild"}},
    {"C++", "cpp", {"c", "cc", "cpp", "cxx", "h", "hh", "hpp"}, {}},
    {"JavaScript", "js", {"js", "jsx", "mjs", "cjs"}, {}},
    {"TypeScript", "ts", {"ts", "tsx"}, {}},
    {"Python", "py", {"py"}, {}},
    {"Ruby", "rb", {"rb"}, {"gemfile", "rakefile"}},
    {"QML", "qml", {"qml"}, {}},
    {"Rust", "rs", {"rs"}, {}},
    {"Go", "go", {"go"}, {}},
    {"JSON", "json", {"json"}, {}},
    {"YAML", "yaml", {"yaml", "yml"}, {}},
    {"TOML", "toml", {"toml"}, {}},
    {"CSS", "css", {"css"}, {}},
    {"SCSS", "scss", {"scss"}, {}},
    {"Sass", "sass", {"sass"}, {}},
    {"Markdown", "md", {"md", "markdown"}, {}},
    {"Lua", "lua", {"lua"}, {}},
};

QString languageFromFilename(const QString &filename) {
  const QFileInfo file(filename.trimmed());
  const QString name = file.fileName().toLower();
  const QString suffix = file.suffix().toLower();
  for (const LanguageProfile &profile : kLanguageProfiles) {
    for (const char *special : profile.filenames) {
      if (!special)
        break;
      if (name == QLatin1StringView(special))
        return QString::fromLatin1(profile.language);
    }
    for (const char *extension : profile.extensions) {
      if (!extension)
        break;
      if (suffix == QLatin1StringView(extension))
        return QString::fromLatin1(profile.language);
    }
  }
  return {};
}

QString extensionForLanguage(const QString &language) {
  for (const LanguageProfile &profile : kLanguageProfiles) {
    if (language == QLatin1StringView(profile.language))
      return QString::fromLatin1(profile.primaryExtension);
  }
  return QStringLiteral("txt");
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

/** Angled statusline segment spanning [left, right]; arrows extend past it. */
QPainterPath powerlineSegment(const QRect &status, int left, int right,
                              bool arrowLeft, bool arrowRight) {
  const qreal bottom = status.bottom() + 1.0;
  const qreal middle = status.center().y() + 0.5;
  QPainterPath segment;
  if (arrowLeft) {
    segment.moveTo(left - kPowerlineAngle, middle);
    segment.lineTo(left, status.top());
  } else {
    segment.moveTo(left, status.top());
  }
  segment.lineTo(right, status.top());
  if (arrowRight)
    segment.lineTo(right + kPowerlineAngle, middle);
  segment.lineTo(right, bottom);
  segment.lineTo(left, bottom);
  segment.closeSubpath();
  return segment;
}

void prepareTextDocument(QTextDocument &document, const QString &snippet,
                         const TextCardTheme &theme, int textWidth,
                         const QString &language, bool withHighlighter) {
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
  if (withHighlighter) {
    auto *highlighter = new TextCardHighlighter(&document, theme, language);
    highlighter->rehighlight();
  }
}
} // namespace

QString detectTextCardLanguage(const QString &text, const QString &filename) {
  const QString fromFilename = languageFromFilename(filename);
  if (!fromFilename.isEmpty())
    return fromFilename;

  const QString trimmed = text.trimmed();
  if (trimmed.startsWith(QStringLiteral("#!"))) {
    const QString firstLine = trimmed.section('\n', 0, 0).toLower();
    if (firstLine.contains(QStringLiteral("python")))
      return QStringLiteral("Python");
    if (firstLine.contains(QStringLiteral("ruby")))
      return QStringLiteral("Ruby");
    return QStringLiteral("Shell");
  }
  static const QRegularExpression jsonKey(QStringLiteral(R"("[^"\n]+"\s*:)"));
  if ((trimmed.startsWith(QLatin1Char('{')) ||
       trimmed.startsWith(QLatin1Char('['))) &&
      jsonKey.match(trimmed).hasMatch())
    return QStringLiteral("JSON");
  static const QRegularExpression cppMarks(
      QStringLiteral(R"((?:^|\n)\s*(?:#include\b|using\s+namespace\b|std::))"));
  if (cppMarks.match(text).hasMatch())
    return QStringLiteral("C++");
  static const QRegularExpression rustMarks(
      QStringLiteral(R"((?:^|\n)\s*(?:fn\s+main\b|let\s+mut\b|use\s+\w+::))"));
  if (rustMarks.match(text).hasMatch())
    return QStringLiteral("Rust");
  static const QRegularExpression goMarks(
      QStringLiteral(R"((?:^|\n)\s*(?:package\s+main\b|func\s+\w+\s*\())"));
  if (goMarks.match(text).hasMatch())
    return QStringLiteral("Go");
  static const QRegularExpression qmlMarks(QStringLiteral(
      R"((?:^|\n)\s*import\s+Qt\w+|(?:^|\n)\s*(?:ApplicationWindow|Item|Rectangle)\s*\{)"));
  if (qmlMarks.match(text).hasMatch())
    return QStringLiteral("QML");
  // A Python def always carries its colon; a Ruby def never does. Checking
  // Python first keeps print(..., end="") away from the Ruby rule below.
  static const QRegularExpression pythonMarks(QStringLiteral(
      R"((?:^|\n)\s*(?:def\s+\w+[^\n]*:|from\s+\w+\s+import\b|import\s+\w+))"));
  if (pythonMarks.match(text).hasMatch())
    return QStringLiteral("Python");
  // Two anchored probes, not one bridging pattern: a [\s\S]* bridge
  // backtracks quadratically on def-heavy text with no closing "end".
  static const QRegularExpression rubyRequire(
      QStringLiteral(R"((?:^|\n)\s*require(?:_relative)?\s+['"])"));
  static const QRegularExpression rubyDefinition(
      QStringLiteral(R"((?:^|\n)\s*(?:class|module|def)\s+\w+)"));
  static const QRegularExpression rubyEnd(
      QStringLiteral(R"((?:^|\n)\s*end\b)"));
  if (rubyRequire.match(text).hasMatch() ||
      (rubyDefinition.match(text).hasMatch() &&
       rubyEnd.match(text).hasMatch()))
    return QStringLiteral("Ruby");
  static const QRegularExpression shellMarks(QStringLiteral(
      R"((?:^|\n)\s*(?:git|cd|curl|echo|make|omasnap|pacman|sudo)\b)"));
  if (shellMarks.match(text).hasMatch())
    return QStringLiteral("Shell");
  static const QRegularExpression typescriptMarks(
      QStringLiteral(R"((?:^|\n)\s*(?:interface|type)\s+\w+\s*[={])"));
  if (typescriptMarks.match(text).hasMatch())
    return QStringLiteral("TypeScript");
  static const QRegularExpression javascriptMarks(
      QStringLiteral(R"((?:^|\n)\s*(?:const|let|function|import|export)\b)"));
  if (javascriptMarks.match(text).hasMatch())
    return QStringLiteral("JavaScript");
  static const QRegularExpression markdownMarks(
      QStringLiteral(R"((?:^|\n)\s*(?:```|#{1,6}\s+))"));
  if (markdownMarks.match(text).hasMatch())
    return QStringLiteral("Markdown");
  static const QRegularExpression tomlMarks(QStringLiteral(
      R"((?:^|\n)[ \t]*\[[A-Za-z0-9_.-]+\][ \t]*\n[ \t]*[A-Za-z0-9_.-]+[ \t]*=)"));
  if (tomlMarks.match(text).hasMatch())
    return QStringLiteral("TOML");
  // The property probe is bounded so brace-heavy non-CSS text cannot make
  // the wildcard backtrack across the whole snippet.
  static const QRegularExpression cssMarks(QStringLiteral(
      R"((?:^|\n)\s*(?:[.#]?[A-Za-z_-][A-Za-z0-9_ >+~.#:-]*)\s*\{[\s\S]{0,2000}\b[A-Za-z-]+\s*:)"));
  if (cssMarks.match(text).hasMatch())
    return QStringLiteral("CSS");
  // Two consecutive key: lines, like the TOML rule above, so a lone
  // "Note: buy milk" prose line stays plain text.
  static const QRegularExpression yamlMarks(QStringLiteral(
      R"((?:^|\n)[ \t]*[A-Za-z_][\w.-]*[ \t]*:\s*\S[^\n]*\n[ \t]*[A-Za-z_][\w.-]*[ \t]*:)"));
  if (yamlMarks.match(text).hasMatch())
    return QStringLiteral("YAML");
  return QStringLiteral("Plain Text");
}

QString textCardSourceError(const QString &text) {
  QString error;
  static_cast<void>(textCardSnippet(text, error));
  return error;
}

QString defaultTextCardFilename(const QString &text) {
  return QStringLiteral("~/clipboard/snippet.%1")
      .arg(extensionForLanguage(detectTextCardLanguage(text)));
}

TextCardTheme loadTextCardTheme() {
  const QString override =
      qEnvironmentVariable("OMASNAP_TEST_OMARCHY_COLORS").trimmed();
  const QString path =
      override.isEmpty()
          ? QDir::homePath() + QStringLiteral("/.local/state/omarchy/"
                                              "current/theme/colors.toml")
          : override;
  const QHash<QString, QColor> colors = readOmarchyColors(path);
  const QHash<QString, QColor> syntaxColors = readOmarchySyntaxColors(
      QFileInfo(path).dir().filePath(QStringLiteral("vscode-theme.json")));
  const QColor editorBackground =
      readableColor(colors, QStringLiteral("background"),
                    QColor(QStringLiteral("#2e3440")));
  const QColor background =
      readableColor(colors, QStringLiteral("dark_background"),
                    QColor(QStringLiteral("#222730")));
  const QColor panel = semanticColor(
      syntaxColors, {QStringLiteral("editor.background")}, editorBackground);
  const QColor header =
      readableColor(colors, QStringLiteral("darker_background"),
                    QColor(QStringLiteral("#191c23")));
  const QColor accent = readableColor(colors, QStringLiteral("accent"),
                                      QColor(QStringLiteral("#81a1c1")));
  const QColor keyword = readableColor(colors, QStringLiteral("magenta"),
                                       QColor(QStringLiteral("#b48ead")));
  const QColor command = readableColor(colors, QStringLiteral("blue"), accent);
  const QColor flag = readableColor(colors, QStringLiteral("cyan"),
                                    QColor(QStringLiteral("#88c0d0")));
  const QColor number = readableColor(colors, QStringLiteral("orange"),
                                      QColor(QStringLiteral("#d5967a")));
  const QColor string = readableColor(colors, QStringLiteral("yellow"),
                                      QColor(QStringLiteral("#ebcb8b")));
  const QColor comment =
      readableColor(colors, QStringLiteral("dark_foreground"),
                    QColor(QStringLiteral("#667080")));
  TextCardTheme theme{
      currentThemeName(),
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
      semanticColor(syntaxColors, {QStringLiteral("keyword")}, keyword),
      semanticColor(syntaxColors,
                    {QStringLiteral("function"), QStringLiteral("method")},
                    command),
      semanticColor(syntaxColors,
                    {QStringLiteral("property"), QStringLiteral("macro"),
                     QStringLiteral("operator")},
                    flag),
      semanticColor(syntaxColors, {QStringLiteral("number")}, number),
      semanticColor(syntaxColors, {QStringLiteral("string")}, string),
      semanticColor(syntaxColors, {QStringLiteral("comment")}, comment)};
  theme.foreground = readableOn(theme.foreground, theme.panel);
  theme.muted = readableOn(theme.muted, theme.panel);
  theme.keyword = readableOn(theme.keyword, theme.panel);
  theme.command = readableOn(theme.command, theme.panel);
  theme.flag = readableOn(theme.flag, theme.panel);
  theme.number = readableOn(theme.number, theme.panel);
  theme.string = readableOn(theme.string, theme.panel);
  theme.comment = readableOn(theme.comment, theme.panel);
  return theme;
}

QSyntaxHighlighter *installTextCardHighlighter(QTextDocument *document,
                                               const TextCardTheme &theme,
                                               const QString &filename) {
  return new TextCardHighlighter(
      document, theme,
      detectTextCardLanguage(document->toPlainText(), filename));
}

TextCardRender renderTextCardLayout(const QString &text,
                                    const TextCardTheme &theme, QString &error,
                                    bool drawText, const QString &mode,
                                    const QString &filename,
                                    TextCardLayout layout, qreal pixelRatio) {
  error.clear();
  const QString snippet = textCardSnippet(text, error);
  if (snippet.isEmpty())
    return {};

  const QString displayFilename =
      filename.trimmed().isEmpty() ? defaultTextCardFilename(snippet)
                                   : filename.trimmed();
  const QString language =
      detectTextCardLanguage(snippet, displayFilename);

  const int textWidth = kPanelWidth - kHorizontalTextPadding * 2 - kGutterWidth;
  QTextDocument document;
  prepareTextDocument(document, snippet, theme, textWidth, language, drawText);
  const int documentHeight =
      std::max(1, qCeil(document.documentLayout()->documentSize().height()));
  const int editorHeight = std::max(kMinimumEditorHeight, documentHeight);
  const int panelHeight = kHeaderHeight + kTopTextPadding + editorHeight +
                          kBottomTextPadding + kStatusHeight;
  const bool compact = layout == TextCardLayout::Compact;
  const int outputWidth = compact
                              ? kCompactPadding + kPanelWidth +
                                    kPanelShadowOffset + kCompactPadding
                              : kOutputWidth;
  const int outputHeight =
      compact ? kCompactPadding + panelHeight + kPanelShadowOffset +
                    kCompactPadding
              : std::max(kMinimumOutputHeight, panelHeight + 160);

  QImage image(qRound(outputWidth * pixelRatio),
               qRound(outputHeight * pixelRatio),
               QImage::Format_ARGB32_Premultiplied);
  image.setDevicePixelRatio(pixelRatio);
  image.fill(theme.background);
  QPainter painter(&image);
  painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);

  // Hard-edged blocks and an offset frame echo Omarchy's terminal/window
  // chrome without borrowing macOS traffic lights or soft card corners.
  const int panelX = compact ? kCompactPadding
                             : (outputWidth - kPanelWidth) / 2;
  const int panelY = compact ? kCompactPadding
                             : (outputHeight - panelHeight) / 2;
  const QRect panel(panelX, panelY, kPanelWidth, panelHeight);
  painter.fillRect(panel.translated(kPanelShadowOffset, kPanelShadowOffset),
                   theme.header);
  painter.fillRect(panel, theme.panel);
  painter.fillRect(
      QRect(panel.left(), panel.top(), panel.width(), kHeaderHeight),
      theme.header);
  painter.setPen(QPen(theme.muted, 1));
  painter.drawLine(panel.left(), panel.top() + kHeaderHeight, panel.right(),
                   panel.top() + kHeaderHeight);

  QFont headerFont = textCardCodeFont();
  headerFont.setPixelSize(kTextCardHeaderPixelSize);
  painter.setFont(headerFont);
  painter.setPen(theme.outline);
  const QRect headerText(panel.left() + 24, panel.top(), panel.width() - 48,
                         kHeaderHeight);
  painter.drawText(headerText, Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("NVIM"));
  const QRect titleRect = headerText.adjusted(58, 0, -190, 0);
  painter.setPen(theme.foreground);
  painter.drawText(
      titleRect, Qt::AlignLeft | Qt::AlignVCenter,
      QFontMetrics(headerFont).elidedText(displayFilename, Qt::ElideMiddle,
                                          titleRect.width()));
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
    // Right edge stays fixed; the box reaches left into the panel padding so
    // three digits (the 120-line cap) draw unclipped at the code pixel size.
    painter.drawText(
        QRectF(editorRect.left() - kGutterWidth - 20, editorRect.top() + top,
               kGutterWidth - 8, codeMetrics.lineSpacing()),
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
  const QString modeText = QStringLiteral(" %1 ").arg(mode);
  const int modeWidth =
      QFontMetrics(headerFont).horizontalAdvance(modeText) + 28;
  const QRect modeRect(status.left(), status.top(), modeWidth, status.height());
  painter.fillPath(
      powerlineSegment(status, status.left(), modeRect.right() + 1, false,
                       true),
      theme.outline);
  painter.setFont(headerFont);
  painter.setPen(theme.outline.lightness() > 130 ? theme.header
                                                 : theme.foreground);
  painter.drawText(modeRect, Qt::AlignCenter, modeText);
  painter.setPen(theme.foreground);
  painter.drawText(status.adjusted(modeWidth + kPowerlineAngle + 18, 0, -24,
                                   0),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QFileInfo(displayFilename).fileName());

  const int formatWidth = 170;
  const int linesWidth = 64;
  const int formatLeft = status.right() - formatWidth + 1;
  const int linesLeft = formatLeft - linesWidth;
  painter.fillPath(powerlineSegment(status, linesLeft, formatLeft, true, false),
                   theme.selection);
  painter.fillPath(
      powerlineSegment(status, formatLeft, status.right() + 1, true, false),
      theme.outline);
  painter.setPen(theme.foreground);
  painter.drawText(QRect(linesLeft, status.top(), linesWidth, status.height()),
                   Qt::AlignCenter, QStringLiteral("%1L").arg(lineNumber - 1));
  painter.setPen(theme.outline.lightness() > 130 ? theme.header
                                                 : theme.foreground);
  painter.drawText(
      QRect(formatLeft, status.top(), formatWidth, status.height()),
      Qt::AlignCenter,
      QStringLiteral("%1  LF").arg(language.toUpper()));
  painter.setPen(QPen(theme.outline, 2));
  painter.setBrush(Qt::NoBrush);
  painter.drawRect(panel.adjusted(1, 1, -1, -1));
  painter.end();
  return {std::move(image), editorRect, titleRect};
}

QSize textCardEditorWindowSize(const QSize &card, const QSize &available) {
  const QSize room = available.isEmpty()
                         ? QSize(1728, 1080)
                         : QSize(qRound(available.width() * 0.9),
                                 qRound(available.height() * 0.9));
  const QSize cardRoom(std::max(1, room.width() - 2 * kCompactWindowSide),
                       std::max(1, room.height() - kCompactWindowTop -
                                       kCompactWindowBottom));
  QSize shown = card;
  if (shown.width() > cardRoom.width() || shown.height() > cardRoom.height())
    shown.scale(cardRoom, Qt::KeepAspectRatio);
  QSize result(shown.width() + 2 * kCompactWindowSide,
               shown.height() + kCompactWindowTop + kCompactWindowBottom);
  result.setWidth(std::min(room.width(), std::max(result.width(), 640)));
  result.setHeight(std::min(room.height(), std::max(result.height(), 420)));
  return result;
}

QImage renderTextCard(const QString &text, QString &error) {
  return renderTextCardLayout(text, loadTextCardTheme(), error).image;
}
