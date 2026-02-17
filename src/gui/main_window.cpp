#include "main_window.h"

#include <algorithm>
#include <QAction>
#include <QApplication>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QScrollArea>
#include <QScreen>
#include <QStackedWidget>
#include <QTabBar>
#include <QVBoxLayout>
#include <QWidget>

#include "gui/pages/audio_page.h"
#include "gui/pages/video_page.h"
#include "studiocast/version.h"

namespace studiocast::gui {

namespace {
QScrollArea* WrapScrollable(QWidget* page, QWidget* parent) {
  auto* scroll = new QScrollArea(parent);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll->setWidget(page);
  return scroll;
}
}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle("StudioCast");

  // Choose a sane initial size that fits on smaller displays.
  constexpr QSize kDesiredSize{1100, 720};
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
  auto* central = new QWidget(this);
  auto* root = new QVBoxLayout(central);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  // Top bar (Broadcast-style tabs).
  topBar_ = new QFrame(central);
  topBar_->setObjectName("scTopBar");
  auto* topLayout = new QHBoxLayout(topBar_);
  topLayout->setContentsMargins(16, 12, 16, 12);
  topLayout->setSpacing(12);

  brandLabel_ = new QLabel("StudioCast", topBar_);
  brandLabel_->setProperty("scRole", "brand");
  topLayout->addWidget(brandLabel_);

  tabs_ = new QTabBar(topBar_);
  tabs_->setExpanding(false);
  tabs_->setDrawBase(false);
  tabs_->addTab("Microphone");
  tabs_->addTab("Speakers");
  tabs_->addTab("Camera");
  tabs_->setCurrentIndex(0);
  topLayout->addStretch(1);
  topLayout->addWidget(tabs_, 0, Qt::AlignCenter);
  topLayout->addStretch(1);

  root->addWidget(topBar_, 0);

  pages_ = new QStackedWidget(central);

  // Each page gets its own scroll area so the header stays fixed.
  pages_->addWidget(WrapScrollable(new AudioPage(AudioPageMode::Microphone, pages_), pages_));
  pages_->addWidget(WrapScrollable(new AudioPage(AudioPageMode::Speakers, pages_), pages_));
  pages_->addWidget(WrapScrollable(new VideoPage(pages_), pages_));

  root->addWidget(pages_, 1);

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
  connect(tabs_, &QTabBar::currentChanged, pages_, &QStackedWidget::setCurrentIndex);
}

}  // namespace studiocast::gui
