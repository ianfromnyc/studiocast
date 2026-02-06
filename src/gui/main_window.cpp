#include "main_window.h"

#include <algorithm>
#include <QAction>
#include <QApplication>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLayout>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QScrollArea>
#include <QScreen>
#include <QStackedWidget>
#include <QWidget>

#include "gui/pages/audio_page.h"
#include "gui/pages/video_page.h"
#include "studiocast/version.h"

namespace studiocast::gui {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle("StudioCast");

  // Choose a sane initial size that fits on smaller displays.
  // If the UI ends up taller than the screen, the central scroll area will
  // allow reaching everything.
  constexpr QSize kDesiredSize{1100, 700};
  const QScreen* screen = QGuiApplication::primaryScreen();
  if (screen) {
    const QSize avail = screen->availableGeometry().size();
    const int w = std::min(kDesiredSize.width(), avail.width() * 95 / 100);
    const int h = std::min(kDesiredSize.height(), avail.height() * 95 / 100);
    resize(std::max(1, w), std::max(1, h));
  } else {
    resize(kDesiredSize);
  }

  BuildUi();
  BuildMenu();
  ConnectSignals();
}

void MainWindow::BuildUi() {
  auto* scrollArea = new QScrollArea(this);
  scrollArea->setWidgetResizable(true);

  auto* central = new QWidget(scrollArea);
  auto* layout = new QHBoxLayout(central);
  layout->setContentsMargins(12, 12, 12, 12);
  layout->setSpacing(12);
  layout->setSizeConstraint(QLayout::SetMinimumSize);

  nav_ = new QListWidget(central);
  nav_->setFixedWidth(220);
  nav_->addItem("Microphone");
  nav_->addItem("Camera");
  nav_->setCurrentRow(0);

  pages_ = new QStackedWidget(central);
  pages_->addWidget(new AudioPage(pages_));
  pages_->addWidget(new VideoPage(pages_));

  layout->addWidget(nav_);
  layout->addWidget(pages_, /*stretch*/ 1);

  scrollArea->setWidget(central);
  setCentralWidget(scrollArea);
}

void MainWindow::BuildMenu() {
  auto* helpMenu = menuBar()->addMenu("&Help");

  auto* aboutAction = new QAction("&About", this);
  connect(aboutAction, &QAction::triggered, this, [] {
    QMessageBox::about(nullptr, "About StudioCast",
                       QString("StudioCast %1 (%2)\n\n"
                               "An open-source Linux app with a Broadcast-style UI.\n"
                               "Not affiliated with NVIDIA.\n")
                           .arg(STUDIOCAST_VERSION)
                           .arg(STUDIOCAST_GIT_SHA));
  });

  helpMenu->addAction(aboutAction);
}

void MainWindow::ConnectSignals() {
  connect(nav_, &QListWidget::currentRowChanged, pages_, &QStackedWidget::setCurrentIndex);
}

}  // namespace studiocast::gui
