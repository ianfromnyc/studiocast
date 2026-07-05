#pragma once

#include <QJsonObject>
#include <QString>
#include <QWidget>

#include <string>
#include <vector>

#include "core/audio/pulse/pactl.h"

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QThread;

namespace studiocast::gui {

struct DaemonStatusSnapshot;

class AdvancedPage final : public QWidget {
  Q_OBJECT

public:
  explicit AdvancedPage(QWidget *parent = nullptr);

  void UpdateStatus(const DaemonStatusSnapshot &snapshot);

private slots:
  void OnAlwaysOnToggled(bool checked);
  void OnCreateVirtualMic();
  void OnDestroyVirtualMic();
  void OnEnableVirtualSpeakers();
  void OnStopSpeakersRouting();
  void OnDestroyVirtualSpeakers();
  void OnSaveAudioModelOverrides();
  void OnSaveVideoModelOverrides();
  void OnRefreshPulseState();
  void OnLegacySourceChanged(int index);
  void OnStartLegacyLoopback();
  void OnStopLegacyLoopback();

private:
  struct PulseRefreshResult {
    bool pactlOk = false;
    std::string pactlDetails;
    std::vector<studiocast::audio::pulse::PactlModule> modules;
    std::string moduleError;
    std::vector<studiocast::audio::pulse::PactlSourceInfo> sources;
    std::string sourceError;
    QString localAudioStatusText;
  };

  void ApplySnapshotJson(const QJsonObject &root);
  void RefreshPulseState();
  void ApplyPulseRefreshResult(const PulseRefreshResult &result);
  void RefreshLegacySources(const std::vector<
                            studiocast::audio::pulse::PactlSourceInfo>
                                &sources);
  void UpdateLegacyPorts();
  void UpdateButtonStates();

  bool SendDaemonRequest(const std::string &request, QString *error);
  bool FetchDaemonJson(const std::string &request, QJsonObject *out,
                       QString *error);
  bool SendAudioPatch(const QJsonObject &patch, QString *error);
  bool SaveAudioModelOverrides(QString *error);
  bool SaveVideoModelOverrides(QString *error);

  bool ConfirmDestructive(const QString &title, const QString &text,
                          const QString &detail,
                          const QString &confirmText);
  void SetResult(const QString &text, const QString &status);
  void ShowFailure(const QString &title, const QString &details);

  QLabel *serviceStateLabel_ = nullptr;
  QLabel *serviceDetailLabel_ = nullptr;
  QLabel *socketPathLabel_ = nullptr;
  QPushButton *copySocketButton_ = nullptr;
  QPushButton *copyRawStatusButton_ = nullptr;
  QPlainTextEdit *rawStatusText_ = nullptr;

  QLabel *cameraStateLabel_ = nullptr;
  QCheckBox *alwaysOnCheck_ = nullptr;

  QLabel *audioLifecycleLabel_ = nullptr;
  QLabel *pulseStateLabel_ = nullptr;
  QPushButton *createVirtualMicButton_ = nullptr;
  QPushButton *destroyVirtualMicButton_ = nullptr;
  QPushButton *enableVirtualSpeakersButton_ = nullptr;
  QPushButton *stopSpeakersRoutingButton_ = nullptr;
  QPushButton *destroyVirtualSpeakersButton_ = nullptr;
  QPushButton *refreshPulseButton_ = nullptr;

  QLineEdit *micModelIdEdit_ = nullptr;
  QLineEdit *micModelPathEdit_ = nullptr;
  QLineEdit *speakerModelIdEdit_ = nullptr;
  QLineEdit *speakerModelPathEdit_ = nullptr;
  QPushButton *saveAudioModelsButton_ = nullptr;

  QLineEdit *virtualBackgroundModelIdEdit_ = nullptr;
  QLineEdit *autoFrameModelIdEdit_ = nullptr;
  QLineEdit *eyeContactModelIdEdit_ = nullptr;
  QLineEdit *denoiseModelIdEdit_ = nullptr;
  QLineEdit *virtualBackgroundReplacePathEdit_ = nullptr;
  QLineEdit *virtualKeyLightHdriPathEdit_ = nullptr;
  QPushButton *saveVideoModelsButton_ = nullptr;

  QGroupBox *legacyLoopbackBox_ = nullptr;
  QComboBox *legacySourceCombo_ = nullptr;
  QComboBox *legacyPortCombo_ = nullptr;
  QSpinBox *legacyLatencySpin_ = nullptr;
  QPushButton *startLegacyLoopbackButton_ = nullptr;
  QPushButton *stopLegacyLoopbackButton_ = nullptr;
  QPlainTextEdit *localAudioStatusText_ = nullptr;

  QLabel *resultLabel_ = nullptr;

  QString currentRawStatus_;
  bool updatingUi_ = false;
  bool daemonReachable_ = false;
  bool currentAlwaysOn_ = false;
  bool pactlOk_ = false;
  bool hasVirtualMicSink_ = false;
  bool hasVirtualMicSource_ = false;
  bool hasLegacyMicLoopback_ = false;
  bool hasVirtualSpeakersSink_ = false;
  bool hasVirtualSpeakersLoopback_ = false;
  bool configuredVirtualMic_ = false;
  bool configuredVirtualSpeakers_ = false;
  bool configuredSpeakersEnabled_ = false;
  bool speakersRoutingActive_ = false;
  QThread *pulseRefreshThread_ = nullptr;

  std::vector<studiocast::audio::pulse::PactlSourceInfo> cachedSources_;
};

} // namespace studiocast::gui
