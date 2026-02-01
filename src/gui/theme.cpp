#include "theme.h"

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

namespace studiocast::gui {

void ApplyDarkTheme(QApplication &app) {
  // Fusion + dark palette gets us close to the Broadcast app aesthetic.
  app.setStyle(QStyleFactory::create("Fusion"));

  QPalette p;
  p.setColor(QPalette::Window, QColor(30, 30, 30));
  p.setColor(QPalette::WindowText, Qt::white);
  p.setColor(QPalette::Base, QColor(22, 22, 22));
  p.setColor(QPalette::AlternateBase, QColor(30, 30, 30));
  p.setColor(QPalette::ToolTipBase, Qt::white);
  p.setColor(QPalette::ToolTipText, Qt::white);
  p.setColor(QPalette::Text, Qt::white);
  p.setColor(QPalette::Button, QColor(38, 38, 38));
  p.setColor(QPalette::ButtonText, Qt::white);
  p.setColor(QPalette::BrightText, Qt::red);
  p.setColor(QPalette::Link, QColor(42, 130, 218));
  p.setColor(QPalette::Highlight, QColor(42, 130, 218));
  p.setColor(QPalette::HighlightedText, Qt::black);

  app.setPalette(p);
}

} // namespace studiocast::gui
