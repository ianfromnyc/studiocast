#pragma once

#include <QMainWindow>

class QFrame;
class QLabel;
class QTabBar;
class QStackedWidget;

namespace studiocast::gui {

class MainWindow final : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(QWidget* parent = nullptr);

 private:
  void BuildUi();
  void ConnectSignals();
  void BuildMenu();

  QFrame* topBar_ = nullptr;
  QLabel* brandLabel_ = nullptr;
  QTabBar* tabs_ = nullptr;
  QStackedWidget* pages_ = nullptr;
};

}  // namespace studiocast::gui
