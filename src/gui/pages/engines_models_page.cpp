#include "engines_models_page.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>

#include "gui/status/daemon_status_snapshot.h"
#include "gui/text_edit_utils.h"

#ifndef STUDIOCAST_SOURCE_DIR
#define STUDIOCAST_SOURCE_DIR ""
#endif

namespace studiocast::gui {
namespace {

constexpr const char *kOpenAudioDefaultModelIds[] = {
    "fastenhancer_s_vd_v1",
    "fastenhancer_m_vd_v1",
};

constexpr const char *kOpenVideoDefaultModelIds[] = {
    "modnet-webnn-256-fp32",
    "yunet_opencv_zoo_2023mar_fp32",
    "dlib_68_ibug_300w",
    "gaze_correction_cam_flx_v0_1_1",
    "fastdvdnet_sigma15",
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

QPlainTextEdit *DetailsBox(QWidget *parent, int minHeight = 94) {
  auto *text = new QPlainTextEdit(parent);
  text->setReadOnly(true);
  text->setLineWrapMode(QPlainTextEdit::WidgetWidth);
  text->setMinimumHeight(minHeight);
  return text;
}

void SetDynamicProperty(QWidget *widget, const char *name,
                        const QString &value) {
  if (!widget)
    return;
  widget->setProperty(name, value);
  widget->style()->unpolish(widget);
  widget->style()->polish(widget);
}

QString FriendlyBackendLabel(const QString &id) {
  const QString v = id.trimmed().toLower();
  if (v.isEmpty())
    return QStringLiteral("Unknown");
  if (v == QStringLiteral("auto"))
    return QStringLiteral("Auto");
  if (v == QStringLiteral("maxine"))
    return QStringLiteral("Maxine");
  if (v == QStringLiteral("open_cuda") || v == QStringLiteral("open_video"))
    return QStringLiteral("Open Video");
  if (v == QStringLiteral("open_source") || v == QStringLiteral("open_audio"))
    return QStringLiteral("Open Audio");
  if (v == QStringLiteral("off"))
    return QStringLiteral("Off");
  if (v == QStringLiteral("passthrough"))
    return QStringLiteral("Pass-through");
  if (v == QStringLiteral("loopback"))
    return QStringLiteral("Loopback / pass-through");
  if (v == QStringLiteral("pipeline"))
    return QStringLiteral("Processed pipeline");
  return id;
}

QString ActiveBackendSummary(const QString &raw,
                             const QString &emptyLabel =
                                 QStringLiteral("Pass-through / idle")) {
  const QString trimmed = raw.trimmed();
  if (trimmed.isEmpty())
    return emptyLabel;

  QStringList labels;
  const QStringList parts = trimmed.split(',', Qt::SkipEmptyParts);
  for (const QString &part : parts) {
    const QString p = part.trimmed();
    const qsizetype colon = p.indexOf(QChar(':'));
    const QString backend = colon >= 0 ? p.mid(colon + 1).trimmed() : p;
    const QString label = FriendlyBackendLabel(backend);
    if (!label.isEmpty() && !labels.contains(label))
      labels.push_back(label);
  }

  if (labels.isEmpty())
    return FriendlyBackendLabel(trimmed);
  if (labels.size() == 1)
    return labels.first();
  return QStringLiteral("Mixed: %1").arg(labels.join(QStringLiteral(", ")));
}

QString EngineProperty(const EngineStatus &engine) {
  if (!engine.present)
    return QStringLiteral("warning");
  if (!(engine.ok || engine.supported))
    return QStringLiteral("error");
  if (engine.missingModelCount > 0 ||
      engine.configuredMissingModelCount > 0 ||
      !engine.blockedEffects.isEmpty()) {
    return QStringLiteral("warning");
  }
  return QStringLiteral("good");
}

QString EngineStateLabel(const EngineStatus &engine,
                         bool selectedByPreference) {
  if (!engine.present)
    return QStringLiteral("Unknown");
  if (!(engine.ok || engine.supported)) {
    return selectedByPreference ? QStringLiteral("Selected unavailable")
                                : QStringLiteral("Unavailable");
  }
  if (engine.missingModelCount > 0) {
    return QStringLiteral("Missing models");
  }
  if (engine.configuredMissingModelCount > 0)
    return QStringLiteral("Model selection review");
  if (!engine.blockedEffects.isEmpty())
    return QStringLiteral("Partially blocked");
  return QStringLiteral("Available");
}

QString EngineSummary(const EngineStatus &engine,
                      bool selectedByPreference) {
  if (!engine.present)
    return QStringLiteral("The daemon did not report %1 diagnostics.")
        .arg(engine.label);

  if (!(engine.ok || engine.supported)) {
    QString summary = engine.summary.trimmed();
    if (summary.isEmpty())
      summary = QStringLiteral("%1 is unavailable.").arg(engine.label);
    if (selectedByPreference)
      summary += QStringLiteral(" This backend is currently selected.");
    return summary;
  }

  if (engine.missingModelCount > 0) {
    if (engine.installedModelCount > 0) {
      return QStringLiteral("%1 has usable model packs, but %2 model pack%3 "
                            "are missing or invalid.")
          .arg(engine.label)
          .arg(engine.missingModelCount)
          .arg(engine.missingModelCount == 1 ? QString()
                                             : QStringLiteral("s"));
    }
    return QStringLiteral("%1 model packs are missing or invalid.")
        .arg(engine.label);
  }

  if (engine.configuredMissingModelCount > 0) {
    return QStringLiteral("Some configured %1 model selections are not "
                          "reported installed by daemon diagnostics.")
        .arg(engine.label);
  }

  if (engine.id == QStringLiteral("open_cuda") &&
      engine.installedModelCount == 0) {
    return QStringLiteral("Open Video runtime is available. Model-backed "
                          "camera effects still need model packs.");
  }

  if (engine.id == QStringLiteral("open_audio") &&
      engine.installedModelCount == 0) {
    return QStringLiteral("Open Audio runtime is available, but no usable "
                          "audio model packs are installed.");
  }

  if (!engine.blockedEffects.isEmpty()) {
    return QStringLiteral("%1 is available, but some effects are blocked.")
        .arg(engine.label);
  }

  return engine.summary.trimmed().isEmpty()
             ? QStringLiteral("%1 is available.").arg(engine.label)
             : engine.summary.trimmed();
}

QString ModelSummary(const EngineStatus &engine) {
  if (!engine.present)
    return QStringLiteral("No model diagnostics reported.");

  QStringList parts;
  if (engine.id == QStringLiteral("maxine")) {
    if (engine.installedModelCount == 0 && engine.missingModelCount == 0) {
      parts << QStringLiteral("No Maxine feature status reported.");
    } else {
      parts << QStringLiteral("%1 feature file%2 installed")
                   .arg(engine.installedModelCount)
                   .arg(engine.installedModelCount == 1 ? QString()
                                                        : QStringLiteral("s"));
    }
  } else if (engine.installedModelCount == 0) {
    parts << QStringLiteral("No installed model packs reported");
  } else {
    parts << QStringLiteral("%1 installed model pack%2")
                 .arg(engine.installedModelCount)
                 .arg(engine.installedModelCount == 1 ? QString()
                                                      : QStringLiteral("s"));
  }

  if (engine.missingModelCount > 0) {
    parts << QStringLiteral("%1 missing or invalid")
                 .arg(engine.missingModelCount);
  }
  if (engine.configuredMissingModelCount > 0) {
    parts << QStringLiteral("%1 configured selection%2 not reported installed")
                 .arg(engine.configuredMissingModelCount)
                 .arg(engine.configuredMissingModelCount == 1
                          ? QString()
                          : QStringLiteral("s"));
  }
  return parts.join(QStringLiteral("; ")) + QChar('.');
}

QString ModelEntryLine(const EngineModelEntry &entry) {
  QString line = entry.displayName.trimmed().isEmpty() ? entry.id.trimmed()
                                                       : entry.displayName.trimmed();
  QStringList suffix;
  if (!entry.category.trimmed().isEmpty())
    suffix << entry.category.trimmed();
  if (!entry.id.trimmed().isEmpty())
    suffix << QStringLiteral("id: %1").arg(entry.id.trimmed());
  if (!entry.details.trimmed().isEmpty())
    suffix << entry.details.trimmed();
  if (!suffix.isEmpty())
    line += QStringLiteral(" (%1)").arg(suffix.join(QStringLiteral("; ")));
  return line;
}

QString ConfiguredModelLine(const EngineStatus &engine,
                            const ConfiguredModelEntry &entry) {
  QStringList parts;
  if (!entry.modelId.isEmpty()) {
    QString state;
    if (entry.modelIdReported) {
      state = QStringLiteral("reported installed");
    } else if (entry.modelIdExplicitlyMissing) {
      state = QStringLiteral("reported missing or invalid");
    } else if (engine.present) {
      state = QStringLiteral("not reported by daemon diagnostics");
    } else {
      state = QStringLiteral("engine diagnostics unavailable");
    }
    parts << QStringLiteral("model_id=%1 (%2)").arg(entry.modelId, state);
  }
  if (!entry.modelPath.isEmpty())
    parts << QStringLiteral("model_path=%1").arg(entry.modelPath);
  return QStringLiteral("%1: %2").arg(entry.owner, parts.join(QStringLiteral("; ")));
}

QString EngineDetailsText(const EngineStatus &engine) {
  QStringList lines;
  lines << QStringLiteral("Summary: %1")
               .arg(engine.summary.trimmed().isEmpty()
                        ? QStringLiteral("No summary reported.")
                        : engine.summary.trimmed());
  if (!engine.blockedReason.trimmed().isEmpty())
    lines << QStringLiteral("Blocked reason: %1").arg(engine.blockedReason);
  if (!engine.blockedDetails.isEmpty()) {
    lines << QStringLiteral("");
    lines << QStringLiteral("Blocked details:");
    for (const QString &detail : engine.blockedDetails)
      lines << QStringLiteral("- %1").arg(detail);
  }
  if (!engine.availableEffects.isEmpty()) {
    lines << QStringLiteral("");
    lines << QStringLiteral("Available effects:");
    for (const QString &effect : engine.availableEffects)
      lines << QStringLiteral("- %1").arg(effect);
  }
  if (!engine.installedModels.empty()) {
    lines << QStringLiteral("");
    lines << QStringLiteral("Installed model details:");
    for (const EngineModelEntry &entry : engine.installedModels)
      lines << QStringLiteral("- %1").arg(ModelEntryLine(entry));
  }
  if (!engine.missingModelEntries.empty()) {
    lines << QStringLiteral("");
    lines << QStringLiteral("Missing or invalid model details:");
    for (const EngineModelEntry &entry : engine.missingModelEntries)
      lines << QStringLiteral("- %1").arg(ModelEntryLine(entry));
  }
  if (!engine.configuredModels.empty()) {
    lines << QStringLiteral("");
    lines << QStringLiteral("Configured model selections:");
    for (const ConfiguredModelEntry &entry : engine.configuredModels)
      lines << QStringLiteral("- %1").arg(ConfiguredModelLine(engine, entry));
  }
  if (!engine.blockedEffects.isEmpty()) {
    lines << QStringLiteral("");
    lines << QStringLiteral("Blocked effects:");
    for (const QString &effect : engine.blockedEffects)
      lines << QStringLiteral("- %1").arg(effect);
  }
  if (!engine.defaultModelId.trimmed().isEmpty()) {
    lines << QStringLiteral("");
    lines << QStringLiteral("Default model ID: %1").arg(engine.defaultModelId);
  }
  return lines.join(QStringLiteral("\n")).trimmed();
}

QStringList DefaultModelIdsForEngine(const QString &engineId) {
  QStringList ids;
  if (engineId == QStringLiteral("open_audio")) {
    for (const char *id : kOpenAudioDefaultModelIds)
      ids.push_back(QString::fromLatin1(id));
  } else if (engineId == QStringLiteral("open_cuda")) {
    for (const char *id : kOpenVideoDefaultModelIds)
      ids.push_back(QString::fromLatin1(id));
  }
  return ids;
}

QStringList MissingModelIdsForInstall(const EngineStatus &engine) {
  QStringList ids;
  for (const EngineModelEntry &entry : engine.missingModelEntries) {
    const QString id = entry.id.trimmed();
    if (!id.isEmpty() && !ids.contains(id))
      ids.push_back(id);
  }
  return ids;
}

QStringList ModelInstallArgsForEngine(const EngineStatus &engine) {
  QStringList args;
  if (engine.id == QStringLiteral("open_audio")) {
    args << QStringLiteral("open-audio-models");
  } else if (engine.id == QStringLiteral("open_cuda")) {
    args << QStringLiteral("open-video-models");
  } else {
    return args;
  }

  QStringList modelIds = MissingModelIdsForInstall(engine);
  if (modelIds.isEmpty())
    modelIds = DefaultModelIdsForEngine(engine.id);

  if (engine.id == QStringLiteral("open_cuda")) {
    const bool installsEyeContact =
        std::any_of(modelIds.cbegin(), modelIds.cend(), [](const QString &id) {
          return id.startsWith(QStringLiteral("gaze_correction_cam"));
        });
    if (installsEyeContact &&
        !modelIds.contains(QStringLiteral("dlib_68_ibug_300w"))) {
      modelIds.push_back(QStringLiteral("dlib_68_ibug_300w"));
    }
  }

  for (const QString &id : modelIds)
    args << QStringLiteral("--model") << id;
  return args;
}

bool HasMissingModelBlock(const EngineStatus &engine) {
  for (const QString &effect : engine.blockedEffects) {
    if (effect.contains(QStringLiteral("missing_model_packs")))
      return true;
  }
  return false;
}

bool ModelInstallRecommended(const EngineStatus &engine) {
  if (engine.id != QStringLiteral("open_audio") &&
      engine.id != QStringLiteral("open_cuda")) {
    return false;
  }
  if (!engine.present)
    return false;
  if (engine.missingModelCount > 0 || engine.configuredMissingModelCount > 0 ||
      HasMissingModelBlock(engine)) {
    return true;
  }
  return (engine.ok || engine.supported) && engine.installedModelCount == 0;
}

QString ModelInstallButtonText(const EngineStatus &engine) {
  const QString label = engine.id == QStringLiteral("open_audio")
                            ? QStringLiteral("Open Audio")
                            : QStringLiteral("Open Video");
  if (engine.missingModelCount > 0)
    return QStringLiteral("Download missing %1 models").arg(label);
  return QStringLiteral("Download default %1 models").arg(label);
}

QString ModelInstallStatusText(const EngineStatus &engine,
                               bool installRecommended) {
  if (engine.id != QStringLiteral("open_audio") &&
      engine.id != QStringLiteral("open_cuda")) {
    return {};
  }
  if (!engine.present)
    return QStringLiteral("Model diagnostics unavailable.");
  if (!installRecommended) {
    if (!(engine.ok || engine.supported) && engine.installedModelCount == 0) {
      return QStringLiteral(
          "Resolve runtime/build issues before downloading model packs.");
    }
    return QStringLiteral("Default model packs appear installed.");
  }

  const QStringList args = ModelInstallArgsForEngine(engine);
  QStringList modelIds;
  for (int i = 0; i + 1 < args.size(); ++i) {
    if (args.at(i) == QStringLiteral("--model"))
      modelIds.push_back(args.at(i + 1));
  }
  if (!modelIds.isEmpty()) {
    return QStringLiteral("Ready to install: %1.")
        .arg(modelIds.join(QStringLiteral(", ")));
  }
  return QStringLiteral("Ready to install default model packs.");
}

QString ManifestSourceDir() {
  const QString dataHome =
      QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
  if (dataHome.isEmpty())
    return {};

  QFile file(QDir(dataHome).filePath(
      QStringLiteral("studiocast/install-manifest.json")));
  if (!file.open(QIODevice::ReadOnly))
    return {};

  const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
  if (!doc.isObject())
    return {};
  return doc.object().value(QStringLiteral("source_path")).toString().trimmed();
}

void AddInstallScriptCandidate(QStringList *candidates,
                               const QString &sourceDir) {
  if (!candidates || sourceDir.trimmed().isEmpty())
    return;
  const QString script =
      QDir(sourceDir.trimmed()).filePath(QStringLiteral("scripts/install.sh"));
  if (!candidates->contains(script))
    candidates->push_back(script);
}

QString InstallHintsText(const EngineStatus &engine) {
  if (engine.installHints.isEmpty())
    return QStringLiteral("No install hints reported by daemon diagnostics.");
  QStringList lines;
  for (const QString &hint : engine.installHints)
    lines << hint;
  return lines.join(QStringLiteral("\n"));
}

bool PreferenceSelectsMaxine(const DaemonStatusSnapshot &snapshot) {
  const QString video = snapshot.videoEffectsEnginePreference.trimmed().toLower();
  const QString audio = snapshot.audioEffectsEnginePreference.trimmed().toLower();
  return video == QStringLiteral("maxine") || audio == QStringLiteral("maxine");
}

bool PreferenceSelectsOpenVideo(const DaemonStatusSnapshot &snapshot) {
  return snapshot.videoEffectsEnginePreference.trimmed().toLower() ==
         QStringLiteral("open_cuda");
}

bool PreferenceSelectsOpenAudio(const DaemonStatusSnapshot &snapshot) {
  const QString audio = snapshot.audioEffectsEnginePreference.trimmed().toLower();
  return audio == QStringLiteral("open_source") ||
         audio == QStringLiteral("open_audio");
}

} // namespace

EnginesModelsPage::EnginesModelsPage(QWidget *parent) : QWidget(parent) {
  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(16, 16, 16, 16);
  root->setSpacing(12);

  auto *backendBox = new QGroupBox(QStringLiteral("Backend Selection"), this);
  auto *backendGrid = new QGridLayout(backendBox);
  backendGrid->setColumnStretch(0, 0);
  backendGrid->setColumnStretch(1, 1);
  backendGrid->setColumnStretch(2, 1);
  backendGrid->setHorizontalSpacing(18);
  backendGrid->setVerticalSpacing(8);

  backendGrid->addWidget(MutedLabel(QStringLiteral("Camera effects"), backendBox),
                         0, 0);
  backendGrid->addWidget(MutedLabel(QStringLiteral("Preference"), backendBox),
                         0, 1);
  backendGrid->addWidget(MutedLabel(QStringLiteral("Active backend"), backendBox),
                         0, 2);
  videoPreferenceLabel_ = ValueLabel(QStringLiteral("Unknown"), backendBox);
  videoActiveLabel_ = ValueLabel(QStringLiteral("Unknown"), backendBox);
  backendGrid->addWidget(videoPreferenceLabel_, 1, 1);
  backendGrid->addWidget(videoActiveLabel_, 1, 2);

  backendGrid->addWidget(MutedLabel(QStringLiteral("Audio cleanup"), backendBox),
                         2, 0);
  backendGrid->addWidget(MutedLabel(QStringLiteral("Preference"), backendBox),
                         2, 1);
  backendGrid->addWidget(MutedLabel(QStringLiteral("Active backends"), backendBox),
                         2, 2);
  audioPreferenceLabel_ = ValueLabel(QStringLiteral("Unknown"), backendBox);
  auto *activeAudio = new QWidget(backendBox);
  auto *activeAudioLayout = new QVBoxLayout(activeAudio);
  activeAudioLayout->setContentsMargins(0, 0, 0, 0);
  activeAudioLayout->setSpacing(2);
  microphoneActiveLabel_ = ValueLabel(QStringLiteral("Microphone: Unknown"),
                                      activeAudio);
  speakersActiveLabel_ =
      ValueLabel(QStringLiteral("Speakers: Unknown"), activeAudio);
  activeAudioLayout->addWidget(microphoneActiveLabel_);
  activeAudioLayout->addWidget(speakersActiveLabel_);
  backendGrid->addWidget(audioPreferenceLabel_, 3, 1);
  backendGrid->addWidget(activeAudio, 3, 2);
  root->addWidget(backendBox);

  maxineCard_ =
      CreateEngineCard(QStringLiteral("Maxine"), QStringLiteral("maxine"),
                       this);
  openVideoCard_ = CreateEngineCard(QStringLiteral("Open Video / Open CUDA"),
                                    QStringLiteral("open_cuda"), this);
  openAudioCard_ = CreateEngineCard(QStringLiteral("Open Audio"),
                                    QStringLiteral("open_audio"), this);
  connect(openVideoCard_.downloadButton, &QPushButton::clicked, this,
          [this] { StartModelInstall(&openVideoCard_); });
  connect(openAudioCard_.downloadButton, &QPushButton::clicked, this,
          [this] { StartModelInstall(&openAudioCard_); });
  root->addWidget(maxineCard_.frame);
  root->addWidget(openVideoCard_.frame);
  root->addWidget(openAudioCard_.frame);
  root->addStretch(1);
}

EnginesModelsPage::EngineCard
EnginesModelsPage::CreateEngineCard(const QString &title,
                                    const QString &engineId, QWidget *parent) {
  EngineCard card;
  card.engineId = engineId;
  card.frame = new QFrame(parent);
  card.frame->setProperty("scRole", "engineCard");
  card.frame->setProperty("scStatus", "warning");
  auto *layout = new QVBoxLayout(card.frame);
  layout->setContentsMargins(14, 14, 14, 14);
  layout->setSpacing(10);

  auto *header = new QHBoxLayout();
  header->setContentsMargins(0, 0, 0, 0);
  header->setSpacing(10);
  card.title = ValueLabel(title, card.frame);
  card.title->setProperty("scRole", "homeCardTitle");
  card.state = new QLabel(QStringLiteral("Unknown"), card.frame);
  card.state->setProperty("scRole", "statusPill");
  card.state->setProperty("scStatus", "warning");
  card.state->setAlignment(Qt::AlignCenter);
  header->addWidget(card.title, 1);
  header->addWidget(card.state, 0);
  layout->addLayout(header);

  card.summary = MutedLabel(QStringLiteral("Diagnostics have not been read."),
                            card.frame);
  card.models = MutedLabel(QStringLiteral("No model diagnostics reported."),
                           card.frame);
  layout->addWidget(card.summary);
  layout->addWidget(card.models);

  auto *actions = new QHBoxLayout();
  actions->setContentsMargins(0, 0, 0, 0);
  actions->setSpacing(10);
  card.downloadButton =
      new QPushButton(QStringLiteral("Download default models"), card.frame);
  card.downloadButton->setProperty("scVariant", "primary");
  card.downloadButton->setVisible(engineId == QStringLiteral("open_cuda") ||
                                  engineId == QStringLiteral("open_audio"));
  card.downloadButton->setEnabled(false);
  card.downloadStatus = MutedLabel(QString(), card.frame);
  card.downloadStatus->setVisible(card.downloadButton->isVisible());
  actions->addWidget(card.downloadButton, 0);
  actions->addWidget(card.downloadStatus, 1);
  layout->addLayout(actions);

  auto *detailsGrid = new QGridLayout();
  detailsGrid->setContentsMargins(0, 0, 0, 0);
  detailsGrid->setHorizontalSpacing(10);
  detailsGrid->setVerticalSpacing(8);
  detailsGrid->addWidget(MutedLabel(QStringLiteral("Details"), card.frame), 0,
                         0);
  detailsGrid->addWidget(MutedLabel(QStringLiteral("Install Hints"), card.frame),
                         0, 1);
  card.details = DetailsBox(card.frame, 118);
  card.installHints = DetailsBox(card.frame, 118);
  detailsGrid->addWidget(card.details, 1, 0);
  detailsGrid->addWidget(card.installHints, 1, 1);
  layout->addLayout(detailsGrid);

  layout->addWidget(MutedLabel(QStringLiteral("Raw Engine Diagnostics"),
                               card.frame));
  card.rawDetails = DetailsBox(card.frame, 130);
  layout->addWidget(card.rawDetails);

  return card;
}

void EnginesModelsPage::UpdateEngineCard(EngineCard *card,
                                         const EngineStatus &engine,
                                         bool selectedByPreference) {
  if (!card)
    return;

  const QString prop = EngineProperty(engine);
  card->state->setText(EngineStateLabel(engine, selectedByPreference));
  SetDynamicProperty(card->state, "scStatus", prop);
  SetDynamicProperty(card->frame, "scStatus", prop);

  card->summary->setText(EngineSummary(engine, selectedByPreference));
  card->models->setText(ModelSummary(engine));
  if (card->downloadButton && card->downloadStatus) {
    const bool relevant = engine.id == QStringLiteral("open_cuda") ||
                          engine.id == QStringLiteral("open_audio");
    card->downloadButton->setVisible(relevant);
    card->downloadStatus->setVisible(relevant);
    if (relevant) {
      card->installArgs = ModelInstallArgsForEngine(engine);
      card->installRecommended = ModelInstallRecommended(engine);
      card->downloadButton->setText(ModelInstallButtonText(engine));
      card->downloadButton->setToolTip(
          card->installArgs.isEmpty()
              ? QStringLiteral("No model installer command is available.")
              : QStringLiteral("./scripts/install.sh %1")
                    .arg(card->installArgs.join(QStringLiteral(" "))));
      card->downloadStatus->setText(
          ModelInstallStatusText(engine, card->installRecommended));
    } else {
      card->installArgs.clear();
      card->installRecommended = false;
      card->downloadStatus->clear();
    }
  }
  SetPlainTextPreservingScroll(card->details, EngineDetailsText(engine));
  SetPlainTextPreservingScroll(card->installHints, InstallHintsText(engine));
  SetPlainTextPreservingScroll(
      card->rawDetails,
      engine.rawJson.trimmed().isEmpty()
          ? QStringLiteral("No raw engine diagnostics reported.")
          : engine.rawJson);
  RefreshDownloadButtons();
}

void EnginesModelsPage::RefreshDownloadButtons() {
  const bool running = modelInstallProcess_ != nullptr;
  auto update = [&](EngineCard *card) {
    if (!card || !card->downloadButton || !card->downloadButton->isVisible())
      return;

    const bool enabled =
        !running && card->installRecommended && !card->installArgs.isEmpty();
    card->downloadButton->setEnabled(enabled);
    if (running && card == activeInstallCard_ && card->downloadStatus) {
      card->downloadStatus->setText(
          QStringLiteral("Downloading model packs..."));
    } else if (running && card->downloadStatus) {
      card->downloadStatus->setText(
          QStringLiteral("Another model download is running."));
    }
  };

  update(&openVideoCard_);
  update(&openAudioCard_);
}

QString EnginesModelsPage::ResolveInstallScript(QString *error) const {
  QStringList candidates;
  const QString envPath =
      QString::fromLocal8Bit(qgetenv("STUDIOCAST_INSTALL_SCRIPT")).trimmed();
  if (!envPath.isEmpty())
    candidates.push_back(envPath);

  AddInstallScriptCandidate(&candidates,
                            QString::fromUtf8(STUDIOCAST_SOURCE_DIR));
  AddInstallScriptCandidate(&candidates, ManifestSourceDir());

  const QDir appDir(QCoreApplication::applicationDirPath());
  AddInstallScriptCandidate(&candidates, appDir.filePath(QStringLiteral("..")));
  AddInstallScriptCandidate(&candidates, QDir::currentPath());

  QStringList checked;
  for (const QString &candidate : candidates) {
    const QFileInfo info(candidate);
    checked.push_back(info.absoluteFilePath());
    if (info.isFile())
      return info.absoluteFilePath();
  }

  if (error) {
    *error = QStringLiteral("Could not find scripts/install.sh. Checked:\n%1")
                 .arg(checked.join(QStringLiteral("\n")));
  }
  return {};
}

void EnginesModelsPage::StartModelInstall(EngineCard *card) {
  if (!card || modelInstallProcess_)
    return;

  QString error;
  const QString script = ResolveInstallScript(&error);
  if (script.isEmpty()) {
    QMessageBox::warning(this, QStringLiteral("StudioCast Models"), error);
    return;
  }

  const QString bash = QStandardPaths::findExecutable(QStringLiteral("bash"));
  if (bash.isEmpty()) {
    QMessageBox::warning(this, QStringLiteral("StudioCast Models"),
                         QStringLiteral("Could not find bash to run %1.")
                             .arg(script));
    return;
  }

  if (card->installArgs.isEmpty()) {
    QMessageBox::information(
        this, QStringLiteral("StudioCast Models"),
        QStringLiteral("No model installer command is available for this "
                       "engine."));
    return;
  }

  modelInstallOutput_.clear();
  activeInstallCard_ = card;
  modelInstallProcess_ = new QProcess(this);
  modelInstallProcess_->setProcessChannelMode(QProcess::SeparateChannels);
  modelInstallProcess_->setWorkingDirectory(QFileInfo(script).absolutePath());

  connect(modelInstallProcess_, &QProcess::readyReadStandardOutput, this,
          [this] {
            if (modelInstallProcess_) {
              modelInstallOutput_ += QString::fromLocal8Bit(
                  modelInstallProcess_->readAllStandardOutput());
            }
          });
  connect(modelInstallProcess_, &QProcess::readyReadStandardError, this,
          [this] {
            if (modelInstallProcess_) {
              modelInstallOutput_ += QString::fromLocal8Bit(
                  modelInstallProcess_->readAllStandardError());
            }
          });
  connect(modelInstallProcess_,
          QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
          [this](int exitCode, QProcess::ExitStatus exitStatus) {
            QProcess *process = modelInstallProcess_;
            if (process) {
              modelInstallOutput_ +=
                  QString::fromLocal8Bit(process->readAllStandardOutput());
              modelInstallOutput_ +=
                  QString::fromLocal8Bit(process->readAllStandardError());
            }

            const bool ok =
                exitStatus == QProcess::NormalExit && exitCode == 0;
            if (activeInstallCard_ && activeInstallCard_->downloadStatus) {
              activeInstallCard_->downloadStatus->setText(
                  ok ? QStringLiteral("Model download completed. Status will "
                                      "refresh shortly.")
                     : QStringLiteral("Model download failed. See details."));
            }

            const QString title = QStringLiteral("StudioCast Models");
            if (ok) {
              QMessageBox::information(
                  this, title,
                  QStringLiteral("Model download completed. StudioCast will "
                                 "refresh engine diagnostics shortly."));
            } else {
              QString details = modelInstallOutput_.trimmed();
              if (details.size() > 4000)
                details = details.right(4000);
              QMessageBox::warning(
                  this, title,
                  QStringLiteral("Model download failed with exit code %1.%2")
                      .arg(exitCode)
                      .arg(details.isEmpty()
                               ? QString()
                               : QStringLiteral("\n\n%1").arg(details)));
            }

            if (process)
              process->deleteLater();
            modelInstallProcess_ = nullptr;
            activeInstallCard_ = nullptr;
            emit ModelsInstallFinished();
            RefreshDownloadButtons();
          });

  QStringList processArgs;
  processArgs << script;
  processArgs << card->installArgs;
  card->downloadStatus->setText(QStringLiteral("Starting model download..."));
  RefreshDownloadButtons();
  modelInstallProcess_->start(bash, processArgs);
  if (!modelInstallProcess_->waitForStarted(1000)) {
    const QString message = modelInstallProcess_->errorString();
    modelInstallProcess_->deleteLater();
    modelInstallProcess_ = nullptr;
    activeInstallCard_ = nullptr;
    RefreshDownloadButtons();
    QMessageBox::warning(this, QStringLiteral("StudioCast Models"),
                         QStringLiteral("Failed to start model installer: %1")
                             .arg(message));
  }
}

void EnginesModelsPage::UpdateStatus(const DaemonStatusSnapshot &snapshot) {
  if (videoPreferenceLabel_)
    videoPreferenceLabel_->setText(
        FriendlyBackendLabel(snapshot.videoEffectsEnginePreference));
  if (videoActiveLabel_) {
    const QString active =
        !snapshot.reachable || !snapshot.parsed
            ? QStringLiteral("Unknown")
            : ActiveBackendSummary(snapshot.videoEffectsActiveBackends);
    videoActiveLabel_->setText(active);
    videoActiveLabel_->setToolTip(snapshot.videoEffectsActiveBackends);
  }

  if (audioPreferenceLabel_)
    audioPreferenceLabel_->setText(
        FriendlyBackendLabel(snapshot.audioEffectsEnginePreference));
  if (microphoneActiveLabel_) {
    microphoneActiveLabel_->setText(
        QStringLiteral("Microphone: %1")
            .arg(FriendlyBackendLabel(snapshot.microphoneActiveBackend)));
    microphoneActiveLabel_->setToolTip(snapshot.microphoneActiveBackend);
  }
  if (speakersActiveLabel_) {
    QString speakerActive;
    const QString routeMode = snapshot.speakersRouteMode.trimmed().toLower();
    if (routeMode == QStringLiteral("loopback")) {
      speakerActive = QStringLiteral("Loopback / pass-through");
    } else if (routeMode == QStringLiteral("off") &&
               snapshot.speakersActiveBackend.trimmed().isEmpty()) {
      speakerActive = QStringLiteral("Off");
    } else {
      speakerActive = FriendlyBackendLabel(snapshot.speakersActiveBackend);
    }
    speakersActiveLabel_->setText(
        QStringLiteral("Speakers: %1").arg(speakerActive));
    speakersActiveLabel_->setToolTip(snapshot.speakersActiveBackend);
  }

  UpdateEngineCard(&maxineCard_, snapshot.maxine,
                   PreferenceSelectsMaxine(snapshot));
  UpdateEngineCard(&openVideoCard_, snapshot.openCuda,
                   PreferenceSelectsOpenVideo(snapshot));
  UpdateEngineCard(&openAudioCard_, snapshot.openAudio,
                   PreferenceSelectsOpenAudio(snapshot));
}

} // namespace studiocast::gui
