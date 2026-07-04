#include "advanced_page.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>
#include <string>
#include <string_view>

#include "core/audio/audio_device_safety.h"
#include "core/audio/virtual_mic.h"
#include "core/audio/virtual_speaker.h"
#include "core/ipc/daemon_client.h"
#include "core/video/effects/broadcast_effect_contract.h"
#include "gui/status/daemon_status_snapshot.h"
#include "gui/text_edit_utils.h"

namespace studiocast::gui {
namespace {

namespace video_contract = studiocast::video::effects::contract;

QLabel *MutedLabel(const QString &text, QWidget *parent) {
  auto *label = new QLabel(text, parent);
  label->setProperty("scRole", "muted");
  label->setWordWrap(true);
  return label;
}

QLabel *ValueLabel(const QString &text, QWidget *parent) {
  auto *label = new QLabel(text, parent);
  label->setProperty("scRole", "value");
  label->setWordWrap(true);
  return label;
}

void SetDynamicProperty(QWidget *widget, const char *name,
                        const QString &value) {
  if (!widget)
    return;
  widget->setProperty(name, value);
  widget->style()->unpolish(widget);
  widget->style()->polish(widget);
}

void CopyText(const QString &text) {
  if (QClipboard *clipboard = QApplication::clipboard())
    clipboard->setText(text);
}

QString HumanDaemonError(const QString &raw) {
  const QString trimmed = raw.trimmed();
  if (trimmed.isEmpty())
    return QStringLiteral("Unknown daemon error.");

  QJsonParseError parseError;
  const QJsonDocument doc =
      QJsonDocument::fromJson(trimmed.toUtf8(), &parseError);
  if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
    const QString error = doc.object().value(QStringLiteral("error"))
                              .toString()
                              .trimmed();
    if (!error.isEmpty())
      return error;
  }

  return trimmed;
}

bool ParseJsonObject(const std::string &json, QJsonObject *out,
                     QString *error) {
  QJsonParseError parseError;
  const QJsonDocument doc =
      QJsonDocument::fromJson(QByteArray::fromStdString(json), &parseError);
  if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
    if (error) {
      *error = QStringLiteral("JSON parse error: %1")
                   .arg(parseError.errorString());
    }
    return false;
  }

  if (out)
    *out = doc.object();
  return true;
}

QJsonObject ObjectValue(const QJsonObject &obj, const QString &key) {
  const QJsonValue value = obj.value(key);
  return value.isObject() ? value.toObject() : QJsonObject{};
}

std::string CompactJson(const QJsonObject &obj) {
  return QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString();
}

bool Contains(const std::string &hay, const std::string &needle) {
  return hay.find(needle) != std::string::npos;
}

QString ContractKey(std::string_view value) {
  return QString::fromUtf8(value.data(), static_cast<int>(value.size()));
}

void SetEditTextIfNotFocused(QLineEdit *edit, const QString &text) {
  if (!edit)
    return;
  if (edit->hasFocus())
    return;
  if (edit->text() != text)
    edit->setText(text);
}

QPlainTextEdit *RawTextBox(QWidget *parent, int minHeight,
                           bool wrap = false) {
  auto *text = new QPlainTextEdit(parent);
  text->setReadOnly(true);
  text->setLineWrapMode(wrap ? QPlainTextEdit::WidgetWidth
                             : QPlainTextEdit::NoWrap);
  text->setMinimumHeight(minHeight);
  return text;
}

QFrame *Panel(QWidget *parent) {
  auto *frame = new QFrame(parent);
  frame->setProperty("scRole", "microphonePanel");
  return frame;
}

void AddStatusLine(QVBoxLayout *layout, const QString &label,
                   QLabel **valueOut, QWidget *parent) {
  auto *row = new QHBoxLayout();
  row->setContentsMargins(0, 0, 0, 0);
  row->setSpacing(12);
  auto *name = MutedLabel(label, parent);
  name->setMinimumWidth(150);
  row->addWidget(name, 0);
  auto *value = ValueLabel(QStringLiteral("Unknown"), parent);
  row->addWidget(value, 1);
  layout->addLayout(row);
  if (valueOut)
    *valueOut = value;
}

QLineEdit *AddEditRow(QVBoxLayout *layout, const QString &label,
                      const QString &placeholder, QWidget *parent) {
  auto *row = new QHBoxLayout();
  row->setContentsMargins(0, 0, 0, 0);
  row->setSpacing(10);
  auto *name = new QLabel(label, parent);
  name->setMinimumWidth(210);
  row->addWidget(name, 0);
  auto *edit = new QLineEdit(parent);
  edit->setPlaceholderText(placeholder);
  edit->setClearButtonEnabled(true);
  row->addWidget(edit, 1);
  layout->addLayout(row);
  return edit;
}

QString BoolLabel(bool value) {
  return value ? QStringLiteral("true") : QStringLiteral("false");
}

QString EffectModelId(const QJsonObject &effects,
                      std::initializer_list<QString> keys) {
  QString fallback;
  for (const QString &key : keys) {
    const QString value = ObjectValue(effects, key)
                              .value(QStringLiteral("model_id"))
                              .toString();
    if (fallback.isEmpty())
      fallback = value;
    if (!value.trimmed().isEmpty())
      return value;
  }
  return fallback;
}

void SetEffectString(QJsonObject *effects, const QString &effectKey,
                     const QString &paramKey, const QString &value) {
  if (!effects)
    return;
  QJsonObject effect = ObjectValue(*effects, effectKey);
  effect.insert(paramKey, value);
  effects->insert(effectKey, effect);
}

} // namespace

AdvancedPage::AdvancedPage(QWidget *parent) : QWidget(parent) {
  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(16, 16, 16, 16);
  root->setSpacing(12);

  auto *daemonBox = new QGroupBox(QStringLiteral("Daemon & IPC"), this);
  auto *daemonLayout = new QVBoxLayout(daemonBox);
  daemonLayout->setSpacing(10);
  AddStatusLine(daemonLayout, QStringLiteral("Service"), &serviceStateLabel_,
                daemonBox);
  AddStatusLine(daemonLayout, QStringLiteral("Detail"), &serviceDetailLabel_,
                daemonBox);

  auto *socketRow = new QHBoxLayout();
  socketRow->setContentsMargins(0, 0, 0, 0);
  socketRow->setSpacing(10);
  auto *socketName = MutedLabel(QStringLiteral("Socket"), daemonBox);
  socketName->setMinimumWidth(150);
  socketRow->addWidget(socketName, 0);
  socketPathLabel_ = ValueLabel(QStringLiteral("Unknown"), daemonBox);
  socketRow->addWidget(socketPathLabel_, 1);
  copySocketButton_ = new QPushButton(QStringLiteral("Copy"), daemonBox);
  copySocketButton_->setIcon(style()->standardIcon(QStyle::SP_FileDialogListView));
  socketRow->addWidget(copySocketButton_, 0);
  daemonLayout->addLayout(socketRow);

  rawStatusText_ = RawTextBox(daemonBox, 260);
  rawStatusText_->setPlaceholderText(
      QStringLiteral("Daemon status has not been read."));
  daemonLayout->addWidget(rawStatusText_, 1);

  auto *rawButtons = new QHBoxLayout();
  copyRawStatusButton_ =
      new QPushButton(QStringLiteral("Copy raw daemon JSON"), daemonBox);
  copyRawStatusButton_->setIcon(
      style()->standardIcon(QStyle::SP_FileDialogDetailedView));
  rawButtons->addWidget(copyRawStatusButton_);
  rawButtons->addStretch(1);
  daemonLayout->addLayout(rawButtons);
  root->addWidget(daemonBox);

  auto *cameraBox = new QGroupBox(QStringLiteral("Camera System"), this);
  auto *cameraLayout = new QVBoxLayout(cameraBox);
  cameraLayout->setSpacing(10);
  cameraStateLabel_ = ValueLabel(QStringLiteral("Waiting for daemon status."),
                                 cameraBox);
  cameraLayout->addWidget(cameraStateLabel_);
  alwaysOnCheck_ =
      new QCheckBox(QStringLiteral("Always-on camera processing"), cameraBox);
  cameraLayout->addWidget(alwaysOnCheck_);
  cameraLayout->addWidget(MutedLabel(
      QStringLiteral("This writes the daemon always_on camera flag directly."),
      cameraBox));
  root->addWidget(cameraBox);

  auto *lifecycleBox =
      new QGroupBox(QStringLiteral("Virtual Audio Lifecycle"), this);
  auto *lifecycleLayout = new QVBoxLayout(lifecycleBox);
  lifecycleLayout->setSpacing(10);
  audioLifecycleLabel_ =
      ValueLabel(QStringLiteral("Waiting for audio status."), lifecycleBox);
  lifecycleLayout->addWidget(audioLifecycleLabel_);
  pulseStateLabel_ =
      MutedLabel(QStringLiteral("PulseAudio status has not been checked."),
                 lifecycleBox);
  lifecycleLayout->addWidget(pulseStateLabel_);

  auto *micButtons = new QHBoxLayout();
  createVirtualMicButton_ =
      new QPushButton(QStringLiteral("Create virtual mic"), lifecycleBox);
  destroyVirtualMicButton_ =
      new QPushButton(QStringLiteral("Destroy virtual mic"), lifecycleBox);
  destroyVirtualMicButton_->setProperty("scVariant", "danger");
  micButtons->addWidget(createVirtualMicButton_);
  micButtons->addWidget(destroyVirtualMicButton_);
  micButtons->addStretch(1);
  lifecycleLayout->addLayout(micButtons);

  auto *speakerButtons = new QHBoxLayout();
  enableVirtualSpeakersButton_ =
      new QPushButton(QStringLiteral("Enable speakers device"), lifecycleBox);
  stopSpeakersRoutingButton_ =
      new QPushButton(QStringLiteral("Stop speaker routing"), lifecycleBox);
  destroyVirtualSpeakersButton_ =
      new QPushButton(QStringLiteral("Destroy speakers device"), lifecycleBox);
  destroyVirtualSpeakersButton_->setProperty("scVariant", "danger");
  speakerButtons->addWidget(enableVirtualSpeakersButton_);
  speakerButtons->addWidget(stopSpeakersRoutingButton_);
  speakerButtons->addWidget(destroyVirtualSpeakersButton_);
  speakerButtons->addStretch(1);
  lifecycleLayout->addLayout(speakerButtons);

  refreshPulseButton_ =
      new QPushButton(QStringLiteral("Refresh PulseAudio state"), lifecycleBox);
  lifecycleLayout->addWidget(refreshPulseButton_, 0, Qt::AlignLeft);
  root->addWidget(lifecycleBox);

  auto *modelsBox =
      new QGroupBox(QStringLiteral("Explicit Model Paths & Raw IDs"), this);
  auto *modelsLayout = new QVBoxLayout(modelsBox);
  modelsLayout->setSpacing(10);

  auto *audioPanel = Panel(modelsBox);
  auto *audioPanelLayout = new QVBoxLayout(audioPanel);
  audioPanelLayout->setSpacing(8);
  audioPanelLayout->addWidget(ValueLabel(QStringLiteral("Open Audio"),
                                         audioPanel));
  micModelIdEdit_ =
      AddEditRow(audioPanelLayout, QStringLiteral("Microphone model ID"),
                 QStringLiteral("<auto>"), audioPanel);
  micModelPathEdit_ =
      AddEditRow(audioPanelLayout, QStringLiteral("Microphone model path"),
                 QStringLiteral("/path/to/model.onnx"), audioPanel);
  speakerModelIdEdit_ =
      AddEditRow(audioPanelLayout, QStringLiteral("Speaker model ID"),
                 QStringLiteral("<auto>"), audioPanel);
  speakerModelPathEdit_ =
      AddEditRow(audioPanelLayout, QStringLiteral("Speaker model path"),
                 QStringLiteral("/path/to/model.onnx"), audioPanel);
  saveAudioModelsButton_ =
      new QPushButton(QStringLiteral("Save Open Audio overrides"), audioPanel);
  audioPanelLayout->addWidget(saveAudioModelsButton_, 0, Qt::AlignLeft);
  modelsLayout->addWidget(audioPanel);

  auto *videoPanel = Panel(modelsBox);
  auto *videoPanelLayout = new QVBoxLayout(videoPanel);
  videoPanelLayout->setSpacing(8);
  videoPanelLayout->addWidget(ValueLabel(QStringLiteral("Open Video"),
                                         videoPanel));
  virtualBackgroundModelIdEdit_ =
      AddEditRow(videoPanelLayout,
                 QStringLiteral("Virtual background model ID"),
                 QStringLiteral("<auto>"), videoPanel);
  autoFrameModelIdEdit_ =
      AddEditRow(videoPanelLayout, QStringLiteral("Auto frame model ID"),
                 QStringLiteral("<auto>"), videoPanel);
  eyeContactModelIdEdit_ =
      AddEditRow(videoPanelLayout, QStringLiteral("Eye contact model ID"),
                 QStringLiteral("<auto>"), videoPanel);
  denoiseModelIdEdit_ =
      AddEditRow(videoPanelLayout, QStringLiteral("Video denoise model ID"),
                 QStringLiteral("<auto>"), videoPanel);
  virtualBackgroundReplacePathEdit_ =
      AddEditRow(videoPanelLayout,
                 QStringLiteral("Background replace image path"),
                 QStringLiteral("/path/to/background.ppm"), videoPanel);
  virtualKeyLightHdriPathEdit_ =
      AddEditRow(videoPanelLayout, QStringLiteral("Key light HDRI path"),
                 QStringLiteral("/path/to/light.hdr"), videoPanel);
  saveVideoModelsButton_ =
      new QPushButton(QStringLiteral("Save Open Video raw values"), videoPanel);
  videoPanelLayout->addWidget(saveVideoModelsButton_, 0, Qt::AlignLeft);
  modelsLayout->addWidget(videoPanel);
  root->addWidget(modelsBox);

  legacyLoopbackBox_ =
      new QGroupBox(QStringLiteral("Legacy Loopback / Debug"), this);
  auto *legacyLayout = new QVBoxLayout(legacyLoopbackBox_);
  legacyLayout->setSpacing(10);

  auto *sourceRow = new QHBoxLayout();
  sourceRow->addWidget(new QLabel(QStringLiteral("Input source:"),
                                  legacyLoopbackBox_));
  legacySourceCombo_ = new QComboBox(legacyLoopbackBox_);
  legacySourceCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  sourceRow->addWidget(legacySourceCombo_, 1);
  legacyLayout->addLayout(sourceRow);

  auto *portRow = new QHBoxLayout();
  portRow->addWidget(new QLabel(QStringLiteral("Input port:"),
                                legacyLoopbackBox_));
  legacyPortCombo_ = new QComboBox(legacyLoopbackBox_);
  legacyPortCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  portRow->addWidget(legacyPortCombo_, 1);
  legacyLayout->addLayout(portRow);

  auto *latencyRow = new QHBoxLayout();
  latencyRow->addWidget(new QLabel(QStringLiteral("Latency (ms):"),
                                   legacyLoopbackBox_));
  legacyLatencySpin_ = new QSpinBox(legacyLoopbackBox_);
  legacyLatencySpin_->setRange(1, 200);
  legacyLatencySpin_->setValue(10);
  latencyRow->addWidget(legacyLatencySpin_, 0);
  latencyRow->addStretch(1);
  legacyLayout->addLayout(latencyRow);

  auto *legacyButtons = new QHBoxLayout();
  startLegacyLoopbackButton_ =
      new QPushButton(QStringLiteral("Start legacy loopback"),
                      legacyLoopbackBox_);
  stopLegacyLoopbackButton_ =
      new QPushButton(QStringLiteral("Stop legacy loopback"),
                      legacyLoopbackBox_);
  legacyButtons->addWidget(startLegacyLoopbackButton_);
  legacyButtons->addWidget(stopLegacyLoopbackButton_);
  legacyButtons->addStretch(1);
  legacyLayout->addLayout(legacyButtons);

  legacyLayout->addWidget(MutedLabel(
      QStringLiteral("Legacy module-loopback is a debug/dev path; the daemon "
                     "pipeline is the normal processed audio path."),
      legacyLoopbackBox_));

  localAudioStatusText_ = RawTextBox(legacyLoopbackBox_, 180, true);
  legacyLayout->addWidget(localAudioStatusText_);
  root->addWidget(legacyLoopbackBox_);

  resultLabel_ = MutedLabel(QString(), this);
  resultLabel_->setVisible(false);
  root->addWidget(resultLabel_);
  root->addStretch(1);

  connect(copySocketButton_, &QPushButton::clicked, this, [this] {
    CopyText(socketPathLabel_ ? socketPathLabel_->text() : QString());
  });
  connect(copyRawStatusButton_, &QPushButton::clicked, this, [this] {
    CopyText(currentRawStatus_);
  });
  connect(alwaysOnCheck_, &QCheckBox::toggled, this,
          &AdvancedPage::OnAlwaysOnToggled);
  connect(createVirtualMicButton_, &QPushButton::clicked, this,
          &AdvancedPage::OnCreateVirtualMic);
  connect(destroyVirtualMicButton_, &QPushButton::clicked, this,
          &AdvancedPage::OnDestroyVirtualMic);
  connect(enableVirtualSpeakersButton_, &QPushButton::clicked, this,
          &AdvancedPage::OnEnableVirtualSpeakers);
  connect(stopSpeakersRoutingButton_, &QPushButton::clicked, this,
          &AdvancedPage::OnStopSpeakersRouting);
  connect(destroyVirtualSpeakersButton_, &QPushButton::clicked, this,
          &AdvancedPage::OnDestroyVirtualSpeakers);
  connect(saveAudioModelsButton_, &QPushButton::clicked, this,
          &AdvancedPage::OnSaveAudioModelOverrides);
  connect(saveVideoModelsButton_, &QPushButton::clicked, this,
          &AdvancedPage::OnSaveVideoModelOverrides);
  connect(refreshPulseButton_, &QPushButton::clicked, this,
          &AdvancedPage::OnRefreshPulseState);
  connect(legacySourceCombo_,
          QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &AdvancedPage::OnLegacySourceChanged);
  connect(startLegacyLoopbackButton_, &QPushButton::clicked, this,
          &AdvancedPage::OnStartLegacyLoopback);
  connect(stopLegacyLoopbackButton_, &QPushButton::clicked, this,
          &AdvancedPage::OnStopLegacyLoopback);

#ifdef NDEBUG
  legacyLoopbackBox_->setTitle(
      QStringLiteral("Legacy Loopback / Debug (disabled in release builds)"));
  startLegacyLoopbackButton_->setVisible(false);
  stopLegacyLoopbackButton_->setVisible(false);
#endif

  RefreshPulseState();
  UpdateButtonStates();
}

void AdvancedPage::UpdateStatus(const DaemonStatusSnapshot &snapshot) {
  daemonReachable_ = snapshot.reachable;
  currentRawStatus_ = snapshot.RawDiagnosticsText();

  if (serviceStateLabel_) {
    serviceStateLabel_->setText(snapshot.ServiceSummary());
    if (!snapshot.reachable) {
      SetDynamicProperty(serviceStateLabel_, "scStatus",
                         QStringLiteral("error"));
    } else if (!snapshot.parsed || !snapshot.serviceRunning) {
      SetDynamicProperty(serviceStateLabel_, "scStatus",
                         QStringLiteral("warning"));
    } else {
      SetDynamicProperty(serviceStateLabel_, "scStatus",
                         QStringLiteral("good"));
    }
  }
  if (serviceDetailLabel_)
    serviceDetailLabel_->setText(snapshot.ServiceDetail());
  if (socketPathLabel_) {
    socketPathLabel_->setText(snapshot.socketPath.trimmed().isEmpty()
                                  ? QStringLiteral("Unknown")
                                  : snapshot.socketPath.trimmed());
  }
  SetPlainTextPreservingScroll(rawStatusText_, currentRawStatus_);

  QJsonParseError parseError;
  const QJsonDocument doc =
      QJsonDocument::fromJson(snapshot.rawJson.toUtf8(), &parseError);
  if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
    ApplySnapshotJson(doc.object());
  } else if (!snapshot.reachable || !snapshot.parsed) {
    cameraStateLabel_->setText(
        QStringLiteral("Camera system status is unavailable."));
    audioLifecycleLabel_->setText(
        QStringLiteral("Audio lifecycle status is unavailable."));
  }

  UpdateButtonStates();
}

void AdvancedPage::ApplySnapshotJson(const QJsonObject &root) {
  updatingUi_ = true;

  const QJsonObject video = ObjectValue(root, QStringLiteral("video"));
  currentAlwaysOn_ = video.value(QStringLiteral("always_on")).toBool(false);
  if (alwaysOnCheck_) {
    alwaysOnCheck_->blockSignals(true);
    alwaysOnCheck_->setChecked(currentAlwaysOn_);
    alwaysOnCheck_->blockSignals(false);
  }

  if (cameraStateLabel_) {
    const bool enabled = video.value(QStringLiteral("enabled")).toBool(false);
    const QJsonObject pipeline = ObjectValue(video, QStringLiteral("pipeline"));
    const QString state =
        pipeline.value(QStringLiteral("state")).toString().trimmed();
    cameraStateLabel_->setText(
        QStringLiteral("enabled=%1, always_on=%2%3")
            .arg(BoolLabel(enabled), BoolLabel(currentAlwaysOn_),
                 state.isEmpty()
                     ? QString()
                     : QStringLiteral(", pipeline=%1").arg(state)));
  }

  const QJsonObject audio = ObjectValue(root, QStringLiteral("audio"));
  configuredVirtualMic_ =
      audio.value(QStringLiteral("create_virtual_mic")).toBool(false);
  configuredVirtualSpeakers_ =
      audio.value(QStringLiteral("create_virtual_speakers")).toBool(false);
  configuredSpeakersEnabled_ =
      audio.value(QStringLiteral("speakers_enabled")).toBool(false);

  const bool micPresent = audio.value(QStringLiteral("mic_present"))
                              .toBool(configuredVirtualMic_);
  const QJsonObject speakers = ObjectValue(audio, QStringLiteral("speakers"));
  const bool speakersPresent = speakers.value(QStringLiteral("present"))
                                   .toBool(configuredVirtualSpeakers_);
  speakersRoutingActive_ =
      speakers.value(QStringLiteral("routing_active")).toBool(false);
  const QString routeMode =
      speakers.value(QStringLiteral("route_mode")).toString().trimmed();

  if (audioLifecycleLabel_) {
    audioLifecycleLabel_->setText(
        QStringLiteral("virtual_mic=%1/%2, speakers=%3/%4, routing=%5%6")
            .arg(configuredVirtualMic_ ? QStringLiteral("configured")
                                       : QStringLiteral("off"),
                 micPresent ? QStringLiteral("present")
                            : QStringLiteral("missing"),
                 configuredVirtualSpeakers_ ? QStringLiteral("configured")
                                            : QStringLiteral("off"),
                 speakersPresent ? QStringLiteral("present")
                                 : QStringLiteral("missing"),
                 speakersRoutingActive_ ? QStringLiteral("active")
                                        : QStringLiteral("stopped"),
                 routeMode.isEmpty()
                     ? QString()
                     : QStringLiteral(" (%1)").arg(routeMode)));
  }

  const QJsonObject audioEffects =
      ObjectValue(audio, QStringLiteral("audio_effects"));
  const QJsonObject mic =
      ObjectValue(audioEffects, QStringLiteral("microphone"));
  const QJsonObject speaker =
      ObjectValue(audioEffects, QStringLiteral("speaker"));
  SetEditTextIfNotFocused(micModelIdEdit_,
                          mic.value(QStringLiteral("model_id")).toString());
  SetEditTextIfNotFocused(micModelPathEdit_,
                          mic.value(QStringLiteral("model_path")).toString());
  SetEditTextIfNotFocused(
      speakerModelIdEdit_,
      speaker.value(QStringLiteral("model_id")).toString());
  SetEditTextIfNotFocused(
      speakerModelPathEdit_,
      speaker.value(QStringLiteral("model_path")).toString());

  const QJsonObject videoEffects =
      ObjectValue(video, QStringLiteral("video_effects"));
  if (!videoEffects.isEmpty()) {
    const QString vbBlur =
        ContractKey(video_contract::kEffectIdVirtualBackgroundBlur);
    const QString vbRemove =
        ContractKey(video_contract::kEffectIdVirtualBackgroundRemove);
    const QString vbReplace =
        ContractKey(video_contract::kEffectIdVirtualBackgroundReplace);
    const QString autoFrame = ContractKey(video_contract::kEffectIdAutoFrame);
    const QString eyeContact = ContractKey(video_contract::kEffectIdEyeContact);
    const QString denoise =
        ContractKey(video_contract::kEffectIdVideoNoiseRemoval);
    const QString keyLight =
        ContractKey(video_contract::kEffectIdVirtualKeyLight);
    const QString modelId = ContractKey(video_contract::param::kModelId);
    const QString replacePath =
        ContractKey(video_contract::param::kVbReplacePath);
    const QString hdriPath = ContractKey(video_contract::param::kHdriPath);

    SetEditTextIfNotFocused(
        virtualBackgroundModelIdEdit_,
        EffectModelId(videoEffects, {vbBlur, vbRemove, vbReplace}));
    SetEditTextIfNotFocused(
        autoFrameModelIdEdit_,
        ObjectValue(videoEffects, autoFrame).value(modelId).toString());
    SetEditTextIfNotFocused(
        eyeContactModelIdEdit_,
        ObjectValue(videoEffects, eyeContact).value(modelId).toString());
    SetEditTextIfNotFocused(
        denoiseModelIdEdit_,
        ObjectValue(videoEffects, denoise).value(modelId).toString());
    SetEditTextIfNotFocused(
        virtualBackgroundReplacePathEdit_,
        ObjectValue(videoEffects, vbReplace).value(replacePath).toString());
    SetEditTextIfNotFocused(
        virtualKeyLightHdriPathEdit_,
        ObjectValue(videoEffects, keyLight).value(hdriPath).toString());
  }

  updatingUi_ = false;
}

void AdvancedPage::OnAlwaysOnToggled(bool checked) {
  if (updatingUi_)
    return;

  QString error;
  const std::string request =
      std::string("SET_VIDEO_CONFIG always_on=") + (checked ? "1" : "0");
  if (!SendDaemonRequest(request, &error)) {
    if (alwaysOnCheck_) {
      alwaysOnCheck_->blockSignals(true);
      alwaysOnCheck_->setChecked(currentAlwaysOn_);
      alwaysOnCheck_->blockSignals(false);
    }
    ShowFailure(QStringLiteral("Always-on Camera Failed"), error);
    return;
  }

  currentAlwaysOn_ = checked;
  SetResult(checked ? QStringLiteral("Always-on camera was enabled.")
                    : QStringLiteral("Always-on camera was disabled."),
            QStringLiteral("good"));
}

void AdvancedPage::OnCreateVirtualMic() {
  if (daemonReachable_) {
    QJsonObject patch;
    patch.insert(QStringLiteral("create_virtual_mic"), true);
    QString error;
    if (!SendAudioPatch(patch, &error)) {
      ShowFailure(QStringLiteral("Create Virtual Mic Failed"), error);
      return;
    }
    SetResult(QStringLiteral("Virtual microphone create was sent to daemon."),
              QStringLiteral("good"));
    RefreshPulseState();
    return;
  }

#ifdef NDEBUG
  ShowFailure(QStringLiteral("Create Virtual Mic Failed"),
              QStringLiteral("Daemon unavailable. Start studiocastd and try "
                             "again."));
#else
  std::string error;
  if (!studiocast::audio::CreateVirtualMic(&error)) {
    ShowFailure(QStringLiteral("Create Virtual Mic Failed"),
                QString::fromStdString(error));
    return;
  }
  SetResult(QStringLiteral("Virtual microphone was created directly."),
            QStringLiteral("good"));
  RefreshPulseState();
#endif
}

void AdvancedPage::OnDestroyVirtualMic() {
  if (!ConfirmDestructive(
          QStringLiteral("Destroy Virtual Microphone"),
          QStringLiteral("Destroy StudioCast Microphone?"),
          QStringLiteral("This stops microphone processing and removes the "
                         "StudioCast virtual microphone device."),
          QStringLiteral("Destroy"))) {
    SetResult(QStringLiteral("Destroy virtual microphone cancelled."),
              QStringLiteral("warning"));
    return;
  }

  if (daemonReachable_) {
    QJsonObject patch;
    patch.insert(QStringLiteral("enabled"), false);
    patch.insert(QStringLiteral("create_virtual_mic"), false);
    QString error;
    if (!SendAudioPatch(patch, &error)) {
      ShowFailure(QStringLiteral("Destroy Virtual Mic Failed"), error);
      return;
    }
    SetResult(QStringLiteral("Virtual microphone destroy was sent to daemon."),
              QStringLiteral("good"));
    RefreshPulseState();
    return;
  }

#ifdef NDEBUG
  ShowFailure(QStringLiteral("Destroy Virtual Mic Failed"),
              QStringLiteral("Daemon unavailable. Start studiocastd and try "
                             "again."));
#else
  std::string error;
  if (!studiocast::audio::DestroyVirtualMic(&error)) {
    ShowFailure(QStringLiteral("Destroy Virtual Mic Failed"),
                QString::fromStdString(error));
    return;
  }
  SetResult(QStringLiteral("Virtual microphone was destroyed directly."),
            QStringLiteral("good"));
  RefreshPulseState();
#endif
}

void AdvancedPage::OnEnableVirtualSpeakers() {
  if (daemonReachable_) {
    QJsonObject patch;
    patch.insert(QStringLiteral("create_virtual_speakers"), true);
    patch.insert(QStringLiteral("speakers_enabled"), true);
    patch.insert(QStringLiteral("speaker_latency_ms"), 10);
    QString error;
    if (!SendAudioPatch(patch, &error)) {
      ShowFailure(QStringLiteral("Enable Speakers Failed"), error);
      return;
    }
    SetResult(QStringLiteral("StudioCast Speakers enable was sent to daemon."),
              QStringLiteral("good"));
    RefreshPulseState();
    return;
  }

#ifdef NDEBUG
  ShowFailure(QStringLiteral("Enable Speakers Failed"),
              QStringLiteral("Daemon unavailable. Start studiocastd and try "
                             "again."));
#else
  std::string error;
  if (!studiocast::audio::CreateVirtualSpeaker(&error)) {
    ShowFailure(QStringLiteral("Enable Speakers Failed"),
                QString::fromStdString(error));
    return;
  }
  error.clear();
  if (!studiocast::audio::StartSpeakerLoopback("", 10, &error)) {
    ShowFailure(QStringLiteral("Start Speaker Loopback Failed"),
                QString::fromStdString(error));
    return;
  }
  SetResult(QStringLiteral("StudioCast Speakers were enabled directly."),
            QStringLiteral("good"));
  RefreshPulseState();
#endif
}

void AdvancedPage::OnStopSpeakersRouting() {
  if (daemonReachable_) {
    QJsonObject patch;
    patch.insert(QStringLiteral("speakers_enabled"), false);
    QString error;
    if (!SendAudioPatch(patch, &error)) {
      ShowFailure(QStringLiteral("Stop Speaker Routing Failed"), error);
      return;
    }
    SetResult(QStringLiteral("Speaker routing stop was sent to daemon."),
              QStringLiteral("good"));
    RefreshPulseState();
    return;
  }

#ifdef NDEBUG
  ShowFailure(QStringLiteral("Stop Speaker Routing Failed"),
              QStringLiteral("Daemon unavailable. Start studiocastd and try "
                             "again."));
#else
  std::string error;
  if (!studiocast::audio::StopSpeakerLoopback(&error)) {
    ShowFailure(QStringLiteral("Stop Speaker Routing Failed"),
                QString::fromStdString(error));
    return;
  }
  SetResult(QStringLiteral("Speaker loopback was stopped directly."),
            QStringLiteral("good"));
  RefreshPulseState();
#endif
}

void AdvancedPage::OnDestroyVirtualSpeakers() {
  if (!ConfirmDestructive(
          QStringLiteral("Destroy StudioCast Speakers"),
          QStringLiteral("Destroy StudioCast Speakers?"),
          QStringLiteral("This stops speaker routing and removes the "
                         "StudioCast Speakers virtual device."),
          QStringLiteral("Destroy"))) {
    SetResult(QStringLiteral("Destroy speakers device cancelled."),
              QStringLiteral("warning"));
    return;
  }

  if (daemonReachable_) {
    QJsonObject patch;
    patch.insert(QStringLiteral("speakers_enabled"), false);
    patch.insert(QStringLiteral("create_virtual_speakers"), false);
    QString error;
    if (!SendAudioPatch(patch, &error)) {
      ShowFailure(QStringLiteral("Destroy Speakers Failed"), error);
      return;
    }
    SetResult(QStringLiteral("StudioCast Speakers destroy was sent to daemon."),
              QStringLiteral("good"));
    RefreshPulseState();
    return;
  }

#ifdef NDEBUG
  ShowFailure(QStringLiteral("Destroy Speakers Failed"),
              QStringLiteral("Daemon unavailable. Start studiocastd and try "
                             "again."));
#else
  std::string error;
  if (!studiocast::audio::DestroyVirtualSpeaker(&error)) {
    ShowFailure(QStringLiteral("Destroy Speakers Failed"),
                QString::fromStdString(error));
    return;
  }
  SetResult(QStringLiteral("StudioCast Speakers were destroyed directly."),
            QStringLiteral("good"));
  RefreshPulseState();
#endif
}

void AdvancedPage::OnSaveAudioModelOverrides() {
  QString error;
  if (!SaveAudioModelOverrides(&error)) {
    ShowFailure(QStringLiteral("Save Open Audio Overrides Failed"), error);
    return;
  }
  SetResult(QStringLiteral("Open Audio raw model values were saved."),
            QStringLiteral("good"));
}

void AdvancedPage::OnSaveVideoModelOverrides() {
  QString error;
  if (!SaveVideoModelOverrides(&error)) {
    ShowFailure(QStringLiteral("Save Open Video Raw Values Failed"), error);
    return;
  }
  SetResult(QStringLiteral("Open Video raw model values were saved."),
            QStringLiteral("good"));
}

void AdvancedPage::OnRefreshPulseState() {
  RefreshPulseState();
  SetResult(QStringLiteral("PulseAudio state refreshed."),
            pactlOk_ ? QStringLiteral("good") : QStringLiteral("warning"));
}

void AdvancedPage::OnLegacySourceChanged(int /*index*/) {
  if (updatingUi_)
    return;
  UpdateLegacyPorts();
}

void AdvancedPage::OnStartLegacyLoopback() {
#ifdef NDEBUG
  ShowFailure(QStringLiteral("Start Legacy Loopback Failed"),
              QStringLiteral("Legacy loopback mutation is disabled in release "
                             "builds."));
  return;
#else
  if (!legacySourceCombo_ || !legacySourceCombo_->isEnabled()) {
    ShowFailure(QStringLiteral("Start Legacy Loopback Failed"),
                QStringLiteral("No valid input source is selected."));
    return;
  }

  const QString selected = legacySourceCombo_->currentData().toString();
  const std::string source = selected.toStdString();
  const int latency = legacyLatencySpin_ ? legacyLatencySpin_->value() : 10;

  if (!source.empty() && legacyPortCombo_ && legacyPortCombo_->isEnabled()) {
    const std::string port =
        legacyPortCombo_->currentData().toString().toStdString();
    if (!port.empty()) {
      std::string error;
      if (!studiocast::audio::pulse::SetSourcePort(source, port, &error)) {
        ShowFailure(QStringLiteral("Set Input Port Failed"),
                    QString::fromStdString(error));
        return;
      }
    }
  }

  std::string error;
  if (!studiocast::audio::StartLoopback(source, latency, &error)) {
    ShowFailure(QStringLiteral("Start Legacy Loopback Failed"),
                QString::fromStdString(error));
    return;
  }

  SetResult(QStringLiteral("Legacy microphone loopback was started directly."),
            QStringLiteral("good"));
  RefreshPulseState();
#endif
}

void AdvancedPage::OnStopLegacyLoopback() {
#ifdef NDEBUG
  ShowFailure(QStringLiteral("Stop Legacy Loopback Failed"),
              QStringLiteral("Legacy loopback mutation is disabled in release "
                             "builds."));
  return;
#else
  std::string error;
  if (!studiocast::audio::StopLoopback(&error)) {
    ShowFailure(QStringLiteral("Stop Legacy Loopback Failed"),
                QString::fromStdString(error));
    return;
  }

  SetResult(QStringLiteral("Legacy microphone loopback was stopped directly."),
            QStringLiteral("good"));
  RefreshPulseState();
#endif
}

void AdvancedPage::RefreshPulseState() {
  std::string details;
  pactlOk_ = studiocast::audio::pulse::PactlAvailable(&details);
  hasVirtualMicSink_ = false;
  hasVirtualMicSource_ = false;
  hasLegacyMicLoopback_ = false;
  hasVirtualSpeakersSink_ = false;
  hasVirtualSpeakersLoopback_ = false;

  if (localAudioStatusText_) {
    SetPlainTextPreservingScroll(
        localAudioStatusText_,
        QString::fromStdString(studiocast::audio::StatusText()));
  }

  if (!pactlOk_) {
    if (pulseStateLabel_) {
      pulseStateLabel_->setText(
          QStringLiteral("pactl unavailable%1")
              .arg(details.empty()
                       ? QString()
                       : QStringLiteral(": %1")
                             .arg(QString::fromStdString(details))));
      SetDynamicProperty(pulseStateLabel_, "scStatus",
                         QStringLiteral("warning"));
    }
    RefreshLegacySources();
    UpdateButtonStates();
    return;
  }

  std::string error;
  const auto modules = studiocast::audio::pulse::ListModules(&error);
  for (const auto &module : modules) {
    if (module.name == "module-null-sink" &&
        Contains(module.args, "sink_name=studiocast_sink")) {
      hasVirtualMicSink_ = true;
    }
    if (module.name == "module-remap-source" &&
        Contains(module.args, "source_name=studiocast_mic")) {
      hasVirtualMicSource_ = true;
    }
    if (module.name == "module-loopback" &&
        Contains(module.args, "sink=studiocast_sink")) {
      hasLegacyMicLoopback_ = true;
    }
    if (module.name == "module-null-sink" &&
        Contains(module.args, "sink_name=studiocast_speakers")) {
      hasVirtualSpeakersSink_ = true;
    }
    if (module.name == "module-loopback" &&
        Contains(module.args, "source=studiocast_speakers.monitor")) {
      hasVirtualSpeakersLoopback_ = true;
    }
  }

  if (pulseStateLabel_) {
    QString text =
        QStringLiteral("pactl available; virtual_mic_modules=%1/%2, "
                       "mic_loopback=%3, speakers_sink=%4, "
                       "speakers_loopback=%5")
            .arg(BoolLabel(hasVirtualMicSink_),
                 BoolLabel(hasVirtualMicSource_),
                 BoolLabel(hasLegacyMicLoopback_),
                 BoolLabel(hasVirtualSpeakersSink_),
                 BoolLabel(hasVirtualSpeakersLoopback_));
    if (!error.empty())
      text += QStringLiteral("; module query warning: %1")
                  .arg(QString::fromStdString(error));
    pulseStateLabel_->setText(text);
    SetDynamicProperty(pulseStateLabel_, "scStatus",
                       error.empty() ? QStringLiteral("good")
                                     : QStringLiteral("warning"));
  }

  RefreshLegacySources();
  UpdateButtonStates();
}

void AdvancedPage::RefreshLegacySources() {
  if (!legacySourceCombo_)
    return;

  const QString prior = legacySourceCombo_->currentData().toString();
  updatingUi_ = true;
  legacySourceCombo_->blockSignals(true);
  legacySourceCombo_->clear();
  cachedSources_.clear();

  if (!pactlOk_) {
    legacySourceCombo_->addItem(QStringLiteral("pactl unavailable"),
                                QString());
    legacySourceCombo_->setEnabled(false);
    legacySourceCombo_->blockSignals(false);
    updatingUi_ = false;
    UpdateLegacyPorts();
    return;
  }

  legacySourceCombo_->setEnabled(true);
  legacySourceCombo_->addItem(QStringLiteral("Default Pulse source"),
                              QString());

  std::string error;
  cachedSources_ = studiocast::audio::pulse::ListSourcesDetailed(&error);
  for (const auto &source : cachedSources_) {
    if (source.name.empty())
      continue;
    std::string reason;
    if (studiocast::audio::IsUnsafeInputSourceName(source.name, &reason))
      continue;
    const std::string label =
        source.description.empty() ? source.name : source.description;
    legacySourceCombo_->addItem(QString::fromStdString(label),
                                QString::fromStdString(source.name));
  }

  if (legacySourceCombo_->count() == 1)
    legacySourceCombo_->addItem(QStringLiteral("<no suitable sources found>"),
                                QString());

  const int idx = legacySourceCombo_->findData(prior);
  legacySourceCombo_->setCurrentIndex(idx >= 0 ? idx : 0);
  legacySourceCombo_->blockSignals(false);
  updatingUi_ = false;
  UpdateLegacyPorts();
}

void AdvancedPage::UpdateLegacyPorts() {
  if (!legacyPortCombo_)
    return;

  legacyPortCombo_->clear();
  legacyPortCombo_->setEnabled(false);
  if (!legacySourceCombo_ || !legacySourceCombo_->isEnabled())
    return;

  const std::string selected =
      legacySourceCombo_->currentData().toString().toStdString();
  if (selected.empty())
    return;

  const studiocast::audio::pulse::PactlSourceInfo *info = nullptr;
  for (const auto &source : cachedSources_) {
    if (source.name == selected) {
      info = &source;
      break;
    }
  }
  if (!info || info->ports.empty())
    return;

  int activeIndex = -1;
  int firstAvailableIndex = -1;
  for (std::size_t i = 0; i < info->ports.size(); ++i) {
    const auto &port = info->ports[i];
    QString label = QString::fromStdString(port.description.empty()
                                               ? port.name
                                               : port.description);
    if (!port.available)
      label += QStringLiteral(" (unavailable)");
    legacyPortCombo_->addItem(label, QString::fromStdString(port.name));
    if (!info->active_port.empty() && port.name == info->active_port)
      activeIndex = static_cast<int>(i);
    if (firstAvailableIndex < 0 && port.available)
      firstAvailableIndex = static_cast<int>(i);
  }

  legacyPortCombo_->setEnabled(true);
  if (activeIndex >= 0)
    legacyPortCombo_->setCurrentIndex(activeIndex);
  else if (firstAvailableIndex >= 0)
    legacyPortCombo_->setCurrentIndex(firstAvailableIndex);
  else
    legacyPortCombo_->setCurrentIndex(0);
}

void AdvancedPage::UpdateButtonStates() {
  if (copySocketButton_)
    copySocketButton_->setEnabled(socketPathLabel_ &&
                                  socketPathLabel_->text() !=
                                      QStringLiteral("Unknown"));
  if (copyRawStatusButton_)
    copyRawStatusButton_->setEnabled(!currentRawStatus_.trimmed().isEmpty());

  if (alwaysOnCheck_)
    alwaysOnCheck_->setEnabled(daemonReachable_);
  if (saveAudioModelsButton_)
    saveAudioModelsButton_->setEnabled(daemonReachable_);
  if (saveVideoModelsButton_)
    saveVideoModelsButton_->setEnabled(daemonReachable_);

#ifdef NDEBUG
  const bool canLifecycleMutate = pactlOk_ && daemonReachable_;
  const bool canLegacyMutate = false;
#else
  const bool canLifecycleMutate = pactlOk_;
  const bool canLegacyMutate = pactlOk_;
#endif

  if (createVirtualMicButton_)
    createVirtualMicButton_->setEnabled(canLifecycleMutate);
  if (destroyVirtualMicButton_) {
    destroyVirtualMicButton_->setEnabled(
        canLifecycleMutate && (configuredVirtualMic_ || hasVirtualMicSink_ ||
                               hasVirtualMicSource_));
  }
  if (enableVirtualSpeakersButton_)
    enableVirtualSpeakersButton_->setEnabled(canLifecycleMutate);
  if (stopSpeakersRoutingButton_) {
    stopSpeakersRoutingButton_->setEnabled(
        canLifecycleMutate &&
        (configuredSpeakersEnabled_ || speakersRoutingActive_ ||
         hasVirtualSpeakersLoopback_));
  }
  if (destroyVirtualSpeakersButton_) {
    destroyVirtualSpeakersButton_->setEnabled(
        canLifecycleMutate &&
        (configuredVirtualSpeakers_ || hasVirtualSpeakersSink_));
  }

  if (startLegacyLoopbackButton_) {
    startLegacyLoopbackButton_->setEnabled(
        canLegacyMutate && legacySourceCombo_ &&
        legacySourceCombo_->isEnabled());
  }
  if (stopLegacyLoopbackButton_)
    stopLegacyLoopbackButton_->setEnabled(canLegacyMutate &&
                                          hasLegacyMicLoopback_);
}

bool AdvancedPage::SendDaemonRequest(const std::string &request,
                                     QString *error) {
  studiocast::ipc::DaemonCallResult res;
  studiocast::ipc::DaemonCallOptions options;
  options.connect_timeout_ms = 500;
  options.io_timeout_ms = 2500;

  std::string transportError;
  if (!studiocast::ipc::DaemonCall(request, &res, &transportError, options)) {
    if (error) {
      *error = QStringLiteral("Daemon unavailable: %1")
                   .arg(QString::fromStdString(transportError));
    }
    return false;
  }

  if (!res.ok) {
    if (error) {
      *error = HumanDaemonError(
          QString::fromStdString(res.error_json.empty()
                                     ? std::string("daemon_error")
                                     : res.error_json));
    }
    return false;
  }

  return true;
}

bool AdvancedPage::FetchDaemonJson(const std::string &request,
                                   QJsonObject *out, QString *error) {
  studiocast::ipc::DaemonCallResult res;
  studiocast::ipc::DaemonCallOptions options;
  options.connect_timeout_ms = 500;
  options.io_timeout_ms = 2500;

  std::string transportError;
  if (!studiocast::ipc::DaemonCall(request, &res, &transportError, options)) {
    if (error) {
      *error = QStringLiteral("Daemon unavailable: %1")
                   .arg(QString::fromStdString(transportError));
    }
    return false;
  }

  if (!res.ok) {
    if (error) {
      *error = HumanDaemonError(
          QString::fromStdString(res.error_json.empty()
                                     ? std::string("daemon_error")
                                     : res.error_json));
    }
    return false;
  }

  return ParseJsonObject(res.json, out, error);
}

bool AdvancedPage::SendAudioPatch(const QJsonObject &patch, QString *error) {
  return SendDaemonRequest(
      std::string("SET_AUDIO_CONFIG ") + CompactJson(patch), error);
}

bool AdvancedPage::SaveAudioModelOverrides(QString *error) {
  if (!daemonReachable_) {
    if (error)
      *error = QStringLiteral("Daemon unavailable.");
    return false;
  }

  QJsonObject audioConfig;
  if (!FetchDaemonJson("GET_AUDIO_CONFIG", &audioConfig, error))
    return false;

  QJsonObject effects =
      ObjectValue(audioConfig, QStringLiteral("audio_effects"));
  if (effects.isEmpty()) {
    if (error) {
      *error = QStringLiteral(
          "Daemon audio config did not include audio_effects.");
    }
    return false;
  }

  QJsonObject mic = ObjectValue(effects, QStringLiteral("microphone"));
  mic.insert(QStringLiteral("model_id"),
             micModelIdEdit_ ? micModelIdEdit_->text().trimmed() : QString());
  mic.insert(
      QStringLiteral("model_path"),
      micModelPathEdit_ ? micModelPathEdit_->text().trimmed() : QString());
  effects.insert(QStringLiteral("microphone"), mic);

  QJsonObject speaker = ObjectValue(effects, QStringLiteral("speaker"));
  speaker.insert(
      QStringLiteral("model_id"),
      speakerModelIdEdit_ ? speakerModelIdEdit_->text().trimmed() : QString());
  speaker.insert(QStringLiteral("model_path"),
                 speakerModelPathEdit_
                     ? speakerModelPathEdit_->text().trimmed()
                     : QString());
  effects.insert(QStringLiteral("speaker"), speaker);

  QJsonObject patch;
  patch.insert(QStringLiteral("audio_effects"), effects);
  return SendAudioPatch(patch, error);
}

bool AdvancedPage::SaveVideoModelOverrides(QString *error) {
  if (!daemonReachable_) {
    if (error)
      *error = QStringLiteral("Daemon unavailable.");
    return false;
  }

  QJsonObject effects;
  if (!FetchDaemonJson("GET_CONFIG", &effects, error))
    return false;
  if (effects.isEmpty()) {
    if (error)
      *error = QStringLiteral("Daemon video config was empty.");
    return false;
  }

  const QString vbBlur =
      ContractKey(video_contract::kEffectIdVirtualBackgroundBlur);
  const QString vbRemove =
      ContractKey(video_contract::kEffectIdVirtualBackgroundRemove);
  const QString vbReplace =
      ContractKey(video_contract::kEffectIdVirtualBackgroundReplace);
  const QString autoFrame = ContractKey(video_contract::kEffectIdAutoFrame);
  const QString eyeContact = ContractKey(video_contract::kEffectIdEyeContact);
  const QString denoise =
      ContractKey(video_contract::kEffectIdVideoNoiseRemoval);
  const QString keyLight =
      ContractKey(video_contract::kEffectIdVirtualKeyLight);
  const QString modelId = ContractKey(video_contract::param::kModelId);
  const QString replacePath =
      ContractKey(video_contract::param::kVbReplacePath);
  const QString hdriPath = ContractKey(video_contract::param::kHdriPath);

  const QString vbModelId =
      virtualBackgroundModelIdEdit_
          ? virtualBackgroundModelIdEdit_->text().trimmed()
          : QString();
  for (const QString &key : {vbBlur, vbRemove, vbReplace})
    SetEffectString(&effects, key, modelId, vbModelId);
  SetEffectString(&effects, autoFrame, modelId,
                  autoFrameModelIdEdit_
                      ? autoFrameModelIdEdit_->text().trimmed()
                      : QString());
  SetEffectString(&effects, eyeContact, modelId,
                  eyeContactModelIdEdit_
                      ? eyeContactModelIdEdit_->text().trimmed()
                      : QString());
  SetEffectString(&effects, denoise, modelId,
                  denoiseModelIdEdit_ ? denoiseModelIdEdit_->text().trimmed()
                                      : QString());
  SetEffectString(&effects, vbReplace, replacePath,
                  virtualBackgroundReplacePathEdit_
                      ? virtualBackgroundReplacePathEdit_->text().trimmed()
                      : QString());
  SetEffectString(&effects, keyLight, hdriPath,
                  virtualKeyLightHdriPathEdit_
                      ? virtualKeyLightHdriPathEdit_->text().trimmed()
                      : QString());

  return SendDaemonRequest(
      std::string("SET_VIDEO_EFFECTS_JSON ") + CompactJson(effects), error);
}

bool AdvancedPage::ConfirmDestructive(const QString &title,
                                      const QString &text,
                                      const QString &detail,
                                      const QString &confirmText) {
  QMessageBox box(this);
  box.setIcon(QMessageBox::Warning);
  box.setWindowTitle(title);
  box.setText(text);
  box.setInformativeText(detail);
  auto *confirmButton = box.addButton(confirmText, QMessageBox::AcceptRole);
  box.addButton(QMessageBox::Cancel);
  box.setDefaultButton(QMessageBox::Cancel);
  box.exec();
  return box.clickedButton() == confirmButton;
}

void AdvancedPage::SetResult(const QString &text, const QString &status) {
  if (!resultLabel_)
    return;
  resultLabel_->setText(text);
  resultLabel_->setVisible(!text.trimmed().isEmpty());
  SetDynamicProperty(resultLabel_, "scStatus", status);
}

void AdvancedPage::ShowFailure(const QString &title, const QString &details) {
  SetResult(QStringLiteral("Advanced action failed."),
            QStringLiteral("error"));
  QMessageBox::critical(this, title,
                        details.trimmed().isEmpty()
                            ? QStringLiteral("The action failed.")
                            : details);
}

} // namespace studiocast::gui
