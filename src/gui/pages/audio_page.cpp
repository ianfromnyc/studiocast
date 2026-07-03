#include "audio_page.h"

#include <QComboBox>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "core/audio/effects/broadcast_audio_effects.h"
#include "core/audio/pulse/pactl.h"
#include "core/audio/virtual_mic.h"
#include "core/audio/virtual_speaker.h"
#include "core/ipc/daemon_client.h"
#include "core/maxine/reason_codes.h"

namespace studiocast::gui {
namespace {
constexpr const char *kStudioCastVirtualMicName = "studiocast_mic";
constexpr const char *kAutoPulseSource = "auto";

bool IsBadLoopbackSourceCandidate(const std::string &name) {
  // Avoid obvious feedback loops / non-mic candidates:
  // - our own virtual mic
  // - monitor sources (sink monitors)
  if (name == kStudioCastVirtualMicName)
    return true;
  if (name.find(".monitor") != std::string::npos)
    return true;
  return false;
}

bool Contains(const std::string &hay, const std::string &needle) {
  return hay.find(needle) != std::string::npos;
}

bool ParseJsonObject(const std::string &json, QJsonObject *outRoot,
                     QString *error) {
  QJsonParseError perr;
  const auto doc =
      QJsonDocument::fromJson(QByteArray::fromStdString(json), &perr);
  if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
    if (error)
      *error = "JSON parse error: " + perr.errorString();
    return false;
  }
  if (outRoot)
    *outRoot = doc.object();
  return true;
}

QString FormatMaxineReasonCode(const QString &code) {
  if (code.isEmpty())
    return {};
  const std::string s = code.toStdString();
  return QString::fromStdString(studiocast::maxine::reasons::ToEnglish(s));
}

bool DaemonRequest(const std::string &request, std::string *outJson,
                   QString *outErr) {
  studiocast::ipc::DaemonCallResult res;
  studiocast::ipc::DaemonCallOptions options;
  options.connect_timeout_ms = 500;
  options.io_timeout_ms = 2500;
  std::string err;
  if (!studiocast::ipc::DaemonCall(request, &res, &err, options)) {
    if (outErr)
      *outErr = QString::fromStdString(err);
    return false;
  }
  if (!res.ok) {
    if (outErr)
      *outErr = QString::fromStdString(res.error_json.empty() ? "daemon_error"
                                                              : res.error_json);
    return false;
  }
  if (outJson)
    *outJson = res.json;
  return true;
}
QString FirstLine(const QString &s) {
  const QString t = s.trimmed();
  const int nl = t.indexOf('\n');
  if (nl < 0)
    return t;
  return t.left(nl).trimmed();
}

QString FriendlyBackendLabel(const QString &id) {
  const QString v = id.trimmed().toLower();
  if (v.isEmpty())
    return QStringLiteral("—");
  if (v == QStringLiteral("maxine"))
    return QStringLiteral("Maxine");
  if (v == QStringLiteral("open_source") || v == QStringLiteral("open_audio"))
    return QStringLiteral("Open Source");
  if (v == QStringLiteral("passthrough"))
    return QStringLiteral("Pass-through");
  if (v == QStringLiteral("loopback"))
    return QStringLiteral("Loopback");
  if (v == QStringLiteral("pipeline"))
    return QStringLiteral("Pipeline");
  if (v == QStringLiteral("off"))
    return QStringLiteral("Off");
  return id;
}

} // namespace

AudioPage::AudioPage(AudioPageMode mode, QWidget *parent)
    : QWidget(parent), mode_(mode) {
  auto *root = new QVBoxLayout(this);
  root->setSpacing(12);
  root->setContentsMargins(16, 16, 16, 16);

  // -----------------------
  // Title row
  // -----------------------
  {
    auto *titleRow = new QHBoxLayout();
    titleRow->setContentsMargins(0, 0, 0, 0);

    titleLabel_ = new QLabel(
        mode_ == AudioPageMode::Microphone ? "Microphone" : "Speakers", this);
    titleLabel_->setProperty("scRole", "title");
    titleRow->addWidget(titleLabel_);

    titleRow->addStretch(1);

    advancedToggle_ = new QToolButton(this);
    advancedToggle_->setText("Advanced");
    advancedToggle_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    advancedToggle_->setCheckable(true);
    advancedToggle_->setChecked(false);
    titleRow->addWidget(advancedToggle_);

    root->addLayout(titleRow);
  }

  // -----------------------
  // Backend / availability
  // -----------------------
  backendBox_ = new QGroupBox("AI backend", this);
  {
    auto *backendLayout = new QVBoxLayout(backendBox_);

    auto *engineRow = new QHBoxLayout();
    engineRow->addWidget(new QLabel("Backend:", backendBox_));
    engineCombo_ = new QComboBox(backendBox_);
    engineCombo_->addItem("Auto", "auto");
    engineCombo_->addItem("Maxine", "maxine");
    engineCombo_->addItem("Open Source", "open_source");
    engineCombo_->addItem("Off", "off");
    engineRow->addWidget(engineCombo_);

    engineRow->addSpacing(12);
    engineRow->addWidget(new QLabel("Active:", backendBox_));
    engineActiveValue_ = new QLabel("—", backendBox_);
    engineActiveValue_->setProperty("scRole", "value");
    engineRow->addWidget(engineActiveValue_);
    engineRow->addStretch(1);
    backendLayout->addLayout(engineRow);

    aiInfoBanner_ = new QLabel(backendBox_);
    aiInfoBanner_->setWordWrap(true);
    aiInfoBanner_->setProperty("scBanner", "info");
    aiInfoBanner_->setVisible(false);
    backendLayout->addWidget(aiInfoBanner_);

    aiBanner_ = new QLabel(backendBox_);
    aiBanner_->setWordWrap(true);
    // Used for "daemon unavailable" and similar top-level issues.
    aiBanner_->setProperty("scBanner", "warning");
    aiBanner_->setVisible(false);
    backendLayout->addWidget(aiBanner_);

    openAudioInstallHintsBtn_ =
        new QPushButton("Open Audio install hints", backendBox_);
    openAudioInstallHintsBtn_->setEnabled(false);
    backendLayout->addWidget(openAudioInstallHintsBtn_, 0, Qt::AlignLeft);
  }
  root->addWidget(backendBox_);

  // -----------------------
  // Mode-specific content
  // -----------------------
  if (mode_ == AudioPageMode::Microphone) {
    // -----------------------
    // Microphone effects
    // -----------------------
    micEffectsBox_ = new QGroupBox("Microphone effects", this);
    {
      auto *aiLayout = new QVBoxLayout(micEffectsBox_);

      // Device selection
      auto *devRow = new QHBoxLayout();
      devRow->addWidget(new QLabel("Input:", micEffectsBox_));
      sourceCombo_ = new QComboBox(micEffectsBox_);
      sourceCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
      devRow->addWidget(sourceCombo_, 1);
      refreshSourcesBtn_ = new QPushButton("Refresh", micEffectsBox_);
      devRow->addWidget(refreshSourcesBtn_);
      aiLayout->addLayout(devRow);

      // Open-source model selection (only shown when Open Source is
      // active/selected).
      auto *modelRow = new QHBoxLayout();
      openAudioModelLabel_ = new QLabel("Model:", micEffectsBox_);
      openAudioModelCombo_ = new QComboBox(micEffectsBox_);
      openAudioModelCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
      openAudioModelCombo_->addItem("<auto>", "");
      modelRow->addWidget(openAudioModelLabel_);
      modelRow->addWidget(openAudioModelCombo_, 1);
      aiLayout->addLayout(modelRow);

      auto *modelPathRow = new QHBoxLayout();
      openAudioModelPathLabel_ = new QLabel("Model path:", micEffectsBox_);
      openAudioModelPathEdit_ = new QLineEdit(micEffectsBox_);
      openAudioModelPathEdit_->setPlaceholderText(
          "(optional) /path/to/model.onnx");
      browseOpenAudioModelBtn_ = new QPushButton("Browse…", micEffectsBox_);
      modelPathRow->addWidget(openAudioModelPathLabel_);
      modelPathRow->addWidget(openAudioModelPathEdit_, 1);
      modelPathRow->addWidget(browseOpenAudioModelBtn_);
      aiLayout->addLayout(modelPathRow);

      // Effect selection (Broadcast-style single selector).
      auto *effectRow = new QHBoxLayout();
      effectRow->addWidget(new QLabel("Effect:", micEffectsBox_));
      micEffectCombo_ = new QComboBox(micEffectsBox_);
      micEffectCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
      micEffectCombo_->addItem("Off", "off");
      micEffectCombo_->addItem("Noise removal", "noise");
      micEffectCombo_->addItem("Room echo removal", "echo");
      micEffectCombo_->addItem("Noise + echo (combined)", "noise_echo");
      micEffectCombo_->addItem("Studio voice", "studio_voice");
      effectRow->addWidget(micEffectCombo_, 1);
      aiLayout->addLayout(effectRow);

      // Strength
      auto *strengthRow = new QHBoxLayout();
      strengthRow->addWidget(new QLabel("Strength:", micEffectsBox_));
      strengthSlider_ = new QSlider(Qt::Horizontal, micEffectsBox_);
      strengthSlider_->setRange(0, 100);
      strengthSlider_->setValue(50);
      strengthRow->addWidget(strengthSlider_, 1);
      strengthValueLabel_ = new QLabel("50", micEffectsBox_);
      strengthValueLabel_->setMinimumWidth(32);
      strengthValueLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
      strengthRow->addWidget(strengthValueLabel_);
      aiLayout->addLayout(strengthRow);

      auto *tip = new QLabel("Tip: Set Effect to Off for pass-through. In "
                             "other apps, choose “StudioCast Microphone”.",
                             micEffectsBox_);
      tip->setWordWrap(true);
      tip->setProperty("scRole", "muted");
      aiLayout->addWidget(tip);
    }
    root->addWidget(micEffectsBox_);

    // -----------------------
    // Legacy loopback controls (advanced)
    // -----------------------
    legacyInputBox_ = new QGroupBox("Input (legacy loopback)", this);
    {
      auto *inputLayout = new QVBoxLayout(legacyInputBox_);

      auto *portRow = new QHBoxLayout();
      portRow->addWidget(new QLabel("Input port:", legacyInputBox_));
      portCombo_ = new QComboBox(legacyInputBox_);
      portRow->addWidget(portCombo_, 1);
      inputLayout->addLayout(portRow);

      auto *latencyRow = new QHBoxLayout();
      latencyRow->addWidget(new QLabel("Latency (ms):", legacyInputBox_));
      latencySpin_ = new QSpinBox(legacyInputBox_);
      latencySpin_->setRange(1, 200);
      latencySpin_->setValue(10);
      latencyRow->addWidget(latencySpin_);
      latencyRow->addStretch(1);
      inputLayout->addLayout(latencyRow);

      auto *loopbackButtons = new QHBoxLayout();
      startBtn_ = new QPushButton("Start loopback", legacyInputBox_);
      stopBtn_ = new QPushButton("Stop loopback", legacyInputBox_);
      loopbackButtons->addWidget(startBtn_);
      loopbackButtons->addWidget(stopBtn_);
      loopbackButtons->addStretch(1);
      inputLayout->addLayout(loopbackButtons);

      inputLayout->addWidget(
          new QLabel("Legacy path: module-loopback (debug/dev only).\n"
                     "The preferred path is the daemon pipeline feeding the "
                     "StudioCast virtual devices.",
                     legacyInputBox_));
    }
    root->addWidget(legacyInputBox_);

    // -----------------------
    // Virtual mic controls (advanced)
    // -----------------------
    vmicBox_ = new QGroupBox("StudioCast Virtual Microphone", this);
    {
      auto *vmicLayout = new QVBoxLayout(vmicBox_);

      auto *buttonsRow = new QHBoxLayout();
      createBtn_ = new QPushButton("Create virtual mic", vmicBox_);
      destroyBtn_ = new QPushButton("Destroy virtual mic", vmicBox_);

      buttonsRow->addWidget(createBtn_);
      buttonsRow->addWidget(destroyBtn_);
      buttonsRow->addStretch(1);
      vmicLayout->addLayout(buttonsRow);

      vmicLayout->addWidget(
          new QLabel("Tip: In other apps, select “StudioCast Microphone”.\n"
                     "Processed feed: the audio pipeline plays into "
                     "“StudioCast Sink”, and apps should use\n"
                     "“StudioCast Microphone” (sink monitor).",
                     vmicBox_));
    }
    root->addWidget(vmicBox_);
  } else {
    // -----------------------
    // Speaker effects
    // -----------------------
    speakerEffectsBox_ = new QGroupBox("Speaker effects", this);
    {
      auto *spkLayout = new QVBoxLayout(speakerEffectsBox_);

      // Open-source model selection (only shown when Open Source is
      // active/selected).
      auto *modelRow = new QHBoxLayout();
      speakerOpenAudioModelLabel_ = new QLabel("Model:", speakerEffectsBox_);
      speakerOpenAudioModelCombo_ = new QComboBox(speakerEffectsBox_);
      speakerOpenAudioModelCombo_->setSizeAdjustPolicy(
          QComboBox::AdjustToContents);
      speakerOpenAudioModelCombo_->addItem("<auto>", "");
      modelRow->addWidget(speakerOpenAudioModelLabel_);
      modelRow->addWidget(speakerOpenAudioModelCombo_, 1);
      spkLayout->addLayout(modelRow);

      auto *modelPathRow = new QHBoxLayout();
      speakerOpenAudioModelPathLabel_ =
          new QLabel("Model path:", speakerEffectsBox_);
      speakerOpenAudioModelPathEdit_ = new QLineEdit(speakerEffectsBox_);
      speakerOpenAudioModelPathEdit_->setPlaceholderText(
          "(optional) /path/to/model.onnx");
      speakerBrowseOpenAudioModelBtn_ =
          new QPushButton("Browse…", speakerEffectsBox_);
      modelPathRow->addWidget(speakerOpenAudioModelPathLabel_);
      modelPathRow->addWidget(speakerOpenAudioModelPathEdit_, 1);
      modelPathRow->addWidget(speakerBrowseOpenAudioModelBtn_);
      spkLayout->addLayout(modelPathRow);

      // Effect selection (Broadcast-style single selector).
      auto *effectRow = new QHBoxLayout();
      effectRow->addWidget(new QLabel("Effect:", speakerEffectsBox_));
      speakerEffectCombo_ = new QComboBox(speakerEffectsBox_);
      speakerEffectCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
      speakerEffectCombo_->addItem("Off", "off");
      speakerEffectCombo_->addItem("Noise removal", "noise");
      speakerEffectCombo_->addItem("Room echo removal", "echo");
      speakerEffectCombo_->addItem("Noise + echo (combined)", "noise_echo");
      effectRow->addWidget(speakerEffectCombo_, 1);
      spkLayout->addLayout(effectRow);

      auto *strengthRow = new QHBoxLayout();
      strengthRow->addWidget(new QLabel("Strength:", speakerEffectsBox_));
      speakerStrengthSlider_ = new QSlider(Qt::Horizontal, speakerEffectsBox_);
      speakerStrengthSlider_->setRange(0, 100);
      speakerStrengthSlider_->setValue(50);
      strengthRow->addWidget(speakerStrengthSlider_, 1);
      speakerStrengthValueLabel_ = new QLabel("50", speakerEffectsBox_);
      speakerStrengthValueLabel_->setMinimumWidth(32);
      speakerStrengthValueLabel_->setAlignment(Qt::AlignRight |
                                               Qt::AlignVCenter);
      strengthRow->addWidget(speakerStrengthValueLabel_);
      spkLayout->addLayout(strengthRow);

      spkLayout->addWidget(new QLabel("Speaker effects apply to audio routed "
                                      "through “StudioCast Speakers”.",
                                      speakerEffectsBox_));
    }
    root->addWidget(speakerEffectsBox_);

    // -----------------------
    // Virtual speakers controls
    // -----------------------
    speakersBox_ = new QGroupBox("StudioCast Speakers", this);
    {
      auto *spkLayout = new QVBoxLayout(speakersBox_);

      auto *buttonsRow = new QHBoxLayout();
      enableSpeakersBtn_ =
          new QPushButton("Enable speakers device", speakersBox_);
      enableSpeakersBtn_->setProperty("scVariant", "primary");
      stopSpeakersBtn_ = new QPushButton("Stop routing", speakersBox_);
      destroySpeakersBtn_ =
          new QPushButton("Destroy speakers device", speakersBox_);
      destroySpeakersBtn_->setProperty("scVariant", "danger");

      buttonsRow->addWidget(enableSpeakersBtn_);
      buttonsRow->addWidget(stopSpeakersBtn_);
      buttonsRow->addWidget(destroySpeakersBtn_);
      buttonsRow->addStretch(1);
      spkLayout->addLayout(buttonsRow);

      spkLayout->addWidget(new QLabel(
          "Tip: In other apps, select “StudioCast Speakers” as the output "
          "device.\n"
          "StudioCast will route that audio to your physical speakers.",
          speakersBox_));
    }
    root->addWidget(speakersBox_);
  }

  // -----------------------
  // Status (advanced)
  // -----------------------
  statusBox_ = new QGroupBox("Status", this);
  {
    auto *statusLayout = new QVBoxLayout(statusBox_);

    statusText_ = new QPlainTextEdit(statusBox_);
    statusText_->setReadOnly(true);
    statusText_->setMinimumHeight(220);
    statusLayout->addWidget(statusText_, 1);

    auto *buttonsRow = new QHBoxLayout();
    refreshStatusBtn_ = new QPushButton("Refresh status", statusBox_);
    buttonsRow->addWidget(refreshStatusBtn_);
    buttonsRow->addStretch(1);
    statusLayout->addLayout(buttonsRow);
  }
  root->addWidget(statusBox_);
  root->addStretch(1);

  // -----------------------
  // Wiring
  // -----------------------
  connect(advancedToggle_, &QToolButton::toggled, this,
          &AudioPage::OnToggleAdvanced);

  if (refreshSourcesBtn_)
    connect(refreshSourcesBtn_, &QPushButton::clicked, this,
            &AudioPage::RefreshSources);
  if (refreshStatusBtn_)
    connect(refreshStatusBtn_, &QPushButton::clicked, this,
            &AudioPage::RefreshStatus);

  if (engineCombo_) {
    connect(engineCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AudioPage::OnAiEngineChanged);
  }
  if (openAudioModelCombo_) {
    connect(openAudioModelCombo_,
            QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &AudioPage::OnAiOpenAudioModelChanged);
  }
  if (openAudioModelPathEdit_) {
    connect(openAudioModelPathEdit_, &QLineEdit::editingFinished, this,
            &AudioPage::OnAiOpenAudioModelPathEdited);
  }
  if (browseOpenAudioModelBtn_) {
    connect(browseOpenAudioModelBtn_, &QPushButton::clicked, this,
            &AudioPage::OnAiBrowseOpenAudioModel);
  }
  if (openAudioInstallHintsBtn_) {
    connect(openAudioInstallHintsBtn_, &QPushButton::clicked, this,
            &AudioPage::OnOpenAudioInstallHints);
  }

  if (micEffectCombo_) {
    connect(micEffectCombo_,
            QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &AudioPage::OnMicEffectChanged);
  }
  if (strengthSlider_)
    connect(strengthSlider_, &QSlider::valueChanged, this,
            &AudioPage::OnAiStrengthChanged);

  if (speakerEffectCombo_) {
    connect(speakerEffectCombo_,
            QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &AudioPage::OnSpeakerEffectChanged);
  }
  if (speakerStrengthSlider_)
    connect(speakerStrengthSlider_, &QSlider::valueChanged, this,
            &AudioPage::OnAiSpeakerStrengthChanged);
  if (speakerOpenAudioModelCombo_) {
    connect(speakerOpenAudioModelCombo_,
            QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &AudioPage::OnAiSpeakerOpenAudioModelChanged);
  }
  if (speakerOpenAudioModelPathEdit_) {
    connect(speakerOpenAudioModelPathEdit_, &QLineEdit::editingFinished, this,
            &AudioPage::OnAiSpeakerOpenAudioModelPathEdited);
  }
  if (speakerBrowseOpenAudioModelBtn_) {
    connect(speakerBrowseOpenAudioModelBtn_, &QPushButton::clicked, this,
            &AudioPage::OnAiSpeakerBrowseOpenAudioModel);
  }

  if (createBtn_)
    connect(createBtn_, &QPushButton::clicked, this,
            &AudioPage::OnCreateVirtualMic);
  if (destroyBtn_)
    connect(destroyBtn_, &QPushButton::clicked, this,
            &AudioPage::OnDestroyVirtualMic);
  if (startBtn_)
    connect(startBtn_, &QPushButton::clicked, this,
            &AudioPage::OnStartLoopback);
  if (stopBtn_)
    connect(stopBtn_, &QPushButton::clicked, this, &AudioPage::OnStopLoopback);

  if (enableSpeakersBtn_)
    connect(enableSpeakersBtn_, &QPushButton::clicked, this,
            &AudioPage::OnEnableVirtualSpeakers);
  if (stopSpeakersBtn_)
    connect(stopSpeakersBtn_, &QPushButton::clicked, this,
            &AudioPage::OnStopSpeakersRouting);
  if (destroySpeakersBtn_)
    connect(destroySpeakersBtn_, &QPushButton::clicked, this,
            &AudioPage::OnDestroyVirtualSpeakers);

  if (sourceCombo_) {
    connect(sourceCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AudioPage::OnSourceChanged);
  }

  // Poll status so the UI reflects external changes (user unloads modules,
  // etc.)
  pollTimer_ = new QTimer(this);
  pollTimer_->setInterval(1500);
  connect(pollTimer_, &QTimer::timeout, this, &AudioPage::RefreshStatus);
  pollTimer_->start();

  SetAdvancedVisible(false);

  // Initial state.
  if (mode_ == AudioPageMode::Microphone) {
    RefreshSources();
  }
  RefreshStatus();

#ifdef NDEBUG
  // In release/production builds we do not support pass-through routing via
  // module-loopback. The processed feed is expected to come from the audio
  // pipeline.
  if (legacyInputBox_) {
    legacyInputBox_->setTitle(
        "Input (legacy loopback - disabled in release builds)");
    legacyInputBox_->setEnabled(false);
  }
  if (startBtn_)
    startBtn_->setVisible(false);
  if (stopBtn_)
    stopBtn_->setVisible(false);
#endif
}

void AudioPage::ShowError(const QString &title, const QString &details) {
  QMessageBox::critical(this, title, details);
}

void AudioPage::OnToggleAdvanced(bool checked) { SetAdvancedVisible(checked); }

void AudioPage::SetAdvancedVisible(bool visible) {
  // Advanced items are intentionally per-mode.
  if (legacyInputBox_)
    legacyInputBox_->setVisible(visible);
  if (vmicBox_)
    vmicBox_->setVisible(visible);
  if (statusBox_)
    statusBox_->setVisible(visible);
}

void AudioPage::RefreshSources() {
  if (!sourceCombo_)
    return;

  updatingSourceUi_ = true;
  sourceCombo_->blockSignals(true);
  sourceCombo_->clear();
  if (portCombo_) {
    portCombo_->clear();
    portCombo_->setEnabled(false);
  }
  cachedSources_.clear();

  std::string pactlDetails;
  if (!studiocast::audio::pulse::PactlAvailable(&pactlDetails)) {
    sourceCombo_->addItem("pactl not available");
    sourceCombo_->setEnabled(false);
    sourceCombo_->blockSignals(false);
    updatingSourceUi_ = false;
    ShowError("Audio", QString("pactl not available.\n\nDetails:\n%1")
                           .arg(QString::fromStdString(pactlDetails)));
    return;
  }

  sourceCombo_->setEnabled(true);
  sourceCombo_->addItem("Auto (Pulse default)",
                        QVariant(QString::fromLatin1(kAutoPulseSource)));

  std::string err;
  cachedSources_ = studiocast::audio::pulse::ListSourcesDetailed(&err);
  if (!err.empty() && statusText_) {
    // Non-fatal warning, but useful
    statusText_->setPlainText(QString::fromStdString("Warning: " + err));
  }

  std::optional<std::string> defaultSource;
  {
    std::string derr;
    defaultSource = studiocast::audio::pulse::GetDefaultSourceName(&derr);
  }

  const QString daemonSource = daemonSource_.trimmed();
  int defaultIndex = 0;
  int daemonIndex = -1;
  int added = 1;

  for (const auto &s : cachedSources_) {
    if (s.name.empty())
      continue;
    if (IsBadLoopbackSourceCandidate(s.name))
      continue;

    const std::string label = s.description.empty() ? s.name : s.description;

    // Display label, but store raw source name in item data.
    sourceCombo_->addItem(QString::fromStdString(label),
                          QVariant(QString::fromStdString(s.name)));

    if (defaultSource && s.name == *defaultSource) {
      defaultIndex = added;
    }
    if (!daemonSource.isEmpty() &&
        daemonSource != QString::fromLatin1(kAutoPulseSource) &&
        s.name == daemonSource.toStdString()) {
      daemonIndex = added;
    }
    ++added;
  }

  if (added == 1) {
    sourceCombo_->addItem("<no suitable sources found>");
  }

  int targetIndex = 0;
  if (daemonSource.isEmpty() ||
      daemonSource == QString::fromLatin1(kAutoPulseSource)) {
    targetIndex = 0;
  } else if (daemonIndex >= 0) {
    targetIndex = daemonIndex;
  } else {
    const int insertAt = std::min(1, sourceCombo_->count());
    sourceCombo_->insertItem(insertAt,
                             QStringLiteral("<missing: %1>").arg(daemonSource),
                             QVariant(daemonSource));
    targetIndex = insertAt;
  }

  if (daemonSource.isEmpty() && defaultIndex >= 0) {
    // Before the first daemon status arrives, show the current Pulse default
    // without writing it back to the daemon.
    targetIndex = defaultIndex;
  }

  sourceCombo_->setCurrentIndex(targetIndex);
  sourceCombo_->blockSignals(false);
  updatingSourceUi_ = false;
  UpdatePortControlsForSelectedSource(false);
}

void AudioPage::OnSourceChanged(int /*index*/) {
  if (updatingSourceUi_)
    return;
  UpdatePortControlsForSelectedSource(true);
}

void AudioPage::UpdatePortControlsForSelectedSource(bool pushDaemon) {
  if (!sourceCombo_)
    return;
  if (portCombo_) {
    portCombo_->clear();
    portCombo_->setEnabled(false);
  }

  if (!sourceCombo_->isEnabled())
    return;

  const std::string srcName =
      sourceCombo_->currentData().toString().toStdString();
  if (srcName.empty())
    return;
  if (srcName == kAutoPulseSource) {
    if (pushDaemon)
      PushDaemonSourceSelection();
    return;
  }

  const studiocast::audio::pulse::PactlSourceInfo *info = nullptr;
  for (const auto &s : cachedSources_) {
    if (s.name == srcName) {
      info = &s;
      break;
    }
  }
  if (!info)
    return;

  if (!portCombo_) {
    // Port selection is an advanced/legacy control.
    if (pushDaemon)
      PushDaemonSourceSelection();
    return;
  }

  if (info->ports.empty()) {
    // Many sources will have no explicit ports; that's fine.
    portCombo_->setEnabled(false);
    if (pushDaemon)
      PushDaemonSourceSelection();
    return;
  }

  portCombo_->setEnabled(true);

  int activeIdx = -1;
  int firstAvailable = -1;

  for (std::size_t i = 0; i < info->ports.size(); ++i) {
    const int idx = static_cast<int>(i);
    const auto &p = info->ports[i];

    std::string label = p.description.empty() ? p.name : p.description;
    if (!p.available)
      label += " (unavailable)";

    portCombo_->addItem(QString::fromStdString(label),
                        QVariant(QString::fromStdString(p.name)));

    if (!info->active_port.empty() && p.name == info->active_port)
      activeIdx = idx;
    if (firstAvailable < 0 && p.available)
      firstAvailable = idx;
  }

  if (activeIdx >= 0)
    portCombo_->setCurrentIndex(activeIdx);
  else if (firstAvailable >= 0)
    portCombo_->setCurrentIndex(firstAvailable);
  else
    portCombo_->setCurrentIndex(0);

  if (pushDaemon)
    PushDaemonSourceSelection();
}

void AudioPage::SyncSourceSelectionFromDaemon(const QString &source) {
  QString wanted = source.trimmed();
  if (wanted.isEmpty())
    wanted = QString::fromLatin1(kAutoPulseSource);
  daemonSource_ = wanted;

  if (!sourceCombo_ || !sourceCombo_->isEnabled())
    return;

  updatingSourceUi_ = true;
  sourceCombo_->blockSignals(true);
  int idx = sourceCombo_->findData(wanted);
  if (idx < 0 && wanted != QString::fromLatin1(kAutoPulseSource)) {
    for (int i = sourceCombo_->count() - 1; i >= 0; --i) {
      if (sourceCombo_->itemText(i).startsWith(QStringLiteral("<missing:"))) {
        sourceCombo_->removeItem(i);
      }
    }
    idx = sourceCombo_->findData(wanted);
    if (idx < 0) {
      const int insertAt = std::min(1, sourceCombo_->count());
      sourceCombo_->insertItem(insertAt,
                               QStringLiteral("<missing: %1>").arg(wanted),
                               QVariant(wanted));
      idx = insertAt;
    }
  }

  if (idx < 0)
    idx = 0;

  sourceCombo_->setCurrentIndex(idx);
  sourceCombo_->blockSignals(false);
  updatingSourceUi_ = false;
  UpdatePortControlsForSelectedSource(false);
}

void AudioPage::RefreshStatus() {
  // Always refresh daemon status; it also syncs UI state.
  RefreshDaemonAudioStatus();

  if (statusText_) {
    QString text = QString::fromStdString(studiocast::audio::StatusText());
    if (!daemonStatusText_.isEmpty()) {
      text += "\n\n---\nDaemon audio status:\n";
      text += daemonStatusText_;
    }
    statusText_->setPlainText(text);
  }

  // Enable/disable buttons based on current loaded modules (best-effort).
  std::string pactlDetails;
  const bool pactlOk = studiocast::audio::pulse::PactlAvailable(&pactlDetails);

  if (createBtn_)
    createBtn_->setEnabled(pactlOk);
  if (destroyBtn_)
    destroyBtn_->setEnabled(pactlOk);
  if (startBtn_)
    startBtn_->setEnabled(pactlOk);
  if (stopBtn_)
    stopBtn_->setEnabled(pactlOk);

  if (enableSpeakersBtn_)
    enableSpeakersBtn_->setEnabled(pactlOk);
  if (stopSpeakersBtn_)
    stopSpeakersBtn_->setEnabled(pactlOk);
  if (destroySpeakersBtn_)
    destroySpeakersBtn_->setEnabled(pactlOk);

  if (!pactlOk)
    return;

  std::string err;
  const auto mods = studiocast::audio::pulse::ListModules(&err);

  bool hasSink = false;
  bool hasRemap = false;
  bool hasLoopback = false;

  bool hasSpeakersSink = false;
  bool hasSpeakersLoopback = false;

  for (const auto &m : mods) {
    if (m.name == "module-null-sink" &&
        Contains(m.args, "sink_name=studiocast_sink")) {
      hasSink = true;
    }
    if (m.name == "module-remap-source" &&
        Contains(m.args, "source_name=studiocast_mic")) {
      hasRemap = true;
    }
    if (m.name == "module-loopback" &&
        Contains(m.args, "sink=studiocast_sink")) {
      hasLoopback = true;
    }

    if (m.name == "module-null-sink" &&
        Contains(m.args, "sink_name=studiocast_speakers")) {
      hasSpeakersSink = true;
    }
    if (m.name == "module-loopback" &&
        Contains(m.args, "source=studiocast_speakers.monitor")) {
      hasSpeakersLoopback = true;
    }
  }

  // Create is always safe; disable Destroy if nothing exists.
  if (destroyBtn_)
    destroyBtn_->setEnabled(hasSink || hasRemap);
  if (startBtn_)
    startBtn_->setEnabled(hasSink && hasRemap);
  if (stopBtn_)
    stopBtn_->setEnabled(hasLoopback);

  if (destroySpeakersBtn_)
    destroySpeakersBtn_->setEnabled(hasSpeakersSink);
  if (stopSpeakersBtn_)
    stopSpeakersBtn_->setEnabled(hasSpeakersLoopback ||
                                 daemonSpeakersRoutingActive_);
}

void AudioPage::SetAiControlsEnabled(bool enabled, const QString &reason) {
  daemonAiSupported_ = enabled;
  daemonAiDisableReason_ = reason;

  if (engineCombo_)
    engineCombo_->setEnabled(enabled);

  // Microphone open-source controls
  if (openAudioModelCombo_)
    openAudioModelCombo_->setEnabled(enabled);
  if (openAudioModelPathEdit_)
    openAudioModelPathEdit_->setEnabled(enabled);
  if (browseOpenAudioModelBtn_)
    browseOpenAudioModelBtn_->setEnabled(enabled);

  // Speaker open-source controls
  if (speakerOpenAudioModelCombo_)
    speakerOpenAudioModelCombo_->setEnabled(enabled);
  if (speakerOpenAudioModelPathEdit_)
    speakerOpenAudioModelPathEdit_->setEnabled(enabled);
  if (speakerBrowseOpenAudioModelBtn_)
    speakerBrowseOpenAudioModelBtn_->setEnabled(enabled);

  // Microphone controls
  if (micEffectCombo_)
    micEffectCombo_->setEnabled(enabled);
  if (strengthSlider_)
    strengthSlider_->setEnabled(enabled);
  if (strengthValueLabel_)
    strengthValueLabel_->setEnabled(enabled);

  // Speaker controls
  if (speakerEffectCombo_)
    speakerEffectCombo_->setEnabled(enabled);
  if (speakerStrengthSlider_)
    speakerStrengthSlider_->setEnabled(enabled);
  if (speakerStrengthValueLabel_)
    speakerStrengthValueLabel_->setEnabled(enabled);

  if (aiBanner_) {
    aiBanner_->setVisible(!enabled && !reason.isEmpty());
    aiBanner_->setText(reason);
  }

  // Avoid stacked banners: when we show a warning, hide the info note.
  if (aiInfoBanner_ && (!enabled || (aiBanner_ && aiBanner_->isVisible()))) {
    aiInfoBanner_->setVisible(false);
    aiInfoBanner_->setToolTip(QString());
  }

  // Disable strength sliders when effect is Off (Broadcast-like).
  if (enabled) {
    if (micEffectCombo_ && strengthSlider_) {
      const bool active = micEffectCombo_->currentData().toString() != "off";
      strengthSlider_->setEnabled(active);
      if (strengthValueLabel_)
        strengthValueLabel_->setEnabled(active);
    }
    if (speakerEffectCombo_ && speakerStrengthSlider_) {
      const bool active =
          speakerEffectCombo_->currentData().toString() != "off";
      speakerStrengthSlider_->setEnabled(active);
      if (speakerStrengthValueLabel_)
        speakerStrengthValueLabel_->setEnabled(active);
    }
  }

  UpdateEngineUiVisibility();
}

void AudioPage::UpdateEngineUiVisibility() {
  const QString eng = engineCombo_ ? engineCombo_->currentData().toString()
                                   : QStringLiteral("auto");

  // AUTO behaves like Maxine when Maxine is actually active; only surface
  // open-source controls when Open Source is explicitly selected, or when AUTO
  // has fallen back to Open Source.
  bool activeIsOpen = false;
  if (engineActiveValue_) {
    QString active = engineActiveValue_->toolTip();
    if (active.isEmpty())
      active = engineActiveValue_->text();
    active = active.trimmed().toLower();
    activeIsOpen =
        (active == "open_source") || (active == "open_audio") ||
        (active == "open_cuda") || (active == "open_video") ||
        (active == "open source") || active.contains("open_source") ||
        active.contains("open_audio") || active.contains("open_cuda") ||
        active.contains("open_video") || active.contains("open source");
  }

  const bool showOpen =
      (eng == "open_source") || (eng == "auto" && activeIsOpen);

  // Microphone model controls
  if (openAudioModelLabel_)
    openAudioModelLabel_->setVisible(showOpen);
  if (openAudioModelCombo_)
    openAudioModelCombo_->setVisible(showOpen);
  if (openAudioModelPathLabel_)
    openAudioModelPathLabel_->setVisible(showOpen);
  if (openAudioModelPathEdit_)
    openAudioModelPathEdit_->setVisible(showOpen);
  if (browseOpenAudioModelBtn_)
    browseOpenAudioModelBtn_->setVisible(showOpen);

  // Speaker model controls
  if (speakerOpenAudioModelLabel_)
    speakerOpenAudioModelLabel_->setVisible(showOpen);
  if (speakerOpenAudioModelCombo_)
    speakerOpenAudioModelCombo_->setVisible(showOpen);
  if (speakerOpenAudioModelPathLabel_)
    speakerOpenAudioModelPathLabel_->setVisible(showOpen);
  if (speakerOpenAudioModelPathEdit_)
    speakerOpenAudioModelPathEdit_->setVisible(showOpen);
  if (speakerBrowseOpenAudioModelBtn_)
    speakerBrowseOpenAudioModelBtn_->setVisible(showOpen);

  // Install hints are only relevant when using Open Source.
  if (openAudioInstallHintsBtn_)
    openAudioInstallHintsBtn_->setVisible(showOpen);
}

void AudioPage::RefreshDaemonAudioStatus() {
  daemonStatusText_.clear();
  daemonSpeakersRoutingActive_ = false;
  daemonSpeakersRouteMode_.clear();

  std::string json;
  QString err;
  if (!DaemonRequest("GET_STATUS", &json, &err)) {
    SetAiControlsEnabled(false, "Daemon unavailable: " + err);
    daemonStatusText_ = "daemon_unavailable: " + err;
    return;
  }

  QJsonObject root;
  QString jerr;
  if (!ParseJsonObject(json, &root, &jerr)) {
    SetAiControlsEnabled(false, "Daemon returned invalid JSON: " + jerr);
    daemonStatusText_ = "invalid_json";
    return;
  }

  // Keep Maxine diagnostics for user guidance, but do not hard-disable the UI.
  // The runtime resolver handles Maxine/OpenAudio selection and fallback.
  QString maxineDiag;
  {
    const auto maxine = root.value("maxine").toObject();
    const bool supported = maxine.value("supported").toBool(false);
    const QString summary = maxine.value("summary").toString();
    const QString blockedReason = maxine.value("blocked_reason").toString();
    const auto blockedDetails = maxine.value("blocked_details").toArray();

    bool gpuOk = true;
    if (maxine.contains("gpu")) {
      gpuOk = maxine.value("gpu").toObject().value("ok").toBool(true);
    }

    bool afxOk = true;
    if (maxine.contains("afx")) {
      afxOk = maxine.value("afx").toObject().value("ok").toBool(true);
    } else if (maxine.contains("components")) {
      afxOk = maxine.value("components")
                  .toObject()
                  .value("afx")
                  .toObject()
                  .value("found")
                  .toBool(true);
    }

    if (!supported || !gpuOk || !afxOk) {
      maxineDiag = "maxine_unavailable: true\n";
      if (!summary.isEmpty())
        maxineDiag += "maxine_summary: " + summary + "\n";
      const auto english = FormatMaxineReasonCode(blockedReason);
      if (!english.isEmpty())
        maxineDiag += "maxine_reason: " + english + "\n";
      if (!blockedDetails.isEmpty()) {
        maxineDiag += "maxine_details:\n";
        for (const auto &v : blockedDetails) {
          maxineDiag += "- " + v.toString() + "\n";
        }
      }
    }
  }

  SetAiControlsEnabled(true, "");

  const auto audio = root.value("audio").toObject();
  const bool audioEnabled = audio.value("enabled").toBool(false);
  SyncSourceSelectionFromDaemon(audio.value("source").toString());
  const QString micMode = audio.value("mic_mode").toString();
  const auto pipeline = audio.value("pipeline").toObject();
  const bool running = pipeline.value("running").toBool(false);
  const bool starting = pipeline.value("starting").toBool(false);
  const QString lastErr = pipeline.value("last_error").toString();
  const QString backendActive = pipeline.value("backend_active").toString();
  const QString effectsNote = pipeline.value("effects_note").toString();

  // Tab-specific summary: in Speakers mode, prefer the speakers pipeline
  // backend and note rather than the microphone pipeline.
  QString backendForUi = backendActive;
  QString noteForUi = effectsNote;
  if (mode_ == AudioPageMode::Speakers) {
    backendForUi.clear();
    noteForUi.clear();

    if (audio.contains("speakers")) {
      const auto spk = audio.value("speakers").toObject();
      const bool spkRouting = spk.value("routing_active").toBool(false);
      const QString spkRouteMode = spk.value("route_mode").toString();
      const QString spkBackend = spk.value("backend_active").toString();
      const QString spkNote = spk.value("effects_note").toString();

      QString spkPipeBackend;
      QString spkPipeNote;
      if (spk.contains("pipeline")) {
        const auto spkPipe = spk.value("pipeline").toObject();
        spkPipeBackend = spkPipe.value("backend_active").toString();
        spkPipeNote = spkPipe.value("effects_note").toString();
      }

      backendForUi = spkBackend.isEmpty() ? spkPipeBackend : spkBackend;
      noteForUi = spkNote.trimmed().isEmpty() ? spkPipeNote : spkNote;

      const QString rm = spkRouteMode.trimmed().toLower();
      if (rm == QStringLiteral("loopback")) {
        backendForUi = QStringLiteral("loopback");
        if (noteForUi.trimmed().isEmpty() && spkRouting) {
          noteForUi =
              QStringLiteral("Speakers routed via loopback (pass-through).");
        }
      } else if (rm == QStringLiteral("off")) {
        backendForUi = QStringLiteral("off");
        noteForUi.clear();
      }
    }
  }

  if (engineActiveValue_) {
    engineActiveValue_->setText(FriendlyBackendLabel(backendForUi));
    engineActiveValue_->setToolTip(backendForUi);
  }

  if (aiInfoBanner_) {
    const QString full = noteForUi.trimmed();
    const QString first = FirstLine(full);
    aiInfoBanner_->setVisible(!first.isEmpty());
    aiInfoBanner_->setText(first);
    aiInfoBanner_->setToolTip(full);
  }

  daemonStatusText_ =
      QString("enabled=%1\nmic_mode=%2\npipeline=%3\n")
          .arg(audioEnabled ? "true" : "false")
          .arg(micMode.isEmpty() ? "(none)" : micMode)
          .arg(running ? "running" : (starting ? "starting" : "stopped"));

  if (pipeline.contains("gpu")) {
    const auto gpu = pipeline.value("gpu").toObject();
    const int idx = gpu.value("index").toInt(-1);
    const QString name = gpu.value("name").toString();
    const QString cc = gpu.value("compute_cap").toString();
    if (idx >= 0 || !name.isEmpty()) {
      daemonStatusText_ += QString("gpu: #%1 %2%3\n")
                               .arg(idx)
                               .arg(name.isEmpty() ? "(unknown)" : name)
                               .arg(cc.isEmpty() ? "" : (" (cc " + cc + ")"));
    }
  }
  if (!lastErr.isEmpty())
    daemonStatusText_ += "last_error: " + lastErr + "\n";

  // Speakers status (if present).
  if (audio.contains("speakers")) {
    const auto spk = audio.value("speakers").toObject();

    daemonSpeakersRoutingActive_ = spk.value("routing_active").toBool(false);
    daemonSpeakersRouteMode_ = spk.value("route_mode").toString();

    const bool spkPresent = spk.value("present").toBool(false);
    const bool spkRouting = spk.value("routing_active").toBool(false);
    const QString spkRouteMode = spk.value("route_mode").toString();
    const QString spkTarget = spk.value("target_sink_active").toString();
    const QString spkErr = spk.value("last_error").toString();

    const bool spkPipeRunning = spk.value("pipeline_running").toBool(false);
    const bool spkPipeStarting = spk.value("pipeline_starting").toBool(false);
    const QString spkBackend = spk.value("backend_active").toString();
    const QString spkNote = spk.value("effects_note").toString();
    const QString spkPipeErr = spk.value("pipeline_last_error").toString();

    daemonStatusText_ +=
        QString("speakers_present=%1 speakers_route=%2 route_mode=%3\n")
            .arg(spkPresent ? "true" : "false")
            .arg(spkRouting ? "true" : "false")
            .arg(spkRouteMode.isEmpty() ? "(none)" : spkRouteMode);

    if (spk.contains("pipeline")) {
      const auto spkPipe = spk.value("pipeline").toObject();
      const QString spkPipeBackend = spkPipe.value("backend_active").toString();
      const QString spkPipeNote = spkPipe.value("effects_note").toString();
      if (!spkPipeBackend.isEmpty())
        daemonStatusText_ +=
            "speakers_pipeline_backend_active: " + spkPipeBackend + "\n";
      if (!spkPipeNote.trimmed().isEmpty())
        daemonStatusText_ +=
            "speakers_pipeline_note: " + spkPipeNote.trimmed() + "\n";
    }

    if (!spkBackend.isEmpty())
      daemonStatusText_ += "speakers_backend_active: " + spkBackend + "\n";
    if (!spkNote.trimmed().isEmpty())
      daemonStatusText_ += "speakers_note: " + spkNote.trimmed() + "\n";
    if (!spkTarget.isEmpty())
      daemonStatusText_ += "speakers_target_sink_active: " + spkTarget + "\n";
    if (!spkErr.isEmpty())
      daemonStatusText_ += "speakers_last_error: " + spkErr + "\n";
    if (!spkPipeErr.isEmpty())
      daemonStatusText_ += "speakers_pipeline_last_error: " + spkPipeErr + "\n";

    if (spk.contains("pipeline_perf")) {
      const auto perf = spk.value("pipeline_perf").toObject();
      const double avgMs = perf.value("process_ms_avg").toDouble(0.0);
      const double lastMs =
          perf.value("process_us_last").toDouble(0.0) / 1000.0;
      const double maxMs = perf.value("process_us_max").toDouble(0.0) / 1000.0;
      const int overruns = perf.value("process_overruns").toInt(0);
      const qint64 frames =
          static_cast<qint64>(perf.value("frames_processed").toDouble(0.0));
      daemonStatusText_ +=
          QString("speakers_proc_ms_avg=%1 speakers_proc_ms_last=%2 "
                  "speakers_proc_ms_max=%3 "
                  "speakers_overruns=%4 speakers_frames=%5\n")
              .arg(avgMs, 0, 'f', 3)
              .arg(lastMs, 0, 'f', 3)
              .arg(maxMs, 0, 'f', 3)
              .arg(overruns)
              .arg(frames);
    }

    // Mirror the most user-relevant pipeline state.
    daemonStatusText_ +=
        QString("speakers_pipeline=%1\n")
            .arg(spkPipeRunning ? "running"
                                : (spkPipeStarting ? "starting" : "stopped"));
  }

  if (!backendActive.isEmpty())
    daemonStatusText_ += "backend_active: " + backendActive + "\n";
  if (!maxineDiag.isEmpty())
    daemonStatusText_ += maxineDiag;

  if (!daemonAiSupported_) {
    // Keep widget states but prevent edits.
    return;
  }

  // Sync UI from daemon config.
  const auto fx = audio.value("audio_effects").toObject();
  lastAudioEffectsObj_ = fx;

  // Engine preference (schema v4).
  const QString engine = fx.value("engine").toString("auto");
  if (engineCombo_) {
    engineCombo_->blockSignals(true);
    const int idx = engineCombo_->findData(engine);
    engineCombo_->setCurrentIndex(idx >= 0 ? idx : 0);
    engineCombo_->blockSignals(false);
  }

  // Open Audio diagnostics.
  openAudioStatusPresent_ = false;
  openAudioOk_ = false;
  openAudioInstallHints_.clear();

  QJsonObject openAudio;
  if (root.contains("engines")) {
    const auto engines = root.value("engines").toObject();
    openAudio = engines.value("open_audio").toObject();
  }
  if (openAudio.isEmpty()) {
    openAudio = root.value("open_audio").toObject();
  }
  if (!openAudio.isEmpty()) {
    openAudioStatusPresent_ = true;
    openAudioOk_ = openAudio.value("ok").toBool(false);
    const auto hints = openAudio.value("install_hints").toArray();
    for (const auto &v : hints) {
      const auto s = v.toString();
      if (!s.isEmpty())
        openAudioInstallHints_.push_back(s);
    }
  }

  if (openAudioStatusPresent_) {
    daemonStatusText_ +=
        QString("open_audio_ok: %1\n").arg(openAudioOk_ ? "true" : "false");
  }

  if (openAudioInstallHintsBtn_) {
    openAudioInstallHintsBtn_->setEnabled(openAudioStatusPresent_ &&
                                          !openAudioInstallHints_.isEmpty());
  }

  // Populate model list from Open Audio diagnostics.
  auto populateModels = [&](QComboBox *combo) {
    if (!combo)
      return;
    const QString prior = combo->currentData().toString();
    combo->blockSignals(true);
    combo->clear();
    combo->addItem("<auto>", "");
    if (!openAudioStatusPresent_) {
      combo->addItem("<Open Audio status not reported>", "");
    } else if (!openAudioOk_) {
      combo->addItem("<Open Audio unavailable>", "");
    } else {
      const auto models = openAudio.value("models").toArray();
      if (models.isEmpty()) {
        combo->addItem("<no models installed>", "");
      }
      for (const auto &mv : models) {
        const auto m = mv.toObject();
        const QString id = m.value("id").toString();
        const QString name = m.value("display_name").toString();
        if (id.isEmpty())
          continue;
        const QString label = name.isEmpty() ? id : (name + " (" + id + ")");
        combo->addItem(label, id);
      }
    }

    int restore = combo->findData(prior);
    if (restore < 0)
      restore = 0;
    combo->setCurrentIndex(restore);
    combo->blockSignals(false);
  };

  populateModels(openAudioModelCombo_);
  populateModels(speakerOpenAudioModelCombo_);

  const auto mic = fx.value("microphone").toObject();
  const bool noise = mic.value("noise_removal_enabled").toBool(false);
  const bool echo = mic.value("room_echo_removal_enabled").toBool(false);
  const bool studio = mic.value("studio_voice_enabled").toBool(false);
  const int strength = mic.value("strength").toInt(50);

  const QString micModelId = mic.value("model_id").toString();
  const QString micModelPath = mic.value("model_path").toString();

  const auto spkFx = fx.value("speaker").toObject();
  const bool spkNoise = spkFx.value("noise_removal_enabled").toBool(false);
  const bool spkEcho = spkFx.value("room_echo_removal_enabled").toBool(false);
  const int spkStrength = spkFx.value("strength").toInt(50);
  const QString spkModelId = spkFx.value("model_id").toString();
  const QString spkModelPath = spkFx.value("model_path").toString();

  updatingAiUi_ = true;

  auto setComboById = [](QComboBox *combo, const QString &id) {
    if (!combo)
      return;
    const int idx = combo->findData(id);
    combo->setCurrentIndex(idx >= 0 ? idx : 0);
  };

  auto setModelComboWithMissing = [](QComboBox *combo, const QString &id) {
    if (!combo)
      return;
    const QString want = id.trimmed();
    combo->blockSignals(true);
    for (int i = combo->count() - 1; i >= 0; --i) {
      if (combo->itemText(i).startsWith(QStringLiteral("<missing:"))) {
        combo->removeItem(i);
      }
    }
    if (want.isEmpty()) {
      combo->setCurrentIndex(0);
      combo->blockSignals(false);
      return;
    }
    int idx = combo->findData(want);
    if (idx >= 0) {
      combo->setCurrentIndex(idx);
      combo->blockSignals(false);
      return;
    }
    const int insertAt = std::min(1, combo->count());
    combo->insertItem(insertAt, QStringLiteral("<missing: %1>").arg(want),
                      want);
    combo->setCurrentIndex(insertAt);
    combo->blockSignals(false);
  };

  if (micEffectCombo_) {
    QString id = "off";
    if (studio) {
      id = "studio_voice";
    } else if (noise && echo) {
      id = "noise_echo";
    } else if (noise) {
      id = "noise";
    } else if (echo) {
      id = "echo";
    }
    setComboById(micEffectCombo_, id);
  }

  if (strengthSlider_) {
    strengthSlider_->setValue(std::max(0, std::min(100, strength)));
  }
  if (strengthValueLabel_ && strengthSlider_) {
    strengthValueLabel_->setText(QString::number(strengthSlider_->value()));
  }
  setModelComboWithMissing(openAudioModelCombo_, micModelId);
  if (openAudioModelPathEdit_) {
    openAudioModelPathEdit_->setText(micModelPath);
  }

  if (speakerEffectCombo_) {
    QString id = "off";
    if (spkNoise && spkEcho) {
      id = "noise_echo";
    } else if (spkNoise) {
      id = "noise";
    } else if (spkEcho) {
      id = "echo";
    }
    setComboById(speakerEffectCombo_, id);
  }

  if (speakerStrengthSlider_) {
    speakerStrengthSlider_->setValue(std::max(0, std::min(100, spkStrength)));
  }
  if (speakerStrengthValueLabel_ && speakerStrengthSlider_) {
    speakerStrengthValueLabel_->setText(
        QString::number(speakerStrengthSlider_->value()));
  }
  setModelComboWithMissing(speakerOpenAudioModelCombo_, spkModelId);
  if (speakerOpenAudioModelPathEdit_) {
    speakerOpenAudioModelPathEdit_->setText(spkModelPath);
  }

  updatingAiUi_ = false;

  // Disable strength sliders when effect is Off (Broadcast-like).
  if (micEffectCombo_ && strengthSlider_) {
    const bool active = daemonAiSupported_ &&
                        (micEffectCombo_->currentData().toString() != "off");
    strengthSlider_->setEnabled(active);
    if (strengthValueLabel_)
      strengthValueLabel_->setEnabled(active);
  }
  if (speakerEffectCombo_ && speakerStrengthSlider_) {
    const bool active =
        daemonAiSupported_ &&
        (speakerEffectCombo_->currentData().toString() != "off");
    speakerStrengthSlider_->setEnabled(active);
    if (speakerStrengthValueLabel_)
      speakerStrengthValueLabel_->setEnabled(active);
  }

  UpdateEngineUiVisibility();
}

void AudioPage::PushDaemonSourceSelection() {
  if (!daemonAiSupported_)
    return;
  if (!sourceCombo_)
    return;
  if (!sourceCombo_->isEnabled())
    return;

  const std::string srcName =
      sourceCombo_->currentData().toString().toStdString();
  if (srcName.empty())
    return;

  QJsonObject patch;
  patch.insert("source", QString::fromStdString(srcName));
  const auto json = QJsonDocument(patch).toJson(QJsonDocument::Compact);

  std::string out;
  QString err;
  if (!DaemonRequest(std::string("SET_AUDIO_CONFIG ") + json.toStdString(),
                     &out, &err)) {
    // Non-fatal: show in status.
    daemonStatusText_ += "\nfailed_to_set_source: " + err;
  }
}

void AudioPage::PushDaemonAudioConfig() {
  if (!daemonAiSupported_)
    return;

  const QString engine = engineCombo_ ? engineCombo_->currentData().toString()
                                      : QStringLiteral("auto");

  QJsonObject effects = lastAudioEffectsObj_;
  effects.insert(
      "schema_version",
      studiocast::audio::effects::kBroadcastAudioEffectsSchemaVersion);
  effects.insert("engine", engine);

  // Microphone (only if this page has mic controls).
  if (micEffectCombo_ && strengthSlider_) {
    const QString sel = micEffectCombo_->currentData().toString();
    const bool studio = (sel == "studio_voice");
    const bool noise = (sel == "noise") || (sel == "noise_echo");
    const bool echo = (sel == "echo") || (sel == "noise_echo");
    const int strength = std::max(0, std::min(100, strengthSlider_->value()));

    QJsonObject mic = effects.value("microphone").toObject();
    mic.insert("studio_voice_enabled", studio);
    mic.insert("noise_removal_enabled", noise);
    mic.insert("room_echo_removal_enabled", echo);
    mic.insert("strength", strength);
    if (openAudioModelCombo_)
      mic.insert("model_id", openAudioModelCombo_->currentData().toString());
    if (openAudioModelPathEdit_)
      mic.insert("model_path", openAudioModelPathEdit_->text().trimmed());
    effects.insert("microphone", mic);
  }

  // Speaker (only if this page has speaker controls).
  if (speakerEffectCombo_ && speakerStrengthSlider_) {
    const QString sel = speakerEffectCombo_->currentData().toString();
    const bool spkNoise = (sel == "noise") || (sel == "noise_echo");
    const bool spkEcho = (sel == "echo") || (sel == "noise_echo");
    const int spkStrength =
        std::max(0, std::min(100, speakerStrengthSlider_->value()));

    QJsonObject spk = effects.value("speaker").toObject();
    spk.insert("noise_removal_enabled", spkNoise);
    spk.insert("room_echo_removal_enabled", spkEcho);
    spk.insert("strength", spkStrength);
    if (speakerOpenAudioModelCombo_)
      spk.insert("model_id",
                 speakerOpenAudioModelCombo_->currentData().toString());
    if (speakerOpenAudioModelPathEdit_)
      spk.insert("model_path",
                 speakerOpenAudioModelPathEdit_->text().trimmed());
    effects.insert("speaker", spk);
  }

  QJsonObject patch;
  patch.insert("audio_effects", effects);

  // In microphone mode we also toggle the service enabled flag.
  //
  // Note: Effect "Off" is still meaningful (it requests pass-through), so we
  // keep the pipeline running unless the backend is explicitly Off.
  if (micEffectCombo_) {
    const bool enabled = (engine != "off");
    patch.insert("enabled", enabled);
    patch.insert("create_virtual_mic", true);
  }

  const auto json = QJsonDocument(patch).toJson(QJsonDocument::Compact);

  std::string out;
  QString err;
  if (!DaemonRequest(std::string("SET_AUDIO_CONFIG ") + json.toStdString(),
                     &out, &err)) {
    ShowError("Audio", "Failed to update daemon audio config:\n\n" + err);
  }
  RefreshDaemonAudioStatus();
}

void AudioPage::OnAiEngineChanged(int /*index*/) {
  if (updatingAiUi_)
    return;
  UpdateEngineUiVisibility();
  PushDaemonAudioConfig();
}

void AudioPage::OnAiOpenAudioModelChanged(int /*index*/) {
  if (updatingAiUi_)
    return;
  PushDaemonAudioConfig();
}

void AudioPage::OnAiOpenAudioModelPathEdited() {
  if (updatingAiUi_)
    return;
  PushDaemonAudioConfig();
}

void AudioPage::OnAiBrowseOpenAudioModel() {
  if (!openAudioModelPathEdit_)
    return;
  const QString start = openAudioModelPathEdit_->text().trimmed();
  const QString path = QFileDialog::getOpenFileName(
      this, "Select ONNX model", start, "ONNX model (*.onnx);;All files (*)");
  if (path.isEmpty())
    return;
  openAudioModelPathEdit_->setText(path);
  if (updatingAiUi_)
    return;
  PushDaemonAudioConfig();
}

void AudioPage::OnOpenAudioInstallHints() {
  if (openAudioInstallHints_.isEmpty()) {
    ShowError("Open Audio", "No install hints were reported by the daemon.");
    return;
  }
  QString msg;
  for (const auto &s : openAudioInstallHints_) {
    msg += "- " + s + "\n";
  }
  QMessageBox::information(this, "Open Audio install hints", msg);
}

void AudioPage::OnMicEffectChanged(int /*index*/) {
  if (updatingAiUi_)
    return;

  if (micEffectCombo_ && strengthSlider_) {
    const bool active = daemonAiSupported_ &&
                        (micEffectCombo_->currentData().toString() != "off");
    strengthSlider_->setEnabled(active);
    if (strengthValueLabel_)
      strengthValueLabel_->setEnabled(active);
  }

  PushDaemonAudioConfig();
}

void AudioPage::OnSpeakerEffectChanged(int /*index*/) {
  if (updatingAiUi_)
    return;

  if (speakerEffectCombo_ && speakerStrengthSlider_) {
    const bool active =
        daemonAiSupported_ &&
        (speakerEffectCombo_->currentData().toString() != "off");
    speakerStrengthSlider_->setEnabled(active);
    if (speakerStrengthValueLabel_)
      speakerStrengthValueLabel_->setEnabled(active);
  }

  PushDaemonAudioConfig();
}

void AudioPage::OnAiSpeakerStrengthChanged(int v) {
  if (speakerStrengthValueLabel_) {
    speakerStrengthValueLabel_->setText(
        QString::number(std::max(0, std::min(100, v))));
  }
  if (updatingAiUi_)
    return;
  PushDaemonAudioConfig();
}

void AudioPage::OnAiSpeakerOpenAudioModelChanged(int /*index*/) {
  if (updatingAiUi_)
    return;
  PushDaemonAudioConfig();
}

void AudioPage::OnAiSpeakerOpenAudioModelPathEdited() {
  if (updatingAiUi_)
    return;
  PushDaemonAudioConfig();
}

void AudioPage::OnAiSpeakerBrowseOpenAudioModel() {
  if (!speakerOpenAudioModelPathEdit_)
    return;
  const QString start = speakerOpenAudioModelPathEdit_->text().trimmed();
  const QString path = QFileDialog::getOpenFileName(
      this, "Select ONNX model", start, "ONNX model (*.onnx);;All files (*)");
  if (path.isEmpty())
    return;
  speakerOpenAudioModelPathEdit_->setText(path);
  if (updatingAiUi_)
    return;
  PushDaemonAudioConfig();
}

void AudioPage::OnAiStrengthChanged(int v) {
  if (strengthValueLabel_) {
    strengthValueLabel_->setText(
        QString::number(std::max(0, std::min(100, v))));
  }
  if (updatingAiUi_)
    return;
  PushDaemonAudioConfig();
}

void AudioPage::OnCreateVirtualMic() {
  std::string err;
  if (!studiocast::audio::CreateVirtualMic(&err)) {
    ShowError("Create virtual mic failed", QString::fromStdString(err));
    return;
  }
  RefreshStatus();
  RefreshSources();
}

void AudioPage::OnDestroyVirtualMic() {
  std::string err;
  if (!studiocast::audio::DestroyVirtualMic(&err)) {
    ShowError("Destroy virtual mic failed", QString::fromStdString(err));
    return;
  }
  RefreshStatus();
  RefreshSources();
}

void AudioPage::OnStartLoopback() {
  if (!sourceCombo_ || !sourceCombo_->isEnabled()) {
    ShowError("Start loopback failed", "No valid input source selected.");
    return;
  }

  const auto selected = sourceCombo_->currentData().toString();
  const std::string source = selected.toStdString();
  const int latency = latencySpin_ ? latencySpin_->value() : 10;

  // Optional: set port before loopback (helps laptop internal mic/headset mic
  // routing).
  if (portCombo_ && portCombo_->isEnabled()) {
    const std::string port = portCombo_->currentData().toString().toStdString();
    if (!port.empty()) {
      std::string perr;
      if (!studiocast::audio::pulse::SetSourcePort(source, port, &perr)) {
        ShowError("Set input port failed", QString::fromStdString(perr));
        return;
      }
    }
  }

  std::string err;
  if (!studiocast::audio::StartLoopback(source, latency, &err)) {
    ShowError("Start loopback failed", QString::fromStdString(err));
    return;
  }
  RefreshStatus();
}

void AudioPage::OnStopLoopback() {
  std::string err;
  if (!studiocast::audio::StopLoopback(&err)) {
    ShowError("Stop loopback failed", QString::fromStdString(err));
    return;
  }
  RefreshStatus();
}

void AudioPage::OnEnableVirtualSpeakers() {
  // Preferred path: let the daemon manage the virtual speakers device +
  // routing.
  if (daemonAiSupported_) {
    QJsonObject patch;
    patch.insert("create_virtual_speakers", true);
    patch.insert("speakers_enabled", true);
    patch.insert("speaker_latency_ms", 10);

    const auto json = QJsonDocument(patch).toJson(QJsonDocument::Compact);
    std::string out;
    QString err;
    if (!DaemonRequest(std::string("SET_AUDIO_CONFIG ") + json.toStdString(),
                       &out, &err)) {
      ShowError("Audio", "Failed to enable speakers via daemon:\n\n" + err);
      return;
    }
    RefreshStatus();
    return;
  }

  // Fallback: direct pactl manipulation (debug/dev only).
  std::string err;
  if (!studiocast::audio::CreateVirtualSpeaker(&err)) {
    ShowError("Enable speakers failed", QString::fromStdString(err));
    return;
  }

  err.clear();
  if (!studiocast::audio::StartSpeakerLoopback("", 10, &err)) {
    ShowError("Start speakers routing failed", QString::fromStdString(err));
    return;
  }

  RefreshStatus();
}

void AudioPage::OnStopSpeakersRouting() {
  if (daemonAiSupported_) {
    QJsonObject patch;
    patch.insert("speakers_enabled", false);
    const auto json = QJsonDocument(patch).toJson(QJsonDocument::Compact);
    std::string out;
    QString err;
    if (!DaemonRequest(std::string("SET_AUDIO_CONFIG ") + json.toStdString(),
                       &out, &err)) {
      ShowError("Audio",
                "Failed to stop speakers routing via daemon:\n\n" + err);
      return;
    }
    RefreshStatus();
    return;
  }

  std::string err;
  if (!studiocast::audio::StopSpeakerLoopback(&err)) {
    ShowError("Stop speakers routing failed", QString::fromStdString(err));
    return;
  }
  RefreshStatus();
}

void AudioPage::OnDestroyVirtualSpeakers() {
  if (daemonAiSupported_) {
    QJsonObject patch;
    patch.insert("speakers_enabled", false);
    patch.insert("create_virtual_speakers", false);
    const auto json = QJsonDocument(patch).toJson(QJsonDocument::Compact);
    std::string out;
    QString err;
    if (!DaemonRequest(std::string("SET_AUDIO_CONFIG ") + json.toStdString(),
                       &out, &err)) {
      ShowError("Audio", "Failed to destroy speakers via daemon:\n\n" + err);
      return;
    }
    RefreshStatus();
    return;
  }

  std::string err;
  if (!studiocast::audio::DestroyVirtualSpeaker(&err)) {
    ShowError("Destroy speakers failed", QString::fromStdString(err));
    return;
  }
  RefreshStatus();
}

} // namespace studiocast::gui
