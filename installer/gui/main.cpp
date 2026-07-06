#include <QApplication>
#include <QMessageBox>

#include <cstdio>
#include <string_view>

#include <unistd.h>

#include "gui/theme.h"
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
  studiocast::gui::ApplyDarkTheme(app);

  // The installer uses Qt's QWizard shell, which the main app does not. Keep
  // this bridge limited to wizard-only selectors while reusing the app theme.
  app.setStyleSheet(app.styleSheet() + QStringLiteral(R"(
QWizard {
  background: #0B0F14;
}
QWizardPage {
  background: #0B0F14;
}
QWizard QLabel {
  background: transparent;
}
QWizard QFrame[scRole="separator"] {
  background: #273247;
  border: none;
  min-height: 1px;
  max-height: 1px;
}
QWizard QProgressBar[scRole="installerPreferenceProgress"] {
  background: transparent;
  border: none;
  min-height: 2px;
  max-height: 2px;
  margin-bottom: 8px;
}
QWizard QProgressBar[scRole="installerPreferenceProgress"]::chunk {
  background-color: #2DD4FF;
  border: none;
}
QRadioButton {
  spacing: 8px;
}
QRadioButton::indicator {
  width: 18px;
  height: 18px;
  border-radius: 9px;
  border: 1px solid #273247;
  background: #0F1520;
}
QRadioButton::indicator:checked {
  background: #2DD4FF;
  border: 5px solid #0F1520;
}
QRadioButton::indicator:checked:hover {
  background: #5BE2FF;
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
