#pragma once

#include <QMainWindow>
#include <QStringList>

class QFrame;
class QLabel;
class QListWidget;
class QStackedWidget;

namespace studiocast::gui {

struct DaemonStatusSnapshot;
class AdvancedPage;
class EnginesModelsPage;
class HomePage;
class SettingsPage;
class SupportPage;
class StatusPoller;

class MainWindow final : public QMainWindow {
  Q_OBJECT

public:
  explicit MainWindow(QWidget *parent = nullptr);

private:
  void BuildUi();
  void ConnectSignals();
  void UpdateStatus(const DaemonStatusSnapshot &snapshot);

  QFrame *sidebar_ = nullptr;
  QFrame *header_ = nullptr;
  QLabel *brandLabel_ = nullptr;
  QListWidget *nav_ = nullptr;
  QLabel *pageTitleLabel_ = nullptr;
  QLabel *serviceStateLabel_ = nullptr;
  QLabel *serviceDetailLabel_ = nullptr;
  QStackedWidget *pages_ = nullptr;
  QStringList pageTitles_;

  HomePage *homePage_ = nullptr;
  EnginesModelsPage *enginesModelsPage_ = nullptr;
  SupportPage *supportPage_ = nullptr;
  SettingsPage *settingsPage_ = nullptr;
  AdvancedPage *advancedPage_ = nullptr;

  StatusPoller *statusPoller_ = nullptr;
};

} // namespace studiocast::gui
