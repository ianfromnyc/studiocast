#pragma once

#include <QJsonObject>
#include <QStringList>
#include <QWidget>

#include <optional>
#include <string>
#include <vector>

#include "core/audio/pulse/pactl.h"
#include "gui/status/pending_daemon_write_guard.h"

class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class QToolButton;
class QTimer;
class QThread;

namespace studiocast::gui {

struct DaemonStatusSnapshot;

enum class AudioPageMode {
  Microphone,
  Speakers,
};

class AudioPage final : public QWidget {
  Q_OBJECT

public:
  explicit AudioPage(AudioPageMode mode, QWidget *parent = nullptr);
  void UpdateStatus(const DaemonStatusSnapshot &snapshot);

signals:
  void StatusRefreshRequested();

private slots:
  void RefreshSources();
  void RefreshSpeakerTargets();
  void RefreshStatus();

  void OnAiStrengthChanged(int v);
  void OnAiSpeakerStrengthChanged(int v);

  void OnAiEngineChanged(int index);

  void OnMicEffectChanged(int index);
  void OnSpeakerEffectChanged(int index);

  void OnAiOpenAudioModelChanged(int index);
  void OnAiOpenAudioModelPathEdited();
  void OnAiBrowseOpenAudioModel();
  void OnOpenAudioInstallHints();

  void OnAiSpeakerOpenAudioModelChanged(int index);
  void OnAiSpeakerOpenAudioModelPathEdited();
  void OnAiSpeakerBrowseOpenAudioModel();

  void OnCreateVirtualMic();
  void OnDestroyVirtualMic();
  void OnStartLoopback();
  void OnStopLoopback();
  void OnSourceChanged(int index);

  void OnEnableVirtualSpeakers();
  void OnStopSpeakersRouting();
  void OnDestroyVirtualSpeakers();
  void OnSpeakerTargetChanged(int index);

  void OnToggleAdvanced(bool checked);

private:
  struct SourceRefreshResult {
    bool pactlOk = false;
    std::string pactlDetails;
    std::vector<studiocast::audio::pulse::PactlSourceInfo> sources;
    std::optional<std::string> defaultSource;
    std::string listError;
  };

  struct SpeakerTargetRefreshResult {
    bool pactlOk = false;
    std::string pactlDetails;
    std::vector<studiocast::audio::pulse::PactlSink> sinks;
    std::optional<std::string> defaultSink;
    std::string listError;
  };

  void ShowError(const QString &title, const QString &details);

  void ApplySourceRefreshResult(const SourceRefreshResult &result);
  void ApplySpeakerTargetRefreshResult(
      const SpeakerTargetRefreshResult &result);
  void RefreshStatusFromCachedDaemon(bool forceControlResync);
  void ApplyCachedDaemonAudioStatus(bool forceControlResync = false);
  void ScheduleDaemonAudioConfigWrite();
  void UpdateReleaseControlsFromCachedStatus();
  void PushDaemonAudioConfig();
  void PushDaemonSourceSelection();
  void SetAiControlsEnabled(bool enabled, const QString &reason);
  void SetMicStatusSummary(const QString &state, const QString &detail,
                           const QString &status);
  void SetSpeakerRouteSummary(const QString &state, const QString &detail,
                              const QString &status,
                              const QString &routeMode,
                              const QString &target);

  void SyncSourceSelectionFromDaemon(const QString &source);
  void SyncSpeakerTargetSelectionFromDaemon(const QString &target);
  void UpdatePortControlsForSelectedSource(bool pushDaemon);
  void UpdateEngineUiVisibility();

  void SetAdvancedVisible(bool visible);

  AudioPageMode mode_ = AudioPageMode::Microphone;

  // --- Shared high-level sections
  QLabel *titleLabel_ = nullptr;
  QToolButton *advancedToggle_ = nullptr;

  QGroupBox *backendBox_ = nullptr;

  // Daemon-driven AFX controls (MVP).
  QLabel *aiBanner_ = nullptr;

  // Backend selection (mirrors video_page.cpp "Effect engine").
  QComboBox *engineCombo_ = nullptr;
  QLabel *engineActiveValue_ = nullptr;

  // Informational banner for backend selection/fallback notes.
  QLabel *aiInfoBanner_ = nullptr;

  // --- Microphone UI (may be nullptr in Speakers mode)
  QGroupBox *micSourceBox_ = nullptr;
  QGroupBox *micEffectsBox_ = nullptr;
  QGroupBox *micDetailsBox_ = nullptr;
  QWidget *micDetailsContent_ = nullptr;
  QComboBox *sourceCombo_ = nullptr;
  QPushButton *refreshSourcesBtn_ = nullptr;
  QLabel *micStateLabel_ = nullptr;
  QLabel *micDetailLabel_ = nullptr;
  QLabel *micSourceStatusLabel_ = nullptr;

  // Broadcast-like single effect selector.
  QComboBox *micEffectCombo_ = nullptr;

  QSlider *strengthSlider_ = nullptr;
  QLabel *strengthValueLabel_ = nullptr;

  // Open-source model selection (Open Audio packs) - microphone.
  QLabel *openAudioModelLabel_ = nullptr;
  QComboBox *openAudioModelCombo_ = nullptr;
  QLabel *openAudioModelPathLabel_ = nullptr;
  QLineEdit *openAudioModelPathEdit_ = nullptr;
  QPushButton *browseOpenAudioModelBtn_ = nullptr;
  QPushButton *openAudioInstallHintsBtn_ = nullptr;

  // Advanced / legacy loopback and virtual device controls (microphone).
  QGroupBox *legacyInputBox_ = nullptr;
  QGroupBox *vmicBox_ = nullptr;

  QComboBox *portCombo_ = nullptr;
  QSpinBox *latencySpin_ = nullptr;
  std::vector<studiocast::audio::pulse::PactlSourceInfo> cachedSources_;

  QPushButton *createBtn_ = nullptr;
  QPushButton *destroyBtn_ = nullptr;
  QPushButton *startBtn_ = nullptr;
  QPushButton *stopBtn_ = nullptr;

  // --- Speakers UI (may be nullptr in Microphone mode)
  QGroupBox *speakerRouteStateBox_ = nullptr;
  QLabel *speakerRouteStateLabel_ = nullptr;
  QLabel *speakerRouteDetailLabel_ = nullptr;
  QLabel *speakerRouteModeValue_ = nullptr;
  QLabel *speakerRouteTargetValue_ = nullptr;

  QGroupBox *speakerEffectsBox_ = nullptr;
  QComboBox *speakerEffectCombo_ = nullptr;

  QSlider *speakerStrengthSlider_ = nullptr;
  QLabel *speakerStrengthValueLabel_ = nullptr;

  // Open-source model selection - speakers.
  QLabel *speakerOpenAudioModelLabel_ = nullptr;
  QComboBox *speakerOpenAudioModelCombo_ = nullptr;
  QLabel *speakerOpenAudioModelPathLabel_ = nullptr;
  QLineEdit *speakerOpenAudioModelPathEdit_ = nullptr;
  QPushButton *speakerBrowseOpenAudioModelBtn_ = nullptr;

  QGroupBox *speakersBox_ = nullptr;
  QComboBox *speakerTargetCombo_ = nullptr;
  QPushButton *refreshSpeakerTargetsBtn_ = nullptr;
  QLabel *speakerTargetStatusLabel_ = nullptr;
  QPushButton *enableSpeakersBtn_ = nullptr;
  QPushButton *stopSpeakersBtn_ = nullptr;
  QPushButton *destroySpeakersBtn_ = nullptr;
  QGroupBox *speakerDetailsBox_ = nullptr;
  QWidget *speakerDetailsContent_ = nullptr;
  QGroupBox *speakerLifecycleBox_ = nullptr;

  // --- Status
  QGroupBox *statusBox_ = nullptr;
  QPlainTextEdit *statusText_ = nullptr;
  QPushButton *refreshStatusBtn_ = nullptr;

  // --- Shared state
  bool updatingAiUi_ = false;
  bool updatingSourceUi_ = false;
  bool updatingSpeakerTargetUi_ = false;
  bool daemonAiSupported_ = false;
  QString daemonAiDisableReason_;
  QString daemonStatusText_;
  QString daemonStatusDetail_;
  QString daemonLastStatusJson_;
  QString daemonSource_;

  // Last speaker routing status reported by the daemon.
  bool daemonSpeakersRoutingActive_ = false;
  bool daemonMicVirtualDevicePresent_ = false;
  bool daemonSpeakersVirtualDevicePresent_ = false;
  QString daemonMicrophoneAction_;
  QString daemonSpeakersAction_;
  QString daemonSpeakersRouteMode_;
  QString daemonSpeakerTarget_;

  // Cached daemon effects blob so we can preserve fields not represented in
  // this UI.
  QJsonObject lastAudioEffectsObj_;

  // Cached Open Audio diagnostics (for install hints dialog).
  bool openAudioStatusPresent_ = false;
  bool openAudioOk_ = false;
  QStringList openAudioInstallHints_;

  QTimer *audioWriteDebounceTimer_ = nullptr;
  PendingDaemonWriteGuard audioWriteGuard_;
  QThread *sourceRefreshThread_ = nullptr;
  QThread *speakerTargetRefreshThread_ = nullptr;
};

} // namespace studiocast::gui
