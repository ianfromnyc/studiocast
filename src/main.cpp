#include <QApplication>
#include <cstdio>
#include <string_view>

#include "gui/main_window.h"
#include "gui/theme.h"
#include "studiocast/version.h"

namespace {
bool hasArg(int argc, char** argv, std::string_view flag) {
  for (int i = 1; i < argc; ++i) {
    if (argv[i] && std::string_view(argv[i]) == flag) return true;
  }
  return false;
}
}  // namespace

int main(int argc, char** argv) {
  if (hasArg(argc, argv, "--version") || hasArg(argc, argv, "-v")) {
    std::printf("studiocast %s (%s)\n", STUDIOCAST_VERSION, STUDIOCAST_GIT_SHA);
    return 0;
  }

  QApplication app(argc, argv);
  QApplication::setApplicationName("StudioCast");
  QApplication::setOrganizationName("StudioCast");
  QApplication::setApplicationVersion(STUDIOCAST_VERSION);

  studiocast::gui::ApplyDarkTheme(app);

  studiocast::gui::MainWindow w;
  w.show();

  return app.exec();
}
