#include "support_page.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QStyle>
#include <QVBoxLayout>

#include <array>
#include <vector>

#include "gui/status/daemon_status_snapshot.h"
#include "studiocast/version.h"

namespace studiocast::gui {
namespace {

enum class IssueSeverity {
  Warning,
  Error,
};

struct SupportIssue {
  IssueSeverity severity = IssueSeverity::Warning;
  QString title;
  QString summary;
  QString details;
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

QPlainTextEdit *DetailsBox(QWidget *parent, int minHeight,
                           bool wrap = true) {
  auto *text = new QPlainTextEdit(parent);
  text->setReadOnly(true);
  text->setLineWrapMode(wrap ? QPlainTextEdit::WidgetWidth
                             : QPlainTextEdit::NoWrap);
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

QString JoinNonEmpty(const QStringList &lines,
                     const QString &separator = QStringLiteral("\n")) {
  QStringList out;
  for (const QString &line : lines) {
    const QString trimmed = line.trimmed();
    if (!trimmed.isEmpty())
      out.push_back(trimmed);
  }
  return out.join(separator);
}

void CopyText(const QString &text) {
  if (QClipboard *clipboard = QApplication::clipboard())
    clipboard->setText(text);
}

void ClearLayout(QLayout *layout) {
  while (QLayoutItem *item = layout->takeAt(0)) {
    if (QWidget *widget = item->widget())
      widget->deleteLater();
    delete item;
  }
}

QString RawStatusText(const DaemonStatusSnapshot &snapshot) {
  if (!snapshot.rawJson.trimmed().isEmpty())
    return snapshot.rawJson;
  if (!snapshot.transportError.trimmed().isEmpty())
    return QStringLiteral("Daemon unavailable: %1").arg(snapshot.transportError);
  if (!snapshot.parseError.trimmed().isEmpty())
    return QStringLiteral("Daemon status parse error: %1")
        .arg(snapshot.parseError);
  return QStringLiteral("Daemon status has not been read.");
}

QString SeverityProperty(IssueSeverity severity) {
  return severity == IssueSeverity::Error ? QStringLiteral("error")
                                          : QStringLiteral("warning");
}

QString SeverityLabel(IssueSeverity severity) {
  return severity == IssueSeverity::Error ? QStringLiteral("Blocked")
                                          : QStringLiteral("Review");
}

bool IsBlockingDeviceState(ReadinessState state) {
  switch (state) {
  case ReadinessState::Ready:
  case ReadinessState::Idle:
  case ReadinessState::Processing:
    return false;
  case ReadinessState::Unknown:
  case ReadinessState::NeedsSetup:
  case ReadinessState::DaemonUnavailable:
  case ReadinessState::MissingVirtualDevice:
  case ReadinessState::NoPhysicalDevice:
  case ReadinessState::RecoverableError:
  case ReadinessState::FatalError:
  case ReadinessState::MissingModel:
    return true;
  }
  return true;
}

IssueSeverity DeviceSeverity(ReadinessState state) {
  switch (state) {
  case ReadinessState::DaemonUnavailable:
  case ReadinessState::RecoverableError:
  case ReadinessState::FatalError:
    return IssueSeverity::Error;
  case ReadinessState::Unknown:
  case ReadinessState::NeedsSetup:
  case ReadinessState::MissingVirtualDevice:
  case ReadinessState::NoPhysicalDevice:
  case ReadinessState::MissingModel:
  case ReadinessState::Ready:
  case ReadinessState::Idle:
  case ReadinessState::Processing:
    return IssueSeverity::Warning;
  }
  return IssueSeverity::Warning;
}

QString DeviceIssueTitle(const QString &name, ReadinessState state) {
  if (state == ReadinessState::MissingVirtualDevice) {
    if (name == QStringLiteral("Camera"))
      return QStringLiteral("StudioCast Camera is not available");
    if (name == QStringLiteral("Microphone"))
      return QStringLiteral("StudioCast Microphone is not available");
    return QStringLiteral("StudioCast Speakers are not available");
  }
  if (state == ReadinessState::NoPhysicalDevice) {
    return name == QStringLiteral("Speakers")
               ? QStringLiteral("Choose a physical speaker output")
               : QStringLiteral("Choose a physical microphone input");
  }
  if (state == ReadinessState::Unknown)
    return QStringLiteral("%1 status is not reported").arg(name);
  if (state == ReadinessState::RecoverableError)
    return QStringLiteral("%1 processing reported a problem").arg(name);
  if (state == ReadinessState::MissingModel)
    return QStringLiteral("%1 needs a model pack").arg(name);
  return QStringLiteral("%1 needs attention").arg(name);
}

QString DeviceIssueSummary(const QString &name, ReadinessState state) {
  if (state == ReadinessState::MissingVirtualDevice) {
    return QStringLiteral("Other apps may not see the StudioCast %1 until its "
                          "virtual device is set up.")
        .arg(name);
  }
  if (state == ReadinessState::NoPhysicalDevice) {
    return name == QStringLiteral("Speakers")
               ? QStringLiteral("The speaker route needs a safe physical "
                                "output target.")
               : QStringLiteral("The microphone route needs a safe physical "
                                "input source.");
  }
  if (state == ReadinessState::Unknown) {
    return QStringLiteral("The daemon response did not include enough %1 "
                          "status to summarize it.")
        .arg(name.toLower());
  }
  if (state == ReadinessState::RecoverableError) {
    return QStringLiteral("The daemon reported an error for %1; technical "
                          "details are below.")
        .arg(name.toLower());
  }
  if (state == ReadinessState::MissingModel) {
    return QStringLiteral("A selected %1 effect cannot run until the required "
                          "model pack is installed.")
        .arg(name.toLower());
  }
  return QStringLiteral("Open %1 for the everyday controls; support details "
                        "are below.")
      .arg(name);
}

QString DeviceDetailsText(const QString &name,
                          const DeviceReadiness &readiness) {
  QStringList lines;
  lines << QStringLiteral("%1: %2").arg(name, ReadinessLabel(readiness.state));
  lines << readiness.summary;
  lines << readiness.detail;
  if (!readiness.notes.isEmpty()) {
    lines << QStringLiteral("Notes:");
    for (const QString &note : readiness.notes)
      lines << QStringLiteral("- %1").arg(note);
  }
  if (!readiness.disabledReasons.isEmpty()) {
    lines << QStringLiteral("Disabled effects:");
    for (const QString &reason : readiness.disabledReasons)
      lines << QStringLiteral("- %1").arg(reason);
  }
  return JoinNonEmpty(lines);
}

void AddDeviceIssues(std::vector<SupportIssue> *issues, const QString &name,
                     const DeviceReadiness &readiness) {
  if (IsBlockingDeviceState(readiness.state)) {
    issues->push_back(SupportIssue{
        DeviceSeverity(readiness.state),
        DeviceIssueTitle(name, readiness.state),
        DeviceIssueSummary(name, readiness.state),
        DeviceDetailsText(name, readiness),
    });
  }

  if (!readiness.disabledReasons.isEmpty()) {
    QStringList details;
    details << QStringLiteral("%1 disabled effects:").arg(name);
    for (const QString &reason : readiness.disabledReasons)
      details << QStringLiteral("- %1").arg(reason);
    issues->push_back(SupportIssue{
        IssueSeverity::Warning,
        QStringLiteral("%1 effects need backend or model attention").arg(name),
        QStringLiteral("One or more effects cannot run with the current "
                       "engine/model state."),
        JoinNonEmpty(details),
    });
  }
}

QString EngineDetailsText(const EngineStatus &engine) {
  QStringList lines;
  lines << QStringLiteral("%1 diagnostics").arg(engine.label);
  lines << QStringLiteral("Summary: %1")
               .arg(engine.summary.trimmed().isEmpty()
                        ? QStringLiteral("No summary reported.")
                        : engine.summary.trimmed());
  if (!engine.blockedReason.trimmed().isEmpty())
    lines << QStringLiteral("Blocked reason: %1").arg(engine.blockedReason);
  if (!engine.blockedDetails.isEmpty()) {
    lines << QStringLiteral("Blocked details:");
    for (const QString &detail : engine.blockedDetails)
      lines << QStringLiteral("- %1").arg(detail);
  }
  if (engine.missingModelCount > 0 && !engine.missingModels.isEmpty()) {
    lines << QStringLiteral("Missing or invalid models:");
    for (const QString &model : engine.missingModels)
      lines << QStringLiteral("- %1").arg(model);
  }
  if (engine.configuredMissingModelCount > 0) {
    lines << QStringLiteral("%1 configured model selection%2 not reported "
                            "installed by daemon diagnostics.")
                 .arg(engine.configuredMissingModelCount)
                 .arg(engine.configuredMissingModelCount == 1
                          ? QString()
                          : QStringLiteral("s"));
  }
  if (!engine.blockedEffects.isEmpty()) {
    lines << QStringLiteral("Blocked effects:");
    for (const QString &effect : engine.blockedEffects)
      lines << QStringLiteral("- %1").arg(effect);
  }
  if (!engine.installHints.isEmpty()) {
    lines << QStringLiteral("Install hints:");
    for (const QString &hint : engine.installHints)
      lines << hint;
  }
  return JoinNonEmpty(lines);
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

void AddEngineIssues(std::vector<SupportIssue> *issues,
                     const EngineStatus &engine, bool selected) {
  if (!engine.present)
    return;

  const bool available = engine.ok || engine.supported;
  if (!available) {
    issues->push_back(SupportIssue{
        selected ? IssueSeverity::Error : IssueSeverity::Warning,
        QStringLiteral("%1 is unavailable").arg(engine.label),
        selected
            ? QStringLiteral("A selected engine cannot run, so related "
                             "effects may fall back or stay off.")
            : QStringLiteral("%1 diagnostics report that this engine cannot "
                             "run on this system.")
                  .arg(engine.label),
        EngineDetailsText(engine),
    });
    return;
  }

  if (engine.missingModelCount > 0 ||
      engine.configuredMissingModelCount > 0) {
    issues->push_back(SupportIssue{
        IssueSeverity::Warning,
        QStringLiteral("%1 model selection needs attention").arg(engine.label),
        QStringLiteral("Some configured or required model packs are missing "
                       "or invalid."),
        EngineDetailsText(engine),
    });
  } else if ((engine.id == QStringLiteral("open_cuda") ||
              engine.id == QStringLiteral("open_audio")) &&
             engine.installedModelCount == 0) {
    issues->push_back(SupportIssue{
        IssueSeverity::Warning,
        QStringLiteral("%1 model packs are not installed").arg(engine.label),
        QStringLiteral("%1 runtime diagnostics are available, but no usable "
                       "model packs were reported.")
            .arg(engine.label),
        EngineDetailsText(engine),
    });
  }

  if (!engine.blockedEffects.isEmpty()) {
    issues->push_back(SupportIssue{
        IssueSeverity::Warning,
        QStringLiteral("Some %1 effects are blocked").arg(engine.label),
        QStringLiteral("The daemon disabled one or more effects for this "
                       "engine."),
        EngineDetailsText(engine),
    });
  }
}

std::vector<SupportIssue> BuildSupportIssues(
    const DaemonStatusSnapshot &snapshot) {
  std::vector<SupportIssue> issues;

  if (!snapshot.reachable) {
    issues.push_back(SupportIssue{
        IssueSeverity::Error,
        QStringLiteral("StudioCast background service is not responding"),
        QStringLiteral("StudioCast cannot read live device or engine status "
                       "until the daemon responds."),
        snapshot.ServiceDetail(),
    });
    return issues;
  }

  if (!snapshot.parsed) {
    issues.push_back(SupportIssue{
        IssueSeverity::Error,
        QStringLiteral("Daemon status is unreadable"),
        QStringLiteral("The daemon responded, but the GUI could not parse the "
                       "status payload."),
        snapshot.ServiceDetail(),
    });
    return issues;
  }

  if (!snapshot.serviceRunning) {
    issues.push_back(SupportIssue{
        IssueSeverity::Error,
        QStringLiteral("StudioCast background service is not ready"),
        QStringLiteral("The daemon connection works, but status reports that "
                       "the service is not ready."),
        snapshot.ServiceDetail(),
    });
    return issues;
  }

  AddDeviceIssues(&issues, QStringLiteral("Camera"), snapshot.camera);
  AddDeviceIssues(&issues, QStringLiteral("Microphone"), snapshot.microphone);
  AddDeviceIssues(&issues, QStringLiteral("Speakers"), snapshot.speakers);

  AddEngineIssues(&issues, snapshot.maxine, PreferenceSelectsMaxine(snapshot));
  AddEngineIssues(&issues, snapshot.openCuda,
                  PreferenceSelectsOpenVideo(snapshot));
  AddEngineIssues(&issues, snapshot.openAudio,
                  PreferenceSelectsOpenAudio(snapshot));

  return issues;
}

QString BuildIssueDetailsText(const DaemonStatusSnapshot &snapshot,
                              const std::vector<SupportIssue> &issues) {
  QStringList lines;
  lines << QStringLiteral("Service");
  lines << QStringLiteral("- State: %1").arg(snapshot.ServiceSummary());
  lines << QStringLiteral("- Detail: %1").arg(snapshot.ServiceDetail());
  if (!snapshot.version.trimmed().isEmpty())
    lines << QStringLiteral("- Daemon version: %1").arg(snapshot.version);
  if (!snapshot.gitSha.trimmed().isEmpty())
    lines << QStringLiteral("- Daemon git SHA: %1").arg(snapshot.gitSha);
  if (!snapshot.socketPath.trimmed().isEmpty())
    lines << QStringLiteral("- Socket: %1").arg(snapshot.socketPath);

  if (issues.empty()) {
    lines << QStringLiteral("");
    lines << QStringLiteral("No current support issues reported by daemon "
                            "status.");
    return JoinNonEmpty(lines);
  }

  lines << QStringLiteral("");
  lines << QStringLiteral("Current issue details");
  for (const SupportIssue &issue : issues) {
    lines << QStringLiteral("");
    lines << QStringLiteral("%1 [%2]")
                 .arg(issue.title, SeverityLabel(issue.severity));
    lines << issue.summary;
    lines << issue.details;
  }
  return JoinNonEmpty(lines);
}

QString BuildInstallHintsText(const DaemonStatusSnapshot &snapshot) {
  QStringList lines;
  const std::array<const EngineStatus *, 3> engines = {
      &snapshot.maxine, &snapshot.openCuda, &snapshot.openAudio};
  for (const EngineStatus *engine : engines) {
    if (!engine || engine->installHints.isEmpty())
      continue;
    lines << QStringLiteral("%1").arg(engine->label);
    for (const QString &hint : engine->installHints)
      lines << hint;
    lines << QStringLiteral("");
  }

  const QString text = JoinNonEmpty(lines);
  return text.isEmpty()
             ? QStringLiteral("No install hints reported by daemon "
                              "diagnostics.")
             : text;
}

QString ResolveStudiocastCtlProgram() {
  const QString local =
      QDir(QCoreApplication::applicationDirPath()).filePath(
          QStringLiteral("studiocastctl"));
  if (QFileInfo::exists(local))
    return local;

  const QString fromPath =
      QStandardPaths::findExecutable(QStringLiteral("studiocastctl"));
  if (!fromPath.isEmpty())
    return fromPath;

  return QStringLiteral("studiocastctl");
}

QString MakeReportPath() {
  QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
  if (dir.trimmed().isEmpty())
    dir = QDir::tempPath();
  QDir().mkpath(dir);
  const QString stamp =
      QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
  return QDir(dir).filePath(
      QStringLiteral("studiocast-debug-report-%1.txt").arg(stamp));
}

QString ProcessOutputText(const QString &standardOutput,
                          const QString &standardError) {
  QStringList lines;
  if (!standardOutput.trimmed().isEmpty())
    lines << QStringLiteral("stdout:") << standardOutput.trimmed();
  if (!standardError.trimmed().isEmpty())
    lines << QStringLiteral("stderr:") << standardError.trimmed();
  return JoinNonEmpty(lines);
}

} // namespace

SupportPage::SupportPage(QWidget *parent) : QWidget(parent) {
  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(16, 16, 16, 16);
  root->setSpacing(12);

  auto *summaryFrame = new QFrame(this);
  summaryFrame->setProperty("scRole", "homeSummary");
  auto *summaryLayout = new QVBoxLayout(summaryFrame);
  summaryLayout->setContentsMargins(14, 14, 14, 14);
  summaryLayout->setSpacing(4);
  summaryLabel_ =
      ValueLabel(QStringLiteral("Checking support status"), summaryFrame);
  summaryLabel_->setProperty("scRole", "homeHeadline");
  summaryDetailLabel_ =
      MutedLabel(QStringLiteral("Waiting for daemon status."), summaryFrame);
  summaryLayout->addWidget(summaryLabel_);
  summaryLayout->addWidget(summaryDetailLabel_);
  root->addWidget(summaryFrame);

  auto *issuesBox = new QGroupBox(QStringLiteral("Current Issues"), this);
  issuesLayout_ = new QVBoxLayout(issuesBox);
  issuesLayout_->setSpacing(10);
  root->addWidget(issuesBox);

  auto *aboutBox = new QGroupBox(QStringLiteral("About"), this);
  auto *aboutGrid = new QGridLayout(aboutBox);
  aboutGrid->setHorizontalSpacing(14);
  aboutGrid->setVerticalSpacing(8);
  aboutGrid->addWidget(MutedLabel(QStringLiteral("Application"), aboutBox), 0,
                       0);
  aboutGrid->addWidget(
      ValueLabel(QStringLiteral("StudioCast %1 (%2)")
                     .arg(STUDIOCAST_VERSION, STUDIOCAST_GIT_SHA),
                 aboutBox),
      0, 1);
  aboutGrid->addWidget(MutedLabel(QStringLiteral("Daemon"), aboutBox), 1, 0);
  daemonVersionLabel_ = ValueLabel(QStringLiteral("Unknown"), aboutBox);
  aboutGrid->addWidget(daemonVersionLabel_, 1, 1);
  aboutGrid->addWidget(MutedLabel(QStringLiteral("Daemon git SHA"), aboutBox),
                       2, 0);
  daemonGitLabel_ = ValueLabel(QStringLiteral("Unknown"), aboutBox);
  aboutGrid->addWidget(daemonGitLabel_, 2, 1);
  aboutGrid->addWidget(MutedLabel(QStringLiteral("Service"), aboutBox), 3, 0);
  serviceStateLabel_ = ValueLabel(QStringLiteral("Checking"), aboutBox);
  aboutGrid->addWidget(serviceStateLabel_, 3, 1);
  aboutGrid->addWidget(MutedLabel(QStringLiteral("Notice"), aboutBox), 4, 0);
  aboutGrid->addWidget(
      MutedLabel(QStringLiteral("An open-source Linux app with a "
                                "Broadcast-style UI. Not affiliated with "
                                "NVIDIA."),
                 aboutBox),
      4, 1);
  aboutGrid->setColumnStretch(1, 1);
  root->addWidget(aboutBox);

  auto *reportBox =
      new QGroupBox(QStringLiteral("Support Report"), this);
  auto *reportLayout = new QVBoxLayout(reportBox);
  reportLayout->setSpacing(10);
  reportStatusLabel_ =
      MutedLabel(QStringLiteral("Generate a support report with "
                                "studiocastctl debug-report."),
                 reportBox);
  reportLayout->addWidget(reportStatusLabel_);
  reportPathLabel_ = MutedLabel(QString(), reportBox);
  reportPathLabel_->setVisible(false);
  reportLayout->addWidget(reportPathLabel_);

  auto *reportButtons = new QHBoxLayout();
  reportButtons->setContentsMargins(0, 0, 0, 0);
  reportButtons->setSpacing(8);
  reportButtons->addStretch(1);
  generateReportButton_ =
      new QPushButton(QStringLiteral("Generate Report"), reportBox);
  generateReportButton_->setProperty("scVariant", "primary");
  connect(generateReportButton_, &QPushButton::clicked, this,
          &SupportPage::GenerateSupportReport);
  reportButtons->addWidget(generateReportButton_);
  copyReportButton_ = new QPushButton(QStringLiteral("Copy Report"), reportBox);
  copyReportButton_->setEnabled(false);
  connect(copyReportButton_, &QPushButton::clicked, this,
          [this] { CopyText(reportText_->toPlainText()); });
  reportButtons->addWidget(copyReportButton_);
  reportLayout->addLayout(reportButtons);

  reportText_ = DetailsBox(reportBox, 220, false);
  reportText_->setPlaceholderText(
      QStringLiteral("Generated report text will appear here."));
  reportLayout->addWidget(reportText_);
  root->addWidget(reportBox);

  auto *detailsBox =
      new QGroupBox(QStringLiteral("Diagnostics Details"), this);
  auto *detailsLayout = new QVBoxLayout(detailsBox);
  detailsLayout->setSpacing(10);

  auto *issueHeader = new QHBoxLayout();
  issueHeader->setContentsMargins(0, 0, 0, 0);
  issueHeader->addWidget(MutedLabel(QStringLiteral("Issue Details"),
                                    detailsBox),
                         1);
  copyIssueDetailsButton_ =
      new QPushButton(QStringLiteral("Copy Details"), detailsBox);
  connect(copyIssueDetailsButton_, &QPushButton::clicked, this,
          [this] { CopyText(currentIssueDetails_); });
  issueHeader->addWidget(copyIssueDetailsButton_);
  detailsLayout->addLayout(issueHeader);
  issueDetails_ = DetailsBox(detailsBox, 150);
  detailsLayout->addWidget(issueDetails_);

  auto *hintsHeader = new QHBoxLayout();
  hintsHeader->setContentsMargins(0, 0, 0, 0);
  hintsHeader->addWidget(MutedLabel(QStringLiteral("Install Hints"),
                                    detailsBox),
                         1);
  copyInstallHintsButton_ =
      new QPushButton(QStringLiteral("Copy Install Hints"), detailsBox);
  connect(copyInstallHintsButton_, &QPushButton::clicked, this,
          [this] { CopyText(currentInstallHints_); });
  hintsHeader->addWidget(copyInstallHintsButton_);
  detailsLayout->addLayout(hintsHeader);
  installHints_ = DetailsBox(detailsBox, 120);
  detailsLayout->addWidget(installHints_);

  auto *rawHeader = new QHBoxLayout();
  rawHeader->setContentsMargins(0, 0, 0, 0);
  rawHeader->addWidget(MutedLabel(QStringLiteral("Raw Daemon Status"),
                                  detailsBox),
                       1);
  copyRawStatusButton_ =
      new QPushButton(QStringLiteral("Copy Raw Status"), detailsBox);
  connect(copyRawStatusButton_, &QPushButton::clicked, this,
          [this] { CopyText(currentRawStatus_); });
  rawHeader->addWidget(copyRawStatusButton_);
  detailsLayout->addLayout(rawHeader);
  rawStatus_ = DetailsBox(detailsBox, 240, false);
  rawStatus_->setPlaceholderText(
      QStringLiteral("Daemon status has not been read."));
  detailsLayout->addWidget(rawStatus_);
  root->addWidget(detailsBox);

  root->addStretch(1);
}

void SupportPage::UpdateStatus(const DaemonStatusSnapshot &snapshot) {
  const std::vector<SupportIssue> issues = BuildSupportIssues(snapshot);

  if (!snapshot.reachable || !snapshot.parsed || !snapshot.serviceRunning) {
    summaryLabel_->setText(snapshot.ServiceSummary());
    summaryDetailLabel_->setText(
        QStringLiteral("Support details and raw daemon status remain "
                       "copyable below."));
    SetDynamicProperty(summaryLabel_, "scStatus", "error");
  } else if (issues.empty()) {
    summaryLabel_->setText(QStringLiteral("No current issues reported"));
    summaryDetailLabel_->setText(
        QStringLiteral("Raw daemon status and support report generation are "
                       "available below."));
    SetDynamicProperty(summaryLabel_, "scStatus", "good");
  } else {
    summaryLabel_->setText(
        QStringLiteral("%1 current support issue%2")
            .arg(issues.size())
            .arg(issues.size() == 1 ? QString() : QStringLiteral("s")));
    summaryDetailLabel_->setText(
        QStringLiteral("Plain-language summaries are listed here; technical "
                       "details stay in the diagnostics sections."));
    SetDynamicProperty(summaryLabel_, "scStatus", "warning");
  }

  ClearLayout(issuesLayout_);
  if (issues.empty()) {
    issuesLayout_->addWidget(
        MutedLabel(QStringLiteral("No current support issues reported by "
                                  "daemon status."),
                   issuesLayout_->parentWidget()));
  } else {
    for (const SupportIssue &issue : issues) {
      auto *row = new QFrame(issuesLayout_->parentWidget());
      row->setProperty("scRole", "supportIssue");
      row->setProperty("scStatus", SeverityProperty(issue.severity));
      auto *rowLayout = new QVBoxLayout(row);
      rowLayout->setContentsMargins(12, 12, 12, 12);
      rowLayout->setSpacing(8);

      auto *header = new QHBoxLayout();
      header->setContentsMargins(0, 0, 0, 0);
      header->setSpacing(10);
      header->addWidget(ValueLabel(issue.title, row), 1);
      auto *pill = new QLabel(SeverityLabel(issue.severity), row);
      pill->setProperty("scRole", "statusPill");
      pill->setProperty("scStatus", SeverityProperty(issue.severity));
      pill->setAlignment(Qt::AlignCenter);
      header->addWidget(pill, 0);
      rowLayout->addLayout(header);
      rowLayout->addWidget(MutedLabel(issue.summary, row));
      issuesLayout_->addWidget(row);
    }
  }

  daemonVersionLabel_->setText(snapshot.version.trimmed().isEmpty()
                                   ? QStringLiteral("Unknown")
                                   : snapshot.version);
  daemonGitLabel_->setText(snapshot.gitSha.trimmed().isEmpty()
                               ? QStringLiteral("Unknown")
                               : snapshot.gitSha);
  serviceStateLabel_->setText(snapshot.ServiceSummary());

  currentIssueDetails_ = BuildIssueDetailsText(snapshot, issues);
  currentInstallHints_ = BuildInstallHintsText(snapshot);
  currentRawStatus_ = RawStatusText(snapshot);

  issueDetails_->setPlainText(currentIssueDetails_);
  installHints_->setPlainText(currentInstallHints_);
  rawStatus_->setPlainText(currentRawStatus_);
}

void SupportPage::GenerateSupportReport() {
  if (reportProcess_)
    return;

  reportProgram_ = ResolveStudiocastCtlProgram();
  reportOutputPath_ = MakeReportPath();

  reportText_->clear();
  copyReportButton_->setEnabled(false);
  generateReportButton_->setEnabled(false);
  reportStatusLabel_->setText(
      QStringLiteral("Generating support report with %1...")
          .arg(reportProgram_));
  reportPathLabel_->setText(
      QStringLiteral("Output file: %1").arg(reportOutputPath_));
  reportPathLabel_->setVisible(true);

  reportProcess_ = new QProcess(this);
  reportProcess_->setProgram(reportProgram_);
  reportProcess_->setArguments(
      {QStringLiteral("debug-report"), QStringLiteral("--out"),
       reportOutputPath_});
  connect(reportProcess_, &QProcess::errorOccurred, this,
          &SupportPage::OnReportError);
  connect(reportProcess_,
          QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
          &SupportPage::OnReportFinished);
  reportProcess_->start();
}

void SupportPage::OnReportError(QProcess::ProcessError error) {
  if (!reportProcess_ || error != QProcess::FailedToStart)
    return;

  const QString message =
      QStringLiteral("Failed to start %1: %2")
          .arg(reportProgram_, reportProcess_->errorString());
  QObject::disconnect(reportProcess_, nullptr, this, nullptr);
  reportProcess_->deleteLater();
  reportProcess_ = nullptr;

  generateReportButton_->setEnabled(true);
  copyReportButton_->setEnabled(false);
  reportStatusLabel_->setText(
      QStringLiteral("%1 Raw daemon status remains copyable below.")
          .arg(message));
  reportText_->setPlainText(message);
}

void SupportPage::OnReportFinished(int exitCode,
                                   QProcess::ExitStatus exitStatus) {
  if (!reportProcess_)
    return;

  QProcess *process = reportProcess_;
  reportProcess_ = nullptr;
  const QString standardOutput =
      QString::fromUtf8(process->readAllStandardOutput());
  const QString standardError =
      QString::fromUtf8(process->readAllStandardError());
  process->deleteLater();

  generateReportButton_->setEnabled(true);

  QString report;
  QFile file(reportOutputPath_);
  if (file.open(QIODevice::ReadOnly | QIODevice::Text))
    report = QString::fromUtf8(file.readAll());

  const bool hasReport = !report.trimmed().isEmpty();
  const bool ok = exitStatus == QProcess::NormalExit && exitCode == 0;

  if (hasReport) {
    reportText_->setPlainText(report);
    copyReportButton_->setEnabled(true);
    if (ok) {
      reportStatusLabel_->setText(
          QStringLiteral("Support report generated."));
    } else if (exitStatus == QProcess::CrashExit) {
      reportStatusLabel_->setText(
          QStringLiteral("studiocastctl crashed, but a partial report was "
                         "written. Raw daemon status remains copyable below."));
    } else {
      reportStatusLabel_->setText(
          QStringLiteral("studiocastctl exited with code %1; the generated "
                         "report includes the failing diagnostics. Raw daemon "
                         "status remains copyable below.")
              .arg(exitCode));
    }
    return;
  }

  copyReportButton_->setEnabled(false);
  QString failure;
  if (exitStatus == QProcess::CrashExit) {
    failure = QStringLiteral("studiocastctl crashed before writing a report.");
  } else {
    failure =
        QStringLiteral("studiocastctl failed with exit code %1 and did not "
                       "write a report.")
            .arg(exitCode);
  }

  const QString processOutput = ProcessOutputText(standardOutput, standardError);
  reportStatusLabel_->setText(
      QStringLiteral("%1 Raw daemon status remains copyable below.")
          .arg(failure));
  reportText_->setPlainText(JoinNonEmpty({failure, processOutput}, "\n\n"));
}

} // namespace studiocast::gui
