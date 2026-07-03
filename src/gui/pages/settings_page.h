#pragma once

#include <QJsonObject>
#include <QString>
#include <QWidget>

#include <string>

class QLabel;
class QPushButton;

namespace studiocast::gui {

struct DaemonStatusSnapshot;

class SettingsPage final : public QWidget {
  Q_OBJECT

public:
  explicit SettingsPage(QWidget *parent = nullptr);

  void UpdateStatus(const DaemonStatusSnapshot &snapshot);

private:
  enum class AudioEffectsScope {
    Microphone,
    Speaker,
  };

  void ResetCameraEffects();
  void ResetMicrophoneEffects();
  void ResetSpeakerEffects();
  void ResetDeviceSelections();
  void RestoreAllDefaults();

  bool ConfirmReset(const QString &title, const QString &text,
                    const QString &detail,
                    const QString &confirmText = QStringLiteral("Reset"));
  bool SendDaemonRequest(const std::string &request, QString *error);
  bool FetchDaemonJson(const std::string &request, QJsonObject *out,
                       QString *error);

  bool WriteDefaultCameraEffects(QString *error);
  bool WriteAudioEffectsReset(AudioEffectsScope scope, QString *error);
  bool WriteDeviceSelectionsToAuto(QString *error);
  bool WriteVideoDeviceSelectionsToAuto(QString *error);
  bool WriteAudioDeviceSelectionsToAuto(QString *error);

  void SetResult(const QString &text, const QString &status);
  void ShowWriteFailure(const QString &title, const QString &details);
  void UpdateButtons();

  QLabel *daemonStateLabel_ = nullptr;
  QLabel *daemonDetailLabel_ = nullptr;
  QLabel *resultLabel_ = nullptr;

  QPushButton *resetCameraEffectsButton_ = nullptr;
  QPushButton *resetMicrophoneEffectsButton_ = nullptr;
  QPushButton *resetSpeakerEffectsButton_ = nullptr;
  QPushButton *resetDeviceSelectionsButton_ = nullptr;
  QPushButton *restoreAllButton_ = nullptr;

  bool daemonReachable_ = false;
};

} // namespace studiocast::gui
