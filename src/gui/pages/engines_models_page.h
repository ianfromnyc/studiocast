#pragma once

#include <QWidget>

class QFrame;
class QLabel;
class QPlainTextEdit;

namespace studiocast::gui {

struct DaemonStatusSnapshot;
struct EngineStatus;

class EnginesModelsPage final : public QWidget {
  Q_OBJECT

public:
  explicit EnginesModelsPage(QWidget *parent = nullptr);

  void UpdateStatus(const DaemonStatusSnapshot &snapshot);

private:
  struct EngineCard {
    QFrame *frame = nullptr;
    QLabel *title = nullptr;
    QLabel *state = nullptr;
    QLabel *summary = nullptr;
    QLabel *models = nullptr;
    QPlainTextEdit *details = nullptr;
    QPlainTextEdit *installHints = nullptr;
    QPlainTextEdit *rawDetails = nullptr;
  };

  EngineCard CreateEngineCard(const QString &title, QWidget *parent);
  void UpdateEngineCard(EngineCard *card, const EngineStatus &engine,
                        bool selectedByPreference);

  QLabel *videoPreferenceLabel_ = nullptr;
  QLabel *videoActiveLabel_ = nullptr;
  QLabel *audioPreferenceLabel_ = nullptr;
  QLabel *microphoneActiveLabel_ = nullptr;
  QLabel *speakersActiveLabel_ = nullptr;

  EngineCard maxineCard_;
  EngineCard openVideoCard_;
  EngineCard openAudioCard_;
};

} // namespace studiocast::gui
