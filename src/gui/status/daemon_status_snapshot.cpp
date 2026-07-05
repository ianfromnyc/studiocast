#include "daemon_status_snapshot.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

#include <algorithm>
#include <cstddef>
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

OpenAudioRuntimeSnapshot ParseOpenAudioRuntime(const QJsonObject &obj) {
  const QJsonObject runtime =
      ObjectValue(obj, QStringLiteral("open_audio_runtime"));
  OpenAudioRuntimeSnapshot out;
  out.present = !runtime.isEmpty();
  if (!out.present)
    return out;

  out.active = runtime.value(QStringLiteral("active")).toBool(false);
  out.usingCpuFallback =
      runtime.value(QStringLiteral("using_cpu_fallback")).toBool(false);
  out.disabled = runtime.value(QStringLiteral("disabled")).toBool(false);
  out.activeProvider =
      runtime.value(QStringLiteral("active_provider")).toString().trimmed();
  out.selectedModelId =
      runtime.value(QStringLiteral("selected_model_id")).toString().trimmed();
  out.selectedModelPath =
      runtime.value(QStringLiteral("selected_model_path")).toString().trimmed();
  out.lastRuntimeWarning =
      runtime.value(QStringLiteral("last_runtime_warning")).toString().trimmed();
  return out;
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

QString LowerTrimmed(const QString &value) { return value.trimmed().toLower(); }

QString PipelineState(const QJsonObject &obj) {
  return LowerTrimmed(obj.value(QStringLiteral("state")).toString());
}

bool StateIs(const QString &state, const QString &want) {
  return state == want;
}

bool LooksLikeCameraInputError(const QString &error) {
  const QString trimmed = error.trimmed();
  return trimmed.contains(QStringLiteral("Failed to open capture device")) ||
         trimmed.contains(QStringLiteral("No readable camera device found")) ||
         trimmed.contains(QStringLiteral("Failed to auto-select a usable camera"));
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

int StdSizeToInt(std::size_t size) {
  constexpr std::size_t kMax =
      static_cast<std::size_t>(std::numeric_limits<int>::max());
  if (size > kMax)
    return std::numeric_limits<int>::max();
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

QString RawObjectText(const QJsonObject &obj) {
  if (obj.isEmpty())
    return {};
  return QString::fromUtf8(
             QJsonDocument(obj).toJson(QJsonDocument::Indented))
      .trimmed();
}

QStringList CombinedStringListValue(const QJsonObject &obj,
                                    std::initializer_list<QString> keys) {
  QStringList out;
  for (const QString &key : keys) {
    for (const QString &item : StringListValue(obj, key)) {
      if (!out.contains(item))
        out.push_back(item);
    }
  }
  return out;
}

bool EngineReportsModelId(const EngineStatus &engine, const QString &modelId) {
  const QString want = modelId.trimmed();
  if (want.isEmpty())
    return false;
  for (const EngineModelEntry &entry : engine.installedModels) {
    if (entry.id == want)
      return true;
  }
  return false;
}

bool EngineExplicitlyMissingModelId(const EngineStatus &engine,
                                    const QString &modelId) {
  const QString want = modelId.trimmed();
  if (want.isEmpty())
    return false;
  for (const EngineModelEntry &entry : engine.missingModelEntries) {
    if (entry.id == want)
      return true;
  }
  return false;
}

QString JoinModelDetails(const QStringList &parts) {
  QStringList out;
  for (const QString &part : parts) {
    const QString trimmed = part.trimmed();
    if (!trimmed.isEmpty())
      out.push_back(trimmed);
  }
  return out.join(QStringLiteral("; "));
}

std::vector<EngineModelEntry> ParseInstalledModelEntries(
    const QJsonObject &obj) {
  std::vector<EngineModelEntry> out;

  const QJsonArray models = obj.value(QStringLiteral("models")).toArray();
  for (const QJsonValue &value : models) {
    if (!value.isObject())
      continue;
    const QJsonObject model = value.toObject();
    const QString id = model.value(QStringLiteral("id")).toString().trimmed();
    if (id.isEmpty())
      continue;

    EngineModelEntry entry;
    entry.id = id;
    entry.displayName =
        FirstNonEmpty({model.value(QStringLiteral("display_name")).toString(),
                       id});
    entry.category = model.value(QStringLiteral("task")).toString().trimmed();

    QStringList details;
    if (!entry.category.isEmpty())
      details << QStringLiteral("task: %1").arg(entry.category);

    const QStringList effects = StringListValue(model, QStringLiteral("effects"));
    if (!effects.isEmpty()) {
      entry.category = effects.join(QStringLiteral(", "));
      details << QStringLiteral("effects: %1")
                     .arg(effects.join(QStringLiteral(", ")));
    }

    const int width = model.value(QStringLiteral("width")).toInt(0);
    const int height = model.value(QStringLiteral("height")).toInt(0);
    if (width > 0 && height > 0)
      details << QStringLiteral("size: %1x%2").arg(width).arg(height);

    const int sampleRate = model.value(QStringLiteral("sample_rate")).toInt(0);
    if (sampleRate > 0)
      details << QStringLiteral("sample rate: %1 Hz").arg(sampleRate);

    const int channels = model.value(QStringLiteral("channels")).toInt(0);
    if (channels > 0)
      details << QStringLiteral("channels: %1").arg(channels);

    entry.details = JoinModelDetails(details);
    out.push_back(entry);
  }

  const QJsonArray installed =
      obj.value(QStringLiteral("installed_models")).toArray();
  for (const QJsonValue &value : installed) {
    const QString id = value.toString().trimmed();
    if (id.isEmpty())
      continue;

    const auto it = std::find_if(
        out.begin(), out.end(),
        [&id](const EngineModelEntry &entry) { return entry.id == id; });
    if (it != out.end())
      continue;

    EngineModelEntry entry;
    entry.id = id;
    entry.displayName = id;
    out.push_back(entry);
  }

  return out;
}

std::vector<EngineModelEntry> ParseMissingModelEntries(
    const QJsonObject &obj) {
  std::vector<EngineModelEntry> out;
  const QJsonObject missing = ObjectValue(obj, QStringLiteral("missing_models"));
  for (auto it = missing.constBegin(); it != missing.constEnd(); ++it) {
    const QString id = it.key().trimmed();
    if (id.isEmpty())
      continue;
    EngineModelEntry entry;
    entry.id = id;
    entry.displayName = id;
    entry.details = it.value().toString().trimmed();
    entry.installed = false;
    out.push_back(entry);
  }
  return out;
}

void AddMaxineFeatureEntriesFromMap(const QString &component,
                                    const QJsonObject &featureStatus,
                                    EngineStatus *engine) {
  if (!engine)
    return;
  for (auto it = featureStatus.constBegin(); it != featureStatus.constEnd();
       ++it) {
    const QString id = it.key().trimmed();
    const QJsonObject feature = it.value().toObject();
    if (id.isEmpty() || feature.isEmpty())
      continue;

    EngineModelEntry entry;
    entry.id = QStringLiteral("%1.%2").arg(component, id);
    entry.displayName = id;
    entry.category = component.toUpper();
    entry.installed = feature.value(QStringLiteral("installed")).toBool(false);
    entry.details = feature.value(QStringLiteral("details")).toString();

    auto &target =
        entry.installed ? engine->installedModels : engine->missingModelEntries;
    const auto existing = std::find_if(
        target.begin(), target.end(),
        [&entry](const EngineModelEntry &candidate) {
          return candidate.id == entry.id;
        });
    if (existing == target.end())
      target.push_back(entry);
  }
}

void AddMaxineFeatureEntries(const QJsonObject &obj, EngineStatus *engine) {
  const QJsonObject components = ObjectValue(obj, QStringLiteral("components"));
  for (auto it = components.constBegin(); it != components.constEnd(); ++it) {
    AddMaxineFeatureEntriesFromMap(
        it.key(), ObjectValue(it.value().toObject(),
                              QStringLiteral("feature_status")),
        engine);
  }

  for (const QString &component :
       {QStringLiteral("vfx"), QStringLiteral("ar"), QStringLiteral("afx")}) {
    const QJsonObject componentObj = ObjectValue(obj, component);
    AddMaxineFeatureEntriesFromMap(
        component, ObjectValue(componentObj, QStringLiteral("feature_status")),
        engine);
  }
}

ConfiguredModelEntry MakeConfiguredModel(const EngineStatus &engine,
                                         const QString &owner,
                                         const QString &modelId,
                                         const QString &modelPath = {}) {
  ConfiguredModelEntry entry;
  entry.owner = owner;
  entry.modelId = modelId.trimmed();
  entry.modelPath = modelPath.trimmed();
  entry.modelIdReported = EngineReportsModelId(engine, entry.modelId);
  entry.modelIdExplicitlyMissing =
      EngineExplicitlyMissingModelId(engine, entry.modelId);
  return entry;
}

void AddConfiguredModelIfNeeded(EngineStatus *engine, const QString &owner,
                                const QString &modelId,
                                const QString &modelPath = {}) {
  if (!engine)
    return;
  ConfiguredModelEntry entry =
      MakeConfiguredModel(*engine, owner, modelId, modelPath);
  if (entry.modelId.isEmpty() && entry.modelPath.isEmpty())
    return;
  if (!entry.modelId.isEmpty() && !entry.modelIdReported)
    ++engine->configuredMissingModelCount;
  engine->configuredModels.push_back(entry);
}

void AddConfiguredVideoModels(const QJsonObject &video,
                              EngineStatus *openCuda) {
  if (!openCuda)
    return;
  const QJsonObject fx = ObjectValue(video, QStringLiteral("video_effects"));
  if (fx.isEmpty())
    return;

  AddConfiguredModelIfNeeded(
      openCuda, QStringLiteral("Camera virtual background"),
      ObjectValue(fx, QStringLiteral("virtual_background"))
          .value(QStringLiteral("model_id"))
          .toString());
  AddConfiguredModelIfNeeded(
      openCuda, QStringLiteral("Camera auto frame"),
      ObjectValue(fx, QStringLiteral("auto_frame"))
          .value(QStringLiteral("model_id"))
          .toString());
  AddConfiguredModelIfNeeded(
      openCuda, QStringLiteral("Camera eye contact"),
      ObjectValue(fx, QStringLiteral("eye_contact"))
          .value(QStringLiteral("model_id"))
          .toString());
  AddConfiguredModelIfNeeded(
      openCuda, QStringLiteral("Camera video noise removal"),
      ObjectValue(fx, QStringLiteral("video_noise_removal"))
          .value(QStringLiteral("model_id"))
          .toString());
}

void AddConfiguredAudioModels(const QJsonObject &audio,
                              EngineStatus *openAudio) {
  if (!openAudio)
    return;
  const QJsonObject fx = ObjectValue(audio, QStringLiteral("audio_effects"));
  if (fx.isEmpty())
    return;

  const QJsonObject mic = ObjectValue(fx, QStringLiteral("microphone"));
  AddConfiguredModelIfNeeded(
      openAudio, QStringLiteral("Microphone cleanup"),
      mic.value(QStringLiteral("model_id")).toString(),
      mic.value(QStringLiteral("model_path")).toString());

  const QJsonObject speaker = ObjectValue(fx, QStringLiteral("speaker"));
  AddConfiguredModelIfNeeded(
      openAudio, QStringLiteral("Speaker cleanup"),
      speaker.value(QStringLiteral("model_id")).toString(),
      speaker.value(QStringLiteral("model_path")).toString());
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

  out.rawJson = RawObjectText(obj);
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
  out.installHints = CombinedStringListValue(
      obj, {QStringLiteral("install_hints"), QStringLiteral("hints")});
  out.availableEffects = CombinedStringListValue(
      obj, {QStringLiteral("available_effects"),
            QStringLiteral("available_audio_effects")});
  out.defaultModelId = obj.value(QStringLiteral("default_model_id")).toString();
  out.installedModels = ParseInstalledModelEntries(obj);
  out.missingModelEntries = ParseMissingModelEntries(obj);
  if (id == QStringLiteral("maxine"))
    AddMaxineFeatureEntries(obj, &out);
  out.installedModelCount = StdSizeToInt(out.installedModels.size());
  out.knownModelCount =
      SizeToInt(obj.value(QStringLiteral("models")).toArray().size());
  out.missingModelCount = out.missingModels.isEmpty()
                              ? CountObjectEntries(
                                    obj, QStringLiteral("missing_models"))
                              : SizeToInt(out.missingModels.size());
  if (out.missingModelCount == 0 && id == QStringLiteral("maxine"))
    out.missingModelCount = StdSizeToInt(out.missingModelEntries.size());

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
  const QString inputDeviceError =
      video.value(QStringLiteral("input_device_error")).toString();
  const QString virtualDeviceError =
      video.value(QStringLiteral("virtual_device_error")).toString();
  const QString consumerError =
      video.value(QStringLiteral("consumer_error")).toString();
  const bool virtualPresent =
      video.value(QStringLiteral("virtual_device_present")).toBool(false);
  const bool virtualAvailable =
      video.value(QStringLiteral("virtual_device_available")).toBool(false);
  if (!virtualPresent || !virtualAvailable || !virtualDeviceError.isEmpty()) {
    return WithNotes(
        MakeDevice(ReadinessState::MissingVirtualDevice,
                   !virtualDeviceError.isEmpty()
                       ? QStringLiteral("StudioCast Camera is not available.")
                       : QStringLiteral("StudioCast Camera needs setup."),
                   virtualDeviceError.isEmpty()
                       ? QStringLiteral("No writable virtual camera is "
                                        "available.")
                       : virtualDeviceError),
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

  // Newer payloads may report a structured input_device_error. Older daemon
  // builds only expose physical capture failures via known pipeline start
  // messages, so keep that fallback narrow.
  if (!inputDeviceError.trimmed().isEmpty() ||
      LooksLikeCameraInputError(lastError)) {
    return WithNotes(
        MakeDevice(ReadinessState::NoPhysicalDevice,
                   QStringLiteral("Choose a physical camera input."),
                   inputDeviceError.trimmed().isEmpty() ? lastError
                                                        : inputDeviceError),
        notes, disabledReasons);
  }

  if (!lastError.isEmpty()) {
    return WithNotes(
        MakeDevice(ReadinessState::RecoverableError,
                   QStringLiteral("Camera reported an error."), lastError),
        notes, disabledReasons);
  }

  const bool enabled = video.value(QStringLiteral("enabled")).toBool(false);
  const bool running = pipeline.value(QStringLiteral("running")).toBool(false);
  const bool starting = pipeline.value(QStringLiteral("starting")).toBool(false);
  const int consumers = video.value(QStringLiteral("consumer_count")).toInt(0);
  const QString pipelineState = PipelineState(pipeline);
  if (enabled &&
      (running || starting || StateIs(pipelineState, QStringLiteral("running")) ||
       StateIs(pipelineState, QStringLiteral("starting")))) {
    return WithNotes(
        MakeDevice(ReadinessState::Processing,
                   starting || StateIs(pipelineState, QStringLiteral("starting"))
                       ? QStringLiteral("Camera processing is starting.")
                       : QStringLiteral("Camera processing is active.")),
        notes, disabledReasons);
  }
  if (enabled) {
    return WithNotes(
        MakeDevice(ReadinessState::Idle,
                   StateIs(pipelineState, QStringLiteral("backing_off"))
                       ? QStringLiteral("Camera is waiting before retrying.")
                       : consumers > 0
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
  const QString consumerError =
      audio.value(QStringLiteral("mic_consumer_error")).toString().trimmed();
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

  const bool enabled = audio.value(QStringLiteral("enabled")).toBool(false);
  const bool running = pipeline.value(QStringLiteral("running")).toBool(false);
  const bool starting = pipeline.value(QStringLiteral("starting")).toBool(false);
  const QString pipelineState = PipelineState(pipeline);
  if (running || starting || StateIs(pipelineState, QStringLiteral("running")) ||
      StateIs(pipelineState, QStringLiteral("starting"))) {
    return WithNotes(
        MakeDevice(ReadinessState::Processing,
                   starting || StateIs(pipelineState, QStringLiteral("starting"))
                       ? QStringLiteral("Microphone processing is starting.")
                       : QStringLiteral("Microphone processing is active.")),
        notes);
  }

  if (!consumerError.isEmpty() && enabled) {
    return WithNotes(
        MakeDevice(ReadinessState::RecoverableError,
                   QStringLiteral("Microphone consumer detection reported an "
                                  "error."),
                   consumerError),
        notes);
  }

  if (enabled && StateIs(pipelineState, QStringLiteral("idle_no_consumer"))) {
    return WithNotes(
        MakeDevice(ReadinessState::Idle,
                   QStringLiteral("Ready. Waiting for an app to use "
                                  "StudioCast Microphone."),
                   pipeline.value(QStringLiteral("idle_reason")).toString()),
        notes);
  }

  if (!enabled || StateIs(pipelineState, QStringLiteral("disabled"))) {
    return WithNotes(
        MakeDevice(ReadinessState::Ready,
                   QStringLiteral("StudioCast Microphone is present; processing "
                                  "is off.")),
        notes);
  }

  // Older daemon payloads did not expose enough microphone consumer/pipeline
  // state to prove idle-no-consumer. Keep this generic instead of claiming
  // no-consumer readiness from absence of activity.
  return WithNotes(
      MakeDevice(ReadinessState::Ready,
                 QStringLiteral("Microphone status is reported; processing is "
                                "not active."),
                 pipeline.value(QStringLiteral("idle_reason")).toString()),
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

  const bool enabled = speakers.value(QStringLiteral("enabled")).toBool(false);
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
      LowerTrimmed(speakers.value(QStringLiteral("route_mode")).toString());
  const bool pipelineRunning =
      speakers.value(QStringLiteral("pipeline_running")).toBool(false);
  const bool pipelineStarting =
      speakers.value(QStringLiteral("pipeline_starting")).toBool(false);
  const QString pipelineState =
      LowerTrimmed(speakers.value(QStringLiteral("pipeline_state")).toString());
  const QString pipelineIdleReason =
      speakers.value(QStringLiteral("pipeline_idle_reason")).toString();

  if (routing || pipelineRunning ||
      StateIs(pipelineState, QStringLiteral("running"))) {
    return WithNotes(
        MakeDevice(ReadinessState::Processing,
                   routeMode == QStringLiteral("pipeline") || pipelineRunning
                       ? QStringLiteral("Processed speaker routing is active.")
                       : QStringLiteral("Speaker pass-through routing is "
                                        "active.")),
        notes);
  }
  if (pipelineStarting || StateIs(pipelineState, QStringLiteral("starting"))) {
    return WithNotes(
        MakeDevice(ReadinessState::Processing,
                   QStringLiteral("Speaker routing is starting.")),
        notes);
  }

  if (!consumerError.isEmpty() && enabled &&
      routeMode == QStringLiteral("pipeline")) {
    return WithNotes(
        MakeDevice(ReadinessState::RecoverableError,
                   QStringLiteral("Speaker consumer detection reported an "
                                  "error."),
                   consumerError),
        notes);
  }

  if (enabled && routeMode == QStringLiteral("pipeline") &&
      StateIs(pipelineState, QStringLiteral("idle_no_consumer"))) {
    return WithNotes(
        MakeDevice(ReadinessState::Idle,
                   QStringLiteral("Ready. Waiting for an app to use "
                                  "StudioCast Speakers."),
                   pipelineIdleReason),
        notes);
  }

  if (!enabled || routeMode == QStringLiteral("off") ||
      StateIs(pipelineState, QStringLiteral("disabled"))) {
    return WithNotes(
        MakeDevice(ReadinessState::Ready,
                   QStringLiteral("StudioCast Speakers are present; routing is "
                                  "off.")),
        notes);
  }

  if (routeMode == QStringLiteral("loopback")) {
    return WithNotes(
        MakeDevice(ReadinessState::Ready,
                   QStringLiteral("Speaker pass-through routing is configured "
                                  "but not active.")),
        notes);
  }

  // Older daemon payloads reported present/routing booleans without enough
  // consumer pipeline state to distinguish idle-no-consumer from stopped.
  return WithNotes(
      MakeDevice(ReadinessState::Ready,
                 QStringLiteral("Speaker routing status is reported; routing is "
                                "not active.")),
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
  out.videoEffectsActiveBackends =
      ObjectValue(video, QStringLiteral("pipeline"))
          .value(QStringLiteral("effects_backends"))
          .toString();
  out.microphoneActiveBackend =
      ObjectValue(audio, QStringLiteral("pipeline"))
          .value(QStringLiteral("backend_active"))
          .toString();
  out.microphoneOpenAudioRuntime =
      ParseOpenAudioRuntime(ObjectValue(audio, QStringLiteral("pipeline")));
  const QJsonObject speakers = ObjectValue(audio, QStringLiteral("speakers"));
  out.speakersRouteMode = speakers.value(QStringLiteral("route_mode")).toString();
  out.speakersActiveBackend =
      FirstNonEmpty({speakers.value(QStringLiteral("backend_active")).toString(),
                     ObjectValue(speakers, QStringLiteral("pipeline"))
                         .value(QStringLiteral("backend_active"))
                         .toString()});
  out.speakersOpenAudioRuntime = ParseOpenAudioRuntime(speakers);

  out.maxine = ParseEngine(EngineObject(root, QStringLiteral("maxine")),
                           QStringLiteral("maxine"), QStringLiteral("Maxine"));
  out.openCuda =
      ParseEngine(EngineObject(root, QStringLiteral("open_cuda")),
                  QStringLiteral("open_cuda"), QStringLiteral("Open Video"));
  out.openAudio =
      ParseEngine(EngineObject(root, QStringLiteral("open_audio")),
                  QStringLiteral("open_audio"), QStringLiteral("Open Audio"));
  AddConfiguredVideoModels(video, &out.openCuda);
  AddConfiguredAudioModels(audio, &out.openAudio);

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

QString DaemonStatusSnapshot::UserServiceSummary() const {
  if (!reachable)
    return QStringLiteral("Background service unavailable");
  if (!parsed)
    return QStringLiteral("Status needs attention");
  if (!serviceRunning)
    return QStringLiteral("Background service not ready");
  return QStringLiteral("Background service connected");
}

QString DaemonStatusSnapshot::UserServiceDetail() const {
  if (!reachable)
    return QStringLiteral(
        "StudioCast background service is unavailable. Open Support for "
        "technical details.");
  if (!parsed)
    return QStringLiteral(
        "StudioCast received an unreadable status update. Open Support for "
        "technical details.");
  if (!serviceRunning)
    return QStringLiteral(
        "StudioCast background service is starting or not ready yet.");
  return QStringLiteral("StudioCast is connected to the background service.");
}

QString DaemonStatusSnapshot::RawDiagnosticsText() const {
  if (!rawJson.trimmed().isEmpty())
    return rawJson;
  if (!transportError.trimmed().isEmpty())
    return QStringLiteral("Daemon unavailable: %1").arg(transportError);
  if (!parseError.trimmed().isEmpty())
    return QStringLiteral("Daemon status parse error: %1").arg(parseError);
  return QStringLiteral("Daemon status has not been read.");
}

QString ReadinessLabel(ReadinessState state) {
  switch (state) {
  case ReadinessState::Ready:
    return QStringLiteral("Ready");
  case ReadinessState::NeedsSetup:
    return QStringLiteral("Needs setup");
  case ReadinessState::DaemonUnavailable:
    return QStringLiteral("Service unavailable");
  case ReadinessState::MissingVirtualDevice:
    return QStringLiteral("Device missing");
  case ReadinessState::NoPhysicalDevice:
    return QStringLiteral("Needs selection");
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
