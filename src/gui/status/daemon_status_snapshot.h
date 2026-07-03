#pragma once

#include <QString>
#include <QStringList>

namespace studiocast::gui {

enum class ReadinessState {
  Unknown,
  Ready,
  NeedsSetup,
  DaemonUnavailable,
  MissingVirtualDevice,
  NoPhysicalDevice,
  Idle,
  Processing,
  RecoverableError,
  FatalError,
  MissingModel,
};

struct DeviceReadiness {
  ReadinessState state = ReadinessState::Unknown;
  QString summary;
  QString detail;
  QStringList notes;
  QStringList disabledReasons;
};

struct EngineStatus {
  QString id;
  QString label;
  bool present = false;
  bool ok = false;
  bool supported = false;
  QString summary;
  QString blockedReason;
  QStringList blockedDetails;
  QStringList missingModels;
  QStringList blockedEffects;
  QStringList installHints;
  int installedModelCount = 0;
  int knownModelCount = 0;
  int missingModelCount = 0;
};

struct DaemonStatusSnapshot {
  bool reachable = false;
  bool parsed = false;
  QString transportError;
  QString parseError;

  QString rawJson;
  QString version;
  QString gitSha;
  QString socketPath;
  bool serviceRunning = false;

  DeviceReadiness camera;
  DeviceReadiness microphone;
  DeviceReadiness speakers;
  QString videoEffectsEnginePreference;
  QString audioEffectsEnginePreference;

  EngineStatus maxine;
  EngineStatus openCuda;
  EngineStatus openAudio;

  static DaemonStatusSnapshot Unreachable(const QString &error);
  static DaemonStatusSnapshot FromJson(const QString &json);

  QString ServiceSummary() const;
  QString ServiceDetail() const;
};

QString ReadinessLabel(ReadinessState state);

} // namespace studiocast::gui
