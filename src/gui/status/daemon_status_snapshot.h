#pragma once

#include <QString>
#include <QStringList>

#include <vector>

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

struct EngineModelEntry {
  QString id;
  QString displayName;
  QString category;
  QString details;
  bool installed = true;
};

struct ConfiguredModelEntry {
  QString owner;
  QString modelId;
  QString modelPath;
  bool modelIdReported = false;
  bool modelIdExplicitlyMissing = false;
};

struct EngineStatus {
  QString id;
  QString label;
  bool present = false;
  bool ok = false;
  bool supported = false;
  QString rawJson;
  QString summary;
  QString blockedReason;
  QStringList blockedDetails;
  QStringList missingModels;
  QStringList blockedEffects;
  QStringList installHints;
  QStringList availableEffects;
  QString defaultModelId;
  std::vector<EngineModelEntry> installedModels;
  std::vector<EngineModelEntry> missingModelEntries;
  std::vector<ConfiguredModelEntry> configuredModels;
  int installedModelCount = 0;
  int knownModelCount = 0;
  int missingModelCount = 0;
  int configuredMissingModelCount = 0;
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
  QString videoEffectsActiveBackends;
  QString microphoneActiveBackend;
  QString speakersActiveBackend;
  QString speakersRouteMode;

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
