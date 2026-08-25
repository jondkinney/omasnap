/** @fileoverview Tests clipboard image loading without a live compositor. */
#include "clipboard-smoke.hpp"

#include "capture.hpp"
#include "editor.hpp"
#include "text-card.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QLineEdit>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextLayout>

#include <cstdlib>

namespace {
/** Text most recently persisted through the fake wl-copy. */
QString copiedSinkText() {
  QFile file(qEnvironmentVariable("OMASNAP_TEST_CLIPBOARD_SINK"));
  if (!file.open(QIODevice::ReadOnly))
    return {};
  return QString::fromUtf8(file.readAll());
}

/** Writes an executable fake command. */
bool writeExecutable(const QString &path, const QByteArray &contents) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly) ||
      file.write(contents) != contents.size())
    return false;
  file.close();
  return QFile::setPermissions(path, QFileDevice::ReadOwner |
                                         QFileDevice::WriteOwner |
                                         QFileDevice::ExeOwner);
}

/** Checks that an offered PNG is decoded. */
bool runImageCheck(QString &error) {
  QImage image;
  if (!loadClipboardImage(image, error))
    return false;
  if (image.size() != QSize(3, 2) ||
      image.pixelColor(1, 0) != QColor(18, 52, 86) ||
      image.pixelColor(2, 1) != QColor(171, 205, 239)) {
    error = QStringLiteral("Clipboard image pixels were not preserved");
    return false;
  }
  return true;
}

/** Checks that a text-only clipboard reports a clear failure. */
bool runTextOnlyCheck(QString &error) {
  qputenv("OMASNAP_TEST_CLIPBOARD_TEXT_ONLY", "1");
  QImage image(1, 1, QImage::Format_ARGB32);
  QString clipboardError;
  if (loadClipboardImage(image, clipboardError) || !image.isNull() ||
      !clipboardError.contains(QStringLiteral("image"),
                               Qt::CaseInsensitive)) {
    error = QStringLiteral("Text-only clipboard was not rejected clearly");
    return false;
  }
  return true;
}

/** Checks text transfer, styled rendering, and the select-phase shortcut. */
QString expectedCardText() {
  return QStringLiteral(
      "const answer = \"hello\";\n# install\nomasnap --version");
}

QString editedCardText() {
  return expectedCardText() + QStringLiteral("\n\techo \"done\"");
}

/** Language and filename detection over the fake clipboard. */
bool runTextCardDetectionCheck(QString &error) {
  qputenv("OMASNAP_TEST_CLIPBOARD_TEXT_ONLY", "1");
  const QString expected = expectedCardText();
  QString text;
  if (!loadClipboardText(text, error) || text != expected) {
    if (error.isEmpty())
      error = QStringLiteral("Clipboard text was not preserved");
    return false;
  }
  if (detectTextCardLanguage(QStringLiteral("echo hello"),
                             QStringLiteral("main.py")) !=
          QStringLiteral("Python") ||
      detectTextCardLanguage(QStringLiteral("{\"ok\": true}")) !=
          QStringLiteral("JSON") ||
      !defaultTextCardFilename(expected).endsWith(QStringLiteral(".sh"))) {
    error = QStringLiteral("Clipboard-card language detection failed");
    return false;
  }
  const QVector<QPair<QString, QString>> addedProfiles{
      {QStringLiteral("snippet.rb"), QStringLiteral("Ruby")},
      {QStringLiteral("Main.qml"), QStringLiteral("QML")},
      {QStringLiteral("theme.toml"), QStringLiteral("TOML")},
      {QStringLiteral("site.css"), QStringLiteral("CSS")},
      {QStringLiteral("site.scss"), QStringLiteral("SCSS")},
      {QStringLiteral("site.sass"), QStringLiteral("Sass")},
      {QStringLiteral("main.cpp"), QStringLiteral("C++")},
      {QStringLiteral("app.ts"), QStringLiteral("TypeScript")},
      {QStringLiteral("lib.rs"), QStringLiteral("Rust")},
      {QStringLiteral("main.go"), QStringLiteral("Go")},
      {QStringLiteral("ci.yml"), QStringLiteral("YAML")},
      {QStringLiteral("README.md"), QStringLiteral("Markdown")},
      {QStringLiteral("init.lua"), QStringLiteral("Lua")},
      {QStringLiteral("CMakeLists.txt"), QStringLiteral("CMake")},
      {QStringLiteral("Dockerfile"), QStringLiteral("Shell")},
  };
  for (const auto &[filename, language] : addedProfiles) {
    if (detectTextCardLanguage(QStringLiteral("sample"), filename) !=
        language) {
      error = QStringLiteral("Clipboard-card did not detect %1 syntax")
                  .arg(language);
      return false;
    }
  }
  if (detectTextCardLanguage(
          QStringLiteral("def greet\n  puts 'hello'\nend")) !=
          QStringLiteral("Ruby") ||
      detectTextCardLanguage(
          QStringLiteral("import QtQuick\nRectangle { width: 100 }")) !=
          QStringLiteral("QML") ||
      detectTextCardLanguage(
          QStringLiteral("[package]\nname = \"omasnap\"")) !=
          QStringLiteral("TOML") ||
      detectTextCardLanguage(
          QStringLiteral(".card {\n  color: #fff;\n}")) !=
          QStringLiteral("CSS")) {
    error = QStringLiteral(
        "Clipboard-card added content detection was shadowed");
    return false;
  }
  if (detectTextCardLanguage(
          QStringLiteral("#!/usr/bin/env python3\nprint(1)")) !=
          QStringLiteral("Python") ||
      detectTextCardLanguage(
          QStringLiteral("def f():\n    print(\"hi\", end=\"\")")) !=
          QStringLiteral("Python") ||
      detectTextCardLanguage(QStringLiteral("# My Notes\nplain words")) !=
          QStringLiteral("Markdown") ||
      detectTextCardLanguage(QStringLiteral("Note: remember the milk")) !=
          QStringLiteral("Plain Text") ||
      detectTextCardLanguage(QStringLiteral("region: us-east\nreplicas: 3")) !=
          QStringLiteral("YAML")) {
    error = QStringLiteral(
        "Clipboard-card content detection mislabeled a lookalike");
    return false;
  }

  return true;
}

/** Card rendering: theme import, pixels, precedence, gutter. */
bool runTextCardRenderCheck(const QString &outputRoot, QString &error) {
  const QString expected = expectedCardText();
  QString renderError;
  const QImage card = renderTextCard(expected, renderError);
  const TextCardTheme theme = loadTextCardTheme();
  const TextCardRender compact = renderTextCardLayout(
      expected, theme, renderError, false, QStringLiteral("NORMAL"),
      QStringLiteral("snippet.sh"), TextCardLayout::Compact);
  const QSize compactWindow =
      textCardEditorWindowSize(compact.image.size(), QSize(1920, 1080));
  if (compact.image.isNull() || compact.image.width() >= card.width() ||
      compact.image.height() >= card.height() ||
      compactWindow.width() < compact.image.width() ||
      compactWindow.width() > compact.image.width() + 120 ||
      compactWindow.height() < compact.image.height() ||
      compactWindow.height() > compact.image.height() + 120) {
    error = QStringLiteral(
        "Compact clipboard editor did not hug its code card and toolbar");
    return false;
  }
  if (!card.save(outputRoot + QStringLiteral("-clipboard-text-card.png"),
                 "PNG")) {
    error = QStringLiteral("Could not save the clipboard text-card fixture");
    return false;
  }
  if (theme.panel != QColor(QStringLiteral("#1b2433")) ||
      theme.keyword != QColor(QStringLiteral("#d0b4fc")) ||
      theme.command != QColor(QStringLiteral("#9ac0fa")) ||
      theme.flag != QColor(QStringLiteral("#86e0f0")) ||
      theme.number != QColor(QStringLiteral("#f0a468")) ||
      theme.string != QColor(QStringLiteral("#f2d878")) ||
      theme.comment != QColor(QStringLiteral("#64748b"))) {
    error = QStringLiteral(
        "Clipboard text card did not import Omarchy's semantic syntax palette "
        "with colors.toml fallbacks");
    return false;
  }
  int panelPixels = 0;
  int outlinePixels = 0;
  int keywordPixels = 0;
  int commandPixels = 0;
  for (int y = 0; y < card.height(); ++y) {
    for (int x = 0; x < card.width(); ++x) {
      const QColor pixel = card.pixelColor(x, y);
      if (pixel == theme.panel)
        ++panelPixels;
      if (pixel == theme.outline)
        ++outlinePixels;
      if (pixel == theme.keyword)
        ++keywordPixels;
      if (pixel == theme.command)
        ++commandPixels;
    }
  }
  if (card.width() != 1200 || card.height() < 675 ||
      card.pixelColor(0, 0) != theme.background || panelPixels < 10000 ||
      outlinePixels < 1000 || keywordPixels + commandPixels < 3) {
    error = QStringLiteral("Clipboard text card lost its Omarchy background, "
                           "square outline, or syntax highlighting: %1")
                .arg(renderError);
    return false;
  }
  QString crispError;
  const TextCardRender base = renderTextCardLayout(
      expected, theme, crispError, false, QStringLiteral("NORMAL"),
      QStringLiteral("snippet.sh"));
  const TextCardRender crisp = renderTextCardLayout(
      expected, theme, crispError, false, QStringLiteral("NORMAL"),
      QStringLiteral("snippet.sh"), TextCardLayout::Share, 2.0);
  if (crisp.image.size() != base.image.size() * 2 ||
      crisp.editorRect != base.editorRect ||
      crisp.titleRect != base.titleRect) {
    error = QStringLiteral(
        "Supersampled card did not keep logical geometry: %1")
                .arg(crispError);
    return false;
  }
  QString precedenceError;
  const QImage precedenceCard =
      renderTextCardLayout(
          QStringLiteral("const url = \"https://example.com/path\"; // done"),
          theme, precedenceError, true, QStringLiteral("NORMAL"),
          QStringLiteral("test.js"))
          .image;
  int stringPixels = 0;
  int commentPixels = 0;
  int flagPixels = 0;
  for (int y = 0; y < precedenceCard.height(); ++y) {
    for (int x = 0; x < precedenceCard.width(); ++x) {
      const QColor pixel = precedenceCard.pixelColor(x, y);
      if (pixel == theme.string)
        ++stringPixels;
      if (pixel == theme.comment)
        ++commentPixels;
      if (pixel == theme.flag)
        ++flagPixels;
    }
  }
  if (stringPixels < 300 || commentPixels < 50 || flagPixels != 0) {
    error = QStringLiteral(
        "String literals lost precedence over comment/URL rules: "
        "string=%1 comment=%2 flag=%3 %4")
                .arg(stringPixels)
                .arg(commentPixels)
                .arg(flagPixels)
                .arg(precedenceError);
    return false;
  }
  QString blockCommentError;
  const QImage blockCommentCard =
      renderTextCardLayout(
          QStringLiteral("/* first line\nsecond line */\nint x = 1;"), theme,
          blockCommentError, true, QStringLiteral("NORMAL"),
          QStringLiteral("test.cpp"))
          .image;
  int blockCommentPixels = 0;
  int blockKeywordPixels = 0;
  for (int y = 0; y < blockCommentCard.height(); ++y) {
    for (int x = 0; x < blockCommentCard.width(); ++x) {
      const QColor pixel = blockCommentCard.pixelColor(x, y);
      if (pixel == theme.comment)
        ++blockCommentPixels;
      if (pixel == theme.keyword)
        ++blockKeywordPixels;
    }
  }
  if (blockCommentPixels < 400 || blockKeywordPixels < 30) {
    error = QStringLiteral(
        "Multi-line comment did not span blocks: comment=%1 keyword=%2 %3")
                .arg(blockCommentPixels)
                .arg(blockKeywordPixels)
                .arg(blockCommentError);
    return false;
  }
  QString spanGuardError;
  const QImage spanGuardCard =
      renderTextCardLayout(
          QStringLiteral("const char *g = \"/*.cpp\";\nint alive = 1;"),
          theme, spanGuardError, true, QStringLiteral("NORMAL"),
          QStringLiteral("test.cpp"))
          .image;
  int spanCommentPixels = 0;
  int spanKeywordPixels = 0;
  for (int y = 0; y < spanGuardCard.height(); ++y) {
    for (int x = 0; x < spanGuardCard.width(); ++x) {
      const QColor pixel = spanGuardCard.pixelColor(x, y);
      if (pixel == theme.comment)
        ++spanCommentPixels;
      if (pixel == theme.keyword)
        ++spanKeywordPixels;
    }
  }
  if (spanCommentPixels > 50 || spanKeywordPixels < 30) {
    error = QStringLiteral(
        "A block-comment opener inside a string opened a span: comment=%1 "
        "keyword=%2 %3")
                .arg(spanCommentPixels)
                .arg(spanKeywordPixels)
                .arg(spanGuardError);
    return false;
  }
  QString quotedCommentError;
  const QImage quotedCommentCard =
      renderTextCardLayout(
          QStringLiteral("int x = 1; // has \"quoted\" words"), theme,
          quotedCommentError, true, QStringLiteral("NORMAL"),
          QStringLiteral("test.cpp"))
          .image;
  int quotedCommentPixels = 0;
  for (int y = 0; y < quotedCommentCard.height(); ++y) {
    for (int x = 0; x < quotedCommentCard.width(); ++x) {
      if (quotedCommentCard.pixelColor(x, y) == theme.comment)
        ++quotedCommentPixels;
    }
  }
  if (quotedCommentPixels < 300) {
    error = QStringLiteral(
        "A comment containing a quoted word lost its color: comment=%1 %2")
                .arg(quotedCommentPixels)
                .arg(quotedCommentError);
    return false;
  }
  QStringList gutterLines;
  for (int line = 1; line <= 105; ++line)
    gutterLines.append(QStringLiteral("line %1").arg(line));
  QString gutterError;
  const TextCardRender gutterCard = renderTextCardLayout(
      gutterLines.join(QLatin1Char('\n')), theme, gutterError, true,
      QStringLiteral("NORMAL"), QStringLiteral("notes.txt"));
  int hundredsDigitPixels = 0;
  for (int y = 0; y < gutterCard.image.height(); ++y) {
    for (int x = gutterCard.editorRect.left() - 74;
         x <= gutterCard.editorRect.left() - 55; ++x) {
      if (gutterCard.image.pixelColor(x, y) == theme.muted)
        ++hundredsDigitPixels;
    }
  }
  if (hundredsDigitPixels < 20) {
    error = QStringLiteral(
        "Three-digit gutter numbers were clipped: leading pixels=%1 %2")
                .arg(hundredsDigitPixels)
                .arg(gutterError);
    return false;
  }

  return true;
}

/** Blank and oversize clipboards are rejected before a card opens. */
bool runTextCardRejectionCheck(const CaptureData &capture, QString &error) {
  const QString expected = expectedCardText();
  qputenv("OMASNAP_TEST_CLIPBOARD_TEXT", "   \n  ");
  CaptureEditor rejectionEditor(capture, CaptureEditor::CaptureMode::Region);
  rejectionEditor.setSuppressSnapshots(true);
  rejectionEditor.resize(640, 480);
  rejectionEditor.show();
  QApplication::processEvents();
  QTest::keyClick(&rejectionEditor, Qt::Key_V,
                  Qt::ControlModifier | Qt::ShiftModifier);
  if (rejectionEditor.clipboardTextCardEditingForTest() ||
      !rejectionEditor.statusForTest().contains(QStringLiteral("empty"))) {
    error = QStringLiteral(
        "A blank clipboard was not rejected before opening a card");
    return false;
  }
  QStringList overflowLines;
  for (int line = 0; line < 130; ++line)
    overflowLines.append(QStringLiteral("line %1").arg(line));
  qputenv("OMASNAP_TEST_CLIPBOARD_TEXT",
          overflowLines.join(QLatin1Char('\n')).toUtf8());
  QTest::keyClick(&rejectionEditor, Qt::Key_V,
                  Qt::ControlModifier | Qt::ShiftModifier);
  if (rejectionEditor.clipboardTextCardEditingForTest() ||
      !rejectionEditor.statusForTest().contains(QStringLiteral("too long"))) {
    error = QStringLiteral(
        "An oversize clipboard was not rejected before opening a card");
    return false;
  }
  rejectionEditor.close();
  qputenv("OMASNAP_TEST_CLIPBOARD_TEXT", expected.toUtf8());
  return true;
}

/** Ctrl+Shift+V entry, filename editing, and live insert typing. */
bool runTextCardEntryCheck(CaptureEditor &editor,
                           QPlainTextEdit *&cardEditor,
                           const QString &outputRoot, QString &error) {
  const QString expected = expectedCardText();
  const TextCardTheme theme = loadTextCardTheme();
  qputenv("OMASNAP_TEST_CLIPBOARD_TEXT_ONLY", "1");
  qputenv("OMASNAP_TEST_CLIPBOARD_TEXT", expected.toUtf8());
  QTest::keyClick(&editor, Qt::Key_V,
                  Qt::ControlModifier | Qt::ShiftModifier);
  QApplication::processEvents();
  if (!editor.clipboardTextCardEditingForTest() ||
      editor.clipboardTextCardTextForTest() != expected ||
      !editor.statusForTest().contains(QStringLiteral("NORMAL"))) {
    error = QStringLiteral(
        "Ctrl+Shift+V did not open the clipboard text in Normal mode");
    return false;
  }

  cardEditor = qobject_cast<QPlainTextEdit *>(QApplication::focusWidget());
  if (!cardEditor) {
    error = QStringLiteral("Clipboard text card did not focus its editor");
    return false;
  }
  if (cardEditor->extraSelections().isEmpty()) {
    error = QStringLiteral("Clipboard-card Normal mode lost its block cursor");
    return false;
  }
  const QImage frameBeforeFilename = editor.captureData().source;
  QTest::keyClick(cardEditor, Qt::Key_F, Qt::ShiftModifier);
  if (editor.captureData().source == frameBeforeFilename) {
    error = QStringLiteral(
        "The card statusline did not switch to FILENAME while renaming");
    return false;
  }
  auto *filenameEditor =
      qobject_cast<QLineEdit *>(QApplication::focusWidget());
  if (!filenameEditor ||
      editor.clipboardTextCardFilenameForTest() !=
          QStringLiteral("~/clipboard/snippet.sh")) {
    error = QStringLiteral("Clipboard-card F did not open filename editing");
    return false;
  }
  QTest::keyClick(filenameEditor, Qt::Key_A, Qt::ControlModifier);
  QTest::keyClicks(filenameEditor, QStringLiteral("~/share/install.sh"));
  QApplication::processEvents();
  if (!editor.grab().save(
          outputRoot + QStringLiteral("-clipboard-text-card-filename.png"),
          "PNG")) {
    error = QStringLiteral("Could not save the filename-editing fixture");
    return false;
  }
  QTest::keyClick(filenameEditor, Qt::Key_Return);
  QApplication::processEvents();
  if (editor.clipboardTextCardFilenameForTest() !=
          QStringLiteral("~/share/install.sh") ||
      QApplication::focusWidget() != cardEditor ||
      !editor.statusForTest().contains(QStringLiteral("Shell syntax"))) {
    error = QStringLiteral(
        "Clipboard-card filename edit did not update the title or language");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_I);
  QTest::keyClick(cardEditor, Qt::Key_Tab, Qt::ShiftModifier);
  filenameEditor = qobject_cast<QLineEdit *>(QApplication::focusWidget());
  if (!filenameEditor) {
    error = QStringLiteral(
        "Clipboard-card Shift+Tab did not focus the filename editor");
    return false;
  }
  QTest::keyClick(filenameEditor, Qt::Key_A, Qt::ControlModifier);
  QTest::keyClicks(filenameEditor, QStringLiteral("cancelled.py"));
  QTest::keyClick(filenameEditor, Qt::Key_Escape);
  if (editor.clipboardTextCardFilenameForTest() !=
      QStringLiteral("~/share/install.sh")) {
    error = QStringLiteral("Clipboard-card filename Esc did not cancel");
    return false;
  }
  QApplication::processEvents();
  if (!editor.grab().save(
          outputRoot + QStringLiteral("-clipboard-text-card-normal.png"),
          "PNG")) {
    error = QStringLiteral("Could not save the Normal-mode text-card fixture");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_G, Qt::ShiftModifier);
  QTest::keyClick(cardEditor, Qt::Key_A, Qt::ShiftModifier);
  if (!editor.statusForTest().contains(QStringLiteral("INSERT"))) {
    error = QStringLiteral("A did not enter clipboard-card Insert mode");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_Return);
  QTest::keyClick(cardEditor, Qt::Key_Tab);
  QTest::keyClicks(cardEditor, QStringLiteral("echo \"done\""));
  QApplication::processEvents();
  if (editor.clipboardTextCardModeForTest() != QStringLiteral("INSERT")) {
    error = QStringLiteral("Mode accessor did not report INSERT while typing");
    return false;
  }
  const OperationLog liveLog = editor.workingLogForTest();
  if (liveLog.textCardText != editedCardText() ||
      !liveLog.textCardEditing ||
      liveLog.textCardCursor !=
          cardEditor->textCursor().position()) {
    error = QStringLiteral(
        "A handoff written mid-insert would drop the live draft");
    return false;
  }
  bool liveStringColor = false;
  const QList<QTextLayout::FormatRange> liveFormats =
      cardEditor->document()->lastBlock().layout()->formats();
  for (const QTextLayout::FormatRange &range : liveFormats) {
    if (range.format.foreground().color() == theme.string)
      liveStringColor = true;
  }
  if (!liveStringColor) {
    error = QStringLiteral(
        "Insert-mode typing was not syntax-highlighted live");
    return false;
  }
  if (!editor.grab().save(
          outputRoot + QStringLiteral("-clipboard-text-card-editing.png"),
          "PNG")) {
    error = QStringLiteral("Could not save the live text-card editor fixture");
    return false;
  }
  return true;
}

/** Normal-mode motions, operators, registers, and undo marks. */
bool runTextCardNormalModeCheck(CaptureEditor &editor,
                                QPlainTextEdit *cardEditor,
                                QString &error) {
  QTest::keyClick(cardEditor, Qt::Key_Escape);
  const QString edited = editedCardText();
  const int beforeH = cardEditor->textCursor().position();
  QTest::keyClick(cardEditor, Qt::Key_H);
  if (editor.clipboardTextCardTextForTest() != edited ||
      cardEditor->textCursor().position() != beforeH - 1 ||
      !editor.statusForTest().contains(QStringLiteral("NORMAL"))) {
    error = QStringLiteral(
        "Clipboard-card Insert editing or Normal-mode h movement failed: "
        "text=%1, cursor=%2/%3, status=%4")
                .arg(editor.clipboardTextCardTextForTest())
                .arg(cardEditor->textCursor().position())
                .arg(beforeH - 1)
                .arg(editor.statusForTest());
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_X);
  if (editor.clipboardTextCardTextForTest() == edited) {
    error = QStringLiteral("Clipboard-card Normal-mode x did not delete");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  if (editor.clipboardTextCardTextForTest() != edited) {
    error = QStringLiteral("Clipboard-card Normal-mode u did not undo");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClicks(cardEditor, QStringLiteral("$"));
  if (cardEditor->textCursor().position() != 22) {
    error = QStringLiteral(
        "Clipboard-card $ did not rest on the line's last character: %1")
                .arg(cardEditor->textCursor().position());
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_X);
  if (!editor.clipboardTextCardTextForTest().startsWith(
          QStringLiteral("const answer = \"hello\"\n"))) {
    error = QStringLiteral("Clipboard-card $ then x did not delete in place");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_X);
  QTest::keyClick(cardEditor, Qt::Key_P);
  if (!editor.clipboardTextCardTextForTest().startsWith(
          QStringLiteral("ocnst"))) {
    error = QStringLiteral("Clipboard-card xp did not swap characters: %1")
                .arg(editor.clipboardTextCardTextForTest().left(8));
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_U);
  if (editor.clipboardTextCardTextForTest() != edited) {
    error = QStringLiteral("Clipboard-card xp did not undo cleanly");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClicks(cardEditor, QStringLiteral("$"));
  QTest::keyClick(cardEditor, Qt::Key_J);
  QTest::keyClick(cardEditor, Qt::Key_J);
  if (cardEditor->textCursor().position() !=
      cardEditor->document()->findBlockByNumber(2).position() + 16) {
    error = QStringLiteral(
        "Clipboard-card $ then j did not stick to line ends: %1")
                .arg(cardEditor->textCursor().position());
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_K);
  QTest::keyClick(cardEditor, Qt::Key_0);
  QTest::keyClick(cardEditor, Qt::Key_H);
  if (cardEditor->textCursor().position() !=
      cardEditor->document()->findBlockByNumber(1).position()) {
    error = QStringLiteral("Clipboard-card h wrapped across a line start");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_W);
  if (cardEditor->textCursor().position() != 6) {
    error = QStringLiteral("Clipboard-card w did not reach the next word");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_B);
  if (cardEditor->textCursor().position() != 0) {
    error = QStringLiteral("Clipboard-card b did not return to the word start");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_A);
  QTest::keyClicks(cardEditor, QStringLiteral("1"));
  QTest::keyClick(cardEditor, Qt::Key_Escape);
  if (!editor.clipboardTextCardTextForTest().startsWith(
          QStringLiteral("c1onst"))) {
    error = QStringLiteral("Clipboard-card a did not append after the cursor");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_O);
  QTest::keyClicks(cardEditor, QStringLiteral("below"));
  QTest::keyClick(cardEditor, Qt::Key_Escape);
  if (!editor.clipboardTextCardTextForTest().contains(
          QStringLiteral(";\nbelow\n"))) {
    error = QStringLiteral("Clipboard-card o did not open a line below");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_O, Qt::ShiftModifier);
  QTest::keyClicks(cardEditor, QStringLiteral("above"));
  QTest::keyClick(cardEditor, Qt::Key_Escape);
  if (!editor.clipboardTextCardTextForTest().startsWith(
          QStringLiteral("above\n"))) {
    error = QStringLiteral("Clipboard-card O did not open a line above");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_G, Qt::ShiftModifier);
  QTest::keyClick(cardEditor, Qt::Key_I, Qt::ShiftModifier);
  QTest::keyClicks(cardEditor, QStringLiteral("X"));
  QTest::keyClick(cardEditor, Qt::Key_Escape);
  if (!editor.clipboardTextCardTextForTest().contains(
          QStringLiteral("\n\tXecho"))) {
    error = QStringLiteral(
        "Clipboard-card I did not insert at the first non-blank");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_W);
  QTest::keyClick(cardEditor, Qt::Key_D);
  QTest::keyClick(cardEditor, Qt::Key_W);
  if (!editor.clipboardTextCardTextForTest().startsWith(
          QStringLiteral("const = \"hello\";\n"))) {
    error = QStringLiteral("Clipboard-card dw did not delete to next word: %1")
                .arg(editor.clipboardTextCardTextForTest().left(20));
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClicks(cardEditor, QStringLiteral("$"));
  QTest::keyClick(cardEditor, Qt::Key_D);
  QTest::keyClick(cardEditor, Qt::Key_W);
  if (!editor.clipboardTextCardTextForTest().startsWith(
          QStringLiteral("const answer = \"hello\"\n# install"))) {
    error = QStringLiteral("Clipboard-card dw at the line end joined lines");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_W);
  QTest::keyClick(cardEditor, Qt::Key_D, Qt::ShiftModifier);
  if (!editor.clipboardTextCardTextForTest().startsWith(
          QStringLiteral("const \n# install")) ||
      !editor.statusForTest().contains(QStringLiteral("deleted to line end"))) {
    error = QStringLiteral("Clipboard-card D did not delete to the line end");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_Y);
  QTest::keyClick(cardEditor, Qt::Key_W);
  QTest::keyClick(cardEditor, Qt::Key_P, Qt::ShiftModifier);
  if (!editor.clipboardTextCardTextForTest().startsWith(
          QStringLiteral("const const answer"))) {
    error = QStringLiteral("Clipboard-card yw then P did not duplicate a word");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_L);
  QTest::keyClick(cardEditor, Qt::Key_Y);
  QTest::keyClick(cardEditor, Qt::Key_I);
  QTest::keyClick(cardEditor, Qt::Key_W);
  QTest::keyClick(cardEditor, Qt::Key_P, Qt::ShiftModifier);
  if (!editor.clipboardTextCardTextForTest().startsWith(
          QStringLiteral("cconstonst"))) {
    error = QStringLiteral("Clipboard-card yiw did not yank the inner word: %1")
                .arg(editor.clipboardTextCardTextForTest().left(12));
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_D);
  QTest::keyClick(cardEditor, Qt::Key_J);
  if (editor.clipboardTextCardTextForTest() != edited ||
      !editor.statusForTest().contains(
          QStringLiteral("not a supported motion"))) {
    error = QStringLiteral(
        "Clipboard-card unsupported operator motion gave no feedback");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_R);
  QTest::keyClicks(cardEditor, QStringLiteral("Z"));
  if (!editor.clipboardTextCardTextForTest().startsWith(
          QStringLiteral("Zonst")) ||
      cardEditor->textCursor().position() != 0) {
    error = QStringLiteral(
        "Clipboard-card r did not replace in place: %1 cursor=%2")
                .arg(editor.clipboardTextCardTextForTest().left(5))
                .arg(cardEditor->textCursor().position());
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_R);
  QTest::keyClick(cardEditor, Qt::Key_Escape);
  if (editor.clipboardTextCardTextForTest() != edited) {
    error = QStringLiteral("Clipboard-card r after Esc still replaced");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_V);
  QTest::keyClick(cardEditor, Qt::Key_L);
  QTest::keyClick(cardEditor, Qt::Key_L);
  QTest::keyClick(cardEditor, Qt::Key_R);
  QTest::keyClicks(cardEditor, QStringLiteral("*"));
  if (!editor.clipboardTextCardTextForTest().startsWith(
          QStringLiteral("***st")) ||
      !editor.statusForTest().contains(QStringLiteral("NORMAL"))) {
    error = QStringLiteral("Clipboard-card visual r did not fill the selection");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  if (editor.clipboardTextCardTextForTest() != edited) {
    error = QStringLiteral("Clipboard-card r commands did not undo");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_G, Qt::ShiftModifier);
  QTest::keyClicks(cardEditor, QStringLiteral(">>"));
  if (!editor.clipboardTextCardTextForTest().contains(
          QStringLiteral("\n\t\techo"))) {
    error = QStringLiteral("Clipboard-card >> did not indent the line");
    return false;
  }
  QTest::keyClicks(cardEditor, QStringLiteral("<<"));
  QTest::keyClicks(cardEditor, QStringLiteral("<<"));
  if (!editor.clipboardTextCardTextForTest().contains(
          QStringLiteral("\necho \"done\""))) {
    error = QStringLiteral("Clipboard-card << did not outdent the line");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_U);
  if (editor.clipboardTextCardTextForTest() != edited) {
    error = QStringLiteral("Clipboard-card indent commands did not undo");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_V, Qt::ShiftModifier);
  QTest::keyClick(cardEditor, Qt::Key_J);
  QTest::keyClick(cardEditor, Qt::Key_Tab);
  if (!editor.clipboardTextCardTextForTest().startsWith(
          QStringLiteral("\tconst answer = \"hello\";\n\t# install"))) {
    error = QStringLiteral("Visual Tab did not indent the selected lines");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_Tab);
  if (!editor.clipboardTextCardTextForTest().startsWith(
          QStringLiteral("\t\tconst")) ||
      editor.clipboardTextCardModeForTest() !=
          QStringLiteral("VISUAL LINE")) {
    error = QStringLiteral("Visual Tab did not repeat on the kept selection");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_Backtab);
  if (!editor.clipboardTextCardTextForTest().startsWith(
          QStringLiteral("\tconst"))) {
    error = QStringLiteral("Visual Shift+Tab did not outdent");
    return false;
  }
  QTest::keyClicks(cardEditor, QStringLiteral(">>"));
  if (!editor.clipboardTextCardTextForTest().startsWith(
          QStringLiteral("\t\tconst")) ||
      editor.clipboardTextCardModeForTest() !=
          QStringLiteral("VISUAL LINE")) {
    error = QStringLiteral("Visual >> did not indent the kept selection");
    return false;
  }
  QTest::keyClicks(cardEditor, QStringLiteral("<<"));
  if (!editor.clipboardTextCardTextForTest().startsWith(
          QStringLiteral("\tconst"))) {
    error = QStringLiteral("Visual << did not outdent the kept selection");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_Escape);
  for (int undo = 0; undo < 5; ++undo)
    QTest::keyClick(cardEditor, Qt::Key_U);
  if (editor.clipboardTextCardTextForTest() != edited) {
    error = QStringLiteral("Visual indent commands did not undo apart");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_Right);
  QTest::keyClick(cardEditor, Qt::Key_Down);
  const int arrowTarget =
      cardEditor->document()->findBlockByNumber(1).position() + 1;
  if (cardEditor->textCursor().position() != arrowTarget) {
    error = QStringLiteral("Clipboard-card arrows did not move in Normal mode");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_End);
  QTest::keyClick(cardEditor, Qt::Key_Delete);
  if (!editor.clipboardTextCardTextForTest().contains(
          QStringLiteral("\n# instal\n"))) {
    error = QStringLiteral(
        "Clipboard-card End/Delete did not edit like $ and x");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_D);
  QTest::keyClick(cardEditor, Qt::Key_J);
  QTest::keyClick(cardEditor, Qt::Key_Escape);
  if (!editor.statusForTest().contains(QStringLiteral("q exits"))) {
    error = QStringLiteral("Clipboard-card Esc did not flash the Normal hint");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_I);
  QTest::keyClicks(cardEditor, QStringLiteral("ZZ"));
  QTest::keyClick(cardEditor, Qt::Key_Z, Qt::ControlModifier);
  if (!editor.clipboardTextCardTextForTest().startsWith(
          QStringLiteral("ZZconst"))) {
    error = QStringLiteral(
        "Ctrl+Z leaked into the insert session's document undo");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_Escape);
  QTest::keyClick(cardEditor, Qt::Key_U);
  if (editor.clipboardTextCardTextForTest() != edited) {
    error = QStringLiteral("Insert session was not one undo step");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_G, Qt::ShiftModifier);
  QTest::keyClick(cardEditor, Qt::Key_A, Qt::ShiftModifier);
  QTest::keyClicks(cardEditor, QStringLiteral("AA"));
  QTest::keyClick(cardEditor, Qt::Key_Escape);
  QTest::keyClick(cardEditor, Qt::Key_A, Qt::ShiftModifier);
  QTest::keyClicks(cardEditor, QStringLiteral("BB"));
  QTest::keyClick(cardEditor, Qt::Key_Escape);
  QTest::keyClick(cardEditor, Qt::Key_U);
  if (!editor.clipboardTextCardTextForTest().endsWith(QStringLiteral("AA"))) {
    error = QStringLiteral("One u rewound two insert sessions: %1")
                .arg(editor.clipboardTextCardTextForTest().right(8));
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  if (editor.clipboardTextCardTextForTest() != edited) {
    error = QStringLiteral("Adjacent insert sessions did not undo apart");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_Y);
  QTest::keyClick(cardEditor, Qt::Key_Y);
  QTest::keyClick(cardEditor, Qt::Key_O);
  QTest::keyClick(cardEditor, Qt::Key_Escape);
  QTest::keyClick(cardEditor, Qt::Key_C, Qt::ShiftModifier);
  QTest::keyClick(cardEditor, Qt::Key_Escape);
  QTest::keyClick(cardEditor, Qt::Key_P);
  if (cardEditor->document()->findBlockByNumber(2).text() !=
      QStringLiteral("const answer = \"hello\";")) {
    error = QStringLiteral(
        "C on an empty line wiped the yank register: %1")
                .arg(cardEditor->document()->findBlockByNumber(2).text());
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_U);
  if (editor.clipboardTextCardTextForTest() != edited) {
    error = QStringLiteral("Empty-line C sequence did not undo");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_G, Qt::ShiftModifier);
  if (cardEditor->textCursor().position() !=
      cardEditor->document()->lastBlock().position() + 1) {
    error = QStringLiteral(
        "Clipboard-card G did not land on the first non-blank column");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_D);
  QTest::keyClick(cardEditor, Qt::Key_D);
  if (cardEditor->textCursor().position() !=
      cardEditor->document()->lastBlock().position()) {
    error = QStringLiteral(
        "Clipboard-card dd did not land on the next line's first non-blank");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_Y);
  QTest::keyClick(cardEditor, Qt::Key_W);
  QTest::keyClick(cardEditor, Qt::Key_P);
  if (cardEditor->textCursor().position() != 6) {
    error = QStringLiteral(
        "Clipboard-card charwise p did not rest on the last pasted "
        "character: %1")
                .arg(cardEditor->textCursor().position());
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_G, Qt::ShiftModifier);
  QTest::keyClick(cardEditor, Qt::Key_Y);
  QTest::keyClick(cardEditor, Qt::Key_Y);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_P);
  if (cardEditor->textCursor().position() !=
      cardEditor->document()->findBlockByNumber(1).position() + 1) {
    error = QStringLiteral(
        "Clipboard-card linewise p did not land on the first non-blank");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  if (editor.clipboardTextCardTextForTest() != edited) {
    error = QStringLiteral("Clipboard-card put placement tests did not undo");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_Q);
  if (!editor.clipboardTextCardEditingForTest() ||
      !editor.statusForTest().contains(QStringLiteral("q again"))) {
    error = QStringLiteral(
        "Clipboard-card q discarded unsaved edits without asking");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_G, Qt::ShiftModifier);
  QTest::keyClick(cardEditor, Qt::Key_D);
  QTest::keyClick(cardEditor, Qt::Key_D);
  const QString deletedLastLine = QStringLiteral(
      "const answer = \"hello\";\n# install\nomasnap --version");
  if (editor.clipboardTextCardTextForTest() != deletedLastLine) {
    error = QStringLiteral("Clipboard-card Normal-mode dd did not delete");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  if (editor.clipboardTextCardTextForTest() != edited) {
    error = QStringLiteral("Clipboard-card Normal-mode u did not restore dd");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_R, Qt::ControlModifier);
  if (editor.clipboardTextCardTextForTest() != deletedLastLine) {
    error = QStringLiteral("Clipboard-card Ctrl+R did not redo dd");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_P);
  if (editor.clipboardTextCardTextForTest() != edited) {
    error = QStringLiteral("Clipboard-card dd did not populate the put register");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_U);
  if (editor.clipboardTextCardTextForTest() != edited) {
    error = QStringLiteral("Clipboard-card dd/put operations did not undo");
    return false;
  }

  return true;
}

/** Visual selections, objects, puts, and multibyte safety. */
bool runTextCardVisualModeCheck(CaptureEditor &editor,
                                QPlainTextEdit *cardEditor,
                                const QString &outputRoot,
                                QString &error) {
  const QString edited = editedCardText();
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_E);
  if (cardEditor->textCursor().position() != 4) {
    error = QStringLiteral("Clipboard-card e did not reach the current word end");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_E);
  if (cardEditor->textCursor().position() != 11) {
    error = QStringLiteral("Clipboard-card e did not reach the next word end");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_W);
  QTest::keyClick(cardEditor, Qt::Key_V);
  QTest::keyClick(cardEditor, Qt::Key_E);
  QTest::keyClick(cardEditor, Qt::Key_Y);
  if (copiedSinkText() != QStringLiteral("answer") ||
      cardEditor->textCursor().position() != 6) {
    error = QStringLiteral("Clipboard-card Visual e did not select to word end");
    return false;
  }

  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_L);
  QTest::keyClick(cardEditor, Qt::Key_L);
  QTest::keyClick(cardEditor, Qt::Key_D);
  QTest::keyClick(cardEditor, Qt::Key_E);
  const QString deletedThroughEnd = QStringLiteral(
      "co answer = \"hello\";\n# install\nomasnap --version\n\techo \"done\"");
  if (editor.clipboardTextCardTextForTest() != deletedThroughEnd) {
    error = QStringLiteral("Clipboard-card de did not delete through word end");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_W);
  QTest::keyClick(cardEditor, Qt::Key_C);
  QTest::keyClick(cardEditor, Qt::Key_E);
  QTest::keyClicks(cardEditor, QStringLiteral("value"));
  QTest::keyClick(cardEditor, Qt::Key_Escape);
  const QString changedThroughEnd = QStringLiteral(
      "const value = \"hello\";\n# install\nomasnap --version\n\techo "
      "\"done\"");
  if (editor.clipboardTextCardTextForTest() != changedThroughEnd) {
    error = QStringLiteral("Clipboard-card ce did not change through word end");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  if (editor.clipboardTextCardTextForTest() != edited) {
    error = QStringLiteral("Clipboard-card de/ce operations did not undo");
    return false;
  }

  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_D);
  QTest::keyClick(cardEditor, Qt::Key_I);
  QTest::keyClick(cardEditor, Qt::Key_W);
  const QString innerWordDeleted = QStringLiteral(
      " answer = \"hello\";\n# install\nomasnap --version\n\techo \"done\"");
  if (editor.clipboardTextCardTextForTest() != innerWordDeleted) {
    error = QStringLiteral("Clipboard-card diw did not delete the inner word");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_D);
  QTest::keyClick(cardEditor, Qt::Key_A);
  QTest::keyClick(cardEditor, Qt::Key_W);
  const QString aroundWordDeleted = QStringLiteral(
      "answer = \"hello\";\n# install\nomasnap --version\n\techo \"done\"");
  if (editor.clipboardTextCardTextForTest() != aroundWordDeleted) {
    error = QStringLiteral("Clipboard-card daw did not delete around the word");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_C);
  QTest::keyClick(cardEditor, Qt::Key_I);
  QTest::keyClick(cardEditor, Qt::Key_W);
  if (editor.clipboardTextCardTextForTest() != innerWordDeleted ||
      !editor.statusForTest().contains(QStringLiteral("INSERT"))) {
    error = QStringLiteral("Clipboard-card ciw did not change the inner word");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_Escape);
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_W);
  const int secondWordStart = cardEditor->textCursor().position();
  QTest::keyClick(cardEditor, Qt::Key_C);
  QTest::keyClick(cardEditor, Qt::Key_W);
  QTest::keyClicks(cardEditor, QStringLiteral("value"));
  QTest::keyClick(cardEditor, Qt::Key_Escape);
  const QString wordChanged = QStringLiteral(
      "const value = \"hello\";\n# install\nomasnap --version\n\techo "
      "\"done\"");
  if (editor.clipboardTextCardTextForTest() != wordChanged) {
    error = QStringLiteral("Clipboard-card cw did not replace the word");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  if (editor.clipboardTextCardTextForTest() != edited ||
      cardEditor->textCursor().position() != secondWordStart) {
    error = QStringLiteral(
        "Clipboard-card cw was not one undo at the change start");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_R, Qt::ControlModifier);
  if (editor.clipboardTextCardTextForTest() != wordChanged ||
      cardEditor->textCursor().position() != secondWordStart) {
    error = QStringLiteral("Clipboard-card cw redo failed: text=%1 cursor=%2/%3")
                .arg(editor.clipboardTextCardTextForTest())
                .arg(cardEditor->textCursor().position())
                .arg(secondWordStart);
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_L);
  QTest::keyClick(cardEditor, Qt::Key_L);
  const int middleOfWord = cardEditor->textCursor().position();
  QTest::keyClick(cardEditor, Qt::Key_C);
  QTest::keyClick(cardEditor, Qt::Key_W);
  QTest::keyClicks(cardEditor, QStringLiteral("X"));
  QTest::keyClick(cardEditor, Qt::Key_Escape);
  const QString wordSuffixChanged = QStringLiteral(
      "coX answer = \"hello\";\n# install\nomasnap --version\n\techo "
      "\"done\"");
  if (editor.clipboardTextCardTextForTest() != wordSuffixChanged) {
    error = QStringLiteral("Clipboard-card cw did not change from the cursor");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  if (editor.clipboardTextCardTextForTest() != edited ||
      cardEditor->textCursor().position() != middleOfWord) {
    error = QStringLiteral(
        "Clipboard-card mid-word cw undo lost the change start");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_C);
  QTest::keyClick(cardEditor, Qt::Key_A);
  QTest::keyClick(cardEditor, Qt::Key_W);
  if (editor.clipboardTextCardTextForTest() != aroundWordDeleted ||
      !editor.statusForTest().contains(QStringLiteral("INSERT"))) {
    error = QStringLiteral("Clipboard-card caw did not change around the word");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_Escape);
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_W);
  QTest::keyClick(cardEditor, Qt::Key_C, Qt::ShiftModifier);
  const QString changedToEnd = QStringLiteral(
      "const \n# install\nomasnap --version\n\techo \"done\"");
  if (editor.clipboardTextCardTextForTest() != changedToEnd ||
      !editor.statusForTest().contains(QStringLiteral("INSERT"))) {
    error = QStringLiteral("Clipboard-card C did not change to end of line");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_Escape);
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_C);
  QTest::keyClick(cardEditor, Qt::Key_C);
  const QString changedLine = QStringLiteral(
      "\n# install\nomasnap --version\n\techo \"done\"");
  if (editor.clipboardTextCardTextForTest() != changedLine ||
      !editor.statusForTest().contains(QStringLiteral("INSERT"))) {
    error = QStringLiteral("Clipboard-card cc did not change the line");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_Escape);
  QTest::keyClick(cardEditor, Qt::Key_U);
  if (editor.clipboardTextCardTextForTest() != edited) {
    error = QStringLiteral("Clipboard-card change operations did not undo");
    return false;
  }

  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_J, Qt::ShiftModifier);
  const QString joined = QStringLiteral(
      "const answer = \"hello\"; # install\nomasnap --version\n\techo "
      "\"done\"");
  if (editor.clipboardTextCardTextForTest() != joined) {
    error = QStringLiteral("Clipboard-card Shift+J did not join lines: %1")
                .arg(editor.clipboardTextCardTextForTest());
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  if (editor.clipboardTextCardTextForTest() != edited) {
    error = QStringLiteral("Clipboard-card u did not undo Shift+J");
    return false;
  }

  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_L);
  QTest::keyClick(cardEditor, Qt::Key_L);
  QTest::keyClick(cardEditor, Qt::Key_V);
  QTest::keyClick(cardEditor, Qt::Key_I);
  QTest::keyClick(cardEditor, Qt::Key_W);
  QTest::keyClick(cardEditor, Qt::Key_Y);
  if (copiedSinkText() != QStringLiteral("const") ||
      cardEditor->textCursor().position() != 0 ||
      !editor.statusForTest().contains(QStringLiteral("NORMAL"))) {
    error = QStringLiteral(
        "Clipboard-card viw did not select and yank the inner word");
    return false;
  }

  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_V);
  QTest::keyClick(cardEditor, Qt::Key_L);
  if (cardEditor->textCursor().position() != 1 ||
      editor.clipboardTextCardModeForTest() != QStringLiteral("VISUAL") ||
      !editor.statusForTest().contains(QStringLiteral("VISUAL"))) {
    error = QStringLiteral("Clipboard-card v did not visually select text");
    return false;
  }
  const QList<QTextEdit::ExtraSelection> visualHighlight =
      cardEditor->extraSelections();
  if (visualHighlight.isEmpty() ||
      visualHighlight.first().cursor.selectionStart() != 0 ||
      visualHighlight.first().cursor.selectionEnd() != 2) {
    error = QStringLiteral(
        "Visual selection was not highlighted over its span");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_Y);
  if (copiedSinkText() != QStringLiteral("co") ||
      cardEditor->textCursor().position() != 0 ||
      cardEditor->textCursor().hasSelection() ||
      !editor.statusForTest().contains(QStringLiteral("NORMAL"))) {
    error = QStringLiteral(
        "Clipboard-card y did not copy or return to the selection start");
    return false;
  }

  QTest::keyClick(cardEditor, Qt::Key_L);
  QTest::keyClick(cardEditor, Qt::Key_L);
  QTest::keyClick(cardEditor, Qt::Key_V, Qt::ShiftModifier);
  QTest::keyClick(cardEditor, Qt::Key_J);
  const int secondLineColumnTwo =
      cardEditor->document()->findBlockByNumber(1).position() + 2;
  if (cardEditor->textCursor().position() != secondLineColumnTwo) {
    error = QStringLiteral(
        "Clipboard-card V movement did not retain the active column");
    return false;
  }
  QApplication::processEvents();
  if (!editor.grab().save(
          outputRoot + QStringLiteral("-clipboard-text-card-visual-line.png"),
          "PNG")) {
    error = QStringLiteral("Could not save the Visual-line text-card fixture");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_Y);
  const QString firstTwoLines =
      QStringLiteral("const answer = \"hello\";\n# install\n");
  if (copiedSinkText() != firstTwoLines ||
      cardEditor->textCursor().position() != 0) {
    error = QStringLiteral(
        "Clipboard-card V/y did not copy whole lines or restore the cursor");
    return false;
  }

  QTest::keyClick(cardEditor, Qt::Key_P);
  const QString putBelow = QStringLiteral(
      "const answer = \"hello\";\nconst answer = \"hello\";\n# install\n"
      "# install\nomasnap --version\n\techo \"done\"");
  if (editor.clipboardTextCardTextForTest() != putBelow) {
    error = QStringLiteral("Clipboard-card V/y/p did not put below the line");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_P, Qt::ShiftModifier);
  const QString putAbove = QStringLiteral(
      "const answer = \"hello\";\n# install\nconst answer = \"hello\";\n"
      "# install\nomasnap --version\n\techo \"done\"");
  if (editor.clipboardTextCardTextForTest() != putAbove) {
    error = QStringLiteral("Clipboard-card V/y/P did not put above the line");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);

  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_Y);
  QTest::keyClick(cardEditor, Qt::Key_Y);
  QTest::keyClick(cardEditor, Qt::Key_P);
  const QString duplicatedLine = QStringLiteral(
      "const answer = \"hello\";\nconst answer = \"hello\";\n# install\n"
      "omasnap --version\n\techo \"done\"");
  if (editor.clipboardTextCardTextForTest() != duplicatedLine ||
      copiedSinkText() != QStringLiteral("const answer = \"hello\";\n")) {
    error = QStringLiteral("Clipboard-card yyp did not duplicate the line");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  if (editor.clipboardTextCardTextForTest() != edited) {
    error = QStringLiteral("Clipboard-card puts were not individually undoable");
    return false;
  }

  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_V, Qt::ShiftModifier);
  QTest::keyClick(cardEditor, Qt::Key_J);
  QTest::keyClick(cardEditor, Qt::Key_D);
  const QString deletedLines =
      QStringLiteral("omasnap --version\n\techo \"done\"");
  if (editor.clipboardTextCardTextForTest() != deletedLines ||
      cardEditor->textCursor().position() != 0 ||
      !editor.statusForTest().contains(QStringLiteral("selection deleted"))) {
    error = QStringLiteral(
        "Clipboard-card Visual-line d did not delete the selected lines");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_P, Qt::ShiftModifier);
  if (editor.clipboardTextCardTextForTest() != edited) {
    error = QStringLiteral(
        "Clipboard-card P did not restore the deleted linewise selection");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_U);
  if (editor.clipboardTextCardTextForTest() != edited) {
    error = QStringLiteral(
        "Clipboard-card Visual-line deletion was not individually undoable");
    return false;
  }

  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_L);
  QTest::keyClick(cardEditor, Qt::Key_V);
  QTest::keyClick(cardEditor, Qt::Key_L);
  QTest::keyClick(cardEditor, Qt::Key_L);
  QTest::keyClick(cardEditor, Qt::Key_X);
  const QString visualCharactersDeleted =
      QStringLiteral("ct answer = \"hello\";\n# install\nomasnap --version\n"
                     "\techo \"done\"");
  if (editor.clipboardTextCardTextForTest() != visualCharactersDeleted) {
    error = QStringLiteral(
        "Clipboard-card Visual x did not delete selected characters");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_L);
  QTest::keyClick(cardEditor, Qt::Key_V);
  QTest::keyClick(cardEditor, Qt::Key_L);
  QTest::keyClick(cardEditor, Qt::Key_L);
  QTest::keyClick(cardEditor, Qt::Key_D);
  if (editor.clipboardTextCardTextForTest() != visualCharactersDeleted) {
    error = QStringLiteral(
        "Clipboard-card Visual d did not delete selected characters");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_R, Qt::ControlModifier);
  QTest::keyClick(cardEditor, Qt::Key_U);
  if (editor.clipboardTextCardTextForTest() != edited) {
    error = QStringLiteral(
        "Clipboard-card Visual-character delete did not undo and redo");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_L);
  QTest::keyClick(cardEditor, Qt::Key_V);
  QTest::keyClick(cardEditor, Qt::Key_L);
  QTest::keyClick(cardEditor, Qt::Key_L);
  QTest::keyClick(cardEditor, Qt::Key_C, Qt::ShiftModifier);
  if (editor.clipboardTextCardTextForTest() != visualCharactersDeleted ||
      !editor.statusForTest().contains(QStringLiteral("INSERT"))) {
    error = QStringLiteral(
        "Clipboard-card Visual C did not change selected characters");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_Escape);
  QTest::keyClick(cardEditor, Qt::Key_U);
  if (editor.clipboardTextCardTextForTest() != edited) {
    error = QStringLiteral("Clipboard-card Visual C did not undo");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_V, Qt::ShiftModifier);
  QTest::keyClick(cardEditor, Qt::Key_J);
  QTest::keyClick(cardEditor, Qt::Key_C);
  const QString visualLinesChanged = QStringLiteral(
      "\nomasnap --version\n\techo \"done\"");
  if (editor.clipboardTextCardTextForTest() != visualLinesChanged ||
      !editor.statusForTest().contains(QStringLiteral("INSERT"))) {
    error = QStringLiteral("Clipboard-card Visual-line c did not change lines");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_Escape);
  QTest::keyClick(cardEditor, Qt::Key_U);
  if (editor.clipboardTextCardTextForTest() != edited) {
    error = QStringLiteral("Clipboard-card Visual-line c did not undo");
    return false;
  }

  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_V);
  QTest::keyClick(cardEditor, Qt::Key_L);
  QTest::keyClick(cardEditor, Qt::Key_L);
  QTest::keyClick(cardEditor, Qt::Key_O);
  if (cardEditor->textCursor().position() != 0 ||
      !editor.statusForTest().contains(QStringLiteral("VISUAL"))) {
    error = QStringLiteral("Clipboard-card visual o did not swap the ends");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_Escape);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_V);
  QTest::keyClick(cardEditor, Qt::Key_I);
  QTest::keyClick(cardEditor, Qt::Key_W);
  QTest::keyClick(cardEditor, Qt::Key_Y);
  QTest::keyClick(cardEditor, Qt::Key_W);
  QTest::keyClick(cardEditor, Qt::Key_V);
  QTest::keyClick(cardEditor, Qt::Key_E);
  QTest::keyClick(cardEditor, Qt::Key_P);
  if (!editor.clipboardTextCardTextForTest().startsWith(
          QStringLiteral("const const = "))) {
    error = QStringLiteral(
        "Clipboard-card visual p did not replace the selection: %1")
                .arg(editor.clipboardTextCardTextForTest().left(16));
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  for (int step = 0; step < 5; ++step)
    QTest::keyClick(cardEditor, Qt::Key_L);
  QTest::keyClick(cardEditor, Qt::Key_D);
  QTest::keyClick(cardEditor, Qt::Key_A);
  QTest::keyClick(cardEditor, Qt::Key_W);
  if (!editor.clipboardTextCardTextForTest().startsWith(
          QStringLiteral("const = "))) {
    error = QStringLiteral(
        "Clipboard-card daw from whitespace missed the following word");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_U);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClicks(cardEditor, QStringLiteral("$"));
  QTest::keyClick(cardEditor, Qt::Key_V);
  QTest::keyClick(cardEditor, Qt::Key_W);
  if (cardEditor->textCursor().position() !=
      cardEditor->document()->findBlockByNumber(1).position()) {
    error = QStringLiteral(
        "Clipboard-card visual w stopped on the line separator");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_Escape);
  cardEditor->setPlainText(QStringLiteral("🙂🙃 hi"));
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_E);
  if (cardEditor->textCursor().position() != 2) {
    error = QStringLiteral(
        "Clipboard-card e split a surrogate pair: cursor=%1")
                .arg(cardEditor->textCursor().position());
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_X);
  if (editor.clipboardTextCardTextForTest() != QStringLiteral("🙂 hi")) {
    error = QStringLiteral("Clipboard-card x left half an emoji behind");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_R);
  QTest::keyClicks(cardEditor, QStringLiteral("z"));
  if (editor.clipboardTextCardTextForTest() != QStringLiteral("z hi")) {
    error = QStringLiteral("Clipboard-card r split a surrogate pair: %1")
                .arg(editor.clipboardTextCardTextForTest());
    return false;
  }
  cardEditor->setPlainText(QStringLiteral("🙂🙃 hi"));
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_D);
  QTest::keyClick(cardEditor, Qt::Key_E);
  if (editor.clipboardTextCardTextForTest() != QStringLiteral(" hi")) {
    error = QStringLiteral("Clipboard-card de split a surrogate pair: %1")
                .arg(editor.clipboardTextCardTextForTest());
    return false;
  }
  cardEditor->setPlainText(edited);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_V);
  QTest::keyClick(cardEditor, Qt::Key_G, Qt::ShiftModifier);
  QTest::keyClick(cardEditor, Qt::Key_D);
  if (!editor.clipboardTextCardTextForTest().isEmpty()) {
    error = QStringLiteral("Clipboard-card Visual delete did not empty source");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_D);
  QTest::keyClick(cardEditor, Qt::Key_D);
  QTest::keyClick(cardEditor, Qt::Key_U);
  if (editor.clipboardTextCardTextForTest() != edited) {
    error = QStringLiteral(
        "Clipboard-card dd on an empty card left a phantom undo mark");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_G);
  QTest::keyClick(cardEditor, Qt::Key_V);
  QTest::keyClick(cardEditor, Qt::Key_G, Qt::ShiftModifier);
  QTest::keyClick(cardEditor, Qt::Key_D);
  if (!editor.clipboardTextCardTextForTest().isEmpty()) {
    error = QStringLiteral("Clipboard-card could not re-empty its source");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_Tab, Qt::ShiftModifier);
  QLineEdit *filenameEditor =
      qobject_cast<QLineEdit *>(QApplication::focusWidget());
  if (!filenameEditor) {
    error = QStringLiteral(
        "Clipboard-card empty source could not focus filename editing");
    return false;
  }
  QTest::keyClick(filenameEditor, Qt::Key_A, Qt::ControlModifier);
  QTest::keyClicks(filenameEditor, QStringLiteral("text.rb"));
  if (editor.clipboardTextCardLanguageForTest() != QStringLiteral("Ruby")) {
    error = QStringLiteral(
        "Clipboard-card empty source did not apply its filename language live");
    return false;
  }
  QTest::keyClick(filenameEditor, Qt::Key_Tab);
  QTest::keyClick(cardEditor, Qt::Key_I);
  QTest::keyClicks(cardEditor, QStringLiteral("def greet"));
  QTest::keyClick(cardEditor, Qt::Key_Return);
  QTest::keyClicks(cardEditor, QStringLiteral("end"));
  QApplication::processEvents();
  if (editor.clipboardTextCardTextForTest() !=
          QStringLiteral("def greet\nend") ||
      editor.clipboardTextCardLanguageForTest() != QStringLiteral("Ruby")) {
    error = QStringLiteral(
        "Clipboard-card Ruby profile did not remain active while typing");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_Escape);
  cardEditor->setPlainText(edited);
  QTest::keyClick(cardEditor, Qt::Key_Tab, Qt::ShiftModifier);
  filenameEditor = qobject_cast<QLineEdit *>(QApplication::focusWidget());
  if (!filenameEditor) {
    error = QStringLiteral(
        "Clipboard-card could not restore its filename after language test");
    return false;
  }
  QTest::keyClick(filenameEditor, Qt::Key_A, Qt::ControlModifier);
  QTest::keyClicks(filenameEditor, QStringLiteral("~/share/install.sh"));
  QTest::keyClick(filenameEditor, Qt::Key_Tab);
  if (editor.clipboardTextCardTextForTest() != edited ||
      editor.clipboardTextCardLanguageForTest() != QStringLiteral("Shell")) {
    error = QStringLiteral(
        "Clipboard-card source did not restore after live language test");
    return false;
  }
  return true;
}

/** Render, annotate, Ctrl+E confirm/reopen, and the way back out. */
bool runTextCardRenderRoundTripCheck(CaptureEditor &editor,
                                     QPlainTextEdit *cardEditor,
                                     QString &error) {
  const QString edited = editedCardText();
  QTest::keyClick(cardEditor, Qt::Key_Return, Qt::ControlModifier);
  QApplication::processEvents();
  QString editedRenderError;
  const QImage editedCard =
      renderTextCardLayout(edited, loadTextCardTheme(), editedRenderError, true,
                           QStringLiteral("NORMAL"),
                           QStringLiteral("~/share/install.sh"))
          .image;
  if (editor.clipboardTextCardEditingForTest() ||
      editor.captureData().source != editedCard ||
      editor.currentSelection() !=
          QRectF(QPointF(), QSizeF(editedCard.size())) ||
      editor.renderCurrentOutput() != editedCard ||
      !editor.statusForTest().contains(QStringLiteral("rendered")) ||
      !editor.clipboardTextCardSourceRetainedForTest()) {
    error = QStringLiteral(
        "Ctrl+Enter did not flatten the edited clipboard text card: %1")
                .arg(editedRenderError);
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_E, Qt::ControlModifier);
  QApplication::processEvents();
  if (!editor.clipboardTextCardEditingForTest() ||
      editor.clipboardTextCardTextForTest() != edited ||
      editor.clipboardTextCardFilenameForTest() !=
          QStringLiteral("~/share/install.sh")) {
    error = QStringLiteral(
        "Ctrl+E did not reopen the rendered text-card source and filename");
    return false;
  }
  auto *reopenedEditor =
      qobject_cast<QPlainTextEdit *>(QApplication::focusWidget());
  if (!reopenedEditor) {
    error = QStringLiteral("Reopened text card did not focus its source");
    return false;
  }
  const QRectF doneButton = editor.textCardDoneButtonRectForTest();
  if (doneButton.isEmpty()) {
    error = QStringLiteral("Live text card lost its Done toolbar action");
    return false;
  }
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                    doneButton.center().toPoint());
  QApplication::processEvents();
  if (editor.clipboardTextCardEditingForTest() ||
      editor.captureData().source != editedCard) {
    error = QStringLiteral("Reopened text card did not render consistently");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_A);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(200, 200));
  QTest::mouseMove(&editor, QPoint(300, 300));
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(300, 300));
  QApplication::processEvents();
  if (editor.operationLog().isEmpty()) {
    error = QStringLiteral("Arrow drag on the rendered card made no operation");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_E, Qt::ControlModifier);
  if (editor.clipboardTextCardEditingForTest() ||
      !editor.statusForTest().contains(QStringLiteral("Ctrl+E again"))) {
    error = QStringLiteral("Ctrl+E discarded card annotations without asking");
    return false;
  }
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(220, 220));
  QTest::mouseMove(&editor, QPoint(320, 320));
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(320, 320));
  QApplication::processEvents();
  QTest::keyClick(&editor, Qt::Key_E, Qt::ControlModifier);
  if (editor.clipboardTextCardEditingForTest() ||
      !editor.statusForTest().contains(QStringLiteral("Ctrl+E again"))) {
    error = QStringLiteral(
        "New mouse annotations did not re-arm the Ctrl+E confirmation");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_E, Qt::ControlModifier);
  QApplication::processEvents();
  if (!editor.clipboardTextCardEditingForTest() ||
      editor.clipboardTextCardTextForTest() != edited) {
    error = QStringLiteral("Second Ctrl+E did not reopen the card source");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_C);
  QTest::keyClick(cardEditor, Qt::Key_I);
  QTest::keyClick(cardEditor, Qt::Key_W);
  QTest::keyClicks(cardEditor, QStringLiteral("fixed"));
  QTest::keyClick(cardEditor, Qt::Key_Escape);
  QTest::keyClick(cardEditor, Qt::Key_Return, Qt::ControlModifier);
  QApplication::processEvents();
  if (editor.clipboardTextCardEditingForTest() ||
      !editor.clipboardTextCardSourceRetainedForTest()) {
    error = QStringLiteral("Card re-render after annotation discard failed");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_E, Qt::ControlModifier);
  QApplication::processEvents();
  if (!editor.clipboardTextCardTextForTest().startsWith(
          QStringLiteral("fixed "))) {
    error = QStringLiteral(
        "A re-edited card did not retain its new source: %1")
                .arg(editor.clipboardTextCardTextForTest().left(10));
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_Return, Qt::ControlModifier);
  QApplication::processEvents();
  const QRectF regionTab =
      editor.selectTabRectForTest(QStringLiteral("REGION"));
  if (regionTab.isEmpty()) {
    error = QStringLiteral("Edit phase lost its way back to the select tabs");
    return false;
  }
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                    regionTab.center().toPoint());
  QApplication::processEvents();
  if (editor.clipboardTextCardSourceRetainedForTest()) {
    error = QStringLiteral(
        "Returning to select kept the stale card source armed");
    return false;
  }
  editor.close();
  return true;
}

/** A window handoff restores the draft, its cursor, and register. */
bool runTextCardHandoffCheck(const CaptureData &capture,
                             const QString &outputRoot,
                             QString &error) {
  const QString edited = editedCardText();
  QString editedRenderError;
  const QImage editedCard =
      renderTextCardLayout(edited, loadTextCardTheme(), editedRenderError, true,
                           QStringLiteral("NORMAL"),
                           QStringLiteral("~/share/install.sh"))
          .image;
  CaptureData handoffCapture;
  handoffCapture.monitor = capture.monitor;
  handoffCapture.source = editedCard;
  handoffCapture.previewSize = editedCard.size();
  OperationLog handoffLog;
  handoffLog.previewSize = editedCard.size();
  handoffLog.textCardText = edited;
  handoffLog.textCardFilename = QStringLiteral("~/share/install.sh");
  handoffLog.textCardEditing = true;
  handoffLog.textCardCursor = 5;
  handoffLog.textCardYank = QStringLiteral("zap");
  CaptureEditor handedOff(std::move(handoffCapture),
                          CaptureEditor::CaptureMode::File,
                          QuickOutputMode::None, std::move(handoffLog));
  handedOff.setSuppressSnapshots(true);
  handedOff.setWindowedPresentation(true);
  handedOff.show();
  QApplication::processEvents();
  QString compactError;
  const QImage compactEdited =
      renderTextCardLayout(edited, loadTextCardTheme(), compactError, false,
                           QStringLiteral("NORMAL"),
                           QStringLiteral("~/share/install.sh"),
                           TextCardLayout::Compact)
          .image;
  if (!handedOff.clipboardTextCardEditingForTest() ||
      handedOff.clipboardTextCardTextForTest() != edited ||
      handedOff.clipboardTextCardFilenameForTest() !=
          QStringLiteral("~/share/install.sh") ||
      handedOff.captureData().source != compactEdited ||
      handedOff.naturalWindowSize(QSize(1920, 1080)) !=
          textCardEditorWindowSize(compactEdited.size(), QSize(1920, 1080))) {
    error = QStringLiteral(
        "Window handoff did not restore the compact live text-card document: "
        "%1")
                .arg(compactError);
    return false;
  }
  if (!handedOff.grab().save(
          outputRoot + QStringLiteral("-clipboard-text-card-window.png"),
          "PNG")) {
    error = QStringLiteral("Could not save the compact text-card fixture");
    return false;
  }
  auto *compactEditor =
      qobject_cast<QPlainTextEdit *>(QApplication::focusWidget());
  if (!compactEditor) {
    error = QStringLiteral("Compact text card did not focus its editor");
    return false;
  }
  if (compactEditor->textCursor().position() != 5) {
    error = QStringLiteral(
        "Window handoff did not restore the draft cursor: %1")
                .arg(compactEditor->textCursor().position());
    return false;
  }
  QTest::keyClick(compactEditor, Qt::Key_P, Qt::ShiftModifier);
  if (!handedOff.clipboardTextCardTextForTest().startsWith(
          QStringLiteral("constzap "))) {
    error = QStringLiteral(
        "Window handoff did not restore the yank register: %1")
                .arg(handedOff.clipboardTextCardTextForTest().left(12));
    return false;
  }
  QTest::keyClick(compactEditor, Qt::Key_U);
  QTest::keyClick(compactEditor, Qt::Key_G);
  QTest::keyClick(compactEditor, Qt::Key_G);
  QTest::keyClick(compactEditor, Qt::Key_V);
  QTest::keyClick(compactEditor, Qt::Key_G, Qt::ShiftModifier);
  QTest::keyClick(compactEditor, Qt::Key_D);
  QTest::keyClick(compactEditor, Qt::Key_W, Qt::ControlModifier);
  QApplication::processEvents();
  if (!handedOff.isVisible() ||
      !handedOff.statusForTest().contains(QStringLiteral("empty"))) {
    error = QStringLiteral(
        "Ctrl+W handed off a draft that cannot render");
    return false;
  }
  QTest::keyClick(compactEditor, Qt::Key_U);
  QTest::keyClick(compactEditor, Qt::Key_X);
  QTest::keyClick(compactEditor, Qt::Key_Q);
  QApplication::processEvents();
  if (!handedOff.isVisible() ||
      !handedOff.statusForTest().contains(QStringLiteral("q again"))) {
    error = QStringLiteral(
        "Clipboard-card q dropped edits in the compact window without asking");
    return false;
  }
  QTest::keyClick(compactEditor, Qt::Key_Q);
  QApplication::processEvents();
  if (handedOff.isVisible() ||
      handedOff.statusForTest().contains(
          QStringLiteral("Screen capture failed"))) {
    error = QStringLiteral(
        "Clipboard-card q did not exit cleanly without a capture failure");
    return false;
  }
  return true;
}

/** Drives the full clipboard text-card flow section by section. */
bool runTextCardCheck(const QString &outputRoot, QString &error) {
  if (!runTextCardDetectionCheck(error) ||
      !runTextCardRenderCheck(outputRoot, error))
    return false;
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 320, 240};
  capture.monitor.pixelSize = {320, 240};
  capture.monitor.scale = 1.0;
  capture.source = QImage(320, 240, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#182030")));
  capture.previewSize = capture.source.size();
  if (!runTextCardRejectionCheck(capture, error))
    return false;
  CaptureEditor editor(capture, CaptureEditor::CaptureMode::Region);
  editor.setSuppressSnapshots(true);
  editor.resize(640, 480);
  editor.show();
  QApplication::processEvents();
  QPlainTextEdit *cardEditor = nullptr;
  if (!runTextCardEntryCheck(editor, cardEditor, outputRoot, error) ||
      !runTextCardNormalModeCheck(editor, cardEditor, error) ||
      !runTextCardVisualModeCheck(editor, cardEditor, outputRoot,
                                  error) ||
      !runTextCardRenderRoundTripCheck(editor, cardEditor, error))
    return false;
  return runTextCardHandoffCheck(capture, outputRoot, error);
}

/** Checks that illegible theme pairings are corrected on load. */
bool runTextCardThemeGuardCheck(QString &error) {
  QTemporaryDir directory;
  if (!directory.isValid()) {
    error = QStringLiteral("Could not create theme-guard directory");
    return false;
  }
  const QString colorsPath =
      QDir(directory.path()).filePath(QStringLiteral("colors.toml"));
  QFile colorsFile(colorsPath);
  const QByteArray colors = QByteArrayLiteral(
      "background = \"#101820\"\n"
      "foreground = \"#697a8b\"\n"
      "dark_foreground = \"#667788\"\n");
  if (!colorsFile.open(QIODevice::WriteOnly) ||
      colorsFile.write(colors) != colors.size()) {
    error = QStringLiteral("Could not write the theme-guard palette");
    return false;
  }
  colorsFile.close();
  const QString vscodePath =
      QDir(directory.path()).filePath(QStringLiteral("vscode-theme.json"));
  QFile vscodeFile(vscodePath);
  const QByteArray vscode = QByteArrayLiteral(
      "{\n"
      "  \"semanticTokenColors\": { \"string\": \"#667788\" },\n"
      "  \"colors\": { \"editor.background\": \"#667788\" }\n"
      "}\n");
  if (!vscodeFile.open(QIODevice::WriteOnly) ||
      vscodeFile.write(vscode) != vscode.size()) {
    error = QStringLiteral("Could not write the theme-guard code theme");
    return false;
  }
  vscodeFile.close();

  const QByteArray previousColors = qgetenv("OMASNAP_TEST_OMARCHY_COLORS");
  qputenv("OMASNAP_TEST_OMARCHY_COLORS", colorsPath.toUtf8());
  const TextCardTheme guarded = loadTextCardTheme();
  qputenv("OMASNAP_TEST_OMARCHY_COLORS", previousColors);
  const auto readable = [&](const QColor &color) {
    return std::abs(color.lightness() - guarded.panel.lightness()) >= 40;
  };
  if (guarded.string == guarded.panel || !readable(guarded.string) ||
      !readable(guarded.comment) || !readable(guarded.foreground)) {
    error = QStringLiteral(
        "Theme loader kept text colors that vanish into the panel");
    return false;
  }
  return true;
}

/** Checks the hard-coded palette on a machine with no Omarchy theme. */
bool runTextCardThemeFallbackCheck(QString &error) {
  const QByteArray previousColors = qgetenv("OMASNAP_TEST_OMARCHY_COLORS");
  qputenv("OMASNAP_TEST_OMARCHY_COLORS", "/nonexistent/omasnap-theme");
  const TextCardTheme fallback = loadTextCardTheme();
  QString renderError;
  const QImage card =
      renderTextCard(QStringLiteral("echo fallback"), renderError);
  qputenv("OMASNAP_TEST_OMARCHY_COLORS", previousColors);
  if (fallback.background != QColor(QStringLiteral("#222730")) ||
      fallback.panel != QColor(QStringLiteral("#2e3440")) || card.isNull()) {
    error = QStringLiteral(
        "A missing Omarchy theme did not fall back to the default palette: %1")
                .arg(renderError);
    return false;
  }
  return true;
}

/** Checks that a failed image transfer keeps the wl-paste error. */
bool runReadFailureCheck(QString &error) {
  qunsetenv("OMASNAP_TEST_CLIPBOARD_TEXT_ONLY");
  qputenv("OMASNAP_TEST_CLIPBOARD_READ_FAILURE", "1");
  QImage image;
  QString clipboardError;
  if (loadClipboardImage(image, clipboardError) || !image.isNull() ||
      !clipboardError.contains(QStringLiteral("clipboard changed"),
                               Qt::CaseInsensitive)) {
    error = QStringLiteral("Clipboard transfer failure lost its cause");
    return false;
  }
  return true;
}
} // namespace

bool runClipboardSmoke(const QString &outputRoot, QString &error) {
  QTemporaryDir directory;
  if (!directory.isValid()) {
    error = QStringLiteral("Could not create clipboard-test directory");
    return false;
  }

  const QString imagePath =
      QDir(directory.path()).filePath(QStringLiteral("clipboard.png"));
  QImage expected(3, 2, QImage::Format_ARGB32);
  expected.fill(Qt::transparent);
  expected.setPixelColor(1, 0, QColor(18, 52, 86));
  expected.setPixelColor(2, 1, QColor(171, 205, 239));
  if (!expected.save(imagePath, "PNG")) {
    error = QStringLiteral("Could not create clipboard-test image");
    return false;
  }

  const QString fakeWlPaste =
      QDir(directory.path()).filePath(QStringLiteral("wl-paste"));
  const QByteArray script = QByteArrayLiteral(
      "#!/usr/bin/env bash\n"
      "set -euo pipefail\n"
      "if [[ \"${1:-}\" == \"--list-types\" ]]; then\n"
      "  if [[ -n \"${OMASNAP_TEST_CLIPBOARD_TEXT_ONLY:-}\" ]]; then\n"
      "    printf 'text/plain;charset=utf-8\\n'\n"
      "  else\n"
      "    printf 'text/plain;charset=utf-8\\nimage/png\\n'\n"
      "  fi\n"
      "  exit 0\n"
      "fi\n"
      "if [[ \"${1:-}\" == \"--no-newline\" && \"${2:-}\" == \"--type\" "
      "&& \"${3:-}\" == \"image/png\" ]]; then\n"
      "  if [[ -n \"${OMASNAP_TEST_CLIPBOARD_READ_FAILURE:-}\" ]]; then\n"
      "    printf 'clipboard changed before image transfer\\n' >&2\n"
      "    exit 1\n"
      "  fi\n"
      "  cat -- \"$OMASNAP_TEST_CLIPBOARD_IMAGE\"\n"
      "  exit 0\n"
      "fi\n"
      // Once a yank writes the sink, text reads serve it (the wl-copy
      // verify path needs the copied bytes back), shadowing the TEXT env.
      "if [[ \"${1:-}\" == \"--no-newline\" && \"${2:-}\" == \"--type\" "
      "&& \"${3:-}\" == text/plain* ]]; then\n"
      "  if [[ -n \"${OMASNAP_TEST_CLIPBOARD_SINK:-}\" && -f "
      "\"$OMASNAP_TEST_CLIPBOARD_SINK\" ]]; then\n"
      "    cat -- \"$OMASNAP_TEST_CLIPBOARD_SINK\"\n"
      "  else\n"
      "    printf '%s' \"$OMASNAP_TEST_CLIPBOARD_TEXT\"\n"
      "  fi\n"
      "  exit 0\n"
      "fi\n"
      "exit 1\n");
  if (!writeExecutable(fakeWlPaste, script)) {
    error = QStringLiteral("Could not create fake wl-paste command");
    return false;
  }
  const QString fakeWlCopy =
      QDir(directory.path()).filePath(QStringLiteral("wl-copy"));
  if (!writeExecutable(fakeWlCopy,
                       QByteArrayLiteral(
                           "#!/usr/bin/env bash\n"
                           "set -euo pipefail\n"
                           "cat > \"$OMASNAP_TEST_CLIPBOARD_SINK\"\n"))) {
    error = QStringLiteral("Could not create fake wl-copy command");
    return false;
  }

  const QString themePath =
      QDir(directory.path()).filePath(QStringLiteral("colors.toml"));
  QFile themeFile(themePath);
  const QByteArray theme = QByteArrayLiteral(
      "background = \"#101820\"\n"
      "dark_background = \"#111827\"\n"
      "darker_background = \"#080d14\"\n"
      "foreground = \"#e5e7eb\"\n"
      "dark_foreground = \"#64748b\"\n"
      "muted = \"#475569\"\n"
      "selection = \"#334155\"\n"
      "accent = \"#7dd3fc\"\n"
      "magenta = \"#c084fc\"\n"
      "blue = \"#60a5fa\"\n"
      "cyan = \"#67e8f9\"\n"
      "orange = \"#fb923c\"\n"
      "yellow = \"#fde047\"\n");
  if (!themeFile.open(QIODevice::WriteOnly) ||
      themeFile.write(theme) != theme.size()) {
    error = QStringLiteral("Could not create the clipboard-card test theme");
    return false;
  }
  themeFile.close();

  const QString codeThemePath =
      QDir(directory.path()).filePath(QStringLiteral("vscode-theme.json"));
  QFile codeThemeFile(codeThemePath);
  const QByteArray codeTheme = QByteArrayLiteral(
      "{\n"
      "  \"semanticTokenColors\": {\n"
      "    \"keyword\": \"#d0b4fc\",\n"
      "    \"function\": { \"foreground\": \"#9ac0fa\" },\n"
      "    \"property\": \"#86e0f0\",\n"
      "    \"number\": \"#f0a468\",\n"
      "    \"string\": \"#f2d878\"\n"
      "  },\n"
      "  \"colors\": { \"editor.background\": \"#1b2433\" }\n"
      "}\n");
  if (!codeThemeFile.open(QIODevice::WriteOnly) ||
      codeThemeFile.write(codeTheme) != codeTheme.size()) {
    error = QStringLiteral(
        "Could not create the clipboard-card semantic test theme");
    return false;
  }
  codeThemeFile.close();

  const bool pathWasSet = qEnvironmentVariableIsSet("PATH");
  const QByteArray oldPath = qgetenv("PATH");
  const bool imageWasSet =
      qEnvironmentVariableIsSet("OMASNAP_TEST_CLIPBOARD_IMAGE");
  const QByteArray oldImage = qgetenv("OMASNAP_TEST_CLIPBOARD_IMAGE");
  const bool textOnlyWasSet =
      qEnvironmentVariableIsSet("OMASNAP_TEST_CLIPBOARD_TEXT_ONLY");
  const QByteArray oldTextOnly = qgetenv("OMASNAP_TEST_CLIPBOARD_TEXT_ONLY");
  const bool readFailureWasSet =
      qEnvironmentVariableIsSet("OMASNAP_TEST_CLIPBOARD_READ_FAILURE");
  const QByteArray oldReadFailure =
      qgetenv("OMASNAP_TEST_CLIPBOARD_READ_FAILURE");
  const bool clipboardTextWasSet =
      qEnvironmentVariableIsSet("OMASNAP_TEST_CLIPBOARD_TEXT");
  const QByteArray oldClipboardText = qgetenv("OMASNAP_TEST_CLIPBOARD_TEXT");
  const bool colorsWereSet =
      qEnvironmentVariableIsSet("OMASNAP_TEST_OMARCHY_COLORS");
  const QByteArray oldColors = qgetenv("OMASNAP_TEST_OMARCHY_COLORS");
  const bool themeNameWasSet =
      qEnvironmentVariableIsSet("OMASNAP_TEST_OMARCHY_THEME_NAME");
  const QByteArray oldThemeName =
      qgetenv("OMASNAP_TEST_OMARCHY_THEME_NAME");
  const bool sinkWasSet =
      qEnvironmentVariableIsSet("OMASNAP_TEST_CLIPBOARD_SINK");
  const QByteArray oldSink = qgetenv("OMASNAP_TEST_CLIPBOARD_SINK");
  const auto restoreEnvironment = qScopeGuard([=] {
    pathWasSet ? qputenv("PATH", oldPath) : qunsetenv("PATH");
    imageWasSet ? qputenv("OMASNAP_TEST_CLIPBOARD_IMAGE", oldImage)
                : qunsetenv("OMASNAP_TEST_CLIPBOARD_IMAGE");
    textOnlyWasSet
        ? qputenv("OMASNAP_TEST_CLIPBOARD_TEXT_ONLY", oldTextOnly)
        : qunsetenv("OMASNAP_TEST_CLIPBOARD_TEXT_ONLY");
    readFailureWasSet
        ? qputenv("OMASNAP_TEST_CLIPBOARD_READ_FAILURE", oldReadFailure)
        : qunsetenv("OMASNAP_TEST_CLIPBOARD_READ_FAILURE");
    clipboardTextWasSet
        ? qputenv("OMASNAP_TEST_CLIPBOARD_TEXT", oldClipboardText)
        : qunsetenv("OMASNAP_TEST_CLIPBOARD_TEXT");
    colorsWereSet ? qputenv("OMASNAP_TEST_OMARCHY_COLORS", oldColors)
                  : qunsetenv("OMASNAP_TEST_OMARCHY_COLORS");
    themeNameWasSet
        ? qputenv("OMASNAP_TEST_OMARCHY_THEME_NAME", oldThemeName)
        : qunsetenv("OMASNAP_TEST_OMARCHY_THEME_NAME");
    sinkWasSet ? qputenv("OMASNAP_TEST_CLIPBOARD_SINK", oldSink)
               : qunsetenv("OMASNAP_TEST_CLIPBOARD_SINK");
  });
  qputenv("PATH", directory.path().toUtf8() + ':' + oldPath);
  qputenv("OMASNAP_TEST_CLIPBOARD_IMAGE", imagePath.toUtf8());
  qunsetenv("OMASNAP_TEST_CLIPBOARD_TEXT_ONLY");
  qunsetenv("OMASNAP_TEST_CLIPBOARD_READ_FAILURE");
  qputenv("OMASNAP_TEST_CLIPBOARD_TEXT",
          "const answer = \"hello\";\n# install\nomasnap --version");
  qputenv("OMASNAP_TEST_CLIPBOARD_SINK",
          QDir(directory.path())
              .filePath(QStringLiteral("clipboard-copy.txt"))
              .toUtf8());
  qputenv("OMASNAP_TEST_OMARCHY_COLORS", themePath.toUtf8());
  qputenv("OMASNAP_TEST_OMARCHY_THEME_NAME", "test-theme");

  return runImageCheck(error) && runTextOnlyCheck(error) &&
         runTextCardCheck(outputRoot, error) &&
         runTextCardThemeGuardCheck(error) &&
         runTextCardThemeFallbackCheck(error) && runReadFailureCheck(error);
}
