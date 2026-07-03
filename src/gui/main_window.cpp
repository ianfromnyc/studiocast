#include "main_window.h"

#include <QAction>
#include <QApplication>
#include <QFrame>
#include <QGuiApplication>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QScreen>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStyle>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>

#include "gui/pages/audio_page.h"
#include "gui/pages/engines_models_page.h"
#include "gui/pages/home_page.h"
#include "gui/pages/support_page.h"
#include "gui/pages/video_page.h"
#include "gui/status/daemon_status_snapshot.h"
#include "gui/status/status_poller.h"
#include "studiocast/version.h"

namespace studiocast::gui {

namespace {
QScrollArea *WrapScrollable(QWidget *page, QWidget *parent) {
  auto *scroll = new QScrollArea(parent);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll->setWidget(page);
  return scroll;
}

QLabel *MutedLabel(const QString &text, QWidget *parent) {
  auto *label = new QLabel(text, parent);
  label->setProperty("scRole", "muted");
  label->setWordWrap(true);
  return label;
}

QLabel *ValueLabel(QWidget *parent) {
  auto *label = new QLabel(parent);
  label->setProperty("scRole", "value");
  label->setWordWrap(true);
  return label;
}

QFrame *StatusRow(const QString &name, QLabel **valueOut, QWidget *parent) {
  auto *row = new QFrame(parent);
  row->setProperty("scRole", "statusRow");
  auto *layout = new QHBoxLayout(row);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(12);

  auto *nameLabel = MutedLabel(name, row);
  nameLabel->setMinimumWidth(120);
  layout->addWidget(nameLabel, 0);

  auto *valueLabel = ValueLabel(row);
  layout->addWidget(valueLabel, 1);

  if (valueOut)
    *valueOut = valueLabel;
  return row;
}

QWidget *PendingPage(const QString &body, QWidget *parent) {
  auto *page = new QWidget(parent);
  auto *layout = new QVBoxLayout(page);
  layout->setContentsMargins(16, 16, 16, 16);
  layout->setSpacing(12);

  auto *box = new QGroupBox(QStringLiteral("Status"), page);
  auto *boxLayout = new QVBoxLayout(box);
  boxLayout->setSpacing(10);
  boxLayout->addWidget(MutedLabel(body, box));
  layout->addWidget(box);
  layout->addStretch(1);
  return page;
}

QPlainTextEdit *RawStatusBox(QWidget *parent) {
  auto *text = new QPlainTextEdit(parent);
  text->setReadOnly(true);
  text->setLineWrapMode(QPlainTextEdit::NoWrap);
  text->setMinimumHeight(240);
  text->setPlaceholderText(QStringLiteral("Daemon status has not been read."));
  return text;
}

QString RawStatusText(const DaemonStatusSnapshot &snapshot) {
  if (!snapshot.rawJson.isEmpty())
    return snapshot.rawJson;
  if (!snapshot.transportError.isEmpty())
    return QStringLiteral("Daemon unavailable: %1").arg(snapshot.transportError);
  if (!snapshot.parseError.isEmpty())
    return QStringLiteral("Daemon status parse error: %1")
        .arg(snapshot.parseError);
  return QStringLiteral("Daemon status has not been read.");
}

void SetStatusProperty(QWidget *widget, const QString &value) {
  widget->setProperty("scStatus", value);
  widget->style()->unpolish(widget);
  widget->style()->polish(widget);
}

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
  setWindowTitle("StudioCast");

  // Choose a sane initial size that fits on smaller displays.
  constexpr QSize kDesiredSize{1180, 760};
  const QScreen *screen = QGuiApplication::primaryScreen();
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

  statusPoller_ = new StatusPoller(this);
  connect(statusPoller_, &StatusPoller::StatusChanged, this,
          &MainWindow::UpdateStatus);
  statusPoller_->Start(2000);
}

void MainWindow::BuildUi() {
  auto *central = new QWidget(this);
  auto *root = new QHBoxLayout(central);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  sidebar_ = new QFrame(central);
  sidebar_->setObjectName("scSidebar");
  sidebar_->setFixedWidth(220);
  auto *sideLayout = new QVBoxLayout(sidebar_);
  sideLayout->setContentsMargins(14, 14, 14, 14);
  sideLayout->setSpacing(14);

  brandLabel_ = new QLabel("StudioCast", sidebar_);
  brandLabel_->setProperty("scRole", "brand");
  sideLayout->addWidget(brandLabel_);

  nav_ = new QListWidget(sidebar_);
  nav_->setProperty("scRole", "nav");
  nav_->setFrameShape(QFrame::NoFrame);
  nav_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  nav_->setSelectionMode(QAbstractItemView::SingleSelection);

  pageTitles_ = {
      QStringLiteral("Home"),
      QStringLiteral("Camera"),
      QStringLiteral("Microphone"),
      QStringLiteral("Speakers"),
      QStringLiteral("Engines & Models"),
      QStringLiteral("Support"),
      QStringLiteral("Settings"),
      QStringLiteral("Advanced"),
  };
  nav_->addItems(pageTitles_);
  nav_->setCurrentRow(0);
  sideLayout->addWidget(nav_, 1);

  root->addWidget(sidebar_, 0);

  auto *content = new QWidget(central);
  auto *contentLayout = new QVBoxLayout(content);
  contentLayout->setContentsMargins(0, 0, 0, 0);
  contentLayout->setSpacing(0);

  header_ = new QFrame(content);
  header_->setObjectName("scPageHeader");
  auto *headerLayout = new QHBoxLayout(header_);
  headerLayout->setContentsMargins(18, 12, 18, 12);
  headerLayout->setSpacing(12);

  pageTitleLabel_ = new QLabel(pageTitles_.value(0), header_);
  pageTitleLabel_->setProperty("scRole", "title");
  headerLayout->addWidget(pageTitleLabel_, 1);

  auto *serviceBox = new QFrame(header_);
  serviceBox->setObjectName("scServiceStatus");
  auto *serviceLayout = new QVBoxLayout(serviceBox);
  serviceLayout->setContentsMargins(10, 6, 10, 6);
  serviceLayout->setSpacing(2);
  serviceStateLabel_ = new QLabel(QStringLiteral("Checking service"), serviceBox);
  serviceStateLabel_->setProperty("scRole", "value");
  serviceDetailLabel_ = MutedLabel(QString(), serviceBox);
  serviceLayout->addWidget(serviceStateLabel_);
  serviceLayout->addWidget(serviceDetailLabel_);
  headerLayout->addWidget(serviceBox, 0);

  contentLayout->addWidget(header_, 0);

  pages_ = new QStackedWidget(content);

  homePage_ = new HomePage(pages_);
  pages_->addWidget(WrapScrollable(homePage_, pages_));

  // Existing device pages stay intact for Milestone 1.
  // TODO(gui-reface): Add microphone meters only after daemon-provided meter
  // data exists.
  // TODO(gui-reface): Add speaker test tone only after backend support exists.
  pages_->addWidget(WrapScrollable(new VideoPage(pages_), pages_));
  pages_->addWidget(
      WrapScrollable(new AudioPage(AudioPageMode::Microphone, pages_), pages_));
  pages_->addWidget(
      WrapScrollable(new AudioPage(AudioPageMode::Speakers, pages_), pages_));

  enginesModelsPage_ = new EnginesModelsPage(pages_);
  pages_->addWidget(WrapScrollable(enginesModelsPage_, pages_));

  supportPage_ = new SupportPage(pages_);
  pages_->addWidget(WrapScrollable(supportPage_, pages_));

  pages_->addWidget(WrapScrollable(
      PendingPage(QStringLiteral("Normal settings and scoped resets will move "
                                 "here in a later milestone. Current "
                                 "write-through controls remain on the device "
                                 "pages."),
                  pages_),
      pages_));

  auto *advanced = new QWidget(pages_);
  auto *advancedLayout = new QVBoxLayout(advanced);
  advancedLayout->setContentsMargins(16, 16, 16, 16);
  advancedLayout->setSpacing(12);
  auto *advancedBox =
      new QGroupBox(QStringLiteral("Daemon Details"), advanced);
  auto *advancedBoxLayout = new QVBoxLayout(advancedBox);
  advancedBoxLayout->setSpacing(10);
  advancedBoxLayout->addWidget(
      StatusRow(QStringLiteral("Socket"), &advancedSocketLabel_, advancedBox));
  advancedRawStatus_ = RawStatusBox(advancedBox);
  advancedBoxLayout->addWidget(advancedRawStatus_);
  advancedLayout->addWidget(advancedBox);
  advancedLayout->addStretch(1);
  pages_->addWidget(WrapScrollable(advanced, pages_));

  contentLayout->addWidget(pages_, 1);
  root->addWidget(content, 1);

  setCentralWidget(central);
}

void MainWindow::BuildMenu() {
  auto *helpMenu = menuBar()->addMenu("&Help");

  auto *aboutAction = new QAction("&About", this);
  connect(aboutAction, &QAction::triggered, this, [] {
    QMessageBox::about(
        nullptr, "About StudioCast",
        QString("StudioCast %1 (%2)\n\n"
                "An open-source Linux app with a Broadcast-style UI.\n"
                "Not affiliated with NVIDIA.\n")
            .arg(STUDIOCAST_VERSION)
            .arg(STUDIOCAST_GIT_SHA));
  });

  helpMenu->addAction(aboutAction);
}

void MainWindow::ConnectSignals() {
  auto navigateTo = [this](const QString &title) {
    for (qsizetype row = 0; row < pageTitles_.size(); ++row) {
      if (pageTitles_.at(row) == title) {
        nav_->setCurrentRow(static_cast<int>(row));
        return;
      }
    }
  };

  connect(homePage_, &HomePage::CameraRequested, this,
          [navigateTo] { navigateTo(QStringLiteral("Camera")); });
  connect(homePage_, &HomePage::MicrophoneRequested, this,
          [navigateTo] { navigateTo(QStringLiteral("Microphone")); });
  connect(homePage_, &HomePage::SpeakersRequested, this,
          [navigateTo] { navigateTo(QStringLiteral("Speakers")); });
  connect(homePage_, &HomePage::EnginesRequested, this,
          [navigateTo] { navigateTo(QStringLiteral("Engines & Models")); });
  connect(homePage_, &HomePage::SupportRequested, this,
          [navigateTo] { navigateTo(QStringLiteral("Support")); });

  connect(nav_, &QListWidget::currentRowChanged, this, [this](int row) {
    if (row < 0 || row >= pages_->count())
      return;
    pages_->setCurrentIndex(row);
    pageTitleLabel_->setText(pageTitles_.value(row));
  });
}

void MainWindow::UpdateStatus(const DaemonStatusSnapshot &snapshot) {
  serviceStateLabel_->setText(snapshot.ServiceSummary());
  serviceDetailLabel_->setText(snapshot.ServiceDetail());

  if (!snapshot.reachable || !snapshot.parsed) {
    SetStatusProperty(serviceStateLabel_, QStringLiteral("error"));
  } else if (!snapshot.serviceRunning) {
    SetStatusProperty(serviceStateLabel_, QStringLiteral("warning"));
  } else {
    SetStatusProperty(serviceStateLabel_, QStringLiteral("good"));
  }

  if (homePage_)
    homePage_->UpdateStatus(snapshot);
  if (enginesModelsPage_)
    enginesModelsPage_->UpdateStatus(snapshot);
  if (supportPage_)
    supportPage_->UpdateStatus(snapshot);

  if (advancedSocketLabel_) {
    advancedSocketLabel_->setText(snapshot.socketPath.isEmpty()
                                      ? QStringLiteral("Unknown")
                                      : snapshot.socketPath);
  }

  const QString raw = RawStatusText(snapshot);
  if (advancedRawStatus_)
    advancedRawStatus_->setPlainText(raw);
}

} // namespace studiocast::gui
