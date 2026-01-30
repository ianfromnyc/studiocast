#pragma once

#include <QWidget>

namespace studiocast::gui {

class VideoPage final : public QWidget {
  Q_OBJECT

 public:
  explicit VideoPage(QWidget* parent = nullptr);
};

}  // namespace studiocast::gui
