/** @fileoverview Neovim-style modal key machine for the clipboard text card. */
#pragma once

#include <QColor>
#include <QObject>
#include <QPair>
#include <QString>
#include <QTextCursor>
#include <QVector>

#include <optional>

class QKeyEvent;
class QPlainTextEdit;

/** Owns the card's modal editing state: modes, motions, operators, the yank
 * register, and Vim-style undo cursors. The host owns the widget, rendering,
 * filename editing, and persistence, and reacts through the signals. */
class TextCardEditor final : public QObject {
  Q_OBJECT

public:
  explicit TextCardEditor(QPlainTextEdit *edit, QObject *parent = nullptr);

  /** Clears mode, register, pending command, and undo cursors for a fresh
   * card. */
  void reset();
  void setSelectionColor(const QColor &color) { selectionColor_ = color; }
  [[nodiscard]] QString mode() const;
  [[nodiscard]] bool insertMode() const { return insertMode_; }
  [[nodiscard]] bool visualMode() const { return visualAnchor_ >= 0; }
  void setInsertMode(bool insertMode);
  void endInsertEdit();
  /** Drops any Visual selection or Insert session back to Normal mode. */
  void exitToNormal();
  /** Dispatches one Normal- or Visual-mode key; true when consumed. */
  [[nodiscard]] bool handleKey(QKeyEvent *key);

signals:
  void statusRequested(const QString &status);
  void modeChanged();
  void cancelRequested();
  void filenameEditRequested();

private:
  void beginInsertEdit(int cursorPosition);
  void recordUndoCursor(int cursorPosition);
  void undo(bool redo);
  void startVisualMode(bool linewise);
  void updateVisualSelection();
  void leaveVisualMode(int cursorPosition);
  [[nodiscard]] bool handleVisualKey(QKeyEvent *key);
  void joinLines();
  [[nodiscard]] int wordEnd(int cursorPosition) const;
  [[nodiscard]] std::optional<QPair<int, int>>
  wordRange(bool around, bool fromCursor = false) const;
  [[nodiscard]] bool applyMotion(QTextCursor &cursor, const QString &command);
  void deleteRange(int first, int last, bool change, const QString &deleted);
  [[nodiscard]] bool applyEndOperator(bool change);
  [[nodiscard]] bool applyWordOperator(bool change, bool around,
                                       bool fromCursor = false);
  void yankLine();
  void put(bool before);

  QPlainTextEdit *edit_ = nullptr;
  QColor selectionColor_;
  bool insertMode_ = false;
  bool visualLineMode_ = false;
  int visualAnchor_ = -1;
  int visualPosition_ = -1;
  int visualSelectionStart_ = -1;
  int visualSelectionEnd_ = -1;
  /** One edit for `u`: [stepsBefore, stepsAfter) on the document's undo
   * stack, with the cursor position both undo and redo restore. */
  struct UndoMark {
    int position = 0;
    int stepsBefore = 0;
    int stepsAfter = 0;
  };

  QString yank_;
  bool yankLinewise_ = false;
  int goalColumn_ = -1;
  bool stickyEol_ = false;
  QString pendingCommand_;
  bool insertEditActive_ = false;
  int insertEditStart_ = -1;
  int insertEditUndoSteps_ = 0;
  QVector<UndoMark> undoMarks_;
  QVector<UndoMark> redoMarks_;
};
