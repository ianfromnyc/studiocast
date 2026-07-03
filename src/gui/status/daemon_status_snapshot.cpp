#include "daemon_status_snapshot.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

#include <limits>

namespace studiocast::gui {
namespace {

QString FirstNonEmpty(std::initializer_list<QString> values) {
  for (const auto &value : values) {
    const QString trimmed = value.trimmed();
    if (!trimmed.isEmpty())
      return trimmed;
  }
  return {};
}

QJsonObject ObjectValue(const QJsonObject &obj, const QString &key) {
  const QJsonValue value = obj.value(key);
  return value.isObject() ? value.toObject() : QJsonObject{};
}

QJsonObject EngineObject(const QJsonObject &root, const QString &key) {
  const QJsonObject engines = ObjectValue(root, QStringLiteral("engines"));
  const QJsonObject nested = ObjectValue(engines, key);
  if (!nested.isEmpty())
    return nested;
  return ObjectValue(root, key);
}

QStringList StringListValue(const QJsonObject &obj, const QString &key) {
  QStringList out;
  const QJsonArray array = obj.value(key).toArray();
  for (const QJsonValue &value : array) {
    const QString text = value.toString().trimmed();
    if (!text.isEmpty())
      out.push_back(text);
  }
  return out;
}

QStringList StringMapDetails(const QJsonObject &obj, const QString &key) {
  QStringList out;
  const QJsonObject map = ObjectValue(obj, key);
  for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
    const QString id = it.key().trimmed();
    const QString reason = it.value().toString().trimmed();
    if (id.isEmpty() && reason.isEmpty())
      continue;
    if (reason.isEmpty()) {
      out.push_back(id);
    } else if (id.isEmpty()) {
      out.push_back(reason);
    } else {
      out.push_back(QStringLiteral("%1: %2").arg(id, reason));
    }
  }
  return out;
}

QStringList DisabledEffectReasons(const QJsonObject &video) {
  QStringList out;
  const QJsonObject plan = ObjectValue(video, QStringLiteral("effects_plan"));
  const QJsonArray disabled = plan.value(QStringLiteral("disabled")).toArray();
  for (const QJsonValue &value : disabled) {
    if (!value.isObject())
      continue;
    const QJsonObject obj = value.toObject();
    const QString id = obj.value(QStringLiteral("id")).toString().trimmed();
    const QString reason =
        obj.value(QStringLiteral("reason")).toString().trimmed();
    if (id.isEmpty() && reason.isEmpty())
      continue;
    if (reason.isEmpty()) {
      out.push_back(id);
    } else if (id.isEmpty()) {
      out.push_back(reason);
    } else {
      out.push_back(QStringLiteral("%1: %2").arg(id, reason));
    }
  }
  return out;
}

int SizeToInt(qsizetype size) {
  constexpr int kMax = std::numeric_limits<int>::max();
  if (size > static_cast<qsizetype>(kMax))
    return kMax;
  return static_cast<int>(size);
}

int CountObjectEntries(const QJsonObject &obj, const QString &key) {
  const QJsonValue value = obj.value(key);
  if (value.isObject())
    return SizeToInt(value.toObject().size());
  if (value.isArray())
    return SizeToInt(value.toArray().size());
  return 0;
}

EngineStatus ParseEngine(const QJsonObject &obj, const QString &id,
                         const QString &label) {
  EngineStatus out;
  out.id = id;
  out.label = label;
  out.present = !obj.isEmpty();
  if (!out.present) {
    out.summary = QStringLiteral("No daemon diagnostics reported.");
    return out;
  }

  out.ok = obj.value(QStringLiteral("ok")).toBool(false);
  out.supported = obj.value(QStringLiteral("supported")).toBool(out.ok);
  out.summary = FirstNonEmpty({
      obj.value(QStringLiteral("summary")).toString(),
      obj.value(QStringLiteral("last_error")).toString(),
      obj.value(QStringLiteral("error")).toString(),
  });
  out.blockedReason = obj.value(QStringLiteral("blocked_reason")).toString();
  out.blockedDetails = StringListValue(obj, QStringLiteral("blocked_details"));
  out.missingModels = StringMapDetails(obj, QStringLiteral("missing_models"));
  out.blockedEffects = StringMapDetails(obj, QStringLiteral("blocked_effects"));
  out.installHints = StringListValue(obj, QStringLiteral("install_hints"));
  out.installedModelCount =
      SizeToInt(obj.value(QStringLiteral("installed_models")).toArray().size());
  out.knownModelCount =
      SizeToInt(obj.value(QStringLiteral("models")).toArray().size());
  out.missingModelCount = out.missingModels.isEmpty()
                              ? CountObjectEntries(
                                    obj, QStringLiteral("missing_models"))
                              : SizeToInt(out.missingModels.size());

  if (out.summary.isEmpty()) {
    if (out.ok || out.supported) {
      out.summary = QStringLiteral("Available.");
    } else if (!out.blockedReason.isEmpty()) {
      out.summary = out.blockedReason;
    } else {
      out.summary = QStringLiteral("Unavailable.");
    }
  }

  return out;
}

DeviceReadiness MakeDevice(ReadinessState state, const QString &summary,
                           const QString &detail = {}) {
  DeviceReadiness out;
  out.state = state;
  out.summary = summary;
  out.detail = detail;
  return out;
}

DeviceReadiness WithNotes(DeviceReadiness out, const QStringList &notes,
                          const QStringList &disabledReasons = {}) {
  out.notes = notes;
  out.disabledReasons = disabledReasons;
  return out;
}

DeviceReadiness ParseCamera(const QJsonObject &video) {
  if (video.isEmpty()) {
    return MakeDevice(ReadinessState::Unknown,
                      QStringLiteral("Camera status is not reported."));
  }

  QStringList notes;
  const QJsonObject pipeline = ObjectValue(video, QStringLiteral("pipeline"));
  const QString effectsNote =
      pipeline.value(QStringLiteral("effects_note")).toString().trimmed();
  if (!effectsNote.isEmpty())
    notes.push_back(effectsNote);
  const QStringList disabledReasons = DisabledEffectReasons(video);

  const QString lastError = video.value(QStringLiteral("last_error")).toString();
  const QString virtualDeviceError =
      video.value(QStringLiteral("virtual_device_error")).toString();
  const QString consumerError =
      video.value(QStringLiteral("consumer_error")).toString();
  if (!lastError.isEmpty()) {
    return WithNotes(
        MakeDevice(ReadinessState::RecoverableError,
                   QStringLiteral("Camera reported an error."), lastError),
        notes, disabledReasons);
  }
  if (!virtualDeviceError.isEmpty()) {
    return WithNotes(
        MakeDevice(ReadinessState::MissingVirtualDevice,
                   QStringLiteral("StudioCast Camera is not available."),
                   virtualDeviceError),
        notes, disabledReasons);
  }
  if (!consumerError.isEmpty()) {
    return WithNotes(
        MakeDevice(ReadinessState::RecoverableError,
                   QStringLiteral("Camera consumer detection reported an "
                                  "error."),
                   consumerError),
        notes, disabledReasons);
  }

  const bool virtualPresent =
      video.value(QStringLiteral("virtual_device_present")).toBool(false);
  const bool virtualAvailable =
      video.value(QStringLiteral("virtual_device_available")).toBool(false);
  if (!virtualPresent || !virtualAvailable) {
    return WithNotes(
        MakeDevice(ReadinessState::MissingVirtualDevice,
                   QStringLiteral("StudioCast Camera needs setup."),
                   QStringLiteral("No writable virtual camera is available.")),
        notes, disabledReasons);
  }

  const bool enabled = video.value(QStringLiteral("enabled")).toBool(false);
  const bool running = pipeline.value(QStringLiteral("running")).toBool(false);
  const bool starting = pipeline.value(QStringLiteral("starting")).toBool(false);
  const int consumers = video.value(QStringLiteral("consumer_count")).toInt(0);
  if (enabled && (running || starting)) {
    return WithNotes(
        MakeDevice(ReadinessState::Processing,
                   QStringLiteral("Camera processing is active.")),
        notes, disabledReasons);
  }
  if (enabled) {
    return WithNotes(
        MakeDevice(ReadinessState::Idle,
                   consumers > 0
                       ? QStringLiteral("Camera is enabled and waiting.")
                       : QStringLiteral("Ready. Waiting for an app to use "
                                        "StudioCast Camera."),
                   pipeline.value(QStringLiteral("idle_reason")).toString()),
        notes, disabledReasons);
  }
  return WithNotes(
      MakeDevice(ReadinessState::Ready,
                 QStringLiteral("StudioCast Camera is available.")),
      notes, disabledReasons);
}

DeviceReadiness ParseMicrophone(const QJsonObject &audio) {
  if (audio.isEmpty()) {
    return MakeDevice(ReadinessState::Unknown,
                      QStringLiteral("Microphone status is not reported."));
  }

  const QString sourceError =
      audio.value(QStringLiteral("source_error")).toString();
  QStringList notes = StringListValue(audio, QStringLiteral("source_warnings"));
  const QJsonObject pipeline = ObjectValue(audio, QStringLiteral("pipeline"));
  const QString effectsNote =
      pipeline.value(QStringLiteral("effects_note")).toString().trimmed();
  if (!effectsNote.isEmpty())
    notes.push_back(effectsNote);
  if (!sourceError.isEmpty()) {
    return WithNotes(
        MakeDevice(ReadinessState::NoPhysicalDevice,
                   QStringLiteral("Choose a physical microphone input."),
                   sourceError),
        notes);
  }

  const bool micPresent = audio.value(QStringLiteral("mic_present")).toBool(
      audio.value(QStringLiteral("create_virtual_mic")).toBool(false));
  if (!micPresent) {
    return WithNotes(
        MakeDevice(ReadinessState::MissingVirtualDevice,
                   QStringLiteral("StudioCast Microphone needs setup."),
                   QStringLiteral("The virtual microphone is not present.")),
        notes);
  }

  const QString lastError =
      pipeline.value(QStringLiteral("last_error")).toString();
  if (!lastError.isEmpty()) {
    return WithNotes(
        MakeDevice(ReadinessState::RecoverableError,
                   QStringLiteral("Microphone processing reported an error."),
                   lastError),
        notes);
  }

  const bool running = pipeline.value(QStringLiteral("running")).toBool(false);
  const bool starting = pipeline.value(QStringLiteral("starting")).toBool(false);
  if (running || starting) {
    return WithNotes(
        MakeDevice(ReadinessState::Processing,
                   QStringLiteral("Microphone processing is active.")),
        notes);
  }
  return WithNotes(
      MakeDevice(ReadinessState::Ready,
                 QStringLiteral("StudioCast Microphone is available.")),
      notes);
}

DeviceReadiness ParseSpeakers(const QJsonObject &audio) {
  if (audio.isEmpty()) {
    return MakeDevice(ReadinessState::Unknown,
                      QStringLiteral("Speakers status is not reported."));
  }

  const QJsonObject speakers = ObjectValue(audio, QStringLiteral("speakers"));
  if (speakers.isEmpty()) {
    return MakeDevice(ReadinessState::Unknown,
                      QStringLiteral("Speakers status is not reported."));
  }

  QStringList notes;
  const QString effectsNote =
      speakers.value(QStringLiteral("effects_note")).toString().trimmed();
  if (!effectsNote.isEmpty())
    notes.push_back(effectsNote);
  const QString consumerError =
      speakers.value(QStringLiteral("consumer_error")).toString().trimmed();
  if (!consumerError.isEmpty())
    notes.push_back(consumerError);

  const QString targetError =
      speakers.value(QStringLiteral("target_sink_error")).toString();
  if (!targetError.isEmpty()) {
    return WithNotes(
        MakeDevice(ReadinessState::NoPhysicalDevice,
                   QStringLiteral("Choose a physical speaker output."),
                   targetError),
        notes);
  }

  const bool present = speakers.value(QStringLiteral("present")).toBool(
      audio.value(QStringLiteral("create_virtual_speakers")).toBool(false));
  if (!present) {
    return WithNotes(
        MakeDevice(ReadinessState::MissingVirtualDevice,
                   QStringLiteral("StudioCast Speakers need setup."),
                   QStringLiteral("The virtual speakers device is not "
                                  "present.")),
        notes);
  }

  const QString lastError =
      FirstNonEmpty({speakers.value(QStringLiteral("last_error")).toString(),
                     speakers.value(QStringLiteral("pipeline_last_error"))
                         .toString()});
  if (!lastError.isEmpty()) {
    return WithNotes(
        MakeDevice(ReadinessState::RecoverableError,
                   QStringLiteral("Speaker routing reported an error."),
                   lastError),
        notes);
  }

  const bool routing =
      speakers.value(QStringLiteral("routing_active")).toBool(false);
  const QString routeMode =
      speakers.value(QStringLiteral("route_mode")).toString();
  if (routing) {
    return WithNotes(
        MakeDevice(ReadinessState::Processing,
                   routeMode == QStringLiteral("pipeline")
                       ? QStringLiteral("Processed speaker routing is active.")
                       : QStringLiteral("Speaker pass-through routing is "
                                        "active.")),
        notes);
  }
  return WithNotes(
      MakeDevice(ReadinessState::Ready,
                 QStringLiteral("StudioCast Speakers are available.")),
      notes);
}

} // namespace

DaemonStatusSnapshot DaemonStatusSnapshot::Unreachable(const QString &error) {
  DaemonStatusSnapshot out;
  out.reachable = false;
  out.transportError = error;
  out.camera = MakeDevice(ReadinessState::DaemonUnavailable,
                          QStringLiteral("Camera status unavailable."), error);
  out.microphone =
      MakeDevice(ReadinessState::DaemonUnavailable,
                 QStringLiteral("Microphone status unavailable."), error);
  out.speakers =
      MakeDevice(ReadinessState::DaemonUnavailable,
                 QStringLiteral("Speakers status unavailable."), error);
  out.maxine = ParseEngine({}, QStringLiteral("maxine"),
                           QStringLiteral("Maxine"));
  out.openCuda = ParseEngine({}, QStringLiteral("open_cuda"),
                             QStringLiteral("Open Video"));
  out.openAudio = ParseEngine({}, QStringLiteral("open_audio"),
                              QStringLiteral("Open Audio"));
  return out;
}

DaemonStatusSnapshot DaemonStatusSnapshot::FromJson(const QString &json) {
  DaemonStatusSnapshot out;
  out.reachable = true;
  out.rawJson = json;

  QJsonParseError parseError;
  const QJsonDocument doc =
      QJsonDocument::fromJson(json.toUtf8(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
    out.parseError = QStringLiteral("JSON parse error: %1")
                         .arg(parseError.errorString());
    out.camera =
        MakeDevice(ReadinessState::RecoverableError,
                   QStringLiteral("Camera status could not be parsed."),
                   out.parseError);
    out.microphone =
        MakeDevice(ReadinessState::RecoverableError,
                   QStringLiteral("Microphone status could not be parsed."),
                   out.parseError);
    out.speakers =
        MakeDevice(ReadinessState::RecoverableError,
                   QStringLiteral("Speakers status could not be parsed."),
                   out.parseError);
    return out;
  }

  const QJsonObject root = doc.object();
  out.parsed = true;
  out.version = root.value(QStringLiteral("version")).toString();
  out.gitSha = root.value(QStringLiteral("git_sha")).toString();
  out.socketPath = root.value(QStringLiteral("socket")).toString();
  out.serviceRunning =
      root.value(QStringLiteral("service_running")).toBool(true);

  const QJsonObject video = ObjectValue(root, QStringLiteral("video"));
  const QJsonObject audio = ObjectValue(root, QStringLiteral("audio"));
  out.camera = ParseCamera(video);
  out.microphone = ParseMicrophone(audio);
  out.speakers = ParseSpeakers(audio);
  out.videoEffectsEnginePreference =
      ObjectValue(video, QStringLiteral("video_effects"))
          .value(QStringLiteral("engine"))
          .toString();
  out.audioEffectsEnginePreference =
      ObjectValue(audio, QStringLiteral("audio_effects"))
          .value(QStringLiteral("engine"))
          .toString();

  out.maxine = ParseEngine(EngineObject(root, QStringLiteral("maxine")),
                           QStringLiteral("maxine"), QStringLiteral("Maxine"));
  out.openCuda =
      ParseEngine(EngineObject(root, QStringLiteral("open_cuda")),
                  QStringLiteral("open_cuda"), QStringLiteral("Open Video"));
  out.openAudio =
      ParseEngine(EngineObject(root, QStringLiteral("open_audio")),
                  QStringLiteral("open_audio"), QStringLiteral("Open Audio"));

  return out;
}

QString DaemonStatusSnapshot::ServiceSummary() const {
  if (!reachable)
    return QStringLiteral("Service unavailable");
  if (!parsed)
    return QStringLiteral("Service status unreadable");
  if (!serviceRunning)
    return QStringLiteral("Service not ready");
  return QStringLiteral("Service connected");
}

QString DaemonStatusSnapshot::ServiceDetail() const {
  if (!reachable)
    return transportError.isEmpty()
               ? QStringLiteral("StudioCast background service is not "
                                "responding.")
               : transportError;
  if (!parsed)
    return parseError;

  QStringList parts;
  if (!version.isEmpty())
    parts << QStringLiteral("studiocastd %1").arg(version);
  if (!socketPath.isEmpty())
    parts << socketPath;
  return parts.isEmpty() ? QStringLiteral("Daemon status is current.")
                         : parts.join(QStringLiteral(" - "));
}

QString ReadinessLabel(ReadinessState state) {
  switch (state) {
  case ReadinessState::Ready:
    return QStringLiteral("Ready");
  case ReadinessState::NeedsSetup:
    return QStringLiteral("Needs setup");
  case ReadinessState::DaemonUnavailable:
    return QStringLiteral("Daemon unavailable");
  case ReadinessState::MissingVirtualDevice:
    return QStringLiteral("Missing virtual device");
  case ReadinessState::NoPhysicalDevice:
    return QStringLiteral("No physical device");
  case ReadinessState::Idle:
    return QStringLiteral("Idle");
  case ReadinessState::Processing:
    return QStringLiteral("Processing");
  case ReadinessState::RecoverableError:
    return QStringLiteral("Needs attention");
  case ReadinessState::FatalError:
    return QStringLiteral("Blocked");
  case ReadinessState::MissingModel:
    return QStringLiteral("Missing model");
  case ReadinessState::Unknown:
  default:
    return QStringLiteral("Unknown");
  }
}

} // namespace studiocast::gui
