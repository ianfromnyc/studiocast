#include "main_window.h"

#include <QAction>
#include <QApplication>
#include <QHBoxLayout>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStackedWidget>
#include <QWidget>

#include "gui/pages/audio_page.h"
#include "gui/pages/video_page.h"
#include "studiocast/version.h"

namespace studiocast::gui {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle("StudioCast");
  resize(1100, 700);

  BuildUi();
  BuildMenu();
  ConnectSignals();
}

void MainWindow::BuildUi() {
  auto* central = new QWidget(this);
  auto* layout = new QHBoxLayout(central);
  layout->setContentsMargins(12, 12, 12, 12);
  layout->setSpacing(12);

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

  setCentralWidget(central);
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
