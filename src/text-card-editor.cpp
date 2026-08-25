/** @fileoverview Modal Normal/Insert/Visual editing on the card's source. */
#include "text-card-editor.hpp"

#include "capture.hpp"

#include <QClipboard>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QTextBlock>

#include <algorithm>
#include <utility>

namespace {
/// Vim word classes: 0 keyword, 1 whitespace, 2 punctuation run.
int characterClass(QChar character) {
  if (character.isLetterOrNumber() || character == QLatin1Char('_'))
    return 0;
  if (character.isSpace())
    return 1;
  return 2;
}

/// UTF-16 units one full character occupies at this position.
int characterWidth(const QString &text, int position) {
  return position + 1 < text.size() && text.at(position).isHighSurrogate() &&
                 text.at(position + 1).isLowSurrogate()
             ? 2
             : 1;
}

/// Vim lands line jumps and linewise puts on the first non-blank column.
void moveToFirstNonBlank(QTextCursor &cursor) {
  const QString line = cursor.block().text();
  int first = 0;
  while (first < line.size() && line.at(first).isSpace())
    ++first;
  cursor.setPosition(cursor.block().position() + first);
}

int snapToCharacterStart(const QString &text, int position) {
  if (position > 0 && position < text.size() &&
      text.at(position).isLowSurrogate() &&
      text.at(position - 1).isHighSurrogate())
    --position;
  return position;
}

/// characterClass over a full character, pairing surrogates first.
int fullCharacterClass(const QString &text, int position) {
  const QChar unit = text.at(position);
  if (unit.isHighSurrogate() && position + 1 < text.size() &&
      text.at(position + 1).isLowSurrogate()) {
    const char32_t codepoint =
        QChar::surrogateToUcs4(unit, text.at(position + 1));
    if (QChar::isLetterOrNumber(codepoint))
      return 0;
    if (QChar::isSpace(codepoint))
      return 1;
    return 2;
  }
  return characterClass(unit);
}

QString normalModeStatus() {
  return QStringLiteral("Text card · NORMAL · hjkl move · i/a/o insert · "
                        "v selects · F filename · Ctrl+W window · "
                        "Ctrl+Enter renders · q exits");
}

/// Normal mode rests ON a character, Vim-style, never past the line end.
void clampToLastCharacter(QTextCursor &cursor) {
  if (cursor.atBlockEnd() && !cursor.atBlockStart())
    cursor.movePosition(QTextCursor::PreviousCharacter);
}

QString visualModeStatus(bool linewise) {
  return linewise
             ? QStringLiteral("Text card · VISUAL LINE · hjkl move · y "
                              "copies · x/d delete · c changes · Esc Normal")
             : QStringLiteral("Text card · VISUAL · hjkl move · y copies "
                              "· x/d delete · c changes · Esc Normal");
}
/// Editing keys a non-Vim user expects still work in the modal layer.
QString modalCommand(const QKeyEvent *key) {
  switch (key->key()) {
  case Qt::Key_Left:
  case Qt::Key_Backspace:
    return QStringLiteral("h");
  case Qt::Key_Right:
    return QStringLiteral("l");
  case Qt::Key_Down:
    return QStringLiteral("j");
  case Qt::Key_Up:
    return QStringLiteral("k");
  case Qt::Key_Home:
    return QStringLiteral("0");
  case Qt::Key_End:
    return QStringLiteral("$");
  case Qt::Key_Delete:
    return QStringLiteral("x");
  default:
    return key->text();
  }
}
} // namespace

TextCardEditor::TextCardEditor(QPlainTextEdit *edit, QObject *parent)
    : QObject(parent), edit_(edit) {}

void TextCardEditor::reset() {
  endInsertEdit();
  goalColumn_ = -1;
  stickyEol_ = false;
  insertMode_ = false;
  visualLineMode_ = false;
  visualAnchor_ = -1;
  visualPosition_ = -1;
  visualSelectionStart_ = -1;
  visualSelectionEnd_ = -1;
  yank_.clear();
  yankLinewise_ = false;
  pendingCommand_.clear();
  insertEditStart_ = -1;
  insertEditUndoSteps_ = 0;
  undoMarks_.clear();
  redoMarks_.clear();
  edit_->setExtraSelections({});
}

void TextCardEditor::exitToNormal() {
  if (visualAnchor_ >= 0)
    leaveVisualMode(visualPosition_);
  if (insertMode_)
    setInsertMode(false);
}

// An insert session is grouped by undo-step marks, not one held edit block:
// Qt defers textChanged and re-highlighting while a block is open, which
// froze the live card for the whole session.
void TextCardEditor::beginInsertEdit(int cursorPosition) {
  if (insertEditActive_)
    return;
  insertEditActive_ = true;
  insertSessionFresh_ = true;
  insertEditStart_ = std::clamp(
      cursorPosition, 0, static_cast<int>(edit_->toPlainText().size()));
  insertEditUndoSteps_ = edit_->document()->availableUndoSteps();
}

bool TextCardEditor::handleInsertKey(QKeyEvent *key) {
  if (key->key() == Qt::Key_Escape) {
    setInsertMode(false);
    return true;
  }
  // Widget undo would rewind past the session grouping; undo belongs to
  // Normal-mode u and Ctrl+R.
  if (key->matches(QKeySequence::Undo) || key->matches(QKeySequence::Redo))
    return true;
  const bool plainText =
      !key->text().isEmpty() && key->text().at(0).isPrint() &&
      !(key->modifiers() &
        (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
  const QString text =
      key->key() == Qt::Key_Tab ? QStringLiteral("\t") : key->text();
  if ((plainText || key->key() == Qt::Key_Tab) && insertSessionFresh_) {
    // The session's first insert is block-flagged: Qt merges contiguous
    // plain inserts into one undo command, which would fuse this session
    // with the previous one and make a single u span both.
    QTextCursor cursor = edit_->textCursor();
    cursor.beginEditBlock();
    cursor.insertText(text);
    cursor.endEditBlock();
    insertSessionFresh_ = false;
    return true;
  }
  if (key->key() == Qt::Key_Tab) {
    QTextCursor cursor = edit_->textCursor();
    cursor.insertText(QStringLiteral("\t"));
    return true;
  }
  if (plainText)
    insertSessionFresh_ = false;
  return false;
}

void TextCardEditor::endInsertEdit() {
  if (!insertEditActive_)
    return;
  insertEditActive_ = false;
  const int after = edit_->document()->availableUndoSteps();
  if (after > insertEditUndoSteps_) {
    undoMarks_.push_back({std::clamp(insertEditStart_, 0,
                                     static_cast<int>(
                                         edit_->toPlainText().size())),
                          insertEditUndoSteps_, after});
    redoMarks_.clear();
  }
  insertEditStart_ = -1;
}

void TextCardEditor::recordUndoCursor(int cursorPosition) {
  const int after = edit_->document()->availableUndoSteps();
  undoMarks_.push_back({std::max(0, cursorPosition), after - 1, after});
  redoMarks_.clear();
}

void TextCardEditor::undo(bool redo) {
  QTextDocument *document = edit_->document();
  if ((redo && !document->isRedoAvailable()) ||
      (!redo && !document->isUndoAvailable()))
    return;
  int cursorPosition = edit_->textCursor().position();
  if (redo) {
    if (!redoMarks_.isEmpty()) {
      const UndoMark mark = redoMarks_.takeLast();
      while (document->availableUndoSteps() < mark.stepsAfter &&
             document->isRedoAvailable())
        edit_->redo();
      undoMarks_.push_back(mark);
      cursorPosition = mark.position;
    } else {
      edit_->redo();
    }
  } else {
    if (!undoMarks_.isEmpty()) {
      const UndoMark mark = undoMarks_.takeLast();
      while (document->availableUndoSteps() > mark.stepsBefore &&
             document->isUndoAvailable())
        edit_->undo();
      redoMarks_.push_back(mark);
      cursorPosition = mark.position;
    } else {
      edit_->undo();
    }
  }
  QTextCursor cursor(edit_->document());
  cursor.setPosition(std::clamp(
      cursorPosition, 0, static_cast<int>(edit_->toPlainText().size())));
  clampToLastCharacter(cursor);
  edit_->setTextCursor(cursor);
}

void TextCardEditor::setInsertMode(bool insertMode) {
  if (insertMode && !insertMode_)
    beginInsertEdit(edit_->textCursor().position());
  else if (!insertMode && insertMode_)
    endInsertEdit();
  insertMode_ = insertMode;
  if (!insertMode) {
    QTextCursor cursor = edit_->textCursor();
    clampToLastCharacter(cursor);
    edit_->setTextCursor(cursor);
  }
  visualLineMode_ = false;
  visualAnchor_ = -1;
  visualPosition_ = -1;
  visualSelectionStart_ = -1;
  visualSelectionEnd_ = -1;
  edit_->setExtraSelections({});
  pendingCommand_.clear();
  emit statusRequested(
      insertMode ? QStringLiteral("Text card · INSERT · Esc Normal · "
                                  "Ctrl+W window · Ctrl+Enter renders")
                 : normalModeStatus());
  emit modeChanged();
}

QString TextCardEditor::mode() const {
  if (insertMode_)
    return QStringLiteral("INSERT");
  if (visualAnchor_ >= 0)
    return visualLineMode_ ? QStringLiteral("VISUAL LINE")
                           : QStringLiteral("VISUAL");
  return QStringLiteral("NORMAL");
}

void TextCardEditor::startVisualMode(bool linewise) {
  pendingCommand_.clear();
  visualLineMode_ = linewise;
  const int lastCharacter = edit_->toPlainText().size() - 1;
  const int position =
      lastCharacter < 0
          ? 0
          : std::clamp(edit_->textCursor().position(), 0, lastCharacter);
  visualAnchor_ = position;
  visualPosition_ = position;
  updateVisualSelection();
  emit statusRequested(visualModeStatus(linewise));
  emit modeChanged();
}

void TextCardEditor::updateVisualSelection() {
  const int textLength = edit_->toPlainText().size();
  if (textLength <= 0) {
    visualSelectionStart_ = 0;
    visualSelectionEnd_ = 0;
    edit_->setExtraSelections({});
    edit_->setTextCursor(QTextCursor(edit_->document()));
    return;
  }

  const QString text = edit_->toPlainText();
  visualAnchor_ = snapToCharacterStart(
      text, std::clamp(visualAnchor_, 0, textLength - 1));
  visualPosition_ = snapToCharacterStart(
      text, std::clamp(visualPosition_, 0, textLength - 1));
  visualSelectionStart_ = std::min(visualAnchor_, visualPosition_);
  const int activeEnd = std::max(visualAnchor_, visualPosition_);
  visualSelectionEnd_ = activeEnd + characterWidth(text, activeEnd);
  if (visualLineMode_) {
    const QTextBlock anchorBlock =
        edit_->document()->findBlock(visualAnchor_);
    const QTextBlock activeBlock =
        edit_->document()->findBlock(visualPosition_);
    const QTextBlock first = anchorBlock.position() <= activeBlock.position()
                                 ? anchorBlock
                                 : activeBlock;
    const QTextBlock last = anchorBlock.position() <= activeBlock.position()
                                ? activeBlock
                                : anchorBlock;
    visualSelectionStart_ = first.position();
    visualSelectionEnd_ =
        std::min(textLength, last.position() + static_cast<int>(last.length()));
  }
  QTextEdit::ExtraSelection highlight;
  highlight.cursor = QTextCursor(edit_->document());
  highlight.cursor.setPosition(visualSelectionStart_);
  highlight.cursor.setPosition(visualSelectionEnd_, QTextCursor::KeepAnchor);
  highlight.format.setBackground(selectionColor_);
  if (visualLineMode_)
    highlight.format.setProperty(QTextFormat::FullWidthSelection, true);
  edit_->setExtraSelections({highlight});

  // The highlight covers complete lines, but this real cursor remains at its
  // motion column so j/k feels like Vim instead of jumping to a line edge.
  QTextCursor active(edit_->document());
  active.setPosition(visualPosition_);
  edit_->setTextCursor(active);
}

void TextCardEditor::leaveVisualMode(int cursorPosition) {
  const int textLength = edit_->toPlainText().size();
  visualLineMode_ = false;
  visualAnchor_ = -1;
  visualPosition_ = -1;
  visualSelectionStart_ = -1;
  visualSelectionEnd_ = -1;
  edit_->setExtraSelections({});
  pendingCommand_.clear();
  QTextCursor cursor(edit_->document());
  cursor.setPosition(std::clamp(cursorPosition, 0, textLength));
  edit_->setTextCursor(cursor);
  emit statusRequested(normalModeStatus());
  emit modeChanged();
}

bool TextCardEditor::handleVisualKey(QKeyEvent *key) {
  const QString command = modalCommand(key);
  const bool shifted = key->modifiers().testFlag(Qt::ShiftModifier);
  if (command != QStringLiteral("j") && command != QStringLiteral("k")) {
    goalColumn_ = -1;
    if (command != QStringLiteral("$"))
      stickyEol_ = false;
  }
  if (key->key() == Qt::Key_Escape) {
    leaveVisualMode(visualPosition_);
    return true;
  }
  if (key->modifiers() &
      (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
    pendingCommand_.clear();
    return true;
  }
  const bool changeSelection = key->key() == Qt::Key_C;
  if (command == QStringLiteral("y") || command == QStringLiteral("d") ||
      command == QStringLiteral("x") || changeSelection) {
    const int first = visualSelectionStart_;
    const int last = visualSelectionEnd_;
    const QString source = edit_->toPlainText();
    const QString text = source.mid(first, last - first);
    yank_ = text;
    yankLinewise_ = visualLineMode_;
    if (command == QStringLiteral("y")) {
      // wl-copy keeps serving the offer after omasnap exits; QClipboard's
      // offer would die with the process.
      QString clipboardError;
      const bool copied = copyTextToClipboard(text, clipboardError);
      leaveVisualMode(first);
      emit statusRequested(
          copied ? QStringLiteral("Text card · NORMAL · yanked %1 character%2 "
                                  "to clipboard · cursor returned to "
                                  "selection start")
                       .arg(text.size())
                       .arg(text.size() == 1 ? QString()
                                             : QStringLiteral("s"))
                 : clipboardError);
      return true;
    }

    QTextCursor deletion(edit_->document());
    int deletionStart = first;
    const bool linewise = visualLineMode_;
    if (changeSelection)
      beginInsertEdit(first);
    if (linewise && !changeSelection && last == source.size() && first > 0)
      deletionStart = first - 1;
    deletion.beginEditBlock();
    deletion.setPosition(deletionStart);
    deletion.setPosition(last, QTextCursor::KeepAnchor);
    deletion.removeSelectedText();
    if (changeSelection && linewise &&
        first < edit_->toPlainText().size()) {
      deletion.setPosition(first);
      deletion.insertText(QStringLiteral("\n"));
    }
    deletion.endEditBlock();
    const int remainingLength =
        static_cast<int>(edit_->toPlainText().size());
    leaveVisualMode(std::min(first, remainingLength));
    if (changeSelection) {
      setInsertMode(true);
      return true;
    }
    recordUndoCursor(std::min(first, remainingLength));
    emit statusRequested(
        QStringLiteral("Text card · NORMAL · selection deleted · "
                       "p puts it back · u undoes"));
    return true;
  }
  if (key->key() == Qt::Key_V) {
    if (shifted == visualLineMode_) {
      leaveVisualMode(visualPosition_);
    } else {
      visualLineMode_ = shifted;
      updateVisualSelection();
      emit statusRequested(visualModeStatus(shifted));
      emit modeChanged();
    }
    return true;
  }
  if (command == QStringLiteral("o")) {
    std::swap(visualAnchor_, visualPosition_);
    updateVisualSelection();
    return true;
  }
  if (command == QStringLiteral("p")) {
    QString pasted = yank_;
    if (pasted.isEmpty())
      pasted = QGuiApplication::clipboard()->text(QClipboard::Clipboard);
    if (pasted.isEmpty())
      return true;
    if (visualLineMode_ && !pasted.endsWith(QLatin1Char('\n')))
      pasted.append(QLatin1Char('\n'));
    const int first = visualSelectionStart_;
    const int last = visualSelectionEnd_;
    const QString source = edit_->toPlainText();
    // Vim swaps: the register replaces the selection, which becomes the
    // register.
    const QString removed = source.mid(first, last - first);
    QTextCursor replace(edit_->document());
    replace.beginEditBlock();
    replace.setPosition(first);
    replace.setPosition(last, QTextCursor::KeepAnchor);
    replace.removeSelectedText();
    replace.insertText(pasted);
    replace.endEditBlock();
    yank_ = removed;
    yankLinewise_ = visualLineMode_;
    leaveVisualMode(first);
    recordUndoCursor(first);
    emit statusRequested(
        QStringLiteral("Text card · NORMAL · selection replaced · u undoes"));
    return true;
  }

  if (pendingCommand_ == QStringLiteral("i")) {
    pendingCommand_.clear();
    if (command == QStringLiteral("w")) {
      const auto range = wordRange(false);
      if (range) {
        visualLineMode_ = false;
        visualAnchor_ = range->first;
        visualPosition_ = range->second - 1;
        updateVisualSelection();
        emit statusRequested(
            QStringLiteral("Text card · VISUAL · inner word selected · "
                           "y copies · x/d delete · c changes · Esc Normal"));
        emit modeChanged();
      }
    }
    return true;
  }
  if (command == QStringLiteral("i")) {
    pendingCommand_ = QStringLiteral("i");
    emit statusRequested(
        QStringLiteral("Text card · VISUAL · iw selects inner word · "
                       "Esc returns to Normal"));
    return true;
  }

  QTextCursor motion(edit_->document());
  motion.setPosition(visualPosition_);
  if (pendingCommand_ == QStringLiteral("g")) {
    pendingCommand_.clear();
    if (command == QStringLiteral("g"))
      motion.movePosition(QTextCursor::Start);
    else
      return true;
  } else if (shifted && key->key() == Qt::Key_G) {
    motion.movePosition(QTextCursor::End);
  } else if (command == QStringLiteral("g")) {
    pendingCommand_ = QStringLiteral("g");
    return true;
  } else if (!applyMotion(motion, command)) {
    pendingCommand_.clear();
    return true;
  }
  const int lastCharacter = edit_->toPlainText().size() - 1;
  visualPosition_ =
      lastCharacter < 0 ? 0 : std::clamp(motion.position(), 0, lastCharacter);
  updateVisualSelection();
  return true;
}

void TextCardEditor::joinLines() {
  QTextCursor cursor = edit_->textCursor();
  const QTextBlock block = cursor.block();
  const QTextBlock next = block.next();
  if (!next.isValid())
    return;

  const QString nextText = next.text();
  int leadingSpace = 0;
  while (leadingSpace < nextText.size() &&
         nextText.at(leadingSpace).isSpace())
    ++leadingSpace;
  const int joinPosition =
      block.position() + static_cast<int>(block.text().size());
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
  edit_->setTextCursor(cursor);
  recordUndoCursor(joinPosition);
  emit statusRequested(
      QStringLiteral("Text card · NORMAL · joined lines · u undoes"));
}

// Next-word boundary for w operators, stopping at the line end like Vim's
// dw instead of eating the newline.
int TextCardEditor::wordForwardStop(const QTextCursor &cursor) const {
  QTextCursor probe = cursor;
  probe.movePosition(QTextCursor::NextWord);
  const int blockEnd = cursor.block().position() +
                       static_cast<int>(cursor.block().text().size());
  if (probe.block() != cursor.block() || probe.position() > blockEnd)
    return blockEnd;
  return probe.position();
}

// Returns the first UTF-16 unit of the word's final character; pair-aware
// so the cursor and deletion ranges never land between surrogate halves.
int TextCardEditor::wordEnd(int cursorPosition) const {
  const QString text = edit_->toPlainText();
  if (text.isEmpty())
    return 0;

  const int original = snapToCharacterStart(
      text, std::clamp(cursorPosition, 0, static_cast<int>(text.size()) - 1));
  int position = original;
  int selectedClass = fullCharacterClass(text, position);
  if (selectedClass != 1) {
    int currentEnd = position;
    int probe = position + characterWidth(text, position);
    while (probe < text.size() &&
           fullCharacterClass(text, probe) == selectedClass) {
      currentEnd = probe;
      probe += characterWidth(text, probe);
    }
    if (currentEnd > position)
      return currentEnd;
    position += characterWidth(text, position);
  }

  while (position < text.size() && text.at(position).isSpace())
    ++position;
  if (position >= text.size())
    return original;

  selectedClass = fullCharacterClass(text, position);
  int currentEnd = position;
  int probe = position + characterWidth(text, position);
  while (probe < text.size() &&
         fullCharacterClass(text, probe) == selectedClass) {
    currentEnd = probe;
    probe += characterWidth(text, probe);
  }
  return currentEnd;
}

std::optional<QPair<int, int>> TextCardEditor::wordRange(
    bool around, bool fromCursor) const {
  const QTextCursor current = edit_->textCursor();
  const QTextBlock block = current.block();
  const QString line = block.text();
  if (line.isEmpty())
    return std::nullopt;

  int position = std::clamp(current.position() - block.position(), 0,
                            static_cast<int>(line.size()));
  if (position == line.size())
    --position;
  position = snapToCharacterStart(line, position);
  const int selectedClass = characterClass(line.at(position));
  int first = position;
  int last = position + 1;
  if (!fromCursor) {
    while (first > 0 &&
           characterClass(line.at(first - 1)) == selectedClass)
      --first;
  }
  while (last < line.size() &&
         characterClass(line.at(last)) == selectedClass)
    ++last;

  if (around && selectedClass == 1 && last < line.size()) {
    const int wordClass = fullCharacterClass(line, last);
    while (last < line.size() &&
           fullCharacterClass(line, last) == wordClass)
      last += characterWidth(line, last);
  } else if (around && selectedClass != 1) {
    const int contentLast = last;
    while (last < line.size() && line.at(last).isSpace())
      ++last;
    if (last == contentLast) {
      while (first > 0 && line.at(first - 1).isSpace())
        --first;
    }
  }

  return QPair{block.position() + first, block.position() + last};
}

void TextCardEditor::deleteRange(int first, int last, bool change,
                                 const QString &deleted) {
  if (first >= last) {
    if (change) {
      beginInsertEdit(first);
      setInsertMode(true);
    }
    return;
  }
  yank_ = edit_->toPlainText().mid(first, last - first);
  yankLinewise_ = false;
  if (change)
    beginInsertEdit(first);
  QTextCursor edit(edit_->document());
  edit.beginEditBlock();
  edit.setPosition(first);
  edit.setPosition(last, QTextCursor::KeepAnchor);
  edit.removeSelectedText();
  edit.endEditBlock();
  edit.setPosition(first);
  edit_->setTextCursor(edit);
  if (change)
    setInsertMode(true);
  else {
    recordUndoCursor(first);
    emit statusRequested(QStringLiteral("Text card · NORMAL · %1 · "
                                        "p puts it back · u undoes")
                             .arg(deleted));
  }
}

bool TextCardEditor::applyEndOperator(bool change) {
  const QString source = edit_->toPlainText();
  if (source.isEmpty())
    return false;
  const int first = snapToCharacterStart(
      source, std::clamp(edit_->textCursor().position(), 0,
                         static_cast<int>(source.size()) - 1));
  const int endIndex = wordEnd(first);
  deleteRange(first, endIndex + characterWidth(source, endIndex), change,
              QStringLiteral("deleted through word end"));
  return true;
}

bool TextCardEditor::applyWordOperator(bool change, bool around,
                                       bool fromCursor) {
  const auto range = wordRange(around, fromCursor);
  if (!range)
    return false;
  deleteRange(range->first, range->second, change,
              QStringLiteral("word deleted"));
  return true;
}

void TextCardEditor::emitUnsupportedMotion(const QString &pending,
                                           const QString &command) {
  if (command.isEmpty())
    return;
  emit statusRequested(
      QStringLiteral("Text card · NORMAL · %1%2 is not a supported motion")
          .arg(pending, command));
}

void TextCardEditor::yankLine() {
  const QTextCursor cursor = edit_->textCursor();
  yank_ = cursor.block().text() + QLatin1Char('\n');
  yankLinewise_ = true;
  QString clipboardError;
  const bool copied = copyTextToClipboard(yank_, clipboardError);
  emit statusRequested(
      copied ? QStringLiteral("Text card · NORMAL · line yanked to "
                              "clipboard · p below · P above")
             : clipboardError);
}

void TextCardEditor::put(bool before) {
  QString text = yank_;
  bool linewise = yankLinewise_;
  if (text.isEmpty()) {
    text = QGuiApplication::clipboard()->text(QClipboard::Clipboard);
    linewise = false;
  }
  if (text.isEmpty())
    return;

  QTextCursor cursor = edit_->textCursor();
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
    if (!before && !cursor.atBlockEnd())
      cursor.movePosition(QTextCursor::NextCharacter);
    placedAt = cursor.position();
    cursor.insertText(text);
  }
  cursor.endEditBlock();
  cursor.setPosition(placedAt);
  if (linewise)
    moveToFirstNonBlank(cursor);
  else
    cursor.setPosition(snapToCharacterStart(
        edit_->toPlainText(),
        std::max(placedAt, placedAt + static_cast<int>(text.size()) - 1)));
  edit_->setTextCursor(cursor);
  recordUndoCursor(placedAt);
  emit statusRequested(
      linewise ? QStringLiteral("Text card · NORMAL · put %1 current line")
                     .arg(before ? QStringLiteral("above")
                                 : QStringLiteral("below"))
               : QStringLiteral("Text card · NORMAL · text put %1 cursor")
                     .arg(before ? QStringLiteral("before")
                                 : QStringLiteral("after")));
}

bool TextCardEditor::applyMotion(QTextCursor &cursor,
                                 const QString &command) {
  if (command == QStringLiteral("h")) {
    // h and l stay on their line, Vim-style.
    if (!cursor.atBlockStart())
      cursor.movePosition(QTextCursor::PreviousCharacter);
  } else if (command == QStringLiteral("j") ||
             command == QStringLiteral("k")) {
    // Logical lines, not wrapped display rows, with the column remembered
    // across shorter lines; after `$` the target is each line's end.
    const QTextBlock block = cursor.block();
    const QTextBlock target =
        command == QStringLiteral("j") ? block.next() : block.previous();
    if (!target.isValid())
      return true;
    if (goalColumn_ < 0)
      goalColumn_ = cursor.position() - block.position();
    const int lastColumn =
        std::max(0, static_cast<int>(target.text().size()) - 1);
    const int column = snapToCharacterStart(
        target.text(),
        stickyEol_ ? lastColumn : std::min(goalColumn_, lastColumn));
    cursor.setPosition(target.position() + column);
  } else if (command == QStringLiteral("l")) {
    if (!cursor.atBlockEnd()) {
      cursor.movePosition(QTextCursor::NextCharacter);
      clampToLastCharacter(cursor);
    }
  } else if (command == QStringLiteral("0"))
    cursor.movePosition(QTextCursor::StartOfBlock);
  else if (command == QStringLiteral("$")) {
    cursor.movePosition(QTextCursor::EndOfBlock);
    clampToLastCharacter(cursor);
    stickyEol_ = true;
  } else if (command == QStringLiteral("w")) {
    cursor.movePosition(QTextCursor::NextWord);
    if (cursor.atBlockEnd() && !cursor.atEnd())
      cursor.movePosition(QTextCursor::NextWord);
  } else if (command == QStringLiteral("b")) {
    cursor.movePosition(QTextCursor::PreviousWord);
    if (cursor.atBlockEnd() && !cursor.atStart())
      cursor.movePosition(QTextCursor::PreviousWord);
  } else if (command == QStringLiteral("e"))
    cursor.setPosition(wordEnd(cursor.position()));
  else
    return false;
  return true;
}

bool TextCardEditor::handleKey(QKeyEvent *key) {
  if (visualAnchor_ >= 0)
    return handleVisualKey(key);
  if (key->modifiers().testFlag(Qt::ControlModifier) &&
      key->key() == Qt::Key_R) {
    undo(true);
    pendingCommand_.clear();
    return true;
  }
  if (key->modifiers() &
      (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
    pendingCommand_.clear();
    return true;
  }

  if (key->key() == Qt::Key_Escape) {
    pendingCommand_.clear();
    emit statusRequested(normalModeStatus());
    return true;
  }
  QTextCursor cursor = edit_->textCursor();
  const QString command = modalCommand(key);
  const bool shifted = key->modifiers().testFlag(Qt::ShiftModifier);
  if (command != QStringLiteral("j") && command != QStringLiteral("k")) {
    goalColumn_ = -1;
    if (command != QStringLiteral("$"))
      stickyEol_ = false;
  }
  if (shifted && key->key() == Qt::Key_J) {
    pendingCommand_.clear();
    joinLines();
    return true;
  }
  if (shifted && key->key() == Qt::Key_F) {
    pendingCommand_.clear();
    emit filenameEditRequested();
    return true;
  }
  if (shifted && key->key() == Qt::Key_C) {
    pendingCommand_.clear();
    const int first = cursor.position();
    cursor.movePosition(QTextCursor::EndOfBlock);
    deleteRange(first, cursor.position(), true, {});
    return true;
  }
  if (shifted && key->key() == Qt::Key_D) {
    pendingCommand_.clear();
    const int first = cursor.position();
    cursor.movePosition(QTextCursor::EndOfBlock);
    if (cursor.position() > first)
      deleteRange(first, cursor.position(), false,
                  QStringLiteral("deleted to line end"));
    return true;
  }
  if (key->key() == Qt::Key_P) {
    pendingCommand_.clear();
    put(shifted);
    return true;
  }
  if (shifted && key->key() == Qt::Key_G) {
    pendingCommand_.clear();
    cursor.movePosition(QTextCursor::End);
    moveToFirstNonBlank(cursor);
    edit_->setTextCursor(cursor);
    return true;
  }
  if (shifted && key->key() == Qt::Key_I) {
    pendingCommand_.clear();
    const QTextBlock block = cursor.block();
    int first = 0;
    while (first < block.text().size() && block.text().at(first).isSpace())
      ++first;
    cursor.setPosition(block.position() + first);
    edit_->setTextCursor(cursor);
    setInsertMode(true);
    return true;
  }
  if (shifted && key->key() == Qt::Key_A) {
    pendingCommand_.clear();
    cursor.movePosition(QTextCursor::EndOfBlock);
    edit_->setTextCursor(cursor);
    setInsertMode(true);
    return true;
  }
  if (shifted && key->key() == Qt::Key_O) {
    pendingCommand_.clear();
    cursor.movePosition(QTextCursor::StartOfBlock);
    const int blankLine = cursor.position();
    beginInsertEdit(blankLine);
    cursor.beginEditBlock();
    cursor.insertText(QStringLiteral("\n"));
    cursor.endEditBlock();
    cursor.setPosition(blankLine);
    edit_->setTextCursor(cursor);
    setInsertMode(true);
    return true;
  }
  if (pendingCommand_ == QStringLiteral("g")) {
    pendingCommand_.clear();
    if (command == QStringLiteral("g")) {
      cursor.movePosition(QTextCursor::Start);
      moveToFirstNonBlank(cursor);
    }
    edit_->setTextCursor(cursor);
    return true;
  }
  if (QStringList{QStringLiteral("di"), QStringLiteral("da"),
                  QStringLiteral("ci"), QStringLiteral("ca"),
                  QStringLiteral("yi"), QStringLiteral("ya")}
          .contains(pendingCommand_)) {
    const QString pending = pendingCommand_;
    pendingCommand_.clear();
    if (command != QStringLiteral("w")) {
      emitUnsupportedMotion(pending, command);
      return true;
    }
    if (pending.startsWith(QLatin1Char('y'))) {
      if (const auto range = wordRange(pending.endsWith(QLatin1Char('a')))) {
        yank_ = edit_->toPlainText().mid(range->first,
                                         range->second - range->first);
        yankLinewise_ = false;
        emit statusRequested(QStringLiteral(
            "Text card · NORMAL · word yanked · p puts it after the cursor"));
      }
      return true;
    }
    static_cast<void>(applyWordOperator(pending.startsWith(QLatin1Char('c')),
                                        pending.endsWith(QLatin1Char('a'))));
    return true;
  }
  if (pendingCommand_ == QStringLiteral("d") ||
      pendingCommand_ == QStringLiteral("c")) {
    const QString pending = pendingCommand_;
    pendingCommand_.clear();
    if (command == QStringLiteral("i") || command == QStringLiteral("a")) {
      pendingCommand_ = pending + command;
      return true;
    }
    if (pending == QStringLiteral("c") && command == QStringLiteral("w")) {
      static_cast<void>(applyWordOperator(true, false, true));
      return true;
    }
    if (pending == QStringLiteral("d") && command == QStringLiteral("w")) {
      const int stop = wordForwardStop(cursor);
      if (stop > cursor.position())
        deleteRange(cursor.position(), stop, false,
                    QStringLiteral("deleted to next word"));
      return true;
    }
    if (command == QStringLiteral("e")) {
      static_cast<void>(applyEndOperator(pending == QStringLiteral("c")));
      return true;
    }
    if (pending == QStringLiteral("d") && command == QStringLiteral("d")) {
      if (edit_->toPlainText().isEmpty())
        return true;
      yank_ = cursor.block().text() + QLatin1Char('\n');
      yankLinewise_ = true;
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
      cursor.beginEditBlock();
      cursor.removeSelectedText();
      cursor.endEditBlock();
      recordUndoCursor(start);
      cursor.setPosition(
          std::min(start, static_cast<int>(edit_->toPlainText().size())));
      moveToFirstNonBlank(cursor);
      emit statusRequested(
          QStringLiteral("Text card · NORMAL · line deleted · "
                         "p puts it back · u undoes"));
    } else if (pending == QStringLiteral("c") &&
               command == QStringLiteral("c")) {
      yank_ = cursor.block().text() + QLatin1Char('\n');
      yankLinewise_ = true;
      cursor.movePosition(QTextCursor::StartOfBlock);
      const int first = cursor.position();
      beginInsertEdit(first);
      cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
      cursor.beginEditBlock();
      cursor.removeSelectedText();
      cursor.endEditBlock();
      cursor.setPosition(first);
      edit_->setTextCursor(cursor);
      setInsertMode(true);
      return true;
    } else {
      emitUnsupportedMotion(pending, command);
    }
    edit_->setTextCursor(cursor);
    return true;
  }
  if (pendingCommand_ == QStringLiteral("y")) {
    pendingCommand_.clear();
    if (command == QStringLiteral("y")) {
      yankLine();
      return true;
    }
    if (command == QStringLiteral("i") || command == QStringLiteral("a")) {
      pendingCommand_ = QStringLiteral("y") + command;
      return true;
    }
    if (command == QStringLiteral("w")) {
      const int stop = wordForwardStop(cursor);
      if (stop > cursor.position()) {
        yank_ = edit_->toPlainText().mid(cursor.position(),
                                         stop - cursor.position());
        yankLinewise_ = false;
        emit statusRequested(QStringLiteral(
            "Text card · NORMAL · yanked to next word · p puts it back"));
      }
      return true;
    }
    if (command == QStringLiteral("e")) {
      const QString source = edit_->toPlainText();
      const int first = snapToCharacterStart(
          source, std::clamp(cursor.position(), 0,
                             std::max(0, static_cast<int>(source.size()) - 1)));
      const int endIndex = wordEnd(first);
      yank_ = source.mid(first,
                         endIndex + characterWidth(source, endIndex) - first);
      yankLinewise_ = false;
      emit statusRequested(QStringLiteral(
          "Text card · NORMAL · yanked through word end · p puts it back"));
      return true;
    }
    emitUnsupportedMotion(QStringLiteral("y"), command);
    return true;
  }

  if (command == QStringLiteral("g")) {
    pendingCommand_ = QStringLiteral("g");
    return true;
  }
  if (command == QStringLiteral("d")) {
    pendingCommand_ = QStringLiteral("d");
    return true;
  }
  if (command == QStringLiteral("c")) {
    pendingCommand_ = QStringLiteral("c");
    return true;
  }
  if (command == QStringLiteral("y")) {
    pendingCommand_ = QStringLiteral("y");
    return true;
  }
  if (key->key() == Qt::Key_V) {
    startVisualMode(shifted);
    return true;
  }
  if (applyMotion(cursor, command)) {
    clampToLastCharacter(cursor);
    edit_->setTextCursor(cursor);
    return true;
  }
  if (command == QStringLiteral("x")) {
    // Vim's x: a no-op at a line break, fills the register, and steps back
    // when the line's last character goes.
    if (cursor.atBlockEnd())
      return true;
    QTextCursor deletion = cursor;
    deletion.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    yank_ = deletion.selectedText();
    yankLinewise_ = false;
    const int first = cursor.position();
    deletion.beginEditBlock();
    deletion.removeSelectedText();
    deletion.endEditBlock();
    recordUndoCursor(first);
    clampToLastCharacter(deletion);
    cursor = deletion;
  } else if (command == QStringLiteral("u")) {
    undo(false);
    return true;
  } else if (command == QStringLiteral("i")) {
    setInsertMode(true);
    return true;
  } else if (command == QStringLiteral("a")) {
    if (!cursor.atBlockEnd())
      cursor.movePosition(QTextCursor::NextCharacter);
    edit_->setTextCursor(cursor);
    setInsertMode(true);
    return true;
  } else if (command == QStringLiteral("o")) {
    cursor.movePosition(QTextCursor::EndOfBlock);
    beginInsertEdit(cursor.position());
    cursor.beginEditBlock();
    cursor.insertText(QStringLiteral("\n"));
    cursor.endEditBlock();
    edit_->setTextCursor(cursor);
    setInsertMode(true);
    return true;
  } else if (command == QStringLiteral("q")) {
    emit cancelRequested();
    return true;
  }
  edit_->setTextCursor(cursor);
  return true;
}
