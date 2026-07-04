#include "settings_page.h"

#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

#include <functional>
#include <string>
#include <vector>

#include "core/audio/effects/broadcast_audio_effects.h"
#include "core/audio/effects/broadcast_audio_effects_json.h"
#include "core/ipc/daemon_client.h"
#include "core/video/broadcast_camera_effects_json.h"
#include "core/video/effects/broadcast_effects.h"
#include "gui/status/daemon_status_snapshot.h"

namespace studiocast::gui {
namespace {

struct ResetStep {
  QString label;
  std::function<bool(QString *)> write;
};

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

QString HumanDaemonError(const QString &raw) {
  const QString trimmed = raw.trimmed();
  if (trimmed.isEmpty())
    return QStringLiteral("Unknown service error.");

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
    if (error)
      *error = QStringLiteral("JSON parse error: %1")
                   .arg(parseError.errorString());
    return false;
  }
  if (out)
    *out = doc.object();
  return true;
}

QJsonObject DefaultAudioEffectsObject() {
  const auto json =
      studiocast::audio::effects::BroadcastAudioEffectsToJson(
          studiocast::audio::effects::BroadcastAudioEffects{});
  QJsonObject out;
  QString error;
  (void)ParseJsonObject(json, &out, &error);
  return out;
}

QJsonObject NormalizedCurrentAudioEffects(const QJsonObject &audioConfig,
                                          QString *error) {
  const QJsonObject defaults = DefaultAudioEffectsObject();
  const QJsonObject current =
      audioConfig.value(QStringLiteral("audio_effects")).toObject();

  if (current.isEmpty()) {
    if (error) {
      *error = QStringLiteral("Background service audio config did not include "
                              "audio_effects; no settings were changed.");
    }
    return {};
  }

  QJsonObject effects = current;
  effects.insert(QStringLiteral("schema_version"),
                 studiocast::audio::effects::
                     kBroadcastAudioEffectsSchemaVersion);
  if (!effects.value(QStringLiteral("engine")).isString()) {
    effects.insert(QStringLiteral("engine"),
                   defaults.value(QStringLiteral("engine")));
  }
  if (!effects.value(QStringLiteral("microphone")).isObject()) {
    effects.insert(QStringLiteral("microphone"),
                   defaults.value(QStringLiteral("microphone")));
  }
  if (!effects.value(QStringLiteral("speaker")).isObject()) {
    effects.insert(QStringLiteral("speaker"),
                   defaults.value(QStringLiteral("speaker")));
  }

  return effects;
}

std::string CompactJson(const QJsonObject &obj) {
  return QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString();
}

QFrame *ActionRow(const QString &title, const QString &detail,
                  const QString &buttonText, QPushButton **buttonOut,
                  QWidget *parent) {
  auto *row = new QFrame(parent);
  row->setProperty("scRole", "settingsAction");
  auto *layout = new QHBoxLayout(row);
  layout->setContentsMargins(12, 10, 12, 10);
  layout->setSpacing(12);

  auto *textLayout = new QVBoxLayout();
  textLayout->setContentsMargins(0, 0, 0, 0);
  textLayout->setSpacing(4);
  textLayout->addWidget(ValueLabel(title, row));
  textLayout->addWidget(MutedLabel(detail, row));
  layout->addLayout(textLayout, 1);

  auto *button = new QPushButton(buttonText, row);
  button->setIcon(row->style()->standardIcon(QStyle::SP_BrowserReload));
  button->setMinimumWidth(132);
  layout->addWidget(button, 0, Qt::AlignTop);

  if (buttonOut)
    *buttonOut = button;
  return row;
}

} // namespace

SettingsPage::SettingsPage(QWidget *parent) : QWidget(parent) {
  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(16, 16, 16, 16);
  root->setSpacing(12);

  auto *statusBox = new QGroupBox(QStringLiteral("Background Service"), this);
  auto *statusLayout = new QVBoxLayout(statusBox);
  statusLayout->setSpacing(6);
  daemonStateLabel_ = ValueLabel(QStringLiteral("Checking service"), statusBox);
  daemonDetailLabel_ =
      MutedLabel(QStringLiteral("Waiting for service status."), statusBox);
  resultLabel_ = MutedLabel(QString(), statusBox);
  resultLabel_->setVisible(false);
  statusLayout->addWidget(daemonStateLabel_);
  statusLayout->addWidget(daemonDetailLabel_);
  statusLayout->addWidget(resultLabel_);
  root->addWidget(statusBox);

  auto *scopedBox = new QGroupBox(QStringLiteral("Scoped Resets"), this);
  auto *scopedLayout = new QVBoxLayout(scopedBox);
  scopedLayout->setSpacing(10);
  scopedLayout->addWidget(ActionRow(
      QStringLiteral("Camera effects"),
      QStringLiteral("Reset camera effects, model selections, and camera "
                     "effect engine preference."),
      QStringLiteral("Reset"), &resetCameraEffectsButton_, scopedBox));
  scopedLayout->addWidget(ActionRow(
      QStringLiteral("Microphone effects"),
      QStringLiteral("Reset microphone cleanup, strength, and microphone "
                     "model selection."),
      QStringLiteral("Reset"), &resetMicrophoneEffectsButton_, scopedBox));
  scopedLayout->addWidget(ActionRow(
      QStringLiteral("Speaker effects"),
      QStringLiteral("Reset speaker cleanup, strength, and speaker model "
                     "selection."),
      QStringLiteral("Reset"), &resetSpeakerEffectsButton_, scopedBox));
  scopedLayout->addWidget(ActionRow(
      QStringLiteral("Device selections"),
      QStringLiteral("Return camera input/output, microphone source, and "
                     "speaker output target to Auto."),
      QStringLiteral("Reset"), &resetDeviceSelectionsButton_, scopedBox));
  root->addWidget(scopedBox);

  auto *restoreBox = new QGroupBox(QStringLiteral("Restore Defaults"), this);
  auto *restoreLayout = new QVBoxLayout(restoreBox);
  restoreLayout->setSpacing(10);
  restoreLayout->addWidget(ActionRow(
      QStringLiteral("All normal settings"),
      QStringLiteral("Reset effects and device selections. Lifecycle, debug, "
                     "and advanced path controls are not changed."),
      QStringLiteral("Restore All"), &restoreAllButton_, restoreBox));
  root->addWidget(restoreBox);
  root->addStretch(1);

  connect(resetCameraEffectsButton_, &QPushButton::clicked, this,
          &SettingsPage::ResetCameraEffects);
  connect(resetMicrophoneEffectsButton_, &QPushButton::clicked, this,
          &SettingsPage::ResetMicrophoneEffects);
  connect(resetSpeakerEffectsButton_, &QPushButton::clicked, this,
          &SettingsPage::ResetSpeakerEffects);
  connect(resetDeviceSelectionsButton_, &QPushButton::clicked, this,
          &SettingsPage::ResetDeviceSelections);
  connect(restoreAllButton_, &QPushButton::clicked, this,
          &SettingsPage::RestoreAllDefaults);

  UpdateButtons();
}

void SettingsPage::UpdateStatus(const DaemonStatusSnapshot &snapshot) {
  daemonReachable_ = snapshot.reachable;

  if (daemonStateLabel_) {
    daemonStateLabel_->setText(snapshot.UserServiceSummary());
    if (!snapshot.reachable) {
      SetDynamicProperty(daemonStateLabel_, "scStatus",
                         QStringLiteral("error"));
    } else if (!snapshot.parsed || !snapshot.serviceRunning) {
      SetDynamicProperty(daemonStateLabel_, "scStatus",
                         QStringLiteral("warning"));
    } else {
      SetDynamicProperty(daemonStateLabel_, "scStatus", QStringLiteral("good"));
    }
  }

  if (daemonDetailLabel_)
    daemonDetailLabel_->setText(snapshot.UserServiceDetail());

  UpdateButtons();
}

void SettingsPage::ResetCameraEffects() {
  if (!ConfirmReset(
          QStringLiteral("Reset Camera Effects"),
          QStringLiteral("Reset camera effects to defaults?"),
          QStringLiteral("This saves the reset immediately. "
                         "Camera device selections and lifecycle controls are "
                         "not changed."))) {
    SetResult(QStringLiteral("Reset cancelled. No settings were changed."),
              QStringLiteral("warning"));
    return;
  }

  QString error;
  if (!WriteDefaultCameraEffects(&error)) {
    ShowWriteFailure(QStringLiteral("Reset Camera Effects Failed"), error);
    return;
  }

  SetResult(QStringLiteral("Camera effects reset was saved."),
            QStringLiteral("good"));
}

void SettingsPage::ResetMicrophoneEffects() {
  if (!ConfirmReset(
          QStringLiteral("Reset Microphone Effects"),
          QStringLiteral("Reset microphone effects to defaults?"),
          QStringLiteral("This saves the reset immediately. "
                         "Speaker effects, device selections, and lifecycle "
                         "controls are not changed."))) {
    SetResult(QStringLiteral("Reset cancelled. No settings were changed."),
              QStringLiteral("warning"));
    return;
  }

  QString error;
  if (!WriteAudioEffectsReset(AudioEffectsScope::Microphone, &error)) {
    ShowWriteFailure(QStringLiteral("Reset Microphone Effects Failed"), error);
    return;
  }

  SetResult(QStringLiteral("Microphone effects reset was saved."),
            QStringLiteral("good"));
}

void SettingsPage::ResetSpeakerEffects() {
  if (!ConfirmReset(
          QStringLiteral("Reset Speaker Effects"),
          QStringLiteral("Reset speaker effects to defaults?"),
          QStringLiteral("This saves the reset immediately. "
                         "Microphone effects, device selections, and lifecycle "
                         "controls are not changed."))) {
    SetResult(QStringLiteral("Reset cancelled. No settings were changed."),
              QStringLiteral("warning"));
    return;
  }

  QString error;
  if (!WriteAudioEffectsReset(AudioEffectsScope::Speaker, &error)) {
    ShowWriteFailure(QStringLiteral("Reset Speaker Effects Failed"), error);
    return;
  }

  SetResult(QStringLiteral("Speaker effects reset was saved."),
            QStringLiteral("good"));
}

void SettingsPage::ResetDeviceSelections() {
  if (!ConfirmReset(
          QStringLiteral("Reset Device Selections"),
          QStringLiteral("Return device selections to Auto?"),
          QStringLiteral("This writes camera input/output, microphone source, "
                         "and speaker output target to Auto. Effects and "
                         "lifecycle controls are not changed."))) {
    SetResult(QStringLiteral("Reset cancelled. No settings were changed."),
              QStringLiteral("warning"));
    return;
  }

  QString error;
  if (!WriteDeviceSelectionsToAuto(&error)) {
    ShowWriteFailure(QStringLiteral("Reset Device Selections Failed"), error);
    return;
  }

  SetResult(QStringLiteral("Device selections were reset to Auto."),
            QStringLiteral("good"));
}

void SettingsPage::RestoreAllDefaults() {
  if (!ConfirmReset(
          QStringLiteral("Restore Defaults"),
          QStringLiteral("Restore all normal StudioCast settings to defaults?"),
          QStringLiteral("Camera, microphone, and speaker effects will reset. "
                         "Device selections return to Auto. Lifecycle, debug, "
                         "and advanced path controls are not changed."),
          QStringLiteral("Restore"))) {
    SetResult(QStringLiteral("Restore cancelled. No settings were changed."),
              QStringLiteral("warning"));
    return;
  }

  const std::vector<ResetStep> steps = {
      {QStringLiteral("audio effects and audio device selections"),
       [this](QString *error) {
         QJsonObject patch;
         patch.insert(QStringLiteral("audio_effects"),
                      DefaultAudioEffectsObject());
         patch.insert(QStringLiteral("source"), QStringLiteral("auto"));
         patch.insert(QStringLiteral("speaker_target_sink"),
                      QStringLiteral("auto"));
         return SendDaemonRequest(
             std::string("SET_AUDIO_CONFIG ") + CompactJson(patch), error);
       }},
      {QStringLiteral("video device selections"),
       [this](QString *error) {
         return WriteVideoDeviceSelectionsToAuto(error);
       }},
      {QStringLiteral("camera effects"),
       [this](QString *error) { return WriteDefaultCameraEffects(error); }},
  };

  QStringList completed;
  for (const ResetStep &step : steps) {
    QString error;
    if (!step.write(&error)) {
      QString details = error;
      if (!completed.isEmpty()) {
        details += QStringLiteral("\n\nAlready saved: %1.")
                       .arg(completed.join(QStringLiteral(", ")));
      }
      ShowWriteFailure(QStringLiteral("Restore Defaults Failed"), details);
      return;
    }
    completed.push_back(step.label);
  }

  SetResult(QStringLiteral("All normal defaults were restored."),
            QStringLiteral("good"));
}

bool SettingsPage::ConfirmReset(const QString &title, const QString &text,
                                const QString &detail,
                                const QString &confirmText) {
  QMessageBox box(this);
  box.setIcon(QMessageBox::Warning);
  box.setWindowTitle(title);
  box.setText(text);
  box.setInformativeText(detail);
  auto *resetButton =
      box.addButton(confirmText, QMessageBox::AcceptRole);
  box.addButton(QMessageBox::Cancel);
  box.setDefaultButton(QMessageBox::Cancel);
  box.exec();
  return box.clickedButton() == resetButton;
}

bool SettingsPage::SendDaemonRequest(const std::string &request,
                                     QString *error) {
  studiocast::ipc::DaemonCallResult res;
  studiocast::ipc::DaemonCallOptions options;
  options.connect_timeout_ms = 500;
  options.io_timeout_ms = 2500;

  std::string transportError;
  if (!studiocast::ipc::DaemonCall(request, &res, &transportError, options)) {
    if (error)
      *error = QStringLiteral("Background service unavailable: %1")
                   .arg(QString::fromStdString(transportError));
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

bool SettingsPage::FetchDaemonJson(const std::string &request,
                                   QJsonObject *out, QString *error) {
  studiocast::ipc::DaemonCallResult res;
  studiocast::ipc::DaemonCallOptions options;
  options.connect_timeout_ms = 500;
  options.io_timeout_ms = 2500;

  std::string transportError;
  if (!studiocast::ipc::DaemonCall(request, &res, &transportError, options)) {
    if (error)
      *error = QStringLiteral("Background service unavailable: %1")
                   .arg(QString::fromStdString(transportError));
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

bool SettingsPage::WriteDefaultCameraEffects(QString *error) {
  const std::string json =
      studiocast::video::BroadcastCameraEffectsContractToJson(
          studiocast::video::effects::BroadcastCameraEffects{});
  return SendDaemonRequest(std::string("SET_VIDEO_EFFECTS_JSON ") + json,
                           error);
}

bool SettingsPage::WriteAudioEffectsReset(AudioEffectsScope scope,
                                          QString *error) {
  QJsonObject audioConfig;
  if (!FetchDaemonJson("GET_AUDIO_CONFIG", &audioConfig, error))
    return false;

  QString normalizeError;
  QJsonObject effects =
      NormalizedCurrentAudioEffects(audioConfig, &normalizeError);
  if (effects.isEmpty()) {
    if (error)
      *error = normalizeError;
    return false;
  }

  const QJsonObject defaults = DefaultAudioEffectsObject();
  if (scope == AudioEffectsScope::Microphone) {
    effects.insert(QStringLiteral("microphone"),
                   defaults.value(QStringLiteral("microphone")));
  } else {
    effects.insert(QStringLiteral("speaker"),
                   defaults.value(QStringLiteral("speaker")));
  }

  QJsonObject patch;
  patch.insert(QStringLiteral("audio_effects"), effects);
  return SendDaemonRequest(
      std::string("SET_AUDIO_CONFIG ") + CompactJson(patch), error);
}

bool SettingsPage::WriteDeviceSelectionsToAuto(QString *error) {
  QString audioError;
  if (!WriteAudioDeviceSelectionsToAuto(&audioError)) {
    if (error)
      *error = audioError;
    return false;
  }

  QString videoError;
  if (!WriteVideoDeviceSelectionsToAuto(&videoError)) {
    if (error) {
      *error = videoError +
               QStringLiteral("\n\nAlready saved: audio device selections.");
    }
    return false;
  }

  return true;
}

bool SettingsPage::WriteVideoDeviceSelectionsToAuto(QString *error) {
  return SendDaemonRequest("SET_VIDEO_CONFIG input=auto output=auto", error);
}

bool SettingsPage::WriteAudioDeviceSelectionsToAuto(QString *error) {
  QJsonObject patch;
  patch.insert(QStringLiteral("source"), QStringLiteral("auto"));
  patch.insert(QStringLiteral("speaker_target_sink"), QStringLiteral("auto"));
  return SendDaemonRequest(
      std::string("SET_AUDIO_CONFIG ") + CompactJson(patch), error);
}

void SettingsPage::SetResult(const QString &text, const QString &status) {
  if (!resultLabel_)
    return;
  resultLabel_->setText(text);
  resultLabel_->setVisible(!text.trimmed().isEmpty());
  SetDynamicProperty(resultLabel_, "scStatus", status);
}

void SettingsPage::ShowWriteFailure(const QString &title,
                                    const QString &details) {
  SetResult(QStringLiteral("Reset failed. Settings were not saved."),
            QStringLiteral("error"));
  QMessageBox box(this);
  box.setIcon(QMessageBox::Critical);
  box.setWindowTitle(title);
  box.setText(QStringLiteral("Settings were not saved."));
  box.setInformativeText(
      QStringLiteral("Open Support for technical details."));
  const QString detailText =
      details.trimmed().isEmpty()
          ? QStringLiteral("The background service rejected the write.")
          : details;
  box.setDetailedText(detailText);
  box.exec();
}

void SettingsPage::UpdateButtons() {
  const bool enabled = daemonReachable_;
  for (QPushButton *button :
       {resetCameraEffectsButton_, resetMicrophoneEffectsButton_,
        resetSpeakerEffectsButton_, resetDeviceSelectionsButton_,
        restoreAllButton_}) {
    if (button)
      button->setEnabled(enabled);
  }
}

} // namespace studiocast::gui
