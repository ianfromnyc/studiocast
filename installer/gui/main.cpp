#include <QApplication>
#include <QMessageBox>
#include <QPalette>
#include <QStyleFactory>

#include <cstdio>
#include <string_view>

#include <unistd.h>

#include "installer_wizard.h"
#include "studiocast/version.h"

namespace {

bool hasArg(int argc, char **argv, std::string_view flag) {
  for (int i = 1; i < argc; ++i) {
    if (argv[i] && std::string_view(argv[i]) == flag) {
      return true;
    }
  }
  return false;
}

void applyInstallerTheme(QApplication &app) {
  app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

  QPalette palette;
  palette.setColor(QPalette::Window, QColor(0xF6, 0xF8, 0xFA));
  palette.setColor(QPalette::WindowText, QColor(0x16, 0x1B, 0x22));
  palette.setColor(QPalette::Base, QColor(0xFF, 0xFF, 0xFF));
  palette.setColor(QPalette::AlternateBase, QColor(0xEA, 0xEF, 0xF4));
  palette.setColor(QPalette::Text, QColor(0x16, 0x1B, 0x22));
  palette.setColor(QPalette::Button, QColor(0xEA, 0xEF, 0xF4));
  palette.setColor(QPalette::ButtonText, QColor(0x16, 0x1B, 0x22));
  palette.setColor(QPalette::Highlight, QColor(0x0B, 0x6B, 0xA8));
  palette.setColor(QPalette::HighlightedText, QColor(0xFF, 0xFF, 0xFF));
  palette.setColor(QPalette::Link, QColor(0x0B, 0x6B, 0xA8));
  app.setPalette(palette);

  app.setStyleSheet(QStringLiteral(R"(
QWizard {
  background: #F6F8FA;
}
QWizardPage {
  background: #F6F8FA;
}
QGroupBox {
  background: #FFFFFF;
  border: 1px solid #D0D7DE;
  border-radius: 6px;
  margin-top: 14px;
  padding: 12px;
}
QGroupBox::title {
  subcontrol-origin: margin;
  left: 10px;
  padding: 0 4px;
  color: #24292F;
  font-weight: 600;
}
QLineEdit,
QComboBox,
QPlainTextEdit {
  background: #FFFFFF;
  border: 1px solid #D0D7DE;
  border-radius: 4px;
  padding: 5px;
}
QPlainTextEdit {
  selection-background-color: #0969DA;
}
QPushButton {
  border: 1px solid #B8C0CA;
  border-radius: 4px;
  padding: 6px 10px;
  background: #FFFFFF;
}
QPushButton:hover {
  background: #EEF4FA;
}
QPushButton:pressed {
  background: #DDEBFA;
}
QLabel[muted="true"] {
  color: #57606A;
}
)"));
}

} // namespace

int main(int argc, char **argv) {
  if (hasArg(argc, argv, "--version") || hasArg(argc, argv, "-v")) {
    std::printf("studiocast-installer %s (%s)\n", STUDIOCAST_VERSION,
                STUDIOCAST_GIT_SHA);
    return 0;
  }

  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("StudioCast Installer"));
  QApplication::setOrganizationName(QStringLiteral("StudioCast"));
  QApplication::setApplicationVersion(QStringLiteral(STUDIOCAST_VERSION));
  applyInstallerTheme(app);

  if (geteuid() == 0) {
    QMessageBox::critical(
        nullptr, QStringLiteral("StudioCast Installer"),
        QStringLiteral("Do not run the StudioCast installer GUI as root. "
                       "Run it as your normal user; the backend will request "
                       "sudo only for specific package or kernel-module work."));
    return 2;
  }

  studiocast::installer::InstallerWizard wizard;
  wizard.show();
  return app.exec();
}
