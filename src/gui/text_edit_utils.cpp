#include "gui/text_edit_utils.h"

#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTextCursor>
#include <QTextDocument>

#include <algorithm>

namespace studiocast::gui {
namespace {

int ClampToDocumentPosition(int position, const QPlainTextEdit *edit) {
  if (!edit || !edit->document())
    return 0;
  const int maxPosition = std::max(0, edit->document()->characterCount() - 1);
  return std::clamp(position, 0, maxPosition);
}

void RestoreScrollBar(QScrollBar *bar, int value, bool wasAtEnd) {
  if (!bar)
    return;
  if (wasAtEnd) {
    bar->setValue(bar->maximum());
    return;
  }
  bar->setValue(std::clamp(value, bar->minimum(), bar->maximum()));
}

} // namespace

void SetPlainTextPreservingScroll(QPlainTextEdit *edit, const QString &text) {
  if (!edit)
    return;
  if (edit->toPlainText() == text)
    return;

  QScrollBar *vertical = edit->verticalScrollBar();
  QScrollBar *horizontal = edit->horizontalScrollBar();
  const int verticalValue = vertical ? vertical->value() : 0;
  const int horizontalValue = horizontal ? horizontal->value() : 0;
  const bool verticalWasAtEnd =
      vertical && vertical->maximum() > vertical->minimum() &&
      vertical->value() == vertical->maximum();
  const bool horizontalWasAtEnd =
      horizontal && horizontal->maximum() > horizontal->minimum() &&
      horizontal->value() == horizontal->maximum();

  const QTextCursor oldCursor = edit->textCursor();
  const int oldSelectionStart = oldCursor.selectionStart();
  const int oldSelectionEnd = oldCursor.selectionEnd();
  const int oldCursorPosition = oldCursor.position();

  const QSignalBlocker blocker(edit);
  edit->setPlainText(text);

  QTextCursor newCursor = edit->textCursor();
  if (oldSelectionStart != oldSelectionEnd) {
    newCursor.setPosition(ClampToDocumentPosition(oldSelectionStart, edit));
    newCursor.setPosition(ClampToDocumentPosition(oldSelectionEnd, edit),
                          QTextCursor::KeepAnchor);
  } else {
    newCursor.setPosition(ClampToDocumentPosition(oldCursorPosition, edit));
  }
  edit->setTextCursor(newCursor);

  RestoreScrollBar(vertical, verticalValue, verticalWasAtEnd);
  RestoreScrollBar(horizontal, horizontalValue, horizontalWasAtEnd);
}

} // namespace studiocast::gui
