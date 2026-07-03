#pragma once

#include <QProcess>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QVBoxLayout;

namespace studiocast::gui {

struct DaemonStatusSnapshot;

class SupportPage final : public QWidget {
  Q_OBJECT

public:
  explicit SupportPage(QWidget *parent = nullptr);

  void UpdateStatus(const DaemonStatusSnapshot &snapshot);

private:
  void GenerateSupportReport();
  void OnReportFinished(int exitCode, QProcess::ExitStatus exitStatus);
  void OnReportError(QProcess::ProcessError error);

  QLabel *summaryLabel_ = nullptr;
  QLabel *summaryDetailLabel_ = nullptr;
  QVBoxLayout *issuesLayout_ = nullptr;

  QLabel *daemonVersionLabel_ = nullptr;
  QLabel *daemonGitLabel_ = nullptr;
  QLabel *serviceStateLabel_ = nullptr;

  QLabel *reportStatusLabel_ = nullptr;
  QLabel *reportPathLabel_ = nullptr;
  QPushButton *generateReportButton_ = nullptr;
  QPushButton *copyReportButton_ = nullptr;
  QPlainTextEdit *reportText_ = nullptr;

  QPushButton *copyIssueDetailsButton_ = nullptr;
  QPushButton *copyInstallHintsButton_ = nullptr;
  QPushButton *copyRawStatusButton_ = nullptr;
  QPlainTextEdit *issueDetails_ = nullptr;
  QPlainTextEdit *installHints_ = nullptr;
  QPlainTextEdit *rawStatus_ = nullptr;

  QProcess *reportProcess_ = nullptr;
  QString reportProgram_;
  QString reportOutputPath_;
  QString currentIssueDetails_;
  QString currentInstallHints_;
  QString currentRawStatus_;
};

} // namespace studiocast::gui
