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

namespace {
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
bool runTextCardCheck(const QString &outputRoot, QString &error) {
  qputenv("OMASNAP_TEST_CLIPBOARD_TEXT_ONLY", "1");
  const QString expected = QStringLiteral(
      "const answer = \"hello\";\n# install\nomasnap --version");
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

  QString renderError;
  const QImage card = renderTextCard(text, renderError);
  if (!card.save(outputRoot + QStringLiteral("-clipboard-text-card.png"),
                 "PNG")) {
    error = QStringLiteral("Could not save the clipboard text-card fixture");
    return false;
  }
  const TextCardTheme theme = loadTextCardTheme();
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

  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 320, 240};
  capture.monitor.pixelSize = {320, 240};
  capture.monitor.scale = 1.0;
  capture.source = QImage(320, 240, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#182030")));
  capture.previewSize = capture.source.size();
  CaptureEditor editor(capture, CaptureEditor::CaptureMode::Region);
  editor.setSuppressSnapshots(true);
  editor.resize(640, 480);
  editor.show();
  QApplication::processEvents();
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

  auto *cardEditor = qobject_cast<QPlainTextEdit *>(QApplication::focusWidget());
  if (!cardEditor) {
    error = QStringLiteral("Clipboard text card did not focus its editor");
    return false;
  }
  if (cardEditor->cursorWidth() <= 2) {
    error = QStringLiteral("Clipboard-card Normal mode lost its block cursor");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_F, Qt::ShiftModifier);
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
  if (!editor.grab().save(
          outputRoot + QStringLiteral("-clipboard-text-card-editing.png"),
          "PNG")) {
    error = QStringLiteral("Could not save the live text-card editor fixture");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_Escape);
  const QString edited = expected + QStringLiteral("\n\techo \"done\"");
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
  QTest::keyClick(cardEditor, Qt::Key_V);
  QTest::keyClick(cardEditor, Qt::Key_L);
  if (cardEditor->textCursor().position() != 1 ||
      !editor.statusForTest().contains(QStringLiteral("VISUAL"))) {
    error = QStringLiteral("Clipboard-card v did not visually select text");
    return false;
  }
  QTest::keyClick(cardEditor, Qt::Key_Y);
  if (QGuiApplication::clipboard()->text(QClipboard::Clipboard) !=
          QStringLiteral("co") ||
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
  if (QGuiApplication::clipboard()->text(QClipboard::Clipboard) !=
          firstTwoLines ||
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
      QGuiApplication::clipboard()->text(QClipboard::Clipboard) !=
          QStringLiteral("const answer = \"hello\";\n")) {
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
      !editor.statusForTest().contains(QStringLiteral("rendered"))) {
    error = QStringLiteral(
        "Ctrl+Enter did not flatten the edited clipboard text card: %1")
                .arg(editedRenderError);
    return false;
  }
  editor.close();
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
      "if [[ \"${1:-}\" == \"--no-newline\" && \"${2:-}\" == \"--type\" "
      "&& \"${3:-}\" == text/plain* ]]; then\n"
      "  printf '%s' \"$OMASNAP_TEST_CLIPBOARD_TEXT\"\n"
      "  exit 0\n"
      "fi\n"
      "exit 1\n");
  if (!writeExecutable(fakeWlPaste, script)) {
    error = QStringLiteral("Could not create fake wl-paste command");
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
  });
  qputenv("PATH", directory.path().toUtf8() + ':' + oldPath);
  qputenv("OMASNAP_TEST_CLIPBOARD_IMAGE", imagePath.toUtf8());
  qunsetenv("OMASNAP_TEST_CLIPBOARD_TEXT_ONLY");
  qunsetenv("OMASNAP_TEST_CLIPBOARD_READ_FAILURE");
  qputenv("OMASNAP_TEST_CLIPBOARD_TEXT",
          "const answer = \"hello\";\n# install\nomasnap --version");
  qputenv("OMASNAP_TEST_OMARCHY_COLORS", themePath.toUtf8());
  qputenv("OMASNAP_TEST_OMARCHY_THEME_NAME", "test-theme");

  return runImageCheck(error) && runTextOnlyCheck(error) &&
         runTextCardCheck(outputRoot, error) &&
         runReadFailureCheck(error);
}
