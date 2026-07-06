#pragma once

#include <QProcess>
#include <QStringList>
#include <QWidget>

class QFrame;
class QLabel;
class QPlainTextEdit;
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
  void SetupRepairFinished();

private:
  enum class SetupRepairPhase {
    Plan,
    Execute,
  };

  struct EngineCard {
    QFrame *frame = nullptr;
    QLabel *title = nullptr;
    QLabel *state = nullptr;
    QLabel *summary = nullptr;
    QFrame *setupDisclaimerBanner = nullptr;
    QLabel *setupDisclaimer = nullptr;
    QPushButton *repairSetupButton = nullptr;
    QLabel *repairSetupStatus = nullptr;
    QLabel *models = nullptr;
    QPushButton *downloadButton = nullptr;
    QLabel *downloadStatus = nullptr;
    QPlainTextEdit *details = nullptr;
    QPlainTextEdit *installHints = nullptr;
    QPlainTextEdit *rawDetails = nullptr;
    QString engineId;
    QStringList installArgs;
    bool installRecommended = false;
    QStringList repairArgs;
    bool repairRecommended = false;
  };

  EngineCard CreateEngineCard(const QString &title, const QString &engineId,
                              QWidget *parent);
  void UpdateEngineCard(EngineCard *card, const EngineStatus &engine,
                        bool selectedByPreference);
  void StartModelInstall(EngineCard *card);
  void StartSetupRepair(EngineCard *card);
  void StartSetupRepairProcess(SetupRepairPhase phase,
                               const QStringList &arguments,
                               const QString &statusText);
  void AppendSetupRepairOutput(const QByteArray &bytes);
  void AppendSetupRepairErrorOutput(const QByteArray &bytes);
  void PromptForSetupRepairPassword();
  void FinishSetupRepairPlan(int exitCode, QProcess::ExitStatus exitStatus);
  void FinishSetupRepairExecution(int exitCode,
                                  QProcess::ExitStatus exitStatus);
  void RefreshDownloadButtons();
  QString ResolveInstallScript(QString *error) const;
  QString ResolveInstallerBackend(QString *error) const;

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

  QProcess *setupRepairProcess_ = nullptr;
  EngineCard *activeRepairCard_ = nullptr;
  QString setupRepairBackend_;
  QString setupRepairOutput_;
  QString setupRepairPlanText_;
  QString setupRepairPromptBuffer_;
  bool setupRepairPasswordDialogOpen_ = false;
  bool setupRepairPasswordCancelled_ = false;
};

} // namespace studiocast::gui
