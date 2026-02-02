#pragma once

#include <QMainWindow>

class QListWidget;
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

  QListWidget* nav_ = nullptr;
  QStackedWidget* pages_ = nullptr;
};

}  // namespace studiocast::gui
