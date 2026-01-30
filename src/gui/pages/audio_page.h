#pragma once

#include <QWidget>

namespace studiocast::gui {

class AudioPage final : public QWidget {
  Q_OBJECT

 public:
  explicit AudioPage(QWidget* parent = nullptr);
};

}  // namespace studiocast::gui
