#include "theme.h"

#include <QApplication>
#include <QFile>
#include <QPalette>
#include <QStyleFactory>

namespace studiocast::gui {

void ApplyDarkTheme(QApplication& app) {
  // Widgets-first theme (Fusion + palette + modest QSS) for deterministic, cross-distro visuals.
  app.setStyle(QStyleFactory::create("Fusion"));

  // Keep tokens aligned with :/styles/studiocast_dark.qss
  const QColor kBg0(0x0B, 0x0F, 0x14);
  const QColor kBg1(0x0F, 0x15, 0x20);
  const QColor kSurface0(0x12, 0x1A, 0x26);
  const QColor kSurface1(0x16, 0x20, 0x32);
  const QColor kBorder(0x27, 0x32, 0x47);
  const QColor kText1(0xE7, 0xEE, 0xF8);
  const QColor kText2(0xA9, 0xB4, 0xC5);
  const QColor kAccent(0x2D, 0xD4, 0xFF);  // energetic/futuristic cyan-blue
  const QColor kDanger(0xFF, 0x5A, 0x5F);

  QPalette p;
  p.setColor(QPalette::Window, kBg0);
  p.setColor(QPalette::WindowText, kText1);
  p.setColor(QPalette::Base, kBg1);
  p.setColor(QPalette::AlternateBase, kSurface0);
  p.setColor(QPalette::ToolTipBase, kSurface0);
  p.setColor(QPalette::ToolTipText, kText1);
  p.setColor(QPalette::Text, kText1);
  p.setColor(QPalette::Button, kSurface1);
  p.setColor(QPalette::ButtonText, kText1);
  p.setColor(QPalette::BrightText, kDanger);
  p.setColor(QPalette::Link, kAccent);

  // Slightly translucent highlight makes selection look less "legacy Qt".
  p.setColor(QPalette::Highlight, QColor(kAccent.red(), kAccent.green(), kAccent.blue(), 90));
  p.setColor(QPalette::HighlightedText, kText1);

  // Disabled state.
  const QColor kDisabledText(0x6D, 0x76, 0x87);
  p.setColor(QPalette::Disabled, QPalette::Text, kDisabledText);
  p.setColor(QPalette::Disabled, QPalette::WindowText, kDisabledText);
  p.setColor(QPalette::Disabled, QPalette::ButtonText, kDisabledText);

#if QT_VERSION >= QT_VERSION_CHECK(5, 12, 0)
  p.setColor(QPalette::PlaceholderText, kText2);
#endif

  // Borders.
  p.setColor(QPalette::Mid, kBorder);
  p.setColor(QPalette::Shadow, kBorder);

  app.setPalette(p);

  // Apply our QSS from resources.
  QFile qssFile(QStringLiteral(":/styles/studiocast_dark.qss"));
  if (qssFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
    app.setStyleSheet(QString::fromUtf8(qssFile.readAll()));
  }
}

}  // namespace studiocast::gui
