#pragma once

#include <QWidget>

#include <array>

class QFrame;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

namespace studiocast::gui {

struct DaemonStatusSnapshot;
struct DeviceReadiness;

class HomePage final : public QWidget {
  Q_OBJECT

public:
  enum class Destination {
    Camera,
    Microphone,
    Speakers,
    Engines,
    Support,
  };

  explicit HomePage(QWidget *parent = nullptr);

  void UpdateStatus(const DaemonStatusSnapshot &snapshot);

signals:
  void CameraRequested();
  void MicrophoneRequested();
  void SpeakersRequested();
  void EnginesRequested();
  void SupportRequested();

private:
  struct ReadinessCard {
    QFrame *frame = nullptr;
    QLabel *title = nullptr;
    QLabel *state = nullptr;
    QLabel *summary = nullptr;
    QLabel *detail = nullptr;
    QLineEdit *deviceName = nullptr;
    QPushButton *copyButton = nullptr;
    QPushButton *openButton = nullptr;
  };

  ReadinessCard CreateCard(const QString &title, const QString &deviceName,
                           Destination destination, QWidget *parent);
  void UpdateCard(ReadinessCard *card, const DeviceReadiness &readiness);
  void OpenDestination(Destination destination);

  QLabel *overallLabel_ = nullptr;
  QLabel *overallDetailLabel_ = nullptr;
  std::array<ReadinessCard, 3> cards_{};
  QGroupBox *repairGroup_ = nullptr;
  QVBoxLayout *repairListLayout_ = nullptr;
};

} // namespace studiocast::gui
