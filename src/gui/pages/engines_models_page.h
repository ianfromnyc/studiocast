#pragma once

#include <QStringList>
#include <QWidget>

class QFrame;
class QLabel;
class QPlainTextEdit;
class QProcess;
class QPushButton;

namespace studiocast::gui {

struct DaemonStatusSnapshot;
struct EngineStatus;

class EnginesModelsPage final : public QWidget {
  Q_OBJECT

public:
  explicit EnginesModelsPage(QWidget *parent = nullptr);

  void UpdateStatus(const DaemonStatusSnapshot &snapshot);

signals:
  void ModelsInstallFinished();

private:
  struct EngineCard {
    QFrame *frame = nullptr;
    QLabel *title = nullptr;
    QLabel *state = nullptr;
    QLabel *summary = nullptr;
    QLabel *setupDisclaimer = nullptr;
    QLabel *models = nullptr;
    QPushButton *downloadButton = nullptr;
    QLabel *downloadStatus = nullptr;
    QPlainTextEdit *details = nullptr;
    QPlainTextEdit *installHints = nullptr;
    QPlainTextEdit *rawDetails = nullptr;
    QString engineId;
    QStringList installArgs;
    bool installRecommended = false;
  };

  EngineCard CreateEngineCard(const QString &title, const QString &engineId,
                              QWidget *parent);
  void UpdateEngineCard(EngineCard *card, const EngineStatus &engine,
                        bool selectedByPreference);
  void StartModelInstall(EngineCard *card);
  void RefreshDownloadButtons();
  QString ResolveInstallScript(QString *error) const;

  QLabel *videoPreferenceLabel_ = nullptr;
  QLabel *videoActiveLabel_ = nullptr;
  QLabel *audioPreferenceLabel_ = nullptr;
  QLabel *microphoneActiveLabel_ = nullptr;
  QLabel *speakersActiveLabel_ = nullptr;

  EngineCard maxineCard_;
  EngineCard openVideoCard_;
  EngineCard openAudioCard_;

  QProcess *modelInstallProcess_ = nullptr;
  EngineCard *activeInstallCard_ = nullptr;
  QString modelInstallOutput_;
};

} // namespace studiocast::gui
