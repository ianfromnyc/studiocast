#pragma once

#include <QJsonObject>
#include <QStringList>
#include <QWidget>

#include <vector>

#include "core/audio/pulse/pactl.h"

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

namespace studiocast::gui {

enum class AudioPageMode {
  Microphone,
  Speakers,
};

class AudioPage final : public QWidget {
  Q_OBJECT

public:
  explicit AudioPage(AudioPageMode mode, QWidget *parent = nullptr);

private slots:
  void RefreshSources();
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

  void OnToggleAdvanced(bool checked);

private:
  void ShowError(const QString &title, const QString &details);

  void RefreshDaemonAudioStatus();
  void PushDaemonAudioConfig();
  void PushDaemonSourceSelection();
  void SetAiControlsEnabled(bool enabled, const QString &reason);

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
  QGroupBox *micEffectsBox_ = nullptr;
  QComboBox *sourceCombo_ = nullptr;
  QPushButton *refreshSourcesBtn_ = nullptr;

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
  QPushButton *enableSpeakersBtn_ = nullptr;
  QPushButton *stopSpeakersBtn_ = nullptr;
  QPushButton *destroySpeakersBtn_ = nullptr;

  // --- Status
  QGroupBox *statusBox_ = nullptr;
  QPlainTextEdit *statusText_ = nullptr;
  QPushButton *refreshStatusBtn_ = nullptr;

  // --- Shared state
  bool updatingAiUi_ = false;
  bool daemonAiSupported_ = false;
  QString daemonAiDisableReason_;
  QString daemonStatusText_;

  // Last speaker routing status reported by the daemon.
  bool daemonSpeakersRoutingActive_ = false;
  QString daemonSpeakersRouteMode_;

  // Cached daemon effects blob so we can preserve fields not represented in
  // this UI.
  QJsonObject lastAudioEffectsObj_;

  // Cached Open Audio diagnostics (for install hints dialog).
  bool openAudioStatusPresent_ = false;
  bool openAudioOk_ = false;
  QStringList openAudioInstallHints_;

  QTimer *pollTimer_ = nullptr;
};

} // namespace studiocast::gui
