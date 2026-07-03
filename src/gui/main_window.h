#pragma once

#include <QMainWindow>
#include <QStringList>

class QFrame;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QStackedWidget;

namespace studiocast::gui {

struct DaemonStatusSnapshot;
class HomePage;
class StatusPoller;

class MainWindow final : public QMainWindow {
  Q_OBJECT

public:
  explicit MainWindow(QWidget *parent = nullptr);

private:
  void BuildUi();
  void ConnectSignals();
  void BuildMenu();
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
  QLabel *maxineHealthLabel_ = nullptr;
  QLabel *openVideoHealthLabel_ = nullptr;
  QLabel *openAudioHealthLabel_ = nullptr;
  QLabel *advancedSocketLabel_ = nullptr;
  QPlainTextEdit *supportRawStatus_ = nullptr;
  QPlainTextEdit *advancedRawStatus_ = nullptr;

  StatusPoller *statusPoller_ = nullptr;
};

} // namespace studiocast::gui
