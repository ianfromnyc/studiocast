#pragma once

#include <QString>

class QPlainTextEdit;

namespace studiocast::gui {

void SetPlainTextPreservingScroll(QPlainTextEdit *edit, const QString &text);

} // namespace studiocast::gui
