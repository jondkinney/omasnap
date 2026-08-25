/** @fileoverview Modal Normal/Insert/Visual editing on the card's source. */
#include "text-card-editor.hpp"

#include <QClipboard>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QTextBlock>

#include <algorithm>

TextCardEditor::TextCardEditor(QPlainTextEdit *edit, QObject *parent)
    : QObject(parent), edit_(edit) {}

void TextCardEditor::reset() {
  endInsertEdit();
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
  undoCursorStack_.clear();
  redoCursorStack_.clear();
  edit_->setExtraSelections({});
}

void TextCardEditor::exitToNormal() {
  if (visualAnchor_ >= 0)
    leaveVisualMode(visualPosition_);
  if (insertMode_)
    setInsertMode(false);
}

void TextCardEditor::beginInsertEdit(int cursorPosition) {
  if (insertEditActive_)
    return;
  insertEditCursor_ = QTextCursor(edit_->document());
  insertEditCursor_.beginEditBlock();
  insertEditActive_ = true;
  insertEditStart_ = std::clamp(
      cursorPosition, 0, static_cast<int>(edit_->toPlainText().size()));
  insertEditUndoSteps_ = edit_->document()->availableUndoSteps();
}

void TextCardEditor::endInsertEdit() {
  if (!insertEditActive_)
    return;
  insertEditCursor_.endEditBlock();
  insertEditActive_ = false;
  if (edit_->document()->availableUndoSteps() > insertEditUndoSteps_)
    recordUndoCursor(insertEditStart_);
  insertEditStart_ = -1;
}

void TextCardEditor::recordUndoCursor(int cursorPosition) {
  undoCursorStack_.push_back(std::clamp(
      cursorPosition, 0, static_cast<int>(edit_->toPlainText().size())));
  redoCursorStack_.clear();
}

void TextCardEditor::undo(bool redo) {
  QTextDocument *document = edit_->document();
  if ((redo && !document->isRedoAvailable()) ||
      (!redo && !document->isUndoAvailable()))
    return;
  int cursorPosition = edit_->textCursor().position();
  if (redo) {
    if (!redoCursorStack_.isEmpty()) {
      cursorPosition = redoCursorStack_.takeLast();
      undoCursorStack_.push_back(cursorPosition);
    }
    edit_->redo();
  } else {
    if (!undoCursorStack_.isEmpty()) {
      cursorPosition = undoCursorStack_.takeLast();
      redoCursorStack_.push_back(cursorPosition);
    }
    edit_->undo();
  }
  QTextCursor cursor(edit_->document());
  cursor.setPosition(std::clamp(
      cursorPosition, 0, static_cast<int>(edit_->toPlainText().size())));
  edit_->setTextCursor(cursor);
}

void TextCardEditor::setInsertMode(bool insertMode) {
  if (insertMode && !insertMode_)
    beginInsertEdit(edit_->textCursor().position());
  else if (!insertMode && insertMode_)
    endInsertEdit();
  insertMode_ = insertMode;
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
                 : QStringLiteral("Text card · NORMAL · hjkl move · i/a/o "
                                  "insert · F filename · Ctrl+W window · "
                                  "Ctrl+Enter renders · q exits"));
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
  emit statusRequested(
      linewise ? QStringLiteral("Text card · VISUAL LINE · hjkl move · y "
                                "copies · x/d delete · c changes · Esc Normal")
               : QStringLiteral("Text card · VISUAL · hjkl move · y copies "
                                "· x/d delete · c changes · Esc Normal"));
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

  visualAnchor_ = std::clamp(visualAnchor_, 0, textLength - 1);
  visualPosition_ = std::clamp(visualPosition_, 0, textLength - 1);
  visualSelectionStart_ = std::min(visualAnchor_, visualPosition_);
  visualSelectionEnd_ = std::max(visualAnchor_, visualPosition_) + 1;
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
  emit statusRequested(
      QStringLiteral("Text card · NORMAL · hjkl move · i/a/o insert · "
                     "v selects · F filename · Ctrl+W window · "
                     "Ctrl+Enter renders · q exits"));
  emit modeChanged();
}

bool TextCardEditor::handleVisualKey(QKeyEvent *key) {
  const QString command = key->text();
  const bool shifted = key->modifiers().testFlag(Qt::ShiftModifier);
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
      QGuiApplication::clipboard()->setText(text, QClipboard::Clipboard);
      leaveVisualMode(first);
      emit statusRequested(
          QStringLiteral("Text card · NORMAL · yanked %1 character%2 to "
                         "clipboard · cursor returned to selection start")
              .arg(text.size())
              .arg(text.size() == 1 ? QString() : QStringLiteral("s")));
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
      emit statusRequested(
          shifted
              ? QStringLiteral("Text card · VISUAL LINE · hjkl move · y "
                               "copies · x/d delete · c changes · Esc Normal")
              : QStringLiteral("Text card · VISUAL · hjkl move · y "
                               "copies · x/d delete · c changes · Esc Normal"));
      emit modeChanged();
    }
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
  } else if (command == QStringLiteral("e")) {
    motion.setPosition(wordEnd(visualPosition_));
  } else {
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

int TextCardEditor::wordEnd(int cursorPosition) const {
  const QString text = edit_->toPlainText();
  if (text.isEmpty())
    return 0;

  const auto characterClass = [](QChar character) {
    if (character.isLetterOrNumber() || character == QLatin1Char('_'))
      return 0;
    if (character.isSpace())
      return 1;
    return 2;
  };
  const int original =
      std::clamp(cursorPosition, 0, static_cast<int>(text.size()) - 1);
  int position = original;
  int selectedClass = characterClass(text.at(position));
  if (selectedClass != 1) {
    int currentEnd = position;
    while (currentEnd + 1 < text.size() &&
           characterClass(text.at(currentEnd + 1)) == selectedClass)
      ++currentEnd;
    if (currentEnd > position)
      return currentEnd;
    ++position;
  }

  while (position < text.size() && text.at(position).isSpace())
    ++position;
  if (position >= text.size())
    return original;

  selectedClass = characterClass(text.at(position));
  while (position + 1 < text.size() &&
         characterClass(text.at(position + 1)) == selectedClass)
    ++position;
  return position;
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
  const auto characterClass = [](QChar character) {
    if (character.isLetterOrNumber() || character == QLatin1Char('_'))
      return 0;
    if (character.isSpace())
      return 1;
    return 2;
  };
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

  if (around && selectedClass != 1) {
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

bool TextCardEditor::applyEndOperator(bool change) {
  const QString source = edit_->toPlainText();
  if (source.isEmpty())
    return false;
  const int first = std::clamp(edit_->textCursor().position(), 0,
                               static_cast<int>(source.size()) - 1);
  const int last = wordEnd(first) + 1;
  yank_ = source.mid(first, last - first);
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
    emit statusRequested(
        QStringLiteral("Text card · NORMAL · deleted through word "
                       "end · p puts it back · u undoes"));
  }
  return true;
}

bool TextCardEditor::applyWordOperator(bool change, bool around,
                                       bool fromCursor) {
  const auto range = wordRange(around, fromCursor);
  if (!range)
    return false;
  const int documentFirst = range->first;
  const int documentLast = range->second;
  yank_ = edit_->toPlainText().mid(documentFirst,
                                   documentLast - documentFirst);
  yankLinewise_ = false;
  if (change)
    beginInsertEdit(documentFirst);
  QTextCursor edit(edit_->document());
  edit.beginEditBlock();
  edit.setPosition(documentFirst);
  edit.setPosition(documentLast, QTextCursor::KeepAnchor);
  edit.removeSelectedText();
  edit.endEditBlock();
  edit.setPosition(documentFirst);
  edit_->setTextCursor(edit);
  if (change)
    setInsertMode(true);
  else {
    recordUndoCursor(documentFirst);
    emit statusRequested(
        QStringLiteral("Text card · NORMAL · word deleted · "
                       "p puts it back · u undoes"));
  }
  return true;
}

void TextCardEditor::yankLine() {
  const QTextCursor cursor = edit_->textCursor();
  yank_ = cursor.block().text() + QLatin1Char('\n');
  yankLinewise_ = true;
  QGuiApplication::clipboard()->setText(yank_, QClipboard::Clipboard);
  emit statusRequested(
      QStringLiteral("Text card · NORMAL · line yanked to clipboard · "
                     "p below · P above"));
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
    if (!before)
      cursor.movePosition(QTextCursor::NextCharacter);
    placedAt = cursor.position();
    cursor.insertText(text);
  }
  cursor.endEditBlock();
  cursor.setPosition(placedAt);
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

  QTextCursor cursor = edit_->textCursor();
  const QString command = key->text();
  const bool shifted = key->modifiers().testFlag(Qt::ShiftModifier);
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
    beginInsertEdit(first);
    cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    yank_ = cursor.selectedText();
    yankLinewise_ = false;
    if (cursor.hasSelection()) {
      cursor.beginEditBlock();
      cursor.removeSelectedText();
      cursor.endEditBlock();
    }
    cursor.setPosition(first);
    edit_->setTextCursor(cursor);
    setInsertMode(true);
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
    cursor.insertText(QStringLiteral("\n"));
    cursor.setPosition(blankLine);
    edit_->setTextCursor(cursor);
    setInsertMode(true);
    return true;
  }
  if (pendingCommand_ == QStringLiteral("g")) {
    pendingCommand_.clear();
    if (command == QStringLiteral("g"))
      cursor.movePosition(QTextCursor::Start);
    edit_->setTextCursor(cursor);
    return true;
  }
  if (QStringList{QStringLiteral("di"), QStringLiteral("da"),
                  QStringLiteral("ci"), QStringLiteral("ca")}
          .contains(pendingCommand_)) {
    const QString pending = pendingCommand_;
    pendingCommand_.clear();
    if (command == QStringLiteral("w"))
      static_cast<void>(applyWordOperator(
          pending.startsWith(QLatin1Char('c')),
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
    if (command == QStringLiteral("e")) {
      static_cast<void>(applyEndOperator(pending == QStringLiteral("c")));
      return true;
    }
    if (pending == QStringLiteral("d") && command == QStringLiteral("d")) {
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
      recordUndoCursor(
          std::min(start, static_cast<int>(edit_->toPlainText().size())));
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
    }
    edit_->setTextCursor(cursor);
    return true;
  }
  if (pendingCommand_ == QStringLiteral("y")) {
    pendingCommand_.clear();
    if (command == QStringLiteral("y"))
      yankLine();
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
  else if (command == QStringLiteral("e"))
    cursor.setPosition(wordEnd(cursor.position()));
  else if (command == QStringLiteral("x")) {
    const int beforeLength = static_cast<int>(edit_->toPlainText().size());
    const int first = cursor.position();
    cursor.beginEditBlock();
    cursor.deleteChar();
    cursor.endEditBlock();
    if (static_cast<int>(edit_->toPlainText().size()) < beforeLength)
      recordUndoCursor(first);
  } else if (command == QStringLiteral("u")) {
    undo(false);
    return true;
  } else if (command == QStringLiteral("i")) {
    setInsertMode(true);
    return true;
  } else if (command == QStringLiteral("a")) {
    cursor.movePosition(QTextCursor::NextCharacter);
    edit_->setTextCursor(cursor);
    setInsertMode(true);
    return true;
  } else if (command == QStringLiteral("o")) {
    cursor.movePosition(QTextCursor::EndOfBlock);
    beginInsertEdit(cursor.position());
    cursor.insertText(QStringLiteral("\n"));
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
